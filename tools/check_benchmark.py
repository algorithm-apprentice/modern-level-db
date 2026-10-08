#!/usr/bin/env python3
"""Validate the benchmark schema and the ADR-0041 severe-regression threshold."""

import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

MAX_SLOWDOWN = 20.0
PHASES = {"write", "read", "scan"}


def check_report(report, diagnostic_only=False):
    if not isinstance(report, dict) or set(report) != {
        "schema_version", "entries", "trials", "samples"
    }:
        raise ValueError("benchmark report has an invalid schema")
    if type(report["schema_version"]) is not int or report["schema_version"] != 1:
        raise ValueError("unsupported benchmark schema version")
    entries, trials = report["entries"], report["trials"]
    if type(entries) is not int or not 1 <= entries <= 1_000_000:
        raise ValueError("invalid benchmark entry count")
    if type(trials) is not int or not 3 <= trials <= 31 or trials % 2 != 1:
        raise ValueError("invalid benchmark trial count")
    samples = report["samples"]
    if not isinstance(samples, dict) or set(samples) != {"modern", "leveldb"}:
        raise ValueError("benchmark must report both implementations")
    medians = {}
    for engine, phases in samples.items():
        if not isinstance(phases, dict) or set(phases) != PHASES:
            raise ValueError(f"{engine}: missing or extra benchmark phases")
        medians[engine] = {}
        for phase, values in phases.items():
            if not isinstance(values, list) or len(values) != trials:
                raise ValueError(f"{engine}/{phase}: incorrect sample count")
            for value in values:
                if (type(value) not in (int, float)
                        or not math.isfinite(value) or value <= 0):
                    raise ValueError(f"{engine}/{phase}: samples must be positive finite numbers")
            medians[engine][phase] = statistics.median(values)
    ratios = {
        phase: medians["modern"][phase] / medians["leveldb"][phase]
        for phase in sorted(PHASES)
    }
    for phase, ratio in ratios.items():
        if not math.isfinite(ratio):
            raise ValueError(f"{phase}: slowdown ratio must remain finite")
        if not diagnostic_only and ratio > MAX_SLOWDOWN:
            raise ValueError(f"{phase}: slowdown {ratio:.3f}x exceeds {MAX_SLOWDOWN:.0f}x")
    return ratios


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def bind_run(report_path, binary, policy_path, diagnostic_only, snapshots=None):
    if snapshots is None:
        snapshots = (binary.read_bytes(), report_path.read_bytes(), policy_path.read_bytes())
    binary_bytes, samples_bytes, policy_bytes = snapshots
    check_report(json.loads(samples_bytes), diagnostic_only=diagnostic_only)
    policy = json.loads(policy_bytes)
    required = {
        "schema_version", "samples_schema_version", "platform", "compiler_id",
        "compiler_version", "build_configuration", "target_architecture",
        "reference_revision", "reference_source", "reference_dirty",
        "modern_file_access", "reference_file_access", "modern_namespace_policy",
        "sync_wal_creation", "workload_writes", "performance_policy",
    }
    if not isinstance(policy, dict) or set(policy) != required:
        raise ValueError("benchmark policy sidecar has an invalid schema")
    expected = "diagnostic-only" if diagnostic_only else "severe-regression"
    if (type(policy["schema_version"]) is not int or policy["schema_version"] != 1
            or type(policy["samples_schema_version"]) is not int
            or policy["samples_schema_version"] != 1
            or policy["performance_policy"] != expected):
        raise ValueError("benchmark sidecar does not match the trusted caller policy")
    text_fields = required - {"schema_version", "samples_schema_version", "sync_wal_creation"}
    if (any(type(policy[field]) is not str or not policy[field] for field in text_fields)
            or policy["sync_wal_creation"] is not True
            or policy["workload_writes"] != "unsynced"
            or policy["reference_file_access"] != "platform_default"):
        raise ValueError("benchmark sidecar has invalid provenance fields")
    if diagnostic_only and (
            policy["platform"] != "Windows" or policy["compiler_id"] != "MSVC"
            or policy["target_architecture"] != "native_x64"
            or policy["modern_file_access"] not in ("copied", "mapped_default")
            or policy["modern_namespace_policy"] != "explicit_weak"):
        raise ValueError("diagnostic baseline policy must describe the admitted Windows backend")
    binding = {
        "schema_version": 1,
        "binary_sha256": hashlib.sha256(binary_bytes).hexdigest(),
        "samples_sha256": hashlib.sha256(samples_bytes).hexdigest(),
        "policy_sha256": hashlib.sha256(policy_bytes).hexdigest(),
        "build_configuration": policy["build_configuration"],
        "performance_policy": expected,
    }
    for key, path in (("binary_sha256", binary), ("samples_sha256", report_path),
                      ("policy_sha256", policy_path)):
        if digest(path) != binding[key]:
            raise ValueError("benchmark artifact changed before publication")
    temporary = report_path.with_suffix(".provenance.json.tmp")
    temporary.write_text(json.dumps(binding, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(report_path.with_suffix(".provenance.json"))
    return binding


@contextlib.contextmanager
def publication_lock(report_path):
    report_path.parent.mkdir(parents=True, exist_ok=True)
    lock = report_path.with_suffix(".run.lock")
    descriptor = os.open(lock, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--run", type=Path, help="Run this benchmark before checking its report")
    parser.add_argument("--diagnostic-only", action="store_true",
                        help="Validate and report measurements without performance admission")
    parser.add_argument("--policy-sidecar", type=Path,
                        help="Bind this executed run to its configured policy and binary")
    args = parser.parse_args()
    try:
        guard = publication_lock(args.report) if args.run is not None else contextlib.nullcontext()
        with guard:
            snapshots = None
            if args.run is not None:
                args.report.with_suffix(".provenance.json").unlink(missing_ok=True)
                binary_bytes = args.run.resolve().read_bytes()
                policy_bytes = (args.policy_sidecar.read_bytes()
                                if args.policy_sidecar is not None else None)
                result = subprocess.run(
                    [str(args.run.resolve())], check=True, stdout=subprocess.PIPE, timeout=180
                )
                samples_bytes = result.stdout
                if args.run.resolve().read_bytes() != binary_bytes:
                    raise ValueError("benchmark binary changed during execution")
                if (args.policy_sidecar is not None
                        and args.policy_sidecar.read_bytes() != policy_bytes):
                    raise ValueError("benchmark policy changed during execution")
                check_report(json.loads(samples_bytes), diagnostic_only=args.diagnostic_only)
                args.report.write_bytes(samples_bytes)
                snapshots = (binary_bytes, samples_bytes, policy_bytes)
            else:
                samples_bytes = args.report.read_bytes()
            ratios = check_report(json.loads(samples_bytes), diagnostic_only=args.diagnostic_only)
            output = {"median_slowdown": ratios}
            if args.policy_sidecar is not None:
                if args.run is None:
                    raise ValueError("run binding requires an actually executed benchmark")
                output["run_binding"] = bind_run(
                    args.report, args.run.resolve(), args.policy_sidecar, args.diagnostic_only,
                    snapshots=snapshots
                )
            print(json.dumps(output, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"benchmark gate failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
