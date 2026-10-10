#!/usr/bin/env python3
"""Run one performance or read-diagnostic workload with explicit provenance."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

import profile_report

READ_CASES = tuple(
    f"{engine}/{workload}/{records}"
    for engine in ("modern", "leveldb")
    for workload in ("readrandom", "readmissing", "scan", "seek_reuse")
    for records in (4096, 65536)
)
MUTATIONS = {
    "overwrite": {"records": 65536, "iterations": 262144, "batch": 1, "reads": 0, "sync": 0},
    "writebatch": {"records": 65536, "iterations": 8192, "batch": 32, "reads": 0, "sync": 0},
    "writesync": {"records": 4096, "iterations": 1024, "batch": 1, "reads": 0, "sync": 1},
    "mixed50": {"records": 65536, "iterations": 262144, "batch": 1, "reads": 1, "sync": 0},
}
MUTATION_CASES = tuple(
    f"{engine}/{workload}/{specification['records']}"
    for engine in ("modern", "leveldb")
    for workload, specification in MUTATIONS.items()
)
CASES = READ_CASES + MUTATION_CASES
READ_DIAGNOSTIC_CASES = tuple(
    f"modern/{workload}/{records}"
    for workload in ("readrandom", "readmissing")
    for records in (4096, 65536)
)
READ_DIAGNOSTIC_OPERATIONS = 4_194_304
READ_DIAGNOSTIC_SAMPLE_SEED = 401
READ_DIAGNOSTIC_SAMPLE_DENOMINATOR = 4_096
READ_DIAGNOSTIC_SAMPLES = 991
READ_DIAGNOSTIC_SAMPLE_SCHEDULE = "splitmix64-v1"
COPIED_FILE_ACCESS = "copied" if sys.platform == "win32" else "pread"
MODERN_FILE_ACCESS_SEMANTICS = (
    "windows-mmap-default-v1" if sys.platform == "win32" else "mmap-default-v1"
)
MODERN_RESULT_OWNERSHIP_SEMANTICS = "reusable-get-v1"
MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS = {
    "copying": "const-copy-v1",
    "exclusive": "exclusive-borrow-v1",
}
MODERN_WAL_CREATION_SEMANTICS = {
    "durable": (
        "file-and-weak-namespace-before-write-v1"
        if sys.platform == "win32"
        else "file-and-directory-before-write-v1"
    ),
    "leveldb": "pinned-leveldb-v1",
}
REFERENCE_HARDWARE_CRC_ROLES = (
    "disabled",
    "google-crc32c-arm64-v1",
    "google-crc32c-external-v1",
)
RESIDUAL_FIELDS = (
    "residual_wal_files",
    "residual_wal_bytes",
    "residual_table_files",
    "residual_table_bytes",
    "residual_manifest_files",
    "residual_manifest_bytes",
    "residual_regular_files",
    "residual_regular_bytes",
)
READ_DIAGNOSTIC_COUNTERS = (
    "gets", "mutable_hits", "immutable_hits", "sstable_hits", "deletions", "misses",
    "level0_candidates", "deeper_candidates", "files_searched", "table_cache_hits",
    "table_cache_misses", "block_cache_hits", "block_cache_misses", "random_read_calls",
    "random_read_requested_bytes", "random_read_returned_bytes", "mapped_view_blocks",
    "mapped_view_bytes", "copied_read_blocks", "copied_read_bytes", "stored_blocks",
    "stored_block_bytes", "decoded_blocks", "decoded_block_bytes", "decompressed_blocks",
    "validation_entries", "restart_entries_decoded", "index_entries_decoded",
    "data_entries_decoded", "internal_key_comparisons", "result_bytes",
)
READ_DIAGNOSTIC_OPEN_REASONS = (
    "mapped", "disabled", "missing_expected_size", "empty_file", "size_mismatch",
    "size_unrepresentable", "count_budget_exhausted", "stat_failed", "mmap_failed",
)
READ_DIAGNOSTIC_STAGES = (
    "get", "candidate_selection", "table_cache_lookup", "block_cache_lookup",
    "random_read", "stored_block_decode", "block_construction", "index_seek",
    "data_seek", "result_copy",
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


def require_native_ascii_path(path, description):
    path = Path(path)
    if sys.platform == "win32":
        if not path.is_absolute():
            raise ValueError(f"{description} must be absolute on Windows")
        try:
            str(path).encode("ascii")
        except UnicodeEncodeError as error:
            raise ValueError(
                f"{description} must be ASCII for the native reference/harness"
            ) from error


def mutation_specification(case, smoke=False):
    _, workload, _ = case_parts(case)
    if workload not in MUTATIONS:
        return None
    specification = dict(MUTATIONS[workload])
    if smoke:
        specification["iterations"] = 1
    return specification


def expected_modern_result_ownership(engine, workload, selected):
    if selected not in ("reusable", "owning"):
        raise ValueError("unknown Modern result ownership")
    if engine != "modern":
        if selected != "reusable":
            raise ValueError("Modern result ownership requires a Modern case")
        return "not_applicable"
    if workload in ("readrandom", "readmissing"):
        return selected
    if selected != "reusable":
        raise ValueError("owning result control requires a Modern point-read case")
    if workload == "mixed50":
        return "reusable"
    return "not_applicable"


def expected_modern_write_batch_ownership(engine, workload, selected):
    if selected not in MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS:
        raise ValueError("unknown Modern write-batch ownership")
    if engine == "modern" and workload == "writebatch":
        return selected
    if selected != "copying":
        raise ValueError("exclusive write-batch ownership requires modern/writebatch/65536")
    return "not_applicable"


def expected_modern_wal_creation(engine, workload, selected):
    if selected not in MODERN_WAL_CREATION_SEMANTICS:
        raise ValueError("unknown Modern WAL-creation policy")
    if engine == "modern" and workload in MUTATIONS:
        return selected
    if selected != "durable":
        raise ValueError("LevelDB-equivalent WAL creation requires a Modern mutable case")
    return "not_applicable"


def validate_benchmark(report, case, repetitions, smoke=False, modern_file_access="default",
                       reference_file_access="default", modern_result_ownership="reusable",
                       modern_write_batch_ownership="copying",
                       modern_wal_creation="durable"):
    engine, workload, records = case_parts(case)
    expected_result_ownership = expected_modern_result_ownership(
        engine, workload, modern_result_ownership
    )
    expected_batch_ownership = expected_modern_write_batch_ownership(
        engine, workload, modern_write_batch_ownership
    )
    expected_wal_creation = expected_modern_wal_creation(
        engine, workload, modern_wal_creation
    )
    if modern_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown Modern file access mode")
    if engine != "modern" and modern_file_access != "default":
        raise ValueError("Modern file access mode requires a Modern case")
    if modern_file_access == COPIED_FILE_ACCESS and workload in MUTATIONS:
        raise ValueError("Modern copied control requires a read-family case")
    if reference_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown reference file access mode")
    if engine != "leveldb" and reference_file_access != "default":
        raise ValueError("reference file access mode requires a LevelDB case")
    if reference_file_access == COPIED_FILE_ACCESS and workload not in ("readrandom", "readmissing"):
        raise ValueError("forced copied control requires a LevelDB point-read case")
    if not isinstance(report, dict) or not isinstance(report.get("context"), dict):
        raise ValueError("missing Google Benchmark context")
    context = report["context"]
    reported_modern_access = context.get("modern_file_access")
    expected_modern_access = modern_file_access if engine == "modern" else "not_applicable"
    if reported_modern_access != expected_modern_access:
        raise ValueError("incorrect Modern file access mode")
    expected_access_semantics = (
        MODERN_FILE_ACCESS_SEMANTICS if engine == "modern" else "not_applicable"
    )
    if context.get("modern_file_access_semantics") != expected_access_semantics:
        raise ValueError("incorrect Modern file access semantics")
    if context.get("modern_result_ownership") != expected_result_ownership:
        raise ValueError("incorrect Modern result ownership")
    expected_result_semantics = (
        MODERN_RESULT_OWNERSHIP_SEMANTICS if engine == "modern" else "not_applicable"
    )
    if context.get("modern_result_ownership_semantics") != expected_result_semantics:
        raise ValueError("incorrect Modern result ownership semantics")
    if context.get("modern_write_batch_ownership") != expected_batch_ownership:
        raise ValueError("incorrect Modern write-batch ownership")
    expected_batch_semantics = (
        MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS[expected_batch_ownership]
        if expected_batch_ownership != "not_applicable"
        else "not_applicable"
    )
    if context.get("modern_write_batch_ownership_semantics") != expected_batch_semantics:
        raise ValueError("incorrect Modern write-batch ownership semantics")
    if context.get("modern_wal_creation") != expected_wal_creation:
        raise ValueError("incorrect Modern WAL-creation policy")
    expected_wal_semantics = (
        MODERN_WAL_CREATION_SEMANTICS[expected_wal_creation]
        if expected_wal_creation != "not_applicable"
        else "not_applicable"
    )
    if context.get("modern_wal_creation_semantics") != expected_wal_semantics:
        raise ValueError("incorrect Modern WAL-creation semantics")
    mutation = mutation_specification(case, smoke)
    if (context.get("library_version") != "v1.9.5"
            or type(context.get("json_schema_version")) is not int
            or context["json_schema_version"] != 1
            or context.get("profile_case") != case
            or context.get("timing") != "wall_and_process_cpu"
            or context.get("build_type") != "Release"
            or context.get("read_diagnostics_compiled") != "false"
            or context.get("reference_file_access")
               != (reference_file_access if engine == "leveldb" else "not_applicable")):
        raise ValueError("incorrect benchmark version, case, timing mode, or build type")
    if (context.get("reference_pread_control_available") not in ("true", "false")
            or not isinstance(context.get("reference_control_patch_sha256"), str)):
        raise ValueError("invalid reference file-access provenance")
    if sys.platform == "win32":
        if (context.get("reference_copied_control_available") not in ("true", "false")
                or context.get("modern_namespace_policy") != "explicit_weak"):
            raise ValueError("invalid native Windows performance policy")
    if reference_file_access == COPIED_FILE_ACCESS:
        patch = context["reference_control_patch_sha256"]
        available = (
            context.get("reference_copied_control_available")
            if sys.platform == "win32"
            else context["reference_pread_control_available"]
        )
        if available != "true" or len(patch) != 64:
            raise ValueError("forced copied control is unavailable or unverified")
    if mutation:
        expected_context = {
            "engine": engine, "workload": workload, "records": str(records),
            "workload_family": "mutable", "measurement_budget": "fixed",
            "mutation_smoke": str(smoke).lower(),
            "measured_sync": str(bool(mutation["sync"])).lower(),
            "batch_size": str(mutation["batch"]), "background_completion": "not_drained",
            "steady_state_claimed": "false",
        }
        if repetitions != 1 or any(context.get(key) != value for key, value in expected_context.items()):
            raise ValueError("incorrect fixed-work mutation context or repetition count")
    rows = report.get("benchmarks")
    if not isinstance(rows, list) or not rows:
        raise ValueError("no benchmark results")
    individuals = []
    expected_name = f"{case}/process_time/real_time"
    if mutation:
        expected_name = f"{case}/iterations:{mutation['iterations']}/repeats:1/process_time/real_time"
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
    if mutation:
        items = mutation["batch"] + mutation["reads"]
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
        if mutation:
            if row["iterations"] != mutation["iterations"]:
                raise ValueError("incorrect fixed mutation iteration count")
            counters = {
                "reads_per_iteration": mutation["reads"],
                "writes_per_iteration": mutation["batch"],
                "batch_size": mutation["batch"],
                "sync_writes_per_iteration": mutation["sync"],
            }
            if any(not number(row.get(key)) or row[key] != value for key, value in counters.items()):
                raise ValueError("incorrect mutation operation counters")
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
        "wall_ns_per_iteration": [row["real_time"] for row in individuals],
        "process_cpu_ns_per_iteration": [row["cpu_time"] for row in individuals],
        "wall_ns_per_item": [row["real_time"] / items for row in individuals],
        "process_cpu_ns_per_item": [row["cpu_time"] / items for row in individuals],
        "statistics_are_not_request_percentiles": True,
    }


def validate_completion(report, case, smoke=False):
    mutation = mutation_specification(case, smoke)
    if mutation:
        return validate_mutation_completion(report, case, mutation, smoke)
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


def validate_mutation_completion(report, case, specification, smoke):
    records = specification["records"]
    iterations = specification["iterations"]
    batch = specification["batch"]
    expected = {
        "schema_version": 3, "case": case, "smoke": smoke, "preparations": 1,
        "verifications": 3, "reopens": 2, "callback_invocations": 1, "cursor_resets": 1,
        "warmup_writes": records, "measured_iterations": iterations, "batch_size": batch,
        "measured_reads": iterations * specification["reads"],
        "measured_writes": iterations * batch, "write_calls": iterations,
        "sync_write_calls": iterations * specification["sync"],
        "logical_write_bytes": iterations * batch * 267,
    }
    expected.update(zip(
        ("record_crc32c", "insertion_crc32c", "present_crc32c", "missing_crc32c"),
        FINGERPRINTS[records],
    ))
    if records == 4096:
        expected.update(write_order_crc32c="f117174a", version_values_crc32c="204ed629",
                        final_crc32c="92030b01" if smoke else "5ff7de22")
    else:
        expected.update(write_order_crc32c="365dce99", version_values_crc32c="c93270ce",
                        final_crc32c=("86c2c994" if batch == 32 else "7dc2dbe1") if smoke else "b9ae033b")
    if not isinstance(report, dict) or set(report) != set(expected) | set(RESIDUAL_FIELDS):
        raise ValueError("invalid mutable completion schema")
    for field, value in expected.items():
        if type(report[field]) is not type(value) or report[field] != value:
            raise ValueError(f"incorrect mutable completion field: {field}")
    for field in RESIDUAL_FIELDS:
        if not integer(report[field], 0):
            raise ValueError(f"invalid residual file field: {field}")
    for prefix in ("wal", "table", "manifest"):
        if report[f"residual_{prefix}_files"] > report["residual_regular_files"]:
            raise ValueError("residual category file count exceeds total")
        if report[f"residual_{prefix}_bytes"] > report["residual_regular_bytes"]:
            raise ValueError("residual category bytes exceed total")
    if (sum(report[f"residual_{prefix}_files"] for prefix in ("wal", "table", "manifest"))
            > report["residual_regular_files"]):
        raise ValueError("residual named file counts exceed total")
    if (sum(report[f"residual_{prefix}_bytes"] for prefix in ("wal", "table", "manifest"))
            > report["residual_regular_bytes"]):
        raise ValueError("residual named file bytes exceed total")
    return report


def validate_read_diagnostics(report, case, modern_file_access="default"):
    if case not in READ_DIAGNOSTIC_CASES:
        raise ValueError("unsupported read diagnostic case")
    if modern_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown Modern file access mode")
    _, workload, records = case_parts(case)
    fields = {
        "schema_version", "case", "operations", "sample_schedule", "sample_seed",
        "sample_denominator", "sampled_gets", "foreground_thread_only",
        "stage_durations_are_inclusive", "setup_warmup_and_verification_excluded",
        "preparations", "verifications", "cursor_resets", "warmup_operations",
        "record_crc32c", "insertion_crc32c", "present_crc32c", "missing_crc32c",
        "setup_file_opens", "counters", "stages", "build",
    }
    if not isinstance(report, dict) or set(report) != fields:
        raise ValueError("invalid read diagnostic schema")
    expected = {
        "schema_version": 5,
        "case": case,
        "operations": READ_DIAGNOSTIC_OPERATIONS,
        "sample_schedule": READ_DIAGNOSTIC_SAMPLE_SCHEDULE,
        "sample_seed": READ_DIAGNOSTIC_SAMPLE_SEED,
        "sample_denominator": READ_DIAGNOSTIC_SAMPLE_DENOMINATOR,
        "foreground_thread_only": True,
        "stage_durations_are_inclusive": True,
        "setup_warmup_and_verification_excluded": True,
        "preparations": 1,
        "verifications": 2,
        "cursor_resets": 1,
        "warmup_operations": records - (workload == "readmissing"),
    }
    for field, value in expected.items():
        if type(report[field]) is not type(value) or report[field] != value:
            raise ValueError(f"incorrect read diagnostic field: {field}")
    if report["sampled_gets"] != READ_DIAGNOSTIC_SAMPLES:
        raise ValueError("invalid sampled Get count")
    for field, value in zip(
        ("record_crc32c", "insertion_crc32c", "present_crc32c", "missing_crc32c"),
        FINGERPRINTS[records],
    ):
        if report[field] != value:
            raise ValueError(f"canonical corpus drift: {field}")

    setup = report["setup_file_opens"]
    if not isinstance(setup, dict) or set(setup) != set(READ_DIAGNOSTIC_OPEN_REASONS):
        raise ValueError("invalid read diagnostic file-open setup")
    setup_files = {}
    for name in READ_DIAGNOSTIC_OPEN_REASONS:
        total = setup[name]
        if (not isinstance(total, dict) or set(total) != {"files", "bytes"}
                or not integer(total["files"], 0) or not integer(total["bytes"], 0)):
            raise ValueError(f"invalid read diagnostic file-open total: {name}")
        setup_files[name] = total["files"]
    if sum(setup_files.values()) == 0:
        raise ValueError("read diagnostic setup opened no random-access files")
    if modern_file_access == "default":
        if setup_files["mapped"] == 0 or setup_files["disabled"] != 0:
            raise ValueError("default diagnostic did not map its table files")
    elif setup_files["mapped"] != 0 or setup_files["disabled"] == 0:
        raise ValueError("copied diagnostic did not retain copied table reads")

    counters = report["counters"]
    if not isinstance(counters, dict) or set(counters) != set(READ_DIAGNOSTIC_COUNTERS):
        raise ValueError("invalid read diagnostic counters")
    totals = {}
    for name in READ_DIAGNOSTIC_COUNTERS:
        counter = counters[name]
        if not isinstance(counter, dict) or set(counter) != {"total", "per_get"}:
            raise ValueError(f"invalid read diagnostic counter: {name}")
        total = counter["total"]
        per_get = counter["per_get"]
        if not integer(total, 0) or not number(per_get):
            raise ValueError(f"invalid read diagnostic counter value: {name}")
        if not math.isclose(
            per_get, total / READ_DIAGNOSTIC_OPERATIONS, rel_tol=1e-12, abs_tol=1e-12
        ):
            raise ValueError(f"incorrect normalized read diagnostic counter: {name}")
        totals[name] = total
    if totals["gets"] != READ_DIAGNOSTIC_OPERATIONS:
        raise ValueError("read diagnostic Get count changed")
    if (totals["mutable_hits"] + totals["immutable_hits"] + totals["sstable_hits"]
            + totals["misses"] != READ_DIAGNOSTIC_OPERATIONS):
        raise ValueError("read diagnostic outcomes do not cover every Get")
    if totals["table_cache_hits"] + totals["table_cache_misses"] != totals["files_searched"]:
        raise ValueError("table cache outcomes do not match searched files")
    if totals["table_cache_misses"] != 0:
        raise ValueError("post-warmup diagnostics unexpectedly reopened a table")
    block_lookups = totals["block_cache_hits"] + totals["block_cache_misses"]
    if block_lookups > totals["files_searched"]:
        raise ValueError("block cache lookups exceed searched files")
    if totals["stored_blocks"] != totals["block_cache_misses"]:
        raise ValueError("stored block reads do not match block cache misses")
    if totals["stored_blocks"] != totals["mapped_view_blocks"] + totals["copied_read_blocks"]:
        raise ValueError("stored blocks do not match mapped and copied reads")
    if totals["stored_block_bytes"] != totals["mapped_view_bytes"] + totals["copied_read_bytes"]:
        raise ValueError("stored bytes do not match mapped and copied reads")
    if totals["decoded_blocks"] != totals["stored_blocks"]:
        raise ValueError("decoded block count does not match stored block count")
    if totals["decompressed_blocks"] != totals["stored_blocks"]:
        raise ValueError("frozen Snappy workload did not decompress every stored block")
    if totals["random_read_calls"] < totals["copied_read_blocks"]:
        raise ValueError("copied blocks have no corresponding random reads")
    if totals["random_read_returned_bytes"] > totals["random_read_requested_bytes"]:
        raise ValueError("random reads returned more bytes than requested")
    if totals["random_read_returned_bytes"] < totals["stored_block_bytes"]:
        if totals["mapped_view_bytes"] == 0:
            raise ValueError("copied stored bytes exceed completed random reads")
    if totals["random_read_returned_bytes"] < totals["copied_read_bytes"]:
        raise ValueError("copied block bytes exceed completed random reads")
    if totals["copied_read_bytes"] > totals["random_read_requested_bytes"]:
        raise ValueError("copied block bytes exceed requested random-read bytes")
    if modern_file_access == "default":
        fallback_files = sum(
            setup_files[name] for name in READ_DIAGNOSTIC_OPEN_REASONS
            if name != "mapped"
        )
        if totals["copied_read_blocks"] != 0 and fallback_files == 0:
            raise ValueError("default copied blocks have no persisted fallback reason")
    elif totals["mapped_view_blocks"] != 0:
        raise ValueError("copied diagnostics unexpectedly used mapped block views")
    if totals["validation_entries"] != 0:
        raise ValueError("lazy block diagnostics performed eager entry validation")
    if totals["files_searched"] < totals["sstable_hits"]:
        raise ValueError("SSTable hits exceed searched files")
    if totals["index_entries_decoded"] < totals["files_searched"]:
        raise ValueError("searched files have no index decode work")
    if totals["data_entries_decoded"] < block_lookups:
        raise ValueError("data-block lookups have no decoded entries")
    decoded_entries = totals["index_entries_decoded"] + totals["data_entries_decoded"]
    if (totals["restart_entries_decoded"] == 0
            or totals["restart_entries_decoded"] > decoded_entries):
        raise ValueError("restart-point decode accounting is inconsistent")
    if totals["internal_key_comparisons"] < decoded_entries:
        raise ValueError("internal-key comparisons undercount decoded entries")
    if workload == "readrandom":
        if totals["sstable_hits"] != READ_DIAGNOSTIC_OPERATIONS or totals["misses"] != 0:
            raise ValueError("present-read diagnostic outcomes changed")
        if totals["result_bytes"] != READ_DIAGNOSTIC_OPERATIONS * 256:
            raise ValueError("present-read diagnostic result bytes changed")
    elif (totals["sstable_hits"] != 0 or totals["misses"] != READ_DIAGNOSTIC_OPERATIONS
          or totals["result_bytes"] != 0):
        raise ValueError("missing-read diagnostic outcomes changed")

    stages = report["stages"]
    if not isinstance(stages, dict) or set(stages) != set(READ_DIAGNOSTIC_STAGES):
        raise ValueError("invalid read diagnostic stages")
    for name in READ_DIAGNOSTIC_STAGES:
        stage = stages[name]
        if not isinstance(stage, dict) or set(stage) != {"events", "total_ns", "mean_ns"}:
            raise ValueError(f"invalid read diagnostic stage: {name}")
        events = stage["events"]
        total = stage["total_ns"]
        mean = stage["mean_ns"]
        if not integer(events, 0) or not integer(total, 0) or not number(mean):
            raise ValueError(f"invalid read diagnostic stage value: {name}")
        if events == 0 and (total != 0 or mean != 0):
            raise ValueError(f"empty read diagnostic stage has timing data: {name}")
        expected_mean = 0 if events == 0 else total / events
        if not math.isclose(mean, expected_mean, rel_tol=1e-12, abs_tol=1e-12):
            raise ValueError(f"incorrect read diagnostic stage mean: {name}")
    if stages["get"]["events"] != report["sampled_gets"]:
        raise ValueError("sampled Get count does not match Get timing events")
    if stages["candidate_selection"]["events"] != report["sampled_gets"]:
        raise ValueError("sampled Get count does not match candidate-selection events")
    if stages["table_cache_lookup"]["events"] != stages["index_seek"]["events"]:
        raise ValueError("sampled table-cache and index-seek events disagree")
    if stages["block_cache_lookup"]["events"] != stages["data_seek"]["events"]:
        raise ValueError("sampled block-cache and data-seek events disagree")
    if stages["table_cache_lookup"]["events"] < stages["block_cache_lookup"]["events"]:
        raise ValueError("sampled block-cache lookups exceed table-cache lookups")
    if workload == "readrandom":
        for name in ("table_cache_lookup", "block_cache_lookup", "index_seek", "data_seek"):
            if stages[name]["events"] < report["sampled_gets"]:
                raise ValueError(f"sampled present Gets have no {name} events")
    expected_copies = report["sampled_gets"] if workload == "readrandom" else 0
    if stages["result_copy"]["events"] != expected_copies:
        raise ValueError("sampled result-copy events changed")
    if records == 65536:
        for counter in ("block_cache_misses", "stored_blocks"):
            if totals[counter] == 0:
                raise ValueError(f"cache-pressure diagnostic has no {counter}")
        for stage in ("stored_block_decode", "block_construction"):
            if stages[stage]["events"] == 0:
                raise ValueError(f"cache-pressure diagnostic has no {stage} samples")
        if modern_file_access == "default":
            if totals["mapped_view_blocks"] == 0 or stages["random_read"]["events"] != 0:
                raise ValueError("default cache-pressure diagnostics retained copied reads")
        elif totals["random_read_calls"] == 0 or stages["random_read"]["events"] == 0:
            raise ValueError("copied cache-pressure diagnostics have no random reads")
    build = report["build"]
    if not isinstance(build, dict):
        raise ValueError("missing read diagnostic build provenance")
    validate_build_context(build)
    if (build.get("build_type") != "Release"
            or build.get("modern_file_access") != modern_file_access
            or build.get("modern_result_ownership") != "reusable"
            or build.get("modern_result_ownership_semantics")
               != MODERN_RESULT_OWNERSHIP_SEMANTICS
            or build.get("reference_file_access") != "not_applicable"
            or build.get("read_diagnostics_compiled") != "true"):
        raise ValueError("report did not come from a read diagnostic build")
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


def read_json_snapshot(path):
    data = Path(path).read_bytes()
    return data, json.loads(data, object_pairs_hook=reject_duplicate_keys)


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def file_digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_state(source):
    source = Path(source)
    commands = {
        "revision": ["rev-parse", "HEAD"],
        "status": ["status", "--porcelain=v1", "-z", "--untracked-files=all"],
        "diff": ["diff", "--binary", "--no-ext-diff", "HEAD", "--"],
        "untracked": ["ls-files", "--others", "--exclude-standard", "-z"],
    }
    result = {}
    for key, args in commands.items():
        value = subprocess.run(
            ["git", "-C", str(source), *args],
            capture_output=True,
            timeout=15,
        )
        if value.returncode != 0:
            return {
                "available": False,
                "reason": value.stderr.decode(errors="replace").strip(),
            }
        result[key] = value.stdout
    worktree = hashlib.sha256()
    worktree.update(b"status\0")
    worktree.update(result["status"])
    worktree.update(b"diff\0")
    worktree.update(result["diff"])
    for raw_path in result["untracked"].split(b"\0"):
        if not raw_path:
            continue
        relative = Path(os.fsdecode(raw_path))
        path = source / relative
        worktree.update(b"untracked\0")
        worktree.update(raw_path)
        worktree.update(b"\0")
        if path.is_symlink():
            worktree.update(b"symlink\0")
            worktree.update(os.fsencode(os.readlink(path)))
        elif path.is_file():
            worktree.update(b"file\0")
            with path.open("rb") as untracked:
                for block in iter(lambda: untracked.read(1024 * 1024), b""):
                    worktree.update(block)
        else:
            worktree.update(b"other\0")
    return {
        "available": True,
        "revision": result["revision"].decode().strip(),
        "dirty": bool(result["status"]),
        "status_sha256": hashlib.sha256(result["status"]).hexdigest(),
        "worktree_sha256": worktree.hexdigest(),
        "untracked_files": sum(bool(path) for path in result["untracked"].split(b"\0")),
    }


def validate_build_context(context):
    required = (
        "source_directory", "build_directory", "configure_revision", "configure_dirty",
        "build_type", "compiler", "c_flags", "cxx_flags", "exe_linker_flags",
        "static_linker_flags", "cmake_generator",
        "target_architecture", "benchmark_requested_revision",
        "benchmark_source_override",
        "reference_requested_revision", "reference_source_override",
        "reference_hardware_crc", "reference_source", "reference_binary_directory",
        "reference_have_crc32c", "reference_crc32c_linked",
        "reference_hardware_patch_sha256",
        "reference_file_access", "reference_pread_control_available",
        "reference_control_patch_sha256", "read_diagnostics_compiled",
        "modern_result_ownership", "modern_result_ownership_semantics",
        "modern_write_batch_ownership", "modern_write_batch_ownership_semantics",
        "modern_wal_creation", "modern_wal_creation_semantics",
        "snappy_target", "snappy_requested_revision", "snappy_source",
        "snappy_source_override", "zstd_target", "zstd_requested_revision",
        "zstd_source", "zstd_source_override", "profile_capture_supported",
        "crc32c_target", "crc32c_provider", "crc32c_source", "crc32c_source_override",
        "crc32c_requested_revision", "crc32c_compiled_arm64", "crc32c_compiled_sse42",
    )
    if any(not isinstance(context.get(key), str) for key in required):
        raise ValueError("benchmark build provenance is incomplete")
    if (context["build_type"] != "Release"
            or context["configure_dirty"] not in ("true", "false", "unknown")
            or context["profile_capture_supported"] not in ("true", "false")
            or context["reference_pread_control_available"] not in ("true", "false")
            or context["read_diagnostics_compiled"] not in ("true", "false")
            or context["reference_file_access"]
               not in ("default", "pread", "copied", "not_applicable")
            or context["reference_hardware_crc"] not in REFERENCE_HARDWARE_CRC_ROLES
            or context["reference_have_crc32c"] not in ("true", "false")
            or context["reference_crc32c_linked"] not in ("true", "false")
            or len(context["reference_control_patch_sha256"]) != 64):
        raise ValueError("invalid build provenance flags")
    hardware_reference = context["reference_hardware_crc"] != "disabled"
    hardware_patch = context["reference_hardware_patch_sha256"]
    if (hardware_reference
            and (context["reference_have_crc32c"] != "true"
                 or context["reference_crc32c_linked"] != "true"
                 or len(hardware_patch) != 64
                 or any(character not in "0123456789abcdef" for character in hardware_patch))):
        raise ValueError("hardware reference provenance is incomplete")
    if (not hardware_reference
            and (context["reference_have_crc32c"] != "false"
                 or context["reference_crc32c_linked"] != "false"
                 or hardware_patch != "not_applicable")):
        raise ValueError("canonical reference reports hardware CRC provenance")
    if sys.platform == "win32":
        if (context.get("reference_copied_control_available") not in ("true", "false")
                or context.get("modern_namespace_policy") != "explicit_weak"):
            raise ValueError("native Windows build provenance is incomplete")
    flags = " ".join(
        context[field]
        for field in ("c_flags", "cxx_flags", "exe_linker_flags", "static_linker_flags")
    )
    if any(flag in flags for flag in ("--coverage", "-fprofile", "-fsanitize")):
        raise ValueError("instrumented build timings are not performance measurements")
    for field in (
        "source_directory",
        "build_directory",
        "reference_source",
        "reference_binary_directory",
    ):
        if not Path(context[field]).is_absolute():
            raise ValueError("build provenance paths must be absolute")
    if not context["cmake_generator"] or not context["target_architecture"]:
        raise ValueError("build generator or target architecture is missing")


def record_diagnostics(manifest, output):
    manifest["artifacts"]["command_logs"] = list(dict.fromkeys(
        command["log"] for command in manifest["commands"]
    ))
    if manifest["mode"] != "cpu_profile":
        return
    if "collector_sha256" in manifest:
        manifest["artifacts"]["target_log"] = "benchmark.log"
        manifest["collector"] = {
            "name": "modern_leveldb_windows_cpu_profile",
            "method": "thread-cpu-delta-stackwalk64-v1",
            "sha256": manifest["collector_sha256"],
        }
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


def validate_native_profile(profile, epochs, measurement, completion, expected_module):
    if not isinstance(expected_module, str) or not expected_module:
        raise ValueError("native profile expected module is missing")
    normalized_module = Path(expected_module).stem.casefold()
    required = {
        "schema_version", "method", "sample_interval_ms", "sample_schedule", "pid", "pdb_matched",
        "total_cpu_100ns", "attributed_cpu_100ns", "unattributed_cpu_100ns",
        "stack_observations", "own_frame_observations", "dropped_epoch_changes",
        "dropped_thread_races", "rejected_foreign_threads", "total_frames",
        "resolved_frames", "unresolved_frames", "repeated_addresses", "truncated_stacks",
        "observed_epochs", "stacks",
    }
    if not isinstance(profile, dict) or set(profile) != required:
        raise ValueError("native stack profile has an invalid schema")
    if (profile["schema_version"] != 2
            or profile["method"] != "thread-cpu-delta-stackwalk64-v1"
            or profile["sample_interval_ms"] != 10 or profile["pdb_matched"] is not True
            or profile["sample_schedule"] != "high-resolution-jitter-5-7-11-13-17-v1"
            or not integer(profile["pid"])
            or any(not integer(profile[key], 0) for key in (
                "total_cpu_100ns", "attributed_cpu_100ns", "unattributed_cpu_100ns",
                "stack_observations", "own_frame_observations", "dropped_epoch_changes",
                "dropped_thread_races", "rejected_foreign_threads", "total_frames",
                "resolved_frames", "unresolved_frames", "repeated_addresses",
                "truncated_stacks",
            ))):
        raise ValueError("native stack profile has invalid metadata")
    if (not isinstance(profile["observed_epochs"], list)
            or any(not integer(value) for value in profile["observed_epochs"])
            or profile["observed_epochs"] != sorted(set(profile["observed_epochs"]))):
        raise ValueError("native stack profile has invalid observed epochs")
    if not isinstance(profile["stacks"], list) or not profile["stacks"]:
        raise ValueError("native stack profile has no stack aggregates")
    stack_cpu = 0
    observations = 0
    attributed_cpu = 0
    own_observations = 0
    sampled_by_epoch = {}
    derived_epochs = set()
    total_frames = 0
    resolved_frames = 0
    unresolved_frames = 0
    repeated_addresses = 0
    for stack in profile["stacks"]:
        if (not isinstance(stack, dict)
                or set(stack) != {
                    "epoch", "cpu_100ns", "observations", "attributed", "unwind_status",
                    "unresolved_frames", "repeated_addresses", "frames"
                }
                or not integer(stack["epoch"]) or not integer(stack["cpu_100ns"])
                or not integer(stack["observations"]) or type(stack["attributed"]) is not bool
                or stack["unwind_status"] not in ("terminated", "zero_pc")
                or not integer(stack["unresolved_frames"], 0)
                or not integer(stack["repeated_addresses"], 0)
                or not isinstance(stack["frames"], list) or not stack["frames"]):
            raise ValueError("native profile contains an invalid stack")
        for frame in stack["frames"]:
            if (not isinstance(frame, dict)
                    or set(frame) != {"module", "symbol", "module_offset", "resolved"}
                    or not isinstance(frame["module"], str) or not frame["module"]
                    or not isinstance(frame["symbol"], str)
                    or not integer(frame["module_offset"], 0)
                    or type(frame["resolved"]) is not bool
                    or (frame["resolved"] and not frame["symbol"])):
                raise ValueError("native profile contains an invalid frame")
        derived_unresolved = sum(not frame["resolved"] for frame in stack["frames"])
        leaf = stack["frames"][0]
        expected_attributed = (
            leaf["resolved"] and Path(leaf["module"]).stem.casefold() == normalized_module
        )
        if (stack["unresolved_frames"] != derived_unresolved
                or stack["attributed"] != expected_attributed):
            raise ValueError("native stack resolution metadata is inconsistent")
        stack_cpu += stack["cpu_100ns"]
        observations += stack["observations"]
        total_frames += len(stack["frames"]) * stack["observations"]
        resolved_frames += (
            sum(frame["resolved"] for frame in stack["frames"]) * stack["observations"]
        )
        unresolved_frames += derived_unresolved * stack["observations"]
        repeated_addresses += stack["repeated_addresses"] * stack["observations"]
        derived_epochs.add(stack["epoch"])
        sampled_by_epoch[stack["epoch"]] = (
            sampled_by_epoch.get(stack["epoch"], 0) + stack["cpu_100ns"]
        )
        if stack["attributed"]:
            attributed_cpu += stack["cpu_100ns"]
            own_observations += stack["observations"]
    if (stack_cpu != profile["total_cpu_100ns"]
            or attributed_cpu != profile["attributed_cpu_100ns"]
            or stack_cpu - attributed_cpu != profile["unattributed_cpu_100ns"]
            or observations != profile["stack_observations"]
            or own_observations != profile["own_frame_observations"]
            or total_frames != profile["total_frames"]
            or resolved_frames != profile["resolved_frames"]
            or unresolved_frames != profile["unresolved_frames"]
            or resolved_frames + unresolved_frames != total_frames
            or repeated_addresses != profile["repeated_addresses"]
            or profile["truncated_stacks"] != 0
            or sorted(derived_epochs) != profile["observed_epochs"]):
        raise ValueError("native stack aggregates do not match profile totals")
    if (stack_cpu <= 0 or attributed_cpu <= 0 or own_observations < 3
            or attributed_cpu / stack_cpu < 0.1):
        raise ValueError("native stack profile has insufficient own-code coverage")
    if (not isinstance(epochs, dict) or set(epochs) != {
            "schema_version", "pid", "qpc_frequency", "epochs"
            } or epochs["schema_version"] != 1 or epochs["pid"] != profile["pid"]
            or not integer(epochs["qpc_frequency"]) or not isinstance(epochs["epochs"], list)
            or not epochs["epochs"]):
        raise ValueError("native epoch ledger has an invalid schema")
    previous = 0
    epoch_ids = set()
    epoch_cpu = 0
    ledger_cpu_by_epoch = {}
    for epoch in epochs["epochs"]:
        if (not isinstance(epoch, dict) or set(epoch) != {
                "id", "expected_iterations", "completed_iterations", "start_qpc",
                "end_qpc", "cpu_100ns"
                } or not integer(epoch["id"]) or epoch["id"] <= previous
                or not integer(epoch["expected_iterations"])
                or epoch["completed_iterations"] != epoch["expected_iterations"]
                or not integer(epoch["start_qpc"], 0)
                or not integer(epoch["end_qpc"], 0) or epoch["end_qpc"] < epoch["start_qpc"]
                or not integer(epoch["cpu_100ns"], 0)):
            raise ValueError("native epoch ledger contains an invalid measured interval")
        previous = epoch["id"]
        epoch_ids.add(epoch["id"])
        epoch_cpu += epoch["cpu_100ns"]
        ledger_cpu_by_epoch[epoch["id"]] = epoch["cpu_100ns"]
    if (not set(profile["observed_epochs"]).issubset(epoch_ids)
            or profile["total_cpu_100ns"] > epoch_cpu
            or any(sampled > ledger_cpu_by_epoch[epoch]
                   for epoch, sampled in sampled_by_epoch.items())):
        raise ValueError("native profile CPU/epochs exceed the child ledger")
    final = epochs["epochs"][-1]
    final_sampled_cpu = sum(
        stack["cpu_100ns"] for stack in profile["stacks"] if stack["epoch"] == final["id"]
    )
    if (len(measurement["iterations"]) != 1
            or final["completed_iterations"] != measurement["iterations"][0]
            or final["id"] not in profile["observed_epochs"]
            or completion["callback_invocations"] != len(epochs["epochs"])
            or final["cpu_100ns"] <= 0
            or final_sampled_cpu / final["cpu_100ns"] < 0.05):
        raise ValueError("native profile did not cover/reconcile the final measured epoch")
    return {
        "method": profile["method"],
        "sample_interval_ms": profile["sample_interval_ms"],
        "sample_schedule": profile["sample_schedule"],
        "final_epoch": final["id"],
        "total_cpu_100ns": profile["total_cpu_100ns"],
        "attributed_cpu_100ns": profile["attributed_cpu_100ns"],
        "attributed_fraction": profile["attributed_cpu_100ns"] / profile["total_cpu_100ns"],
        "final_epoch_cpu_coverage": final_sampled_cpu / final["cpu_100ns"],
        "stack_observations": profile["stack_observations"],
        "dropped_epoch_changes": profile["dropped_epoch_changes"],
        "dropped_thread_races": profile["dropped_thread_races"],
    }


# One fresh output directory binds the executable, workload/policies, validated
# completion, and raw artifacts. Setup and measured work have separate contracts;
# exact report shapes are defined by validators, not this execution map.
def run_case(binary, case, output, capture_cpu=False, smoke=False, repetitions=None,
             min_time=None, timeout=None, modern_file_access="default",
             reference_file_access="default", modern_result_ownership="reusable",
             modern_write_batch_ownership="copying", modern_wal_creation="durable",
             native_collector=None, native_symbols=None):
    engine, workload, _ = case_parts(case)
    expected_result_ownership = expected_modern_result_ownership(
        engine, workload, modern_result_ownership
    )
    expected_batch_ownership = expected_modern_write_batch_ownership(
        engine, workload, modern_write_batch_ownership
    )
    expected_wal_creation = expected_modern_wal_creation(
        engine, workload, modern_wal_creation
    )
    if modern_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown Modern file access mode")
    if engine != "modern" and modern_file_access != "default":
        raise ValueError("Modern file access mode requires a Modern case")
    if modern_file_access == COPIED_FILE_ACCESS and workload in MUTATIONS:
        raise ValueError("Modern copied control requires a read-family case")
    if reference_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown reference file access mode")
    if engine != "leveldb" and reference_file_access != "default":
        raise ValueError("reference file access mode requires a LevelDB case")
    if reference_file_access == COPIED_FILE_ACCESS and workload not in ("readrandom", "readmissing"):
        raise ValueError("forced copied control requires a LevelDB point-read case")
    mutation = mutation_specification(case, smoke)
    if mutation:
        if repetitions is not None and (not integer(repetitions) or repetitions != 1):
            raise ValueError("mutable cases require one repetition; use independent fresh processes")
        if min_time is not None:
            raise ValueError("mutable cases have fixed work; --min-time is not supported")
    binary = Path(binary).resolve(strict=True)
    output = Path(output).absolute()
    require_native_ascii_path(binary, "benchmark binary")
    require_native_ascii_path(output, "performance output")
    if not binary.is_file():
        raise ValueError("benchmark binary is not a file")
    if capture_cpu and sys.platform not in ("darwin", "win32"):
        raise ValueError("CPU collection requires a supported native collector")
    if capture_cpu and sys.platform == "win32":
        if native_collector is None or native_symbols is None:
            raise ValueError("native Windows capture requires collector and benchmark PDB arguments")
        native_collector = Path(native_collector).resolve(strict=True)
        native_symbols = Path(native_symbols).resolve(strict=True)
        require_native_ascii_path(native_collector, "native collector")
        require_native_ascii_path(native_symbols, "benchmark PDB")
        if not all(path.is_file() for path in (native_collector, native_symbols)):
            raise ValueError("native Windows capture artifacts must be files")
    if capture_cpu and smoke:
        raise ValueError("a one-iteration smoke run is not a CPU profile")
    repetitions = repetitions if repetitions is not None else (1 if capture_cpu or smoke or mutation else 3)
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
        "modern_file_access": (
            modern_file_access if engine == "modern" else "not_applicable"
        ),
        "modern_result_ownership": expected_result_ownership,
        "modern_write_batch_ownership": expected_batch_ownership,
        "modern_write_batch_ownership_semantics": (
            MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS[expected_batch_ownership]
            if expected_batch_ownership != "not_applicable"
            else "not_applicable"
        ),
        "modern_wal_creation": expected_wal_creation,
        "modern_wal_creation_semantics": (
            MODERN_WAL_CREATION_SEMANTICS[expected_wal_creation]
            if expected_wal_creation != "not_applicable"
            else "not_applicable"
        ),
        "reference_file_access": (
            reference_file_access if engine == "leveldb" else "not_applicable"
        ),
        "recording_timings_are_not_speedup_evidence": capture_cpu,
    }
    manifest_path = output / "manifest.json"
    write_json(manifest_path, manifest)
    # Mutable workloads use fixed work: adaptive iteration counts change their
    # state and maintenance pressure, so they cannot share read-workload timing policy.
    benchmark_time = f"{mutation['iterations']}x" if mutation else ("1x" if smoke else f"{min_time}s")
    command = [
        "--case", case, "--database", str(work / "db"),
        "--completion-report", str(output / "completion.json"),
        f"--benchmark_min_time={benchmark_time}",
        f"--benchmark_repetitions={repetitions}", "--benchmark_min_warmup_time=0",
        "--benchmark_enable_random_interleaving=false", "--benchmark_dry_run=false",
        "--benchmark_list_tests=false",
        "--benchmark_report_aggregates_only=false", "--benchmark_time_unit=ns",
        f"--benchmark_out={output / 'benchmark.json'}", "--benchmark_out_format=json",
        "--benchmark_color=false",
    ]
    if engine == "modern" and modern_file_access != "default":
        command.extend(["--modern-file-access", modern_file_access])
    if engine == "modern" and modern_result_ownership != "reusable":
        command.extend(["--modern-result-ownership", modern_result_ownership])
    if engine == "modern" and workload == "writebatch":
        command.extend(["--modern-write-batch-ownership", modern_write_batch_ownership])
    if engine == "modern" and mutation:
        command.extend(["--modern-wal-creation", modern_wal_creation])
    if engine == "leveldb":
        command.extend(["--reference-file-access", reference_file_access])
    if mutation and smoke:
        command.append("--smoke")
    started = time.monotonic()
    try:
        if capture_cpu and sys.platform == "darwin":
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
        elif capture_cpu:
            program_dir = output / "profile-program"
            collector_dir = output / "profile-collector"
            program_dir.mkdir()
            collector_dir.mkdir()
            snapshot = program_dir / binary.name
            snapshot_symbols = program_dir / native_symbols.name
            collector = collector_dir / native_collector.name
            for source, destination in (
                    (binary, snapshot), (native_symbols, snapshot_symbols),
                    (native_collector, collector)):
                shutil.copy2(source, destination)
            if file_digest(snapshot) != manifest["executable_sha256"]:
                raise ValueError("benchmark executable changed while preparing capture")
            command.append("--profile-markers")
            manifest["artifacts"].update(
                executable=str(snapshot.relative_to(output)),
                symbols=str(snapshot_symbols.relative_to(output)),
                collector=str(collector.relative_to(output)),
                native_profile="profile.json", native_epochs="epochs.json",
            )
            manifest["symbol_sha256"] = file_digest(snapshot_symbols)
            manifest["collector_sha256"] = file_digest(collector)
            original_artifacts = {
                binary: manifest["executable_sha256"],
                native_symbols: manifest["symbol_sha256"],
                native_collector: manifest["collector_sha256"],
            }
            captured_artifacts = {
                snapshot: manifest["executable_sha256"],
                snapshot_symbols: manifest["symbol_sha256"],
                collector: manifest["collector_sha256"],
            }
            profile_report.run_owned(
                [
                    str(collector), "--sample", "--binary", str(snapshot),
                    "--symbols", str(snapshot_symbols), "--log", str(output / "benchmark.log"),
                    "--report", str(output / "profile.json"),
                    "--epochs", str(output / "epochs.json"),
                    "--timeout-ms", str(max(1, math.ceil(timeout * 1000))), "--",
                    *command,
                ],
                output / "collector.log", timeout + 10, manifest["commands"],
            )
            if any(file_digest(path) != expected
                   for path, expected in captured_artifacts.items()):
                raise ValueError("native capture artifact changed during execution")
            if any(file_digest(path) != expected
                   for path, expected in original_artifacts.items()):
                raise ValueError("native source capture artifact changed during execution")
        else:
            profile_report.run_owned(
                [str(binary), *command], output / "benchmark.log", timeout, manifest["commands"]
            )
        artifact_snapshots = {}
        benchmark_path = output / "benchmark.json"
        benchmark_bytes, raw = read_json_snapshot(benchmark_path)
        artifact_snapshots[benchmark_path] = hashlib.sha256(benchmark_bytes).hexdigest()
        completion_path = output / "completion.json"
        completion_bytes, completion_report = read_json_snapshot(completion_path)
        artifact_snapshots[completion_path] = hashlib.sha256(completion_bytes).hexdigest()
        manifest["artifact_sha256"] = {
            str(path.relative_to(output)): digest
            for path, digest in artifact_snapshots.items()
        }
        manifest["measurement"] = validate_benchmark(
            raw, case, repetitions, smoke=smoke,
            modern_file_access=modern_file_access,
            reference_file_access=reference_file_access,
            modern_result_ownership=modern_result_ownership,
            modern_write_batch_ownership=modern_write_batch_ownership,
            modern_wal_creation=modern_wal_creation,
        )
        manifest["completion"] = validate_completion(completion_report, case, smoke=smoke)
        context = raw["context"]
        validate_build_context(context)
        manifest["reference_hardware_crc"] = context["reference_hardware_crc"]
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
        if capture_cpu and sys.platform == "darwin":
            if context["profile_capture_supported"] != "true":
                raise ValueError("benchmark build does not support macOS profile markers")
            summary = profile_report.summarize_trace(
                output / "toc.xml", output / "markers.xml", output / "samples.xml",
                case, manifest["measurement"]["iterations"][0],
            )
            write_json(output / "profile-summary.json", summary)
        elif capture_cpu:
            if context["profile_capture_supported"] != "true":
                raise ValueError("benchmark build does not support native profile epochs")
            profile_path = output / "profile.json"
            profile_bytes, profile = read_json_snapshot(profile_path)
            epoch_path = output / "epochs.json"
            epoch_bytes, epochs = read_json_snapshot(epoch_path)
            artifact_snapshots[profile_path] = hashlib.sha256(profile_bytes).hexdigest()
            artifact_snapshots[epoch_path] = hashlib.sha256(epoch_bytes).hexdigest()
            manifest["artifact_sha256"].update({
                str(profile_path.relative_to(output)): artifact_snapshots[profile_path],
                str(epoch_path.relative_to(output)): artifact_snapshots[epoch_path],
            })
            manifest["profile_summary"] = validate_native_profile(
                profile, epochs,
                manifest["measurement"], manifest["completion"], snapshot.stem,
            )
        # Bind the bytes actually validated, rejecting artifact replacement between
        # reading a report and publishing a successful experiment manifest.
        if any(file_digest(path) != digest for path, digest in artifact_snapshots.items()):
            raise ValueError("validated performance artifact changed before publication")
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


def run_read_diagnostics(binary, case, output, timeout=300.0, modern_file_access="default"):
    if case not in READ_DIAGNOSTIC_CASES:
        raise ValueError("unsupported read diagnostic case")
    if modern_file_access not in ("default", COPIED_FILE_ACCESS):
        raise ValueError("unknown Modern file access mode")
    if not number(timeout) or timeout <= 0:
        raise ValueError("timeout must be positive and finite")
    binary = Path(binary).resolve(strict=True)
    output = Path(output).absolute()
    require_native_ascii_path(binary, "diagnostic binary")
    require_native_ascii_path(output, "diagnostic output")
    if not binary.is_file():
        raise ValueError("diagnostic binary is not a file")
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    work = output / "work"
    work.mkdir()
    manifest = {
        "schema_version": 1,
        "case": case,
        "mode": "read_diagnostics",
        "status": "running",
        "executable_sha256": file_digest(binary),
        "original_executable": str(binary),
        "host_platform": platform.platform(),
        "python_version": platform.python_version(),
        "commands": [],
        "artifacts": {"read_diagnostics": "read-diagnostics.json"},
        "modern_file_access": modern_file_access,
        "modern_result_ownership": "reusable",
        "recording_timings_are_not_speedup_evidence": True,
    }
    manifest_path = output / "manifest.json"
    write_json(manifest_path, manifest)
    command = [
        str(binary), "--case", case, "--database", str(work / "db"),
        "--diagnostic-report", str(output / "read-diagnostics.json"),
    ]
    if modern_file_access != "default":
        command.extend(["--modern-file-access", modern_file_access])
    started = time.monotonic()
    try:
        profile_report.run_owned(
            command, output / "diagnostics.log", timeout, manifest["commands"]
        )
        report = validate_read_diagnostics(
            read_json(output / "read-diagnostics.json"), case,
            modern_file_access=modern_file_access,
        )
        manifest["diagnostics"] = report
        manifest["build"] = report["build"]
        source = Path(report["build"]["source_directory"])
        manifest["runtime_source"] = source_state(source)
        manifest["configure_revision_matches_runtime"] = (
            manifest["runtime_source"].get("revision")
            == report["build"].get("configure_revision")
        )
        compile_commands = Path(report["build"]["build_directory"]) / "compile_commands.json"
        if compile_commands.is_file():
            shutil.copy2(compile_commands, output / "compile_commands.json")
            manifest["artifacts"]["compile_commands"] = "compile_commands.json"
            manifest["compile_commands_sha256"] = file_digest(output / "compile_commands.json")
        else:
            manifest["compile_commands_unavailable"] = True
        manifest["status"] = "complete"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
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
    parser.add_argument("--native-collector", type=Path)
    parser.add_argument("--native-symbols", type=Path)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--min-time", type=float)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--modern-file-access", choices=("default", COPIED_FILE_ACCESS),
                        default="default")
    parser.add_argument("--modern-result-ownership", choices=("reusable", "owning"),
                        default="reusable")
    parser.add_argument("--modern-write-batch-ownership", choices=("copying", "exclusive"),
                        default="copying")
    parser.add_argument("--modern-wal-creation", choices=("durable", "leveldb"),
                        default="durable")
    parser.add_argument("--reference-file-access", choices=("default", COPIED_FILE_ACCESS),
                        default="default")
    parser.add_argument("--read-diagnostics", action="store_true")
    args = parser.parse_args()
    try:
        if args.read_diagnostics:
            if (args.capture_cpu or args.smoke or args.repetitions is not None
                    or args.min_time is not None or args.reference_file_access != "default"
                    or args.modern_result_ownership != "reusable"
                    or args.modern_write_batch_ownership != "copying"
                    or args.modern_wal_creation != "durable"):
                raise ValueError("read diagnostics cannot combine with benchmark options")
            result = run_read_diagnostics(
                args.binary, args.case, args.output,
                args.timeout if args.timeout is not None else 300.0,
                args.modern_file_access,
            )
        else:
            result = run_case(
                args.binary, args.case, args.output, args.capture_cpu, args.smoke,
                args.repetitions, args.min_time, args.timeout, args.modern_file_access,
                args.reference_file_access, args.modern_result_ownership,
                args.modern_write_batch_ownership, args.modern_wal_creation,
                args.native_collector, args.native_symbols,
            )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(f"performance run failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("performance run cancelled; see preserved artifacts", file=sys.stderr)
        return 130
    summary = {"case": result["case"], "mode": result["mode"], "artifacts": str(args.output)}
    if result["mode"] == "read_diagnostics":
        summary["operations"] = result["diagnostics"]["operations"]
        summary["sampled_gets"] = result["diagnostics"]["sampled_gets"]
    else:
        summary["measurement"] = result["measurement"]
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
