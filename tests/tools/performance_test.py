import argparse
import copy
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_performance import CASES, read_json, record_diagnostics, run_case, validate_benchmark, validate_completion

ROOT = Path(__file__).resolve().parents[2]
BINARY = None


def report(case="modern/readrandom/4096", repetitions=3):
    return {
        "context": {
            "library_version": "v1.9.5",
            "json_schema_version": 1,
            "profile_case": case,
            "build_type": "Release",
            "timing": "wall_and_process_cpu",
        },
        "benchmarks": [
            {
                "name": case + "/process_time/real_time",
                "run_name": case + "/process_time/real_time",
                "run_type": "iteration",
                "repetitions": repetitions,
                "repetition_index": index,
                "threads": 1,
                "iterations": 10,
                "real_time": 123.0,
                "cpu_time": 100.0,
                "time_unit": "ns",
                "items_per_iteration": 1,
                "items_per_second": 1e9 / 123.0,
            }
            for index in range(repetitions)
        ],
    }


def completion(case="modern/readrandom/4096"):
    return {
        "schema_version": 1,
        "case": case,
        "preparations": 1,
        "verifications": 2,
        "callback_invocations": 1,
        "cursor_resets": 1,
        "warmup_operations": 4096,
        "retained_iterators": 0,
        "scan_creations": 2,
        "scan_destructions": 2,
        "record_crc32c": "e966aa2f",
        "insertion_crc32c": "387c287f",
        "present_crc32c": "c3e3b3de",
        "missing_crc32c": "7883c9b4",
    }


class PerformanceReportTest(unittest.TestCase):
    def test_accepts_individual_runs_and_ignores_aggregate_counters(self):
        data = report()
        aggregate = copy.deepcopy(data["benchmarks"][0])
        aggregate.update(run_type="aggregate", aggregate_name="stddev",
                         items_per_iteration=0, real_time=0)
        data["benchmarks"].append(aggregate)
        result = validate_benchmark(data, "modern/readrandom/4096", 3)
        self.assertEqual(result["wall_ns_per_item"], [123.0] * 3)
        self.assertEqual(result["process_cpu_ns_per_item"], [100.0] * 3)

    def test_normalizes_scan_time_per_record(self):
        data = report("leveldb/scan/65536")
        for row in data["benchmarks"]:
            row.update(items_per_iteration=65536, real_time=65536.0,
                       cpu_time=131072.0, items_per_second=1e9)
        result = validate_benchmark(data, "leveldb/scan/65536", 3)
        self.assertEqual(result["wall_ns_per_item"], [1.0] * 3)
        self.assertEqual(result["process_cpu_ns_per_item"], [2.0] * 3)

    def test_rejects_errors_skips_missing_runs_and_duplicate_repetitions(self):
        for key in ("error_occurred", "skipped"):
            data = report()
            data["benchmarks"][0][key] = True
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_benchmark(data, "modern/readrandom/4096", 3)
        for replacement in ([], report()["benchmarks"][:-1]):
            data = report()
            data["benchmarks"] = replacement
            with self.assertRaises(ValueError):
                validate_benchmark(data, "modern/readrandom/4096", 3)
        data = report()
        data["benchmarks"][1]["repetition_index"] = 0
        with self.assertRaises(ValueError):
            validate_benchmark(data, "modern/readrandom/4096", 3)

    def test_rejects_wrong_case_counts_units_and_nonfinite_times(self):
        for field, invalid in (
            ("run_name", "other"), ("threads", 2), ("iterations", 0),
            ("repetitions", 1), ("items_per_iteration", 2), ("time_unit", "ms"),
            ("real_time", 0), ("real_time", math.nan), ("cpu_time", -1),
            ("real_time", math.inf), ("iterations", True), ("cpu_time", "100"),
        ):
            data = report()
            data["benchmarks"][0][field] = invalid
            with self.subTest(field=field, invalid=invalid), self.assertRaises(ValueError):
                validate_benchmark(data, "modern/readrandom/4096", 3)
        data = report()
        data["benchmarks"][0]["cpu_time"] = 0
        validate_benchmark(data, "modern/readrandom/4096", 3)

    def test_completion_requires_canonical_fingerprints_and_one_shot_lifecycle(self):
        validate_completion(completion(), "modern/readrandom/4096")
        for field, value in (
            ("preparations", 2), ("verifications", 1), ("cursor_resets", 0),
            ("record_crc32c", "00000000"), ("insertion_crc32c", "00000000"),
            ("present_crc32c", "00000000"), ("missing_crc32c", "00000000"),
            ("scan_destructions", 1), ("warmup_operations", 1),
        ):
            data = completion()
            data[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_completion(data, "modern/readrandom/4096")

    def test_rejects_duplicate_and_partial_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report.json"
            for text in ('{"benchmarks":[],"benchmarks":[]}', '{"context":'):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    read_json(path)

    def test_runner_preserves_artifacts_and_removes_only_its_scratch_after_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "failed"
            with self.assertRaises(subprocess.CalledProcessError):
                run_case(Path("/usr/bin/false"), "modern/readrandom/4096", output, smoke=True)
            self.assertFalse((output / "work").exists())
            self.assertTrue((output / "benchmark.log").exists())
            manifest = read_json(output / "manifest.json")
            self.assertEqual(manifest["status"], "failed")
            self.assertEqual(manifest["artifacts"]["command_logs"], ["benchmark.log"])
            self.assertEqual(manifest["commands"][0]["log"], "benchmark.log")
            sentinel = output / "keep"
            sentinel.write_text("keep")
            with self.assertRaises(FileExistsError):
                run_case(Path("/usr/bin/false"), "modern/readrandom/4096", output, smoke=True)
            self.assertEqual(sentinel.read_text(), "keep")

    def test_capture_manifest_indexes_collector_version_and_all_command_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            logs = ("collector-version.log", "symbols.log", "collector.log",
                    "toc.xml.log", "markers.xml.log", "samples.xml.log")
            for log in logs:
                (root / log).write_text("diagnostic")
            (root / "collector-version.log").write_text("xctrace version 27.0 (27A266a)\n")
            (root / "target.log").write_text("workload output")
            manifest = {
                "mode": "cpu_profile", "status": "complete", "artifacts": {},
                "commands": [{"log": log} for log in logs],
            }
            record_diagnostics(manifest, root)
            self.assertEqual(manifest["collector"],
                             {"name": "xctrace", "version": "27.0", "build": "27A266a"})
            self.assertEqual(manifest["artifacts"]["command_logs"], list(logs))
            self.assertEqual(manifest["artifacts"]["target_log"], "target.log")
            self.assertEqual(manifest["artifacts"]["collector_version_log"], "collector-version.log")
            (root / "collector-version.log").write_text("not a version")
            with self.assertRaises(ValueError):
                record_diagnostics(manifest, root)


class PerformanceExecutableTest(unittest.TestCase):
    def setUp(self):
        if BINARY is None:
            self.skipTest("pass --binary to exercise the built benchmark")
        self.temporary = tempfile.TemporaryDirectory(prefix="modern-perf-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def invoke(self, *args, check=False):
        return subprocess.run([str(BINARY), *args], cwd=self.root, capture_output=True,
                              text=True, check=check, timeout=90)

    def test_lists_exactly_sixteen_cases_without_creating_a_database(self):
        result = self.invoke("--list-cases", check=True)
        self.assertEqual(set(result.stdout.splitlines()), set(CASES))
        self.assertEqual(list(self.root.iterdir()), [])
        self.invoke("--help", check=True)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_rejects_invalid_cases_and_empty_framework_filter(self):
        for case in ("modern/readrandom/4097", "other/scan/4096", "modern/write/4096"):
            result = self.invoke("--case", case, "--database", str(self.root / "db"))
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((self.root / "db").exists())
        result = self.invoke(
            "--case", "modern/readrandom/4096", "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_filter=^nothing$")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "db").exists())

    def test_operation_failure_cannot_return_success(self):
        (self.root / "db").write_text("not a database")
        result = self.invoke(
            "--case", "modern/readrandom/4096", "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_time=1x",
            f"--benchmark_out={self.root / 'benchmark.json'}", "--benchmark_out_format=json")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "completion.json").exists())
        self.assertEqual((self.root / "db").read_text(), "not a database")

    def test_zero_repetitions_cannot_produce_a_successful_empty_measurement(self):
        result = self.invoke(
            "--case", "modern/readrandom/4096", "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_repetitions=0", "--benchmark_min_time=1x")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "completion.json").exists())

    def test_adaptive_repetitions_prepare_and_verify_only_once(self):
        self.invoke(
            "--case", "modern/readrandom/4096", "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_time=0.03s", "--benchmark_repetitions=3",
            f"--benchmark_out={self.root / 'benchmark.json'}",
            "--benchmark_out_format=json", check=True)
        done = json.loads((self.root / "completion.json").read_text())
        validate_completion(done, "modern/readrandom/4096")
        self.assertGreater(done["callback_invocations"], 3)
        data = json.loads((self.root / "benchmark.json").read_text())
        validate_benchmark(data, "modern/readrandom/4096", 3)
        context = data["context"]
        self.assertIn(context["crc32c_target"], ("crc32c", "Crc32c::crc32c"))
        self.assertIn(context["crc32c_provider"],
                      ("pinned-source", "source-override", "parent-target"))
        self.assertEqual(context["crc32c_requested_revision"],
                         "2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07")
        self.assertIn(context["crc32c_compiled_arm64"], ("true", "false", "external"))
        self.assertIn(context["crc32c_compiled_sse42"], ("true", "false", "external"))
        for field in ("crc32c_source", "crc32c_source_override"):
            self.assertIsInstance(context[field], str)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path)
    arguments, remaining = parser.parse_known_args()
    BINARY = arguments.binary.resolve() if arguments.binary else None
    unittest.main(argv=[sys.argv[0], *remaining])
