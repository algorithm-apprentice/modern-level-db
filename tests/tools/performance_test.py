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
from run_performance import (
    CASES,
    FINGERPRINTS,
    READ_DIAGNOSTIC_COMPARISON_COUNTERS,
    READ_DIAGNOSTIC_COUNTERS,
    READ_DIAGNOSTIC_OPERATIONS,
    READ_DIAGNOSTIC_OPEN_REASONS,
    READ_DIAGNOSTIC_SAMPLE_DENOMINATOR,
    READ_DIAGNOSTIC_SAMPLE_SCHEDULE,
    READ_DIAGNOSTIC_SAMPLE_SEED,
    READ_DIAGNOSTIC_SAMPLES,
    READ_DIAGNOSTIC_STAGES,
    compare_read_diagnostics,
    read_json,
    record_diagnostics,
    run_case,
    run_read_diagnostics,
    validate_benchmark,
    validate_completion,
    validate_read_diagnostics,
)

ROOT = Path(__file__).resolve().parents[2]
BINARY = None
REFERENCE_PREAD_CONTROL = None


def report(case="modern/readrandom/4096", repetitions=3):
    engine = case.split("/", 1)[0]
    return {
        "context": {
            "library_version": "v1.9.5",
            "json_schema_version": 1,
            "profile_case": case,
            "build_type": "Release",
            "timing": "wall_and_process_cpu",
            "modern_file_access": "default" if engine == "modern" else "not_applicable",
            "reference_file_access": "default" if engine == "leveldb" else "not_applicable",
            "reference_pread_control_available": "true",
            "reference_control_patch_sha256": "a" * 64,
            "read_diagnostics_compiled": "false",
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


def diagnostic_report(
        case="modern/readrandom/4096", modern_file_access="default", schema_version=3):
    _, workload, records = case.split("/")
    counters = {
        name: {"total": 0, "per_get": 0.0}
        for name in READ_DIAGNOSTIC_COUNTERS
    }
    counters["gets"] = {
        "total": READ_DIAGNOSTIC_OPERATIONS,
        "per_get": 1.0,
    }
    outcome = "sstable_hits" if workload == "readrandom" else "misses"
    counters[outcome] = {
        "total": READ_DIAGNOSTIC_OPERATIONS,
        "per_get": 1.0,
    }
    for name, total in (
        ("deeper_candidates", READ_DIAGNOSTIC_OPERATIONS),
        ("files_searched", READ_DIAGNOSTIC_OPERATIONS),
        ("table_cache_hits", READ_DIAGNOSTIC_OPERATIONS),
        ("block_cache_hits", READ_DIAGNOSTIC_OPERATIONS),
        ("index_entries_decoded", READ_DIAGNOSTIC_OPERATIONS * 2),
        ("data_entries_decoded", READ_DIAGNOSTIC_OPERATIONS * 4),
        ("restart_entries_decoded", READ_DIAGNOSTIC_OPERATIONS * 2),
        ("internal_key_comparisons", READ_DIAGNOSTIC_OPERATIONS * 8),
    ):
        counters[name] = {
            "total": total,
            "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
        }
    if int(records) == 65536:
        misses = 1024
        counters["block_cache_hits"] = {
            "total": READ_DIAGNOSTIC_OPERATIONS - misses,
            "per_get": (READ_DIAGNOSTIC_OPERATIONS - misses) / READ_DIAGNOSTIC_OPERATIONS,
        }
        for name, total in (
            ("block_cache_misses", misses),
            ("stored_blocks", misses),
            ("decoded_blocks", misses),
            ("decompressed_blocks", misses),
        ):
            counters[name] = {
                "total": total,
                "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
            }
        for name in ("stored_block_bytes",):
            total = misses * 1024
            counters[name] = {
                "total": total,
                "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
            }
        if modern_file_access == "mmap":
            for name in ("mapped_view_blocks",):
                counters[name] = {
                    "total": misses,
                    "per_get": misses / READ_DIAGNOSTIC_OPERATIONS,
                }
            total = misses * 1024
            counters["mapped_view_bytes"] = {
                "total": total,
                "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
            }
        else:
            for name in ("random_read_calls", "copied_read_blocks"):
                counters[name] = {
                    "total": misses,
                    "per_get": misses / READ_DIAGNOSTIC_OPERATIONS,
                }
            total = misses * 1024
            for name in ("random_read_requested_bytes", "random_read_returned_bytes",
                         "copied_read_bytes"):
                counters[name] = {
                    "total": total,
                    "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
                }
        total = misses * 4096
        counters["decoded_block_bytes"] = {
            "total": total,
            "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
        }
        if schema_version == 2:
            total = misses * 16
            counters["validation_entries"] = {
                "total": total,
                "per_get": total / READ_DIAGNOSTIC_OPERATIONS,
            }
    if workload == "readrandom":
        total = READ_DIAGNOSTIC_OPERATIONS * 256
        counters["result_bytes"] = {"total": total, "per_get": 256.0}
    stages = {
        name: {"events": 0, "total_ns": 0, "mean_ns": 0.0}
        for name in READ_DIAGNOSTIC_STAGES
    }
    stages["get"] = {
        "events": READ_DIAGNOSTIC_SAMPLES,
        "total_ns": READ_DIAGNOSTIC_SAMPLES * 100,
        "mean_ns": 100.0,
    }
    for name in ("candidate_selection", "table_cache_lookup", "block_cache_lookup",
                 "index_seek", "data_seek"):
        stages[name] = {
            "events": READ_DIAGNOSTIC_SAMPLES,
            "total_ns": READ_DIAGNOSTIC_SAMPLES * 10,
            "mean_ns": 10.0,
        }
    if workload == "readrandom":
        stages["result_copy"] = {
            "events": READ_DIAGNOSTIC_SAMPLES,
            "total_ns": READ_DIAGNOSTIC_SAMPLES * 10,
            "mean_ns": 10.0,
        }
    if int(records) == 65536:
        for name in ("stored_block_decode", "block_construction"):
            stages[name] = {"events": 1, "total_ns": 10, "mean_ns": 10.0}
        if modern_file_access == "default":
            stages["random_read"] = {"events": 1, "total_ns": 10, "mean_ns": 10.0}
    build = {
        "source_directory": "/source",
        "build_directory": "/build",
        "configure_revision": "revision",
        "configure_dirty": "false",
        "build_type": "Release",
        "compiler": "compiler",
        "c_flags": "-O3",
        "cxx_flags": "-O3",
        "benchmark_requested_revision": "192ef10025eb2c4cdd392bc502f0c852196baa48",
        "benchmark_source_override": "",
        "reference_requested_revision": "7ee830d02b623e8ffe0b95d59a74db1e58da04c5",
        "reference_source_override": "",
        "reference_hardware_crc": "disabled",
        "modern_file_access": modern_file_access,
        "reference_file_access": "not_applicable",
        "reference_pread_control_available": "true",
        "reference_control_patch_sha256": "a" * 64,
        "snappy_target": "snappy",
        "snappy_source": "/snappy",
        "snappy_source_override": "",
        "zstd_target": "zstd",
        "zstd_source": "/zstd",
        "zstd_source_override": "",
        "crc32c_target": "crc32c",
        "crc32c_provider": "pinned-source",
        "crc32c_source": "/crc32c",
        "crc32c_source_override": "",
        "crc32c_requested_revision": "2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07",
        "crc32c_compiled_arm64": "true",
        "crc32c_compiled_sse42": "false",
        "profile_capture_supported": "false",
        "read_diagnostics_compiled": "true",
    }
    return {
        "schema_version": schema_version,
        "case": case,
        "operations": READ_DIAGNOSTIC_OPERATIONS,
        "sample_schedule": READ_DIAGNOSTIC_SAMPLE_SCHEDULE,
        "sample_seed": READ_DIAGNOSTIC_SAMPLE_SEED,
        "sample_denominator": READ_DIAGNOSTIC_SAMPLE_DENOMINATOR,
        "sampled_gets": READ_DIAGNOSTIC_SAMPLES,
        "foreground_thread_only": True,
        "stage_durations_are_inclusive": True,
        "setup_warmup_and_verification_excluded": True,
        "preparations": 1,
        "verifications": 2,
        "cursor_resets": 1,
        "warmup_operations": int(records) - (workload == "readmissing"),
        "record_crc32c": FINGERPRINTS[int(records)][0],
        "insertion_crc32c": FINGERPRINTS[int(records)][1],
        "present_crc32c": FINGERPRINTS[int(records)][2],
        "missing_crc32c": FINGERPRINTS[int(records)][3],
        "setup_file_opens": {
            name: {
                "files": 8 if name == ("mapped" if modern_file_access == "mmap" else "disabled")
                         else 0,
                "bytes": 371673
                         if name == ("mapped" if modern_file_access == "mmap" else "disabled")
                         else 0,
            }
            for name in READ_DIAGNOSTIC_OPEN_REASONS
        },
        "counters": counters,
        "stages": stages,
        "build": build,
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

    def test_validates_reference_file_access_provenance(self):
        data = report("leveldb/readrandom/4096")
        validate_benchmark(data, "leveldb/readrandom/4096", 3)
        data["context"]["reference_file_access"] = "pread"
        validate_benchmark(
            data, "leveldb/readrandom/4096", 3, reference_file_access="pread"
        )
        data["context"]["reference_pread_control_available"] = "false"
        with self.assertRaises(ValueError):
            validate_benchmark(
                data, "leveldb/readrandom/4096", 3, reference_file_access="pread"
            )
        with self.assertRaises(ValueError):
            validate_benchmark(
                report(), "modern/readrandom/4096", 3, reference_file_access="pread"
            )
        data = report()
        data["context"]["read_diagnostics_compiled"] = "true"
        with self.assertRaises(ValueError):
            validate_benchmark(data, "modern/readrandom/4096", 3)
        with self.assertRaises(ValueError):
            validate_benchmark(
                report("leveldb/scan/4096"),
                "leveldb/scan/4096",
                3,
                reference_file_access="pread",
            )

    def test_validates_modern_file_access_provenance(self):
        data = report()
        validate_benchmark(data, "modern/readrandom/4096", 3)
        data["context"]["modern_file_access"] = "mmap"
        validate_benchmark(
            data, "modern/readrandom/4096", 3, modern_file_access="mmap"
        )
        with self.assertRaises(ValueError):
            validate_benchmark(data, "modern/readrandom/4096", 3)
        with self.assertRaises(ValueError):
            validate_benchmark(
                report("leveldb/readrandom/4096"),
                "leveldb/readrandom/4096",
                3,
                modern_file_access="mmap",
            )

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


class ReadDiagnosticsReportTest(unittest.TestCase):
    def test_accepts_fixed_present_and_missing_reports(self):
        validate_read_diagnostics(
            diagnostic_report("modern/readrandom/4096"), "modern/readrandom/4096"
        )
        validate_read_diagnostics(
            diagnostic_report("modern/readmissing/65536"), "modern/readmissing/65536"
        )
        validate_read_diagnostics(
            diagnostic_report("modern/readmissing/65536", "mmap"),
            "modern/readmissing/65536",
            modern_file_access="mmap",
        )

    def test_rejects_changed_epoch_outcomes_and_normalization(self):
        for field, value in (
            ("operations", 1),
            ("sample_seed", 1),
            ("sample_denominator", 1),
            ("sampled_gets", 0),
            ("foreground_thread_only", False),
            ("verifications", 1),
            ("cursor_resets", 2),
        ):
            changed = diagnostic_report()
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_read_diagnostics(changed, "modern/readrandom/4096")

        changed = diagnostic_report()
        changed["counters"]["gets"]["per_get"] = 2.0
        with self.assertRaises(ValueError):
            validate_read_diagnostics(changed, "modern/readrandom/4096")

        changed = diagnostic_report()
        changed["counters"]["misses"]["total"] = 1
        changed["counters"]["misses"]["per_get"] = 1 / READ_DIAGNOSTIC_OPERATIONS
        with self.assertRaises(ValueError):
            validate_read_diagnostics(changed, "modern/readrandom/4096")

        changed = diagnostic_report()
        changed["stages"]["get"]["events"] = 1023
        with self.assertRaises(ValueError):
            validate_read_diagnostics(changed, "modern/readrandom/4096")

        changed = diagnostic_report()
        changed["counters"]["validation_entries"] = {
            "total": 1,
            "per_get": 1 / READ_DIAGNOSTIC_OPERATIONS,
        }
        with self.assertRaises(ValueError):
            validate_read_diagnostics(changed, "modern/readrandom/4096")

    def test_accepts_schema2_only_as_an_explicit_historical_report(self):
        historical = diagnostic_report(
            "modern/readmissing/65536", schema_version=2
        )
        with self.assertRaises(ValueError):
            validate_read_diagnostics(
                historical, "modern/readmissing/65536"
            )
        validate_read_diagnostics(
            historical,
            "modern/readmissing/65536",
            historical_schema2=True,
        )

        historical["counters"]["validation_entries"] = {
            "total": 0,
            "per_get": 0.0,
        }
        with self.assertRaises(ValueError):
            validate_read_diagnostics(
                historical,
                "modern/readmissing/65536",
                historical_schema2=True,
            )

    def test_rejects_incomplete_counter_stage_and_build_provenance(self):
        for section, key in (
            ("counters", "files_searched"),
            ("stages", "data_seek"),
            ("build", "read_diagnostics_compiled"),
        ):
            changed = diagnostic_report()
            del changed[section][key]
            with self.subTest(section=section, key=key), self.assertRaises(ValueError):
                validate_read_diagnostics(changed, "modern/readrandom/4096")

    def test_rejects_changed_compression_work(self):
        changed = diagnostic_report("modern/readmissing/65536", "mmap")
        changed["counters"]["decompressed_blocks"] = {"total": 0, "per_get": 0.0}
        with self.assertRaises(ValueError):
            validate_read_diagnostics(
                changed, "modern/readmissing/65536", modern_file_access="mmap"
            )

    def test_compares_frozen_storage_work_with_two_percent_limit(self):
        baseline = diagnostic_report("modern/readmissing/65536")
        candidate = diagnostic_report("modern/readmissing/65536", "mmap")
        result = compare_read_diagnostics(
            baseline, candidate, "modern/readmissing/65536"
        )
        self.assertEqual(set(result["counters"]), set(READ_DIAGNOSTIC_COMPARISON_COUNTERS))
        for name in READ_DIAGNOSTIC_COMPARISON_COUNTERS:
            changed = copy.deepcopy(candidate)
            counter = changed["counters"][name]
            counter["total"] = int(counter["total"] * 1.03) + 1
            counter["per_get"] = counter["total"] / READ_DIAGNOSTIC_OPERATIONS
            with self.subTest(name=name), self.assertRaises(ValueError):
                compare_read_diagnostics(
                    baseline, changed, "modern/readmissing/65536"
                )

    def test_compares_historical_schema2_without_validation_entry_drift(self):
        baseline = diagnostic_report(
            "modern/readmissing/65536", schema_version=2
        )
        candidate = diagnostic_report(
            "modern/readmissing/65536", "mmap"
        )
        with self.assertRaises(ValueError):
            compare_read_diagnostics(
                baseline, candidate, "modern/readmissing/65536"
            )
        result = compare_read_diagnostics(
            baseline,
            candidate,
            "modern/readmissing/65536",
            historical_schema2_baseline=True,
        )
        self.assertNotIn("validation_entries", result["counters"])
        self.assertEqual(
            set(result["incomparable_counters"]), {"validation_entries"}
        )
        self.assertGreater(
            result["incomparable_counters"]["validation_entries"][
                "baseline_per_get"
            ],
            0,
        )
        self.assertEqual(
            result["incomparable_counters"]["validation_entries"][
                "candidate_per_get"
            ],
            0,
        )

        schema2_candidate = diagnostic_report(
            "modern/readmissing/65536", "mmap", schema_version=2
        )
        with self.assertRaises(ValueError):
            compare_read_diagnostics(
                baseline,
                schema2_candidate,
                "modern/readmissing/65536",
                historical_schema2_baseline=True,
            )

    def test_comparison_cli_requires_the_historical_schema2_flag(self):
        case = "modern/readmissing/65536"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline.json"
            candidate = root / "candidate.json"
            baseline.write_text(json.dumps(
                diagnostic_report(case, schema_version=2)
            ))
            candidate.write_text(json.dumps(
                diagnostic_report(case, "mmap")
            ))
            command = [
                sys.executable,
                str(ROOT / "tools" / "compare_read_diagnostics.py"),
                "--baseline", str(baseline),
                "--candidate", str(candidate),
                "--case", case,
            ]

            rejected = subprocess.run(
                command, capture_output=True, text=True, check=False
            )
            self.assertNotEqual(rejected.returncode, 0)
            accepted = subprocess.run(
                command + ["--historical-schema2-baseline"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            comparison = json.loads(accepted.stdout)
            self.assertEqual(
                set(comparison["incomparable_counters"]),
                {"validation_entries"},
            )

            candidate.write_text(json.dumps(
                diagnostic_report(case, "mmap", schema_version=2)
            ))
            rejected_candidate = subprocess.run(
                command + ["--historical-schema2-baseline"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(rejected_candidate.returncode, 0)

    def test_accepts_completed_short_random_reads(self):
        changed = diagnostic_report("modern/readmissing/65536")
        requested = changed["counters"]["random_read_requested_bytes"]["total"] * 2
        changed["counters"]["random_read_requested_bytes"] = {
            "total": requested,
            "per_get": requested / READ_DIAGNOSTIC_OPERATIONS,
        }
        validate_read_diagnostics(changed, "modern/readmissing/65536")

    def test_runner_preserves_failure_artifacts_and_rejects_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "failed"
            with self.assertRaises(subprocess.CalledProcessError):
                run_read_diagnostics(
                    Path("/usr/bin/false"), "modern/readrandom/4096", output
                )
            self.assertFalse((output / "work").exists())
            manifest = read_json(output / "manifest.json")
            self.assertEqual(manifest["status"], "failed")
            self.assertEqual(manifest["artifacts"]["command_logs"], ["diagnostics.log"])
            with self.assertRaises(FileExistsError):
                run_read_diagnostics(
                    Path("/usr/bin/false"), "modern/readrandom/4096", output
                )


class MutationReportTest(unittest.TestCase):
    CASES = (
        ("overwrite", 65536, 262144, 1, 0, 0),
        ("writebatch", 65536, 8192, 32, 0, 0),
        ("writesync", 4096, 1024, 1, 0, 1),
        ("mixed50", 65536, 262144, 1, 1, 0),
    )

    def reports(self, engine, specification, smoke=False):
        workload, records, iterations, batch, reads, sync = specification
        iterations = 1 if smoke else iterations
        case = f"{engine}/{workload}/{records}"
        data = report(case, 1)
        data["context"].update(
            engine=engine, workload=workload, records=str(records),
            workload_family="mutable", measurement_budget="fixed",
            mutation_smoke=str(smoke).lower(), measured_sync=str(bool(sync)).lower(),
            batch_size=str(batch), background_completion="not_drained",
            steady_state_claimed="false",
        )
        row = data["benchmarks"][0]
        name = f"{case}/iterations:{iterations}/repeats:1/process_time/real_time"
        row.update(name=name, run_name=name, iterations=iterations,
                   items_per_iteration=batch + reads, reads_per_iteration=reads,
                   writes_per_iteration=batch, batch_size=batch,
                   sync_writes_per_iteration=sync,
                   items_per_second=(batch + reads) * 1e9 / row["real_time"])
        done = completion(case)
        for field in ("warmup_operations", "retained_iterators", "scan_creations", "scan_destructions"):
            del done[field]
        done.update(
            schema_version=2, smoke=smoke, verifications=3, reopens=2,
            warmup_writes=records, measured_iterations=iterations, batch_size=batch,
            measured_reads=iterations * reads, measured_writes=iterations * batch,
            write_calls=iterations, sync_write_calls=iterations * sync,
            logical_write_bytes=iterations * batch * 267,
            write_order_crc32c="f117174a", version_values_crc32c="204ed629",
            final_crc32c="92030b01" if smoke else "5ff7de22",
        )
        if records == 65536:
            done.update(
                record_crc32c="3fbabb34", insertion_crc32c="347ed266",
                present_crc32c="2422abad", missing_crc32c="8ee790ec",
                write_order_crc32c="365dce99", version_values_crc32c="c93270ce",
                final_crc32c=("86c2c994" if batch == 32 else "7dc2dbe1") if smoke else "b9ae033b",
            )
        return case, data, done

    def test_all_normal_and_smoke_mutation_contracts(self):
        for engine in ("modern", "leveldb"):
            for specification in self.CASES:
                for smoke in (False, True):
                    case, data, done = self.reports(engine, specification, smoke)
                    with self.subTest(case=case, smoke=smoke):
                        measured = validate_benchmark(data, case, 1, smoke=smoke)
                        validate_completion(done, case, smoke=smoke)
                        self.assertEqual(measured["iterations"], [done["measured_iterations"]])
                        self.assertEqual(measured["wall_ns_per_item"],
                                         [123.0 / data["benchmarks"][0]["items_per_iteration"]])

    def test_rejects_changed_counts_ratios_names_and_context(self):
        case, data, _ = self.reports("modern", self.CASES[3])
        for field, value in (
            ("iterations", 1), ("reads_per_iteration", 0), ("writes_per_iteration", 2),
            ("batch_size", 32), ("sync_writes_per_iteration", True),
            ("name", case + "/process_time/real_time"),
        ):
            changed = copy.deepcopy(data)
            changed["benchmarks"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_benchmark(changed, case, 1)
        for field in ("engine", "workload", "records", "workload_family", "measurement_budget", "mutation_smoke",
                      "measured_sync", "batch_size", "background_completion", "steady_state_claimed"):
            changed = copy.deepcopy(data)
            changed["context"][field] = "wrong"
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_benchmark(changed, case, 1)
            del changed["context"][field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                validate_benchmark(changed, case, 1)
        with self.assertRaises(ValueError):
            validate_benchmark(data, case, 3)

    def test_rejects_mutation_completion_drift_and_mode_mismatch(self):
        case, _, done = self.reports("leveldb", self.CASES[1])
        for field, value in (
            ("schema_version", 1), ("smoke", 0), ("preparations", 2),
            ("callback_invocations", 2), ("cursor_resets", 2), ("verifications", 2),
            ("reopens", 1), ("warmup_writes", 0), ("measured_iterations", 1),
            ("batch_size", 1), ("measured_reads", 1), ("measured_writes", 8192),
            ("write_calls", 262144), ("sync_write_calls", 1), ("logical_write_bytes", 0),
            ("record_crc32c", "00000000"), ("write_order_crc32c", "00000000"),
            ("version_values_crc32c", "00000000"), ("final_crc32c", "00000000"),
        ):
            changed = copy.deepcopy(done)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_completion(changed, case)
        with self.assertRaises(ValueError):
            validate_completion(done, case, smoke=True)
        for changed in ({**done, "extra": 0}, {k: v for k, v in done.items() if k != "final_crc32c"}):
            with self.assertRaises(ValueError):
                validate_completion(changed, case)

    def test_runner_rejects_mutable_calibration_options_without_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "unused"
            for options in ({"repetitions": 3}, {"min_time": 0.2}):
                with self.subTest(options=options), self.assertRaises(ValueError):
                    run_case(Path("/usr/bin/false"), "modern/overwrite/65536", output, **options)
                self.assertFalse(output.exists())


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

    def test_lists_exactly_twenty_four_cases_without_creating_a_database(self):
        result = self.invoke("--list-cases", check=True)
        self.assertEqual(len(result.stdout.splitlines()), 24)
        self.assertEqual(set(result.stdout.splitlines()), set(CASES))
        self.assertEqual(list(self.root.iterdir()), [])
        self.invoke("--help", check=True)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_recording_diagnostic_uses_no_database(self):
        self.invoke("--check-mutation-stream", check=True)
        self.assertEqual(list(self.root.iterdir()), [])
        result = self.invoke("--check-mutation-stream", "--case", "modern/overwrite/65536")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_mutable_fixed_counts_override_cli_and_smoke_is_explicit(self):
        self.invoke(
            "--case", "modern/writesync/4096", "--smoke",
            "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_time=999x", "--benchmark_repetitions=7",
            "--benchmark_min_warmup_time=0",
            f"--benchmark_out={self.root / 'benchmark.json'}",
            "--benchmark_out_format=json", check=True)
        validate_benchmark(read_json(self.root / "benchmark.json"), "modern/writesync/4096", 1, smoke=True)
        validate_completion(read_json(self.root / "completion.json"), "modern/writesync/4096", smoke=True)

    def test_mutable_dry_run_cannot_silently_reduce_normal_work(self):
        result = self.invoke(
            "--case", "modern/overwrite/65536", "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_dry_run=true")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "db").exists())
        self.assertFalse((self.root / "completion.json").exists())

    def test_mutable_warmup_cannot_yield_a_successful_second_callback(self):
        result = self.invoke(
            "--case", "modern/writesync/4096", "--smoke",
            "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_warmup_time=0.001")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "completion.json").exists())

    def test_rejects_smoke_profile_combination_before_opening(self):
        result = self.invoke(
            "--case", "modern/writesync/4096", "--smoke", "--profile-markers",
            "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "db").exists())

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

    def test_forced_reference_pread_is_explicit_and_reproducible(self):
        access = "pread" if REFERENCE_PREAD_CONTROL else "default"
        result = self.invoke(
            "--case", "leveldb/readmissing/4096",
            "--reference-file-access", access,
            "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_time=1x", "--benchmark_repetitions=1",
            f"--benchmark_out={self.root / 'benchmark.json'}",
            "--benchmark_out_format=json",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = read_json(self.root / "benchmark.json")
        validate_benchmark(
            data, "leveldb/readmissing/4096", 1, reference_file_access=access
        )
        self.assertEqual(data["context"]["reference_file_access"], access)
        self.assertEqual(
            data["context"]["reference_pread_control_available"],
            str(REFERENCE_PREAD_CONTROL).lower(),
        )
        if not REFERENCE_PREAD_CONTROL:
            unavailable = self.invoke(
                "--case", "leveldb/readmissing/4096",
                "--reference-file-access", "pread",
                "--database", str(self.root / "unavailable-db"),
                "--completion-report", str(self.root / "unavailable-completion.json"),
            )
            self.assertNotEqual(unavailable.returncode, 0)
            self.assertFalse((self.root / "unavailable-db").exists())
        rejected = self.invoke(
            "--case", "leveldb/scan/4096",
            "--reference-file-access", "pread",
            "--database", str(self.root / "scan-db"),
            "--completion-report", str(self.root / "scan-completion.json"),
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.root / "scan-db").exists())

    def test_modern_mmap_is_explicit_and_read_only(self):
        result = self.invoke(
            "--case", "modern/readmissing/4096",
            "--modern-file-access", "mmap",
            "--database", str(self.root / "db"),
            "--completion-report", str(self.root / "completion.json"),
            "--benchmark_min_time=1x", "--benchmark_repetitions=1",
            f"--benchmark_out={self.root / 'benchmark.json'}",
            "--benchmark_out_format=json",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = read_json(self.root / "benchmark.json")
        validate_benchmark(
            data, "modern/readmissing/4096", 1, modern_file_access="mmap"
        )
        self.assertEqual(data["context"]["modern_file_access"], "mmap")

        rejected = self.invoke(
            "--case", "modern/overwrite/65536",
            "--modern-file-access", "mmap",
            "--database", str(self.root / "write-db"),
            "--completion-report", str(self.root / "write-completion.json"),
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.root / "write-db").exists())

    def test_normal_binary_rejects_read_diagnostics(self):
        rejected = self.invoke(
            "--case", "modern/readrandom/4096",
            "--database", str(self.root / "normal-db"),
            "--diagnostic-report", str(self.root / "normal-report.json"),
        )
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.root / "normal-report.json").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--reference-pread-control", choices=("TRUE", "FALSE"), default="TRUE")
    arguments, remaining = parser.parse_known_args()
    BINARY = arguments.binary.resolve() if arguments.binary else None
    REFERENCE_PREAD_CONTROL = arguments.reference_pread_control == "TRUE"
    unittest.main(argv=[sys.argv[0], *remaining])
