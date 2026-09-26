#!/usr/bin/env python3
"""Run one ADR-0042 workload, preserving native results and explicit provenance."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

import profile_report

CASES = tuple(
    f"{engine}/{workload}/{records}"
    for engine in ("modern", "leveldb")
    for workload in ("readrandom", "readmissing", "scan", "seek_reuse")
    for records in (4096, 65536)
)
FINGERPRINTS = {
    4096: ("e966aa2f", "387c287f", "c3e3b3de", "7883c9b4"),
    65536: ("3fbabb34", "347ed266", "2422abad", "8ee790ec"),
}


def case_parts(case):
    if case not in CASES:
        raise ValueError("unknown performance case")
    engine, workload, records = case.split("/")
    return engine, workload, int(records)


def integer(value, minimum=1):
    return type(value) is int and value >= minimum


def number(value, minimum=0):
    return type(value) in (int, float) and math.isfinite(value) and value >= minimum


def validate_benchmark(report, case, repetitions):
    _, workload, records = case_parts(case)
    if not isinstance(report, dict) or not isinstance(report.get("context"), dict):
        raise ValueError("missing Google Benchmark context")
    context = report["context"]
    if (context.get("library_version") != "v1.9.5"
            or type(context.get("json_schema_version")) is not int
            or context["json_schema_version"] != 1
            or context.get("profile_case") != case
            or context.get("timing") != "wall_and_process_cpu"
            or context.get("build_type") != "Release"):
        raise ValueError("incorrect benchmark version, case, timing mode, or build type")
    rows = report.get("benchmarks")
    if not isinstance(rows, list) or not rows:
        raise ValueError("no benchmark results")
    individuals = []
    expected_name = f"{case}/process_time/real_time"
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("malformed benchmark row")
        if row.get("error_occurred") or row.get("skipped"):
            raise ValueError("benchmark reported an error or skipped a case")
        if row.get("run_name") != expected_name:
            raise ValueError("benchmark returned a different case")
        if row.get("run_type") == "iteration":
            individuals.append(row)
        elif row.get("run_type") != "aggregate":
            raise ValueError("unknown benchmark row type")
    if len(individuals) != repetitions or not integer(repetitions):
        raise ValueError("incorrect individual repetition count")
    items = records if workload == "scan" else 1
    seen = set()
    for row in individuals:
        index = row.get("repetition_index")
        if not integer(index, 0) or index >= repetitions or index in seen:
            raise ValueError("invalid or duplicate repetition index")
        seen.add(index)
        if (not integer(row.get("repetitions")) or row["repetitions"] != repetitions
                or type(row.get("threads")) is not int or row["threads"] != 1
                or not integer(row.get("iterations")) or row.get("time_unit") != "ns"
                or row.get("name") != expected_name):
            raise ValueError("incorrect benchmark counts, case, threads, or units")
        if (not number(row.get("items_per_iteration"), 1)
                or row["items_per_iteration"] != items):
            raise ValueError("incorrect work items per benchmark iteration")
        if not number(row.get("real_time")) or row["real_time"] <= 0:
            raise ValueError("wall time must be positive and finite")
        if not number(row.get("cpu_time")):
            raise ValueError("process CPU time must be nonnegative and finite")
        rate = row.get("items_per_second")
        if (not number(rate) or rate <= 0
                or not math.isclose(rate, items * 1e9 / row["real_time"], rel_tol=1e-6)):
            raise ValueError("work-item throughput does not match the timing unit")
    individuals.sort(key=lambda row: row["repetition_index"])
    return {
        "iterations": [row["iterations"] for row in individuals],
        "items_per_iteration": items,
        "wall_ns_per_item": [row["real_time"] / items for row in individuals],
        "process_cpu_ns_per_item": [row["cpu_time"] / items for row in individuals],
        "statistics_are_not_request_percentiles": True,
    }


def validate_completion(report, case):
    _, workload, records = case_parts(case)
    fields = {
        "schema_version", "case", "preparations", "verifications", "callback_invocations",
        "cursor_resets", "warmup_operations", "retained_iterators", "scan_creations",
        "scan_destructions", "record_crc32c", "insertion_crc32c", "present_crc32c", "missing_crc32c",
    }
    if not isinstance(report, dict) or set(report) != fields:
        raise ValueError("invalid workload completion schema")
    if (type(report["schema_version"]) is not int or report["schema_version"] != 1
            or report["case"] != case):
        raise ValueError("incorrect workload completion version or case")
    for field in ("preparations", "verifications", "callback_invocations", "cursor_resets",
                  "warmup_operations", "retained_iterators", "scan_creations", "scan_destructions"):
        if not integer(report[field], 0):
            raise ValueError(f"invalid completion counter: {field}")
    if report["preparations"] != 1 or report["verifications"] != 2:
        raise ValueError("fixture setup and verification must be one-shot")
    if (report["callback_invocations"] < 1
            or report["cursor_resets"] != report["callback_invocations"]):
        raise ValueError("query cursors did not reset on every callback")
    warmup = 1 if workload == "scan" else records - (workload == "readmissing")
    if report["warmup_operations"] != warmup:
        raise ValueError("incorrect selected-workload warmup")
    if report["retained_iterators"] != (1 if workload == "seek_reuse" else 0):
        raise ValueError("incorrect retained iterator lifetime")
    scans = report["scan_creations"]
    if scans != report["scan_destructions"]:
        raise ValueError("a scan iterator outlived its operation")
    if workload == "scan":
        if scans < report["callback_invocations"] + 3:
            raise ValueError("missing scan lifecycle operations")
    elif scans != 2:
        raise ValueError("verification ran inside calibration")
    for field, expected in zip(
        ("record_crc32c", "insertion_crc32c", "present_crc32c", "missing_crc32c"),
        FINGERPRINTS[records],
    ):
        if report[field] != expected:
            raise ValueError(f"canonical corpus drift: {field}")
    return report


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"),
                      object_pairs_hook=reject_duplicate_keys)


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
                          encoding="utf-8")


def file_digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_state(source):
    result = {}
    for key, args in (
        ("revision", ["rev-parse", "HEAD"]),
        ("status", ["status", "--porcelain", "--untracked-files=normal"]),
    ):
        value = subprocess.run(["git", "-C", str(source), *args], text=True,
                               capture_output=True, timeout=15)
        if value.returncode != 0:
            return {"available": False, "reason": value.stderr.strip()}
        result[key] = value.stdout.strip()
    return {"available": True, "revision": result["revision"], "dirty": bool(result["status"]),
            "status_sha256": hashlib.sha256(result["status"].encode()).hexdigest()}


def validate_build_context(context):
    required = (
        "source_directory", "build_directory", "configure_revision", "configure_dirty",
        "compiler", "c_flags", "cxx_flags", "benchmark_requested_revision", "benchmark_source_override",
        "reference_requested_revision", "reference_source_override",
        "snappy_target", "snappy_source", "snappy_source_override",
        "zstd_target", "zstd_source", "zstd_source_override", "profile_capture_supported",
    )
    if any(not isinstance(context.get(key), str) for key in required):
        raise ValueError("benchmark build provenance is incomplete")
    if (context["configure_dirty"] not in ("true", "false", "unknown")
            or context["profile_capture_supported"] not in ("true", "false")):
        raise ValueError("invalid build provenance flags")
    flags = context["c_flags"] + " " + context["cxx_flags"]
    if any(flag in flags for flag in ("--coverage", "-fprofile", "-fsanitize")):
        raise ValueError("instrumented build timings are not performance measurements")
    if not Path(context["source_directory"]).is_absolute() or not Path(context["build_directory"]).is_absolute():
        raise ValueError("build provenance paths must be absolute")


def record_diagnostics(manifest, output):
    manifest["artifacts"]["command_logs"] = list(dict.fromkeys(
        command["log"] for command in manifest["commands"]
    ))
    if manifest["mode"] != "cpu_profile":
        return
    manifest["artifacts"]["target_log"] = "target.log"
    manifest["artifacts"]["collector_version_log"] = "collector-version.log"
    version_log = output / "collector-version.log"
    if version_log.is_file():
        try:
            manifest["collector"] = profile_report.collector_version(version_log)
        except ValueError as error:
            manifest["collector_version_error"] = str(error)
            if manifest["status"] == "complete":
                raise
    elif manifest["status"] == "complete":
        raise ValueError("successful capture has no collector version record")


def run_case(binary, case, output, capture_cpu=False, smoke=False, repetitions=None,
             min_time=None, timeout=None):
    case_parts(case)
    binary = Path(binary).resolve(strict=True)
    output = Path(output).absolute()
    if not binary.is_file():
        raise ValueError("benchmark binary is not a file")
    if capture_cpu and sys.platform != "darwin":
        raise ValueError("CPU collection requires macOS Xcode Time Profiler")
    if capture_cpu and smoke:
        raise ValueError("a one-iteration smoke run is not a CPU profile")
    repetitions = repetitions if repetitions is not None else (1 if capture_cpu or smoke else 3)
    min_time = min_time if min_time is not None else (5.0 if capture_cpu else 0.2)
    timeout = timeout if timeout is not None else (180.0 if capture_cpu else 300.0)
    if not integer(repetitions) or repetitions > 31:
        raise ValueError("repetitions must be in [1, 31]")
    if (capture_cpu or smoke) and repetitions != 1:
        raise ValueError("capture and smoke modes require one repetition")
    if not number(min_time) or not 0 < min_time <= 60:
        raise ValueError("minimum time must be finite and in (0, 60]")
    if not number(timeout) or timeout <= 0:
        raise ValueError("timeout must be positive and finite")
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    work = output / "work"
    work.mkdir()
    manifest = {
        "schema_version": 1,
        "case": case,
        "mode": "cpu_profile" if capture_cpu else ("smoke" if smoke else "benchmark"),
        "status": "running",
        "executable_sha256": file_digest(binary),
        "original_executable": str(binary),
        "host_platform": platform.platform(),
        "python_version": platform.python_version(),
        "commands": [],
        "artifacts": {"raw_benchmark": "benchmark.json", "completion": "completion.json"},
        "recording_timings_are_not_speedup_evidence": capture_cpu,
    }
    manifest_path = output / "manifest.json"
    write_json(manifest_path, manifest)
    command = [
        "--case", case, "--database", str(work / "db"),
        "--completion-report", str(output / "completion.json"),
        f"--benchmark_min_time={'1x' if smoke else str(min_time) + 's'}",
        f"--benchmark_repetitions={repetitions}", "--benchmark_min_warmup_time=0",
        "--benchmark_enable_random_interleaving=false", "--benchmark_dry_run=false",
        "--benchmark_list_tests=false",
        "--benchmark_report_aggregates_only=false", "--benchmark_time_unit=ns",
        f"--benchmark_out={output / 'benchmark.json'}", "--benchmark_out_format=json",
        "--benchmark_color=false",
    ]
    started = time.monotonic()
    try:
        if capture_cpu:
            snapshot = output / "profile-program"
            shutil.copy2(binary, snapshot)
            if file_digest(snapshot) != manifest["executable_sha256"]:
                raise ValueError("benchmark executable changed while preparing capture")
            command.append("--profile-markers")
            manifest["artifacts"].update(
                executable="profile-program", symbols="profile-program.dSYM",
                trace="profile.trace", toc="toc.xml", markers="markers.xml",
                samples="samples.xml", profile_summary="profile-summary.json",
            )
            profile_report.capture(snapshot, command, output, manifest["commands"], timeout)
        else:
            profile_report.run_owned(
                [str(binary), *command], output / "benchmark.log", timeout, manifest["commands"]
            )
        raw = read_json(output / "benchmark.json")
        manifest["measurement"] = validate_benchmark(raw, case, repetitions)
        manifest["completion"] = validate_completion(read_json(output / "completion.json"), case)
        context = raw["context"]
        validate_build_context(context)
        manifest["build"] = {
            key: value for key, value in context.items()
            if key not in ("date", "host_name", "executable", "caches", "load_avg")
        }
        source = Path(context["source_directory"])
        manifest["runtime_source"] = source_state(source)
        manifest["configure_revision_matches_runtime"] = (
            manifest["runtime_source"].get("revision") == context.get("configure_revision")
        )
        compile_commands = Path(context["build_directory"]) / "compile_commands.json"
        if compile_commands.is_file():
            shutil.copy2(compile_commands, output / "compile_commands.json")
            manifest["artifacts"]["compile_commands"] = "compile_commands.json"
            manifest["compile_commands_sha256"] = file_digest(output / "compile_commands.json")
        else:
            manifest["compile_commands_unavailable"] = True
        if capture_cpu:
            if context["profile_capture_supported"] != "true":
                raise ValueError("benchmark build does not support macOS profile markers")
            summary = profile_report.summarize_trace(
                output / "toc.xml", output / "markers.xml", output / "samples.xml",
                case, manifest["measurement"]["iterations"][0],
            )
            write_json(output / "profile-summary.json", summary)
        manifest["status"] = "complete"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    except KeyboardInterrupt:
        manifest["status"] = "cancelled"
        manifest["error"] = "interrupted by user"
        raise
    finally:
        manifest["elapsed_seconds"] = time.monotonic() - started
        cleanup_verified = all(item["cleanup_verified"] for item in manifest["commands"])
        manifest["process_cleanup_verified"] = cleanup_verified
        try:
            record_diagnostics(manifest, output)
            if cleanup_verified:
                if work.is_symlink() or work.resolve().parent != output:
                    raise RuntimeError("scratch ownership changed; refusing cleanup")
                shutil.rmtree(work)
            else:
                manifest["scratch_retained"] = "work"
        except (OSError, RuntimeError, ValueError) as error:
            manifest["status"] = "failed"
            manifest["cleanup_error"] = str(error)
            raise
        finally:
            write_json(manifest_path, manifest)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture-cpu", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--min-time", type=float)
    parser.add_argument("--timeout", type=float)
    args = parser.parse_args()
    try:
        result = run_case(
            args.binary, args.case, args.output, args.capture_cpu, args.smoke,
            args.repetitions, args.min_time, args.timeout,
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(f"performance run failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("performance run cancelled; see preserved artifacts", file=sys.stderr)
        return 130
    print(json.dumps({"case": result["case"], "mode": result["mode"],
                      "measurement": result["measurement"], "artifacts": str(args.output)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
