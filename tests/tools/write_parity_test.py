import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_performance import (
    MODERN_WAL_CREATION_SEMANTICS,
    MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS,
    file_digest,
    mutation_specification,
    read_json,
    write_json,
)
from run_write_parity import (
    BUILD_IDENTITY_FIELDS,
    EVIDENCE_GATES,
    MATRIX_NAME,
    aggregate_cells,
    enumerate_cells,
    run_matrix,
    validate_cell_manifest,
    validate_plan,
    verify_compile_alignment,
    verify_profile_evidence,
    verify_reference_files,
    verify_role_files,
)


def make_plan(root):
    roles = {}
    empty_status = hashlib.sha256(b"").hexdigest()
    for name in ("final", "baseline", "canonical"):
        source = (root / f"{name}-source").resolve()
        build_directory = (root / f"{name}-build").resolve()
        reference_source = build_directory / "_deps/modern_leveldb_reference-src"
        reference_binary = build_directory / "_deps/modern_leveldb_reference-build"
        source.mkdir()
        (build_directory / "benchmarks").mkdir(parents=True)
        (source / "src/engine").mkdir(parents=True)
        (source / "src/engine/write_path.cc").write_text("// test\n", encoding="utf-8")
        (reference_source / "util").mkdir(parents=True)
        (reference_source / "util/crc32c.cc").write_text("// test\n", encoding="utf-8")
        (reference_binary / "include/port").mkdir(parents=True)
        executable = build_directory / "benchmarks" / "modern_leveldb_performance"
        executable.write_bytes(f"{name}-executable".encode())
        have_crc32c = name == "final"
        hardware_patch = "b" * 64 if have_crc32c else "not_applicable"
        port_config = reference_binary / "include/port/port_config.h"
        port_config.write_text(
            f"#define HAVE_CRC32C {1 if have_crc32c else 0}\n",
            encoding="utf-8",
        )
        archive = reference_binary / "libleveldb.a"
        archive.write_bytes(f"{name}-leveldb".encode())
        compile_commands = build_directory / "compile_commands.json"
        modern_command = (
            f"/usr/bin/c++ -DMODERN_LEVELDB_READ_DIAGNOSTICS=0 "
            f"-I{source}/include -isystem {build_directory}/_deps/snappy "
            f"-O3 -arch arm64 -o {build_directory}/write_path.o "
            f"-c {source}/src/engine/write_path.cc"
        )
        reference_command = (
            f"/usr/bin/c++ -I{reference_binary}/include -I{reference_source} "
            + (
                f"-isystem {build_directory}/_deps/crc32c/include "
                if have_crc32c else ""
            )
            + f"-O3 -arch arm64 -o {build_directory}/reference_crc32c.o "
              f"-c {reference_source}/util/crc32c.cc"
        )
        write_json(
            compile_commands,
            [
                {
                    "directory": str(build_directory),
                    "file": str(source / "src/engine/write_path.cc"),
                    "command": modern_command,
                },
                {
                    "directory": str(build_directory),
                    "file": str(reference_source / "util/crc32c.cc"),
                    "command": reference_command,
                },
            ],
        )
        revision_role = "final" if name == "canonical" else name
        revision = hashlib.sha1(revision_role.encode()).hexdigest()
        hardware_crc = "google-crc32c-arm64-v1" if name == "final" else "disabled"
        dirty = name != "canonical"
        build = {
            "source_directory": str(source),
            "build_directory": str(build_directory),
            "configure_revision": revision,
            "configure_dirty": str(dirty).lower(),
            "build_type": "Release",
            "compiler": "Test C++ 1.0",
            "c_flags": "-O3 -DNDEBUG",
            "cxx_flags": "-O3 -DNDEBUG",
            "exe_linker_flags": "",
            "static_linker_flags": "",
            "cmake_generator": "Ninja",
            "target_architecture": "arm64",
            "benchmark_requested_revision": "192ef10025eb2c4cdd392bc502f0c852196baa48",
            "benchmark_source_override": "",
            "reference_requested_revision": "7ee830d02b623e8ffe0b95d59a74db1e58da04c5",
            "reference_source_override": "",
            "reference_hardware_crc": hardware_crc,
            "reference_source": str(reference_source),
            "reference_binary_directory": str(reference_binary),
            "reference_have_crc32c": str(have_crc32c).lower(),
            "reference_crc32c_linked": str(have_crc32c).lower(),
            "reference_hardware_patch_sha256": hardware_patch,
            "reference_pread_control_available": "true",
            "reference_control_patch_sha256": "a" * 64,
            "read_diagnostics_compiled": "false",
            "snappy_target": "snappy",
            "snappy_requested_revision": "9c28114a38866f6deeaa826db918293bc28ae410",
            "snappy_source": str(build_directory / "_deps/snappy"),
            "snappy_source_override": "",
            "zstd_target": "libzstd_static",
            "zstd_requested_revision": "f8745da6ff1ad1e7bab384bd1f9d742439278e99",
            "zstd_source": str(build_directory / "_deps/zstd"),
            "zstd_source_override": "",
            "crc32c_target": "crc32c",
            "crc32c_provider": "pinned-source",
            "crc32c_source": str(build_directory / "_deps/crc32c"),
            "crc32c_source_override": "",
            "crc32c_requested_revision": "2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07",
            "crc32c_compiled_arm64": "true",
            "crc32c_compiled_sse42": "false",
            "profile_capture_supported": "true",
        }
        assert set(build) == set(BUILD_IDENTITY_FIELDS)
        roles[name] = {
            "executable": str(executable),
            "executable_sha256": file_digest(executable),
            "compile_commands_sha256": file_digest(compile_commands),
            "build": build,
            "runtime_source": {
                "available": True,
                "revision": revision,
                "dirty": dirty,
                "status_sha256": "c" * 64 if dirty else empty_status,
                "worktree_sha256": (
                    "d" * 64
                    if dirty
                    else hashlib.sha256(b"status\0diff\0").hexdigest()
                ),
                "untracked_files": 0,
            },
            "reference": {
                "port_config": str(port_config),
                "port_config_sha256": file_digest(port_config),
                "have_crc32c": have_crc32c,
                "archive": str(archive),
                "archive_sha256": file_digest(archive),
                "hardware_patch_sha256": hardware_patch,
            },
        }
    profile_directory = (root / "hardware-profile").resolve()
    profile_directory.mkdir()
    profile_summary = profile_directory / "profile-summary.json"
    write_json(
        profile_summary,
        {
            "schema_version": 1,
            "case": "leveldb/readrandom/65536",
            "sample_count": 500,
            "low_confidence": False,
            "inclusive": [
                {
                    "module": "modern_leveldb_performance",
                    "symbol": (
                        "crc32c::ExtendArm64("
                        "unsigned int, unsigned char const*, unsigned long)"
                    ),
                    "sample_weight_ns": 100,
                }
            ],
        },
    )
    profile_manifest = profile_directory / "manifest.json"
    final = roles["final"]
    write_json(
        profile_manifest,
        {
            "status": "complete",
            "mode": "cpu_profile",
            "case": "leveldb/readrandom/65536",
            "executable_sha256": final["executable_sha256"],
            "compile_commands_sha256": final["compile_commands_sha256"],
            "runtime_source": final["runtime_source"],
            "reference_hardware_crc": final["build"]["reference_hardware_crc"],
            "recording_timings_are_not_speedup_evidence": True,
            "build": final["build"],
            "artifacts": {"profile_summary": profile_summary.name},
        },
    )
    evidence = {
        "final_revision": roles["final"]["build"]["configure_revision"],
        "hardware_crc_profile": {
            "manifest": str(profile_manifest),
            "manifest_sha256": file_digest(profile_manifest),
            "summary": str(profile_summary),
            "summary_sha256": file_digest(profile_summary),
        },
    }
    evidence.update({gate: True for gate in EVIDENCE_GATES})
    return {
        "schema_version": 1,
        "matrix": MATRIX_NAME,
        "roles": roles,
        "evidence": evidence,
    }


def completion_for(cell):
    specification = mutation_specification(cell["case"])
    fingerprint = {
        "overwrite": "11111111",
        "writebatch": "22222222",
        "writesync": "33333333",
        "mixed50": "44444444",
    }[cell["workload"]]
    return {
        "schema_version": 3,
        "case": cell["case"],
        "smoke": False,
        "preparations": 1,
        "verifications": 3,
        "reopens": 2,
        "callback_invocations": 1,
        "cursor_resets": 1,
        "warmup_writes": specification["records"],
        "measured_iterations": specification["iterations"],
        "batch_size": specification["batch"],
        "measured_reads": specification["iterations"] * specification["reads"],
        "measured_writes": specification["iterations"] * specification["batch"],
        "write_calls": specification["iterations"],
        "sync_write_calls": specification["iterations"] * specification["sync"],
        "logical_write_bytes": specification["iterations"] * specification["batch"] * 267,
        "record_crc32c": fingerprint,
        "insertion_crc32c": "55555555",
        "present_crc32c": "66666666",
        "missing_crc32c": "77777777",
        "write_order_crc32c": "88888888",
        "version_values_crc32c": "99999999",
        "final_crc32c": fingerprint,
        "residual_wal_files": 1,
        "residual_wal_bytes": 100,
        "residual_table_files": 1,
        "residual_table_bytes": 200,
        "residual_manifest_files": 1,
        "residual_manifest_bytes": 50,
        "residual_regular_files": 5,
        "residual_regular_bytes": 500,
    }


def expected_roles(cell):
    batch = (
        cell["modern_write_batch_ownership"]
        if cell["engine"] == "modern" and cell["workload"] == "writebatch"
        else "not_applicable"
    )
    wal = cell["modern_wal_creation"] if cell["engine"] == "modern" else "not_applicable"
    return batch, wal


def manifest_for(plan, cell, wall=100.0, cpu=100.0):
    role = plan["roles"][cell["binary_role"]]
    batch, wal = expected_roles(cell)
    build = copy.deepcopy(role["build"])
    build.update(
        profile_case=cell["case"],
        engine=cell["engine"],
        workload=cell["workload"],
        modern_write_batch_ownership=batch,
        modern_wal_creation=wal,
    )
    specification = mutation_specification(cell["case"])
    return {
        "schema_version": 1,
        "status": "complete",
        "case": cell["case"],
        "mode": "benchmark",
        "executable_sha256": role["executable_sha256"],
        "original_executable": role["executable"],
        "compile_commands_sha256": role["compile_commands_sha256"],
        "runtime_source": copy.deepcopy(role["runtime_source"]),
        "configure_revision_matches_runtime": True,
        "reference_hardware_crc": role["build"]["reference_hardware_crc"],
        "modern_write_batch_ownership": batch,
        "modern_write_batch_ownership_semantics": (
            MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS[batch]
            if batch != "not_applicable" else "not_applicable"
        ),
        "modern_wal_creation": wal,
        "modern_wal_creation_semantics": (
            MODERN_WAL_CREATION_SEMANTICS[wal]
            if wal != "not_applicable" else "not_applicable"
        ),
        "process_cleanup_verified": True,
        "commands": [{}],
        "build": build,
        "measurement": {
            "iterations": [specification["iterations"]],
            "items_per_iteration": specification["batch"] + specification["reads"],
            "wall_ns_per_iteration": [wall],
            "process_cpu_ns_per_iteration": [cpu],
            "wall_ns_per_item": [999999.0],
            "process_cpu_ns_per_item": [999999.0],
        },
        "completion": completion_for(cell),
    }


def completed_cells(plan):
    result = []
    for cell in enumerate_cells():
        if cell["side"] == "reference":
            wall = cpu = 100.0
        elif cell["matrix"] == "primary":
            wall = 104.0
            cpu = 104.0
            if cell["workload"] == "writesync" and cell["round"] == 1:
                wall = 125.0
        elif cell["matrix"] == "production":
            wall = 109.0 if cell["workload"] == "writesync" else 104.0
            cpu = 104.0
        elif cell["matrix"] == "batch_ownership":
            wall = cpu = 110.0
        else:
            wall = cpu = 95.0
        result.append(validate_cell_manifest(cell, manifest_for(plan, cell, wall, cpu), plan))
    return result


class MatrixEnumerationTest(unittest.TestCase):
    def test_enumerates_exact_adjacent_alternating_110_process_matrix(self):
        cells = enumerate_cells()
        self.assertEqual(len(cells), 110)
        self.assertEqual(
            {matrix: sum(cell["matrix"] == matrix for cell in cells) for matrix in (
                "primary", "production", "batch_ownership", "crc_continuity"
            )},
            {"primary": 40, "production": 40, "batch_ownership": 6, "crc_continuity": 24},
        )
        for index in range(0, len(cells), 2):
            first, second = cells[index:index + 2]
            self.assertEqual(
                (first["matrix"], first["round"], first["workload"]),
                (second["matrix"], second["round"], second["workload"]),
            )
            expected = ("reference", "candidate") if first["round"] % 2 else (
                "candidate", "reference"
            )
            self.assertEqual((first["side"], second["side"]), expected)
        self.assertEqual(cells[0]["label"], "hardware_leveldb")
        self.assertEqual(cells[1]["label"], "matched_modern")
        self.assertEqual(cells[8]["label"], "matched_modern")
        self.assertEqual(cells[9]["label"], "hardware_leveldb")
        self.assertEqual(cells[39]["matrix"], "primary")
        self.assertEqual(cells[40]["matrix"], "production")
        self.assertEqual(cells[80]["matrix"], "batch_ownership")
        self.assertEqual(cells[86]["matrix"], "crc_continuity")


class FrozenPlanTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="write-parity-plan-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.plan = make_plan(self.root)

    def test_accepts_exact_frozen_plan_and_detects_changed_files(self):
        validate_plan(self.plan)
        role = self.plan["roles"]["final"]
        with mock.patch(
            "run_write_parity.source_state", return_value=role["runtime_source"]
        ), mock.patch("run_write_parity.verify_reference_files"):
            verify_role_files("final", role)
        Path(role["executable"]).write_bytes(b"changed")
        with mock.patch(
            "run_write_parity.source_state", return_value=role["runtime_source"]
        ), mock.patch("run_write_parity.verify_reference_files"):
            with self.assertRaises(ValueError):
                verify_role_files("final", role)

    def test_rejects_schema_identity_crc_and_evidence_drift(self):
        changes = []
        missing_role = copy.deepcopy(self.plan)
        del missing_role["roles"]["baseline"]
        changes.append(missing_role)
        mixed_source = copy.deepcopy(self.plan)
        mixed_source["roles"]["baseline"]["build"]["source_directory"] = (
            mixed_source["roles"]["final"]["build"]["source_directory"]
        )
        changes.append(mixed_source)
        final_crc = copy.deepcopy(self.plan)
        final_crc["roles"]["final"]["build"]["reference_hardware_crc"] = "disabled"
        changes.append(final_crc)
        canonical_crc = copy.deepcopy(self.plan)
        canonical_crc["roles"]["canonical"]["build"][
            "reference_hardware_crc"
        ] = "google-crc32c-arm64-v1"
        changes.append(canonical_crc)
        missing_build = copy.deepcopy(self.plan)
        del missing_build["roles"]["final"]["build"]["compiler"]
        changes.append(missing_build)
        changed_revision = copy.deepcopy(self.plan)
        changed_revision["roles"]["final"]["runtime_source"]["revision"] = "other"
        changes.append(changed_revision)
        failed_evidence = copy.deepcopy(self.plan)
        failed_evidence["evidence"]["review"] = False
        changes.append(failed_evidence)
        mismatched_baseline = copy.deepcopy(self.plan)
        mismatched_baseline["roles"]["baseline"]["build"]["cxx_flags"] = "-O0"
        changes.append(mismatched_baseline)
        overridden_dependency = copy.deepcopy(self.plan)
        overridden_dependency["roles"]["canonical"]["build"][
            "reference_source_override"
        ] = "/other/reference"
        changes.append(overridden_dependency)
        for index, changed in enumerate(changes):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate_plan(changed)

    def test_verifies_reference_compile_archive_and_runtime_profile_evidence(self):
        final = self.plan["roles"]["final"]
        canonical = self.plan["roles"]["canonical"]
        for output in (
            "         U crc32c::Extend(unsigned int, char const*, unsigned long)\n",
            "crc32c::Extend(unsigned int, unsigned char const*, unsigned long)\n",
        ):
            hardware_symbols = subprocess.CompletedProcess(["nm"], 0, output, "")
            with self.subTest(output=output), mock.patch(
                "run_write_parity.subprocess.run", return_value=hardware_symbols
            ):
                verify_reference_files("final", final)
        for output in (
            "         U leveldb::crc32c::Extend(unsigned int, char const*, unsigned long)\n",
            "leveldb::crc32c::Extend(unsigned int, char const*, unsigned long)\n",
        ):
            canonical_symbols = subprocess.CompletedProcess(["nm"], 0, output, "")
            with self.subTest(output=output), mock.patch(
                "run_write_parity.subprocess.run", return_value=canonical_symbols
            ):
                verify_reference_files("canonical", canonical)
        verify_compile_alignment(self.plan)
        verify_profile_evidence(self.plan)

        changed_compile = Path(
            self.plan["roles"]["baseline"]["build"]["build_directory"]
        ) / "compile_commands.json"
        commands = read_json(changed_compile)
        commands[0]["command"] = commands[0]["command"].replace("-O3", "-O0")
        write_json(changed_compile, commands)
        with self.assertRaises(ValueError):
            verify_compile_alignment(self.plan)

        summary = Path(self.plan["evidence"]["hardware_crc_profile"]["summary"])
        data = read_json(summary)
        data["inclusive"] = []
        write_json(summary, data)
        self.plan["evidence"]["hardware_crc_profile"]["summary_sha256"] = file_digest(summary)
        with self.assertRaises(ValueError):
            verify_profile_evidence(self.plan)


class CellAndAggregationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="write-parity-cells-")
        self.addCleanup(self.temporary.cleanup)
        self.plan = make_plan(Path(self.temporary.name).resolve())
        self.cells = completed_cells(self.plan)

    def test_validates_cell_identity_fixed_counts_roles_and_residuals(self):
        cell = enumerate_cells()[1]
        manifest = manifest_for(self.plan, cell)
        retained = validate_cell_manifest(cell, manifest, self.plan)
        self.assertEqual(retained["id"], cell["id"])
        for mutation in (
            lambda value: value.update(executable_sha256="0" * 64),
            lambda value: value["build"].update(compiler="other"),
            lambda value: value.update(modern_wal_creation="durable"),
            lambda value: value["measurement"].update(iterations=[1]),
            lambda value: value["completion"].update(residual_regular_files=0),
        ):
            changed = copy.deepcopy(manifest)
            mutation(changed)
            with self.assertRaises(ValueError):
                validate_cell_manifest(cell, changed, self.plan)

    def test_aggregates_raw_iteration_values_and_applies_exact_admission(self):
        report = aggregate_cells(self.cells, self.plan["evidence"])
        self.assertEqual(report["processes"], 110)
        self.assertTrue(report["admission"]["passed"])
        primary_batch = report["primary"]["writebatch"]
        self.assertAlmostEqual(primary_batch["aggregate_wall_delta"], 0.04)
        self.assertEqual(primary_batch["candidate_median_wall_ns_per_iteration"], 104.0)
        self.assertEqual(primary_batch["candidate_median_wall_ns_per_written_key"], 3.25)
        self.assertNotIn("writebatch", report["wal_durability"])
        self.assertEqual(
            set(report["wal_durability"]), {"overwrite", "writesync", "mixed50"}
        )
        sync_rounds = report["primary"]["writesync"]["rounds"]
        self.assertEqual(sum(item["wall_delta"] <= 0.20 for item in sync_rounds), 4)

    def test_rejects_missing_duplicate_reordered_and_changed_completion_cells(self):
        variants = [
            self.cells[:-1],
            [self.cells[0], *self.cells[:-1]],
            [self.cells[1], self.cells[0], *self.cells[2:]],
        ]
        changed_fingerprint = copy.deepcopy(self.cells)
        changed_fingerprint[1]["completion"]["final_crc32c"] = "00000000"
        variants.append(changed_fingerprint)
        missing_residual = copy.deepcopy(self.cells)
        del missing_residual[0]["completion"]["residual_wal_files"]
        variants.append(missing_residual)
        for index, changed in enumerate(variants):
            with self.subTest(index=index), self.assertRaises(ValueError):
                aggregate_cells(changed, self.plan["evidence"])

    def test_reports_threshold_failures_without_rerunning_cells(self):
        changed = copy.deepcopy(self.cells)
        for cell in changed:
            if (cell["matrix"] == "primary" and cell["workload"] == "overwrite"
                    and cell["side"] == "candidate"):
                cell["measurement"]["wall_ns_per_iteration"] = [106.0]
                cell["measurement"]["process_cpu_ns_per_iteration"] = [106.0]
        report = aggregate_cells(changed, self.plan["evidence"])
        self.assertFalse(report["admission"]["passed"])
        failed = {
            check["name"] for check in report["admission"]["checks"] if not check["passed"]
        }
        self.assertIn("primary.overwrite.aggregate_wall", failed)
        self.assertIn("primary.overwrite.aggregate_process_cpu", failed)

    def test_admission_accepts_exact_decimal_threshold_boundaries(self):
        changed = copy.deepcopy(self.cells)
        for cell in changed:
            if (cell["matrix"] == "primary" and cell["workload"] == "overwrite"
                    and cell["side"] == "candidate"):
                cell["measurement"]["wall_ns_per_iteration"] = [105.0]
                cell["measurement"]["process_cpu_ns_per_iteration"] = [105.0]
        report = aggregate_cells(changed, self.plan["evidence"])
        checks = {
            check["name"]: check["passed"] for check in report["admission"]["checks"]
        }
        self.assertTrue(checks["primary.overwrite.aggregate_wall"])
        self.assertTrue(checks["primary.overwrite.aggregate_process_cpu"])


class MatrixExecutionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="write-parity-run-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.plan = make_plan(self.root)
        self.plan_path = self.root / "plan.json"
        write_json(self.plan_path, self.plan)

    def test_runs_only_the_fixed_matrix_and_rejects_resume(self):
        expected = enumerate_cells()
        calls = []

        def fake_run_case(binary, case, output, **options):
            cell = expected[len(calls)]
            self.assertEqual(
                Path(binary),
                Path(self.plan["roles"][cell["binary_role"]]["executable"]),
            )
            self.assertEqual(case, cell["case"])
            self.assertEqual(options["repetitions"], 1)
            self.assertNotIn("min_time", options)
            self.assertEqual(
                options["modern_write_batch_ownership"],
                cell["modern_write_batch_ownership"],
            )
            self.assertEqual(options["modern_wal_creation"], cell["modern_wal_creation"])
            Path(output).mkdir()
            write_json(Path(output) / "manifest.json", manifest_for(self.plan, cell))
            calls.append(cell["id"])

        output = self.root / "matrix"
        with mock.patch("run_write_parity.verify_all_role_files"), mock.patch(
            "run_write_parity.retain_source_patches", return_value={}
        ), mock.patch(
            "run_write_parity.run_case", side_effect=fake_run_case
        ):
            result = run_matrix(self.plan_path, output)
        self.assertEqual(result["status"], "complete")
        self.assertEqual(calls, [cell["id"] for cell in expected])
        self.assertEqual(read_json(output / "manifest.json")["status"], "complete")
        self.assertTrue(read_json(output / "report.json")["admission"]["passed"])
        with mock.patch("run_write_parity.verify_all_role_files"), mock.patch(
            "run_write_parity.retain_source_patches", return_value={}
        ):
            with self.assertRaises(FileExistsError):
                run_matrix(self.plan_path, output)

    def test_retains_failed_matrix_without_cell_replacement(self):
        calls = 0

        def fail_on_third(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise subprocess.CalledProcessError(1, ["benchmark"])
            cell = enumerate_cells()[calls - 1]
            output = Path(args[2])
            output.mkdir()
            write_json(output / "manifest.json", manifest_for(self.plan, cell))

        output = self.root / "failed-matrix"
        with mock.patch("run_write_parity.verify_all_role_files"), mock.patch(
            "run_write_parity.retain_source_patches", return_value={}
        ), mock.patch(
            "run_write_parity.run_case", side_effect=fail_on_third
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                run_matrix(self.plan_path, output)
        manifest = read_json(output / "manifest.json")
        self.assertEqual(manifest["status"], "failed")
        self.assertEqual(len(manifest["cells"]), 2)
        with mock.patch("run_write_parity.verify_all_role_files"), mock.patch(
            "run_write_parity.retain_source_patches", return_value={}
        ):
            with self.assertRaises(FileExistsError):
                run_matrix(self.plan_path, output)


if __name__ == "__main__":
    unittest.main()
