#!/usr/bin/env python3
"""Run the predeclared ADR-0060 fixed-work write-path parity matrix."""

import argparse
import copy
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import sys

from run_performance import (
    MODERN_WAL_CREATION_SEMANTICS,
    MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS,
    REFERENCE_HARDWARE_CRC_ROLES,
    RESIDUAL_FIELDS,
    file_digest,
    mutation_specification,
    read_json,
    run_case,
    source_state,
    write_json,
)

MATRIX_NAME = "write-path-parity-v1"
ROLE_NAMES = ("final", "baseline", "canonical")
WORKLOADS = ("overwrite", "writebatch", "writesync", "mixed50")
WORKLOAD_RECORDS = {
    "overwrite": 65536,
    "writebatch": 65536,
    "writesync": 4096,
    "mixed50": 65536,
}
EVIDENCE_GATES = (
    "correctness",
    "compatibility",
    "crash",
    "sanitizers",
    "compilers",
    "coverage",
    "benchmark_contracts",
    "review",
)
BUILD_IDENTITY_FIELDS = (
    "source_directory",
    "build_directory",
    "configure_revision",
    "configure_dirty",
    "build_type",
    "compiler",
    "c_flags",
    "cxx_flags",
    "exe_linker_flags",
    "static_linker_flags",
    "cmake_generator",
    "target_architecture",
    "benchmark_requested_revision",
    "benchmark_source_override",
    "reference_requested_revision",
    "reference_source_override",
    "reference_hardware_crc",
    "reference_source",
    "reference_binary_directory",
    "reference_have_crc32c",
    "reference_crc32c_linked",
    "reference_hardware_patch_sha256",
    "reference_pread_control_available",
    "reference_control_patch_sha256",
    "read_diagnostics_compiled",
    "snappy_target",
    "snappy_requested_revision",
    "snappy_source",
    "snappy_source_override",
    "zstd_target",
    "zstd_requested_revision",
    "zstd_source",
    "zstd_source_override",
    "crc32c_target",
    "crc32c_provider",
    "crc32c_source",
    "crc32c_source_override",
    "crc32c_requested_revision",
    "crc32c_compiled_arm64",
    "crc32c_compiled_sse42",
    "profile_capture_supported",
)
COMPLETION_IDENTITY_FIELDS = (
    "schema_version",
    "smoke",
    "preparations",
    "verifications",
    "reopens",
    "callback_invocations",
    "cursor_resets",
    "warmup_writes",
    "measured_iterations",
    "batch_size",
    "measured_reads",
    "measured_writes",
    "write_calls",
    "sync_write_calls",
    "logical_write_bytes",
    "record_crc32c",
    "insertion_crc32c",
    "present_crc32c",
    "missing_crc32c",
    "write_order_crc32c",
    "version_values_crc32c",
    "final_crc32c",
)
HEX_SHA256 = re.compile(r"[0-9a-f]{64}")
HEX_REVISION = re.compile(r"[0-9a-f]{40}")
REFERENCE_PROOF_FIELDS = {
    "port_config",
    "port_config_sha256",
    "have_crc32c",
    "archive",
    "archive_sha256",
    "hardware_patch_sha256",
}
PROFILE_EVIDENCE_FIELDS = {
    "manifest",
    "manifest_sha256",
    "summary",
    "summary_sha256",
}


def case_name(engine, workload):
    return f"{engine}/{workload}/{WORKLOAD_RECORDS[workload]}"


def matrix_member(label, side, binary_role, engine, workload, batch_ownership, wal_creation):
    return {
        "label": label,
        "side": side,
        "binary_role": binary_role,
        "engine": engine,
        "case": case_name(engine, workload),
        "modern_write_batch_ownership": batch_ownership,
        "modern_wal_creation": wal_creation,
    }


def pair_members(matrix, workload):
    if matrix == "primary":
        return (
            matrix_member(
                "hardware_leveldb", "reference", "final", "leveldb", workload,
                "copying", "durable",
            ),
            matrix_member(
                "matched_modern", "candidate", "final", "modern", workload,
                "exclusive" if workload == "writebatch" else "copying", "leveldb",
            ),
        )
    if matrix == "production":
        return (
            matrix_member(
                "baseline_modern", "reference", "baseline", "modern", workload,
                "copying", "durable",
            ),
            matrix_member(
                "production_modern", "candidate", "final", "modern", workload,
                "copying", "durable",
            ),
        )
    if matrix == "batch_ownership":
        return (
            matrix_member(
                "exclusive_modern", "reference", "final", "modern", workload,
                "exclusive", "leveldb",
            ),
            matrix_member(
                "copying_modern", "candidate", "final", "modern", workload,
                "copying", "leveldb",
            ),
        )
    if matrix == "crc_continuity":
        return (
            matrix_member(
                "canonical_leveldb", "reference", "canonical", "leveldb", workload,
                "copying", "durable",
            ),
            matrix_member(
                "hardware_leveldb", "candidate", "final", "leveldb", workload,
                "copying", "durable",
            ),
        )
    raise ValueError("unknown write-path parity matrix")


def enumerate_cells():
    cells = []
    sequence = 0
    for matrix, rounds, workloads in (
        ("primary", 5, WORKLOADS),
        ("production", 5, WORKLOADS),
        ("batch_ownership", 3, ("writebatch",)),
        ("crc_continuity", 3, WORKLOADS),
    ):
        for round_number in range(1, rounds + 1):
            for workload in workloads:
                reference, candidate = pair_members(matrix, workload)
                ordered = (reference, candidate) if round_number % 2 else (candidate, reference)
                for member in ordered:
                    sequence += 1
                    cell = {
                        "sequence": sequence,
                        "matrix": matrix,
                        "round": round_number,
                        "workload": workload,
                        **member,
                    }
                    cell["id"] = (
                        f"{sequence:03d}-{matrix}-r{round_number}-{workload}-{member['label']}"
                    )
                    cells.append(cell)
    if len(cells) != 110:
        raise AssertionError("the write-path parity matrix must contain exactly 110 cells")
    return cells


def valid_sha256(value):
    return isinstance(value, str) and HEX_SHA256.fullmatch(value) is not None


def valid_revision(value):
    return isinstance(value, str) and HEX_REVISION.fullmatch(value) is not None


def validate_runtime_source(runtime):
    fields = {
        "available",
        "revision",
        "dirty",
        "status_sha256",
        "worktree_sha256",
        "untracked_files",
    }
    if not isinstance(runtime, dict) or set(runtime) != fields:
        raise ValueError("invalid frozen runtime source state")
    if (runtime["available"] is not True
            or not valid_revision(runtime["revision"])
            or type(runtime["dirty"]) is not bool
            or not valid_sha256(runtime["status_sha256"])
            or not valid_sha256(runtime["worktree_sha256"])
            or type(runtime["untracked_files"]) is not int
            or runtime["untracked_files"] != 0):
        raise ValueError("invalid frozen runtime source values")


def validate_role(name, role):
    fields = {
        "executable",
        "executable_sha256",
        "compile_commands_sha256",
        "build",
        "runtime_source",
        "reference",
    }
    if not isinstance(role, dict) or set(role) != fields:
        raise ValueError(f"invalid frozen role schema: {name}")
    executable = Path(role["executable"])
    if not executable.is_absolute() or not executable.is_file() or executable.is_symlink():
        raise ValueError(f"frozen role executable is not a regular absolute file: {name}")
    if executable.resolve() != executable:
        raise ValueError(f"frozen role executable path is not canonical: {name}")
    if not valid_sha256(role["executable_sha256"]):
        raise ValueError(f"invalid executable SHA-256: {name}")
    if not valid_sha256(role["compile_commands_sha256"]):
        raise ValueError(f"invalid compile-commands SHA-256: {name}")
    build = role["build"]
    if not isinstance(build, dict) or set(build) != set(BUILD_IDENTITY_FIELDS):
        raise ValueError(f"invalid frozen build identity: {name}")
    if any(not isinstance(build[field], str) for field in BUILD_IDENTITY_FIELDS):
        raise ValueError(f"non-string frozen build identity: {name}")
    source_directory = Path(build["source_directory"])
    build_directory = Path(build["build_directory"])
    if (not source_directory.is_absolute() or not source_directory.is_dir()
            or not build_directory.is_absolute() or not build_directory.is_dir()):
        raise ValueError(f"frozen source/build directory is invalid: {name}")
    if (source_directory.resolve() != source_directory
            or build_directory.resolve() != build_directory):
        raise ValueError(f"frozen source/build path is not canonical: {name}")
    reference_source = Path(build["reference_source"])
    reference_binary = Path(build["reference_binary_directory"])
    if (not reference_source.is_absolute() or not reference_source.is_dir()
            or not reference_binary.is_absolute() or not reference_binary.is_dir()
            or reference_source.resolve() != reference_source
            or reference_binary.resolve() != reference_binary):
        raise ValueError(f"frozen reference source/build path is invalid: {name}")
    try:
        reference_binary.relative_to(build_directory)
    except ValueError as error:
        raise ValueError(f"reference binary directory is outside its build: {name}") from error
    try:
        executable.relative_to(build_directory)
    except ValueError as error:
        raise ValueError(f"frozen executable is outside its build directory: {name}") from error
    compile_commands = build_directory / "compile_commands.json"
    if not compile_commands.is_file() or compile_commands.is_symlink():
        raise ValueError(f"frozen compile commands are unavailable: {name}")
    if (build["build_type"] != "Release"
            or build["configure_dirty"] not in ("true", "false")
            or build["read_diagnostics_compiled"] != "false"
            or build["profile_capture_supported"] not in ("true", "false")
            or build["reference_pread_control_available"] not in ("true", "false")
            or build["reference_have_crc32c"] not in ("true", "false")
            or build["reference_crc32c_linked"] not in ("true", "false")
            or build["reference_hardware_crc"] not in REFERENCE_HARDWARE_CRC_ROLES
            or not valid_sha256(build["reference_control_patch_sha256"])):
        raise ValueError(f"invalid frozen build flags: {name}")
    for field in (
        "benchmark_requested_revision",
        "reference_requested_revision",
        "snappy_requested_revision",
        "zstd_requested_revision",
        "crc32c_requested_revision",
    ):
        if not valid_revision(build[field]):
            raise ValueError(f"frozen dependency revision is invalid: {name}/{field}")
    flags = " ".join(
        build[field]
        for field in ("c_flags", "cxx_flags", "exe_linker_flags", "static_linker_flags")
    )
    if any(flag in flags for flag in ("--coverage", "-fprofile", "-fsanitize")):
        raise ValueError(f"instrumented frozen build is not admissible: {name}")
    validate_runtime_source(role["runtime_source"])
    runtime = role["runtime_source"]
    if (build["configure_revision"] != runtime["revision"]
            or build["configure_dirty"] != str(runtime["dirty"]).lower()):
        raise ValueError(f"configure/runtime source identity differs: {name}")
    reference = role["reference"]
    if not isinstance(reference, dict) or set(reference) != REFERENCE_PROOF_FIELDS:
        raise ValueError(f"invalid frozen reference proof: {name}")
    for field in ("port_config", "archive"):
        path = Path(reference[field])
        if (not path.is_absolute() or not path.is_file() or path.is_symlink()
                or path.resolve() != path):
            raise ValueError(f"invalid frozen reference proof path: {name}/{field}")
        try:
            path.relative_to(Path(build["reference_binary_directory"]))
        except ValueError as error:
            raise ValueError(
                f"reference proof is outside its binary directory: {name}/{field}"
            ) from error
    if (not valid_sha256(reference["port_config_sha256"])
            or not valid_sha256(reference["archive_sha256"])
            or type(reference["have_crc32c"]) is not bool):
        raise ValueError(f"invalid frozen reference proof values: {name}")
    patch_sha256 = reference["hardware_patch_sha256"]
    if patch_sha256 != "not_applicable" and not valid_sha256(patch_sha256):
        raise ValueError(f"invalid hardware-reference patch SHA-256: {name}")
    expected_have = str(reference["have_crc32c"]).lower()
    if (build["reference_have_crc32c"] != expected_have
            or build["reference_crc32c_linked"] != expected_have
            or build["reference_hardware_patch_sha256"] != patch_sha256):
        raise ValueError(f"reference proof and build provenance differ: {name}")


def validate_plan(plan):
    if (not isinstance(plan, dict)
            or set(plan) != {"schema_version", "matrix", "roles", "evidence"}
            or plan["schema_version"] != 1
            or plan["matrix"] != MATRIX_NAME):
        raise ValueError("invalid write-path parity plan")
    roles = plan["roles"]
    if not isinstance(roles, dict) or set(roles) != set(ROLE_NAMES):
        raise ValueError("the plan must declare exactly final, baseline, and canonical roles")
    for name in ROLE_NAMES:
        validate_role(name, roles[name])
    for field in ("executable",):
        values = [roles[name][field] for name in ROLE_NAMES]
        if len(set(values)) != len(values):
            raise ValueError(f"frozen role {field} values must be distinct")
    for field in ("source_directory", "build_directory"):
        values = [roles[name]["build"][field] for name in ROLE_NAMES]
        if len(set(values)) != len(values):
            raise ValueError(f"frozen role {field} values must be distinct")
    final_build = roles["final"]["build"]
    canonical_build = roles["canonical"]["build"]
    baseline_build = roles["baseline"]["build"]
    final_crc = final_build["reference_hardware_crc"]
    canonical_crc = canonical_build["reference_hardware_crc"]
    if final_crc == "disabled":
        raise ValueError("the final role must use the pinned hardware-CRC reference")
    if canonical_crc != "disabled" or baseline_build["reference_hardware_crc"] != "disabled":
        raise ValueError("canonical and baseline roles must disable reference hardware CRC")
    if (final_build["crc32c_provider"] != "pinned-source"
            or final_build["crc32c_source_override"]
            or final_build["crc32c_compiled_arm64"] != "true"):
        raise ValueError("the final role does not prove pinned ARM64 CRC32C support")
    if (roles["final"]["reference"]["have_crc32c"] is not True
            or roles["canonical"]["reference"]["have_crc32c"] is not False
            or roles["baseline"]["reference"]["have_crc32c"] is not False):
        raise ValueError("frozen reference HAVE_CRC32C roles are incorrect")
    if (not valid_sha256(roles["final"]["reference"]["hardware_patch_sha256"])
            or roles["canonical"]["reference"]["hardware_patch_sha256"] != "not_applicable"
            or roles["baseline"]["reference"]["hardware_patch_sha256"] != "not_applicable"):
        raise ValueError("frozen hardware-reference patch roles are incorrect")
    if (roles["final"]["runtime_source"]["dirty"] is not True
            or roles["canonical"]["runtime_source"]["dirty"] is not False
            or roles["baseline"]["runtime_source"]["dirty"] is not True):
        raise ValueError("frozen source dirty-state roles are incorrect")
    if roles["final"]["executable_sha256"] == roles["canonical"]["executable_sha256"]:
        raise ValueError("hardware and canonical reference binaries must differ")
    if (final_build["configure_revision"] != canonical_build["configure_revision"]
            or final_build["configure_revision"] == baseline_build["configure_revision"]):
        raise ValueError("final, canonical, and pre-parity revisions are inconsistent")
    override_fields = (
        "benchmark_source_override",
        "reference_source_override",
        "snappy_source_override",
        "zstd_source_override",
        "crc32c_source_override",
    )
    if any(roles[name]["build"][field] for name in ROLE_NAMES for field in override_fields):
        raise ValueError("frozen comparison roles cannot use dependency source overrides")
    common_fields = (
        "build_type",
        "cmake_generator",
        "target_architecture",
        "compiler",
        "c_flags",
        "cxx_flags",
        "exe_linker_flags",
        "static_linker_flags",
        "benchmark_requested_revision",
        "reference_requested_revision",
        "reference_pread_control_available",
        "reference_control_patch_sha256",
        "snappy_target",
        "snappy_requested_revision",
        "zstd_target",
        "zstd_requested_revision",
        "crc32c_target",
        "crc32c_provider",
        "crc32c_requested_revision",
        "crc32c_compiled_arm64",
        "crc32c_compiled_sse42",
        "profile_capture_supported",
    )
    for field in common_fields:
        values = {roles[name]["build"][field] for name in ROLE_NAMES}
        if len(values) != 1:
            raise ValueError(f"frozen comparison build identity differs: {field}")
    evidence = plan["evidence"]
    if (not isinstance(evidence, dict)
            or set(evidence) != {"final_revision", "hardware_crc_profile", *EVIDENCE_GATES}
            or not isinstance(evidence["final_revision"], str)
            or any(evidence[gate] is not True for gate in EVIDENCE_GATES)):
        raise ValueError("correctness and review evidence is incomplete")
    final_revision = roles["final"]["build"]["configure_revision"]
    if (evidence["final_revision"] != final_revision
            or roles["final"]["runtime_source"]["revision"] != final_revision):
        raise ValueError("evidence does not describe the frozen final revision")
    profile = evidence["hardware_crc_profile"]
    if not isinstance(profile, dict) or set(profile) != PROFILE_EVIDENCE_FIELDS:
        raise ValueError("hardware CRC runtime-profile evidence is incomplete")
    for field in ("manifest", "summary"):
        path = Path(profile[field])
        if (not path.is_absolute() or not path.is_file() or path.is_symlink()
                or path.resolve() != path):
            raise ValueError(f"invalid hardware CRC profile path: {field}")
    if (not valid_sha256(profile["manifest_sha256"])
            or not valid_sha256(profile["summary_sha256"])):
        raise ValueError("invalid hardware CRC profile SHA-256")
    return plan


def load_plan(path):
    return validate_plan(read_json(path))


def compile_command_tokens(entry):
    if not isinstance(entry, dict):
        raise ValueError("malformed compile command entry")
    arguments = entry.get("arguments")
    command = entry.get("command")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return arguments
    if isinstance(command, str):
        return shlex.split(command)
    raise ValueError("compile command entry has no command or arguments")


def selected_compile_command(role, source, required_token=None):
    commands = read_json(
        Path(role["build"]["build_directory"]) / "compile_commands.json"
    )
    if not isinstance(commands, list):
        raise ValueError("compile commands must be a JSON array")
    source = Path(source)
    matches = []
    for entry in commands:
        if not isinstance(entry, dict) or Path(entry.get("file", "")) != source:
            continue
        tokens = compile_command_tokens(entry)
        if required_token is None or required_token in tokens:
            matches.append(tokens)
    if len(matches) != 1:
        raise ValueError(f"expected exactly one compile command for {source}")
    return matches[0]


def normalized_modern_compile_command(role):
    build = role["build"]
    source = Path(build["source_directory"]) / "src/engine/write_path.cc"
    tokens = selected_compile_command(
        role, source, "-DMODERN_LEVELDB_READ_DIAGNOSTICS=0"
    )
    replacements = (
        (build["build_directory"], "<BUILD>"),
        (build["source_directory"], "<SOURCE>"),
    )
    normalized = []
    for token in tokens:
        for original, replacement in replacements:
            token = token.replace(original, replacement)
        normalized.append(token)
    return tuple(normalized)


def verify_reference_files(name, role):
    build = role["build"]
    reference = role["reference"]
    if file_digest(reference["port_config"]) != reference["port_config_sha256"]:
        raise ValueError(f"frozen LevelDB port config changed: {name}")
    if file_digest(reference["archive"]) != reference["archive_sha256"]:
        raise ValueError(f"frozen LevelDB archive changed: {name}")
    port_config = Path(reference["port_config"]).read_text(encoding="utf-8")
    matches = re.findall(r"^#define HAVE_CRC32C ([01])$", port_config, re.MULTILINE)
    if len(matches) != 1 or (matches[0] == "1") != reference["have_crc32c"]:
        raise ValueError(f"frozen LevelDB HAVE_CRC32C proof differs: {name}")
    crc_source = Path(build["reference_source"]) / "util/crc32c.cc"
    command = selected_compile_command(role, crc_source)
    include_directory = str(Path(build["crc32c_source"]) / "include")
    has_crc_include = any(include_directory in token for token in command)
    if has_crc_include != reference["have_crc32c"]:
        raise ValueError(f"frozen LevelDB CRC include proof differs: {name}")
    symbols = subprocess.run(
        ["nm", "-u", "-C", reference["archive"]],
        text=True,
        capture_output=True,
        timeout=30,
    )
    if symbols.returncode != 0:
        raise ValueError(f"cannot inspect frozen LevelDB archive: {name}")
    uses_google_crc = re.search(
        r"(?m)^(?:\s*U\s+)?crc32c::Extend\(",
        symbols.stdout,
    ) is not None
    if uses_google_crc != reference["have_crc32c"]:
        raise ValueError(f"frozen LevelDB CRC symbol proof differs: {name}")


def verify_role_files(name, role):
    if file_digest(role["executable"]) != role["executable_sha256"]:
        raise ValueError(f"frozen executable changed: {name}")
    compile_commands = Path(role["build"]["build_directory"]) / "compile_commands.json"
    if file_digest(compile_commands) != role["compile_commands_sha256"]:
        raise ValueError(f"frozen compile commands changed: {name}")
    if source_state(role["build"]["source_directory"]) != role["runtime_source"]:
        raise ValueError(f"frozen runtime source state changed: {name}")
    verify_reference_files(name, role)


def verify_profile_evidence(plan):
    profile = plan["evidence"]["hardware_crc_profile"]
    if (file_digest(profile["manifest"]) != profile["manifest_sha256"]
            or file_digest(profile["summary"]) != profile["summary_sha256"]):
        raise ValueError("hardware CRC runtime-profile evidence changed")
    manifest = read_json(profile["manifest"])
    summary = read_json(profile["summary"])
    final = plan["roles"]["final"]
    if (manifest.get("status") != "complete"
            or manifest.get("mode") != "cpu_profile"
            or manifest.get("case") != "leveldb/readrandom/65536"
            or manifest.get("executable_sha256") != final["executable_sha256"]
            or manifest.get("compile_commands_sha256")
               != final["compile_commands_sha256"]
            or manifest.get("runtime_source") != final["runtime_source"]
            or manifest.get("reference_hardware_crc")
               != final["build"]["reference_hardware_crc"]
            or manifest.get("recording_timings_are_not_speedup_evidence") is not True):
        raise ValueError("hardware CRC runtime-profile manifest is inconsistent")
    build = manifest.get("build")
    if (not isinstance(build, dict)
            or any(build.get(field) != final["build"][field]
                   for field in BUILD_IDENTITY_FIELDS)):
        raise ValueError("hardware CRC runtime-profile build identity differs")
    summary_path = Path(profile["summary"])
    if (summary_path.parent != Path(profile["manifest"]).parent
            or manifest.get("artifacts", {}).get("profile_summary") != summary_path.name):
        raise ValueError("hardware CRC runtime-profile artifacts are inconsistent")
    inclusive = summary.get("inclusive")
    if (summary.get("schema_version") != 1
            or summary.get("case") != "leveldb/readrandom/65536"
            or summary.get("low_confidence") is not False
            or type(summary.get("sample_count")) is not int
            or summary["sample_count"] < 100
            or not isinstance(inclusive, list)
            or not any(
                isinstance(frame, dict)
                and "crc32c::ExtendArm64" in frame.get("symbol", "")
                and type(frame.get("sample_weight_ns")) in (int, float)
                and frame["sample_weight_ns"] > 0
                for frame in inclusive
            )):
        raise ValueError("hardware CRC runtime dispatch was not proven")


def verify_compile_alignment(plan):
    commands = {
        name: normalized_modern_compile_command(plan["roles"][name])
        for name in ROLE_NAMES
    }
    if len(set(commands.values())) != 1:
        raise ValueError("Modern write-path compile commands differ across frozen roles")


def verify_all_role_files(plan):
    for name in ROLE_NAMES:
        verify_role_files(name, plan["roles"][name])
    verify_compile_alignment(plan)
    verify_profile_evidence(plan)


def retain_source_patches(plan, output):
    patch_directory = output / "patches"
    patch_directory.mkdir()
    artifacts = {}
    for name in ROLE_NAMES:
        role = plan["roles"][name]
        result = subprocess.run(
            [
                "git",
                "-C",
                role["build"]["source_directory"],
                "diff",
                "--binary",
                "--no-ext-diff",
                "HEAD",
                "--",
            ],
            capture_output=True,
            timeout=30,
        )
        if result.returncode != 0:
            raise ValueError(
                f"cannot retain frozen source patch: {name}: "
                f"{result.stderr.decode(errors='replace').strip()}"
            )
        dirty = role["runtime_source"]["dirty"]
        if dirty != bool(result.stdout):
            raise ValueError(f"frozen dirty state has no exact tracked patch: {name}")
        path = patch_directory / f"{name}.patch"
        path.write_bytes(result.stdout)
        patch_sha256 = file_digest(path)
        expected_hardware_patch = role["reference"]["hardware_patch_sha256"]
        if expected_hardware_patch != "not_applicable" and patch_sha256 != expected_hardware_patch:
            raise ValueError(f"retained hardware-reference patch differs: {name}")
        artifacts[name] = {
            "path": f"patches/{name}.patch",
            "sha256": patch_sha256,
        }
    return artifacts


def expected_role_value(cell, field):
    if field == "modern_write_batch_ownership":
        return (
            cell[field]
            if cell["engine"] == "modern" and cell["workload"] == "writebatch"
            else "not_applicable"
        )
    if field == "modern_wal_creation":
        return cell[field] if cell["engine"] == "modern" else "not_applicable"
    raise ValueError("unknown cell role field")


def validate_residuals(completion):
    for field in RESIDUAL_FIELDS:
        if type(completion.get(field)) is not int or completion[field] < 0:
            raise ValueError(f"invalid matrix residual field: {field}")
    for prefix in ("wal", "table", "manifest"):
        if completion[f"residual_{prefix}_files"] > completion["residual_regular_files"]:
            raise ValueError("matrix residual file category exceeds total")
        if completion[f"residual_{prefix}_bytes"] > completion["residual_regular_bytes"]:
            raise ValueError("matrix residual byte category exceeds total")
    if (sum(completion[f"residual_{prefix}_files"]
            for prefix in ("wal", "table", "manifest"))
            > completion["residual_regular_files"]):
        raise ValueError("matrix residual file categories exceed total")
    if (sum(completion[f"residual_{prefix}_bytes"]
            for prefix in ("wal", "table", "manifest"))
            > completion["residual_regular_bytes"]):
        raise ValueError("matrix residual byte categories exceed total")


def validate_cell_manifest(cell, manifest, plan):
    role = plan["roles"][cell["binary_role"]]
    expected_batch = expected_role_value(cell, "modern_write_batch_ownership")
    expected_wal = expected_role_value(cell, "modern_wal_creation")
    if (not isinstance(manifest, dict)
            or manifest.get("schema_version") != 1
            or manifest.get("status") != "complete"
            or manifest.get("case") != cell["case"]
            or manifest.get("mode") != "benchmark"
            or manifest.get("executable_sha256") != role["executable_sha256"]
            or Path(manifest.get("original_executable", "")) != Path(role["executable"])
            or manifest.get("compile_commands_sha256") != role["compile_commands_sha256"]
            or manifest.get("runtime_source") != role["runtime_source"]
            or manifest.get("configure_revision_matches_runtime") is not True
            or manifest.get("reference_hardware_crc")
               != role["build"]["reference_hardware_crc"]
            or manifest.get("modern_write_batch_ownership") != expected_batch
            or manifest.get("modern_wal_creation") != expected_wal
            or manifest.get("process_cleanup_verified") is not True
            or len(manifest.get("commands", [])) != 1):
        raise ValueError(f"matrix cell manifest identity mismatch: {cell['id']}")
    expected_batch_semantics = (
        MODERN_WRITE_BATCH_OWNERSHIP_SEMANTICS[expected_batch]
        if expected_batch != "not_applicable" else "not_applicable"
    )
    expected_wal_semantics = (
        MODERN_WAL_CREATION_SEMANTICS[expected_wal]
        if expected_wal != "not_applicable" else "not_applicable"
    )
    if (manifest.get("modern_write_batch_ownership_semantics")
            != expected_batch_semantics
            or manifest.get("modern_wal_creation_semantics") != expected_wal_semantics):
        raise ValueError(f"matrix cell role semantics mismatch: {cell['id']}")
    build = manifest.get("build")
    if not isinstance(build, dict):
        raise ValueError(f"matrix cell has no retained build identity: {cell['id']}")
    if any(build.get(field) != role["build"][field] for field in BUILD_IDENTITY_FIELDS):
        raise ValueError(f"matrix cell build identity mismatch: {cell['id']}")
    if (build.get("profile_case") != cell["case"]
            or build.get("engine") != cell["engine"]
            or build.get("workload") != cell["workload"]
            or build.get("reference_hardware_crc")
               != role["build"]["reference_hardware_crc"]
            or build.get("modern_write_batch_ownership") != expected_batch
            or build.get("modern_wal_creation") != expected_wal):
        raise ValueError(f"matrix cell context mismatch: {cell['id']}")
    specification = mutation_specification(cell["case"])
    measurement = manifest.get("measurement")
    if (not isinstance(measurement, dict)
            or measurement.get("iterations") != [specification["iterations"]]
            or measurement.get("items_per_iteration")
               != specification["batch"] + specification["reads"]):
        raise ValueError(f"matrix cell fixed-work counts changed: {cell['id']}")
    for field in ("wall_ns_per_iteration", "process_cpu_ns_per_iteration"):
        values = measurement.get(field)
        if (not isinstance(values, list) or len(values) != 1
                or type(values[0]) not in (int, float) or values[0] <= 0):
            raise ValueError(f"matrix cell raw timing is invalid: {cell['id']}")
    completion = manifest.get("completion")
    if (not isinstance(completion, dict)
            or completion.get("schema_version") != 3
            or completion.get("case") != cell["case"]
            or completion.get("smoke") is not False
            or completion.get("callback_invocations") != 1
            or completion.get("cursor_resets") != 1
            or completion.get("measured_iterations") != specification["iterations"]):
        raise ValueError(f"matrix cell completion mismatch: {cell['id']}")
    validate_residuals(completion)
    return {
        **copy.deepcopy(cell),
        "manifest": f"cells/{cell['id']}/manifest.json",
        "measurement": copy.deepcopy(measurement),
        "completion": copy.deepcopy(completion),
    }


def validate_cells(cells):
    expected = enumerate_cells()
    if not isinstance(cells, list) or len(cells) != len(expected):
        raise ValueError("write-path parity cells are missing or duplicated")
    if any(
        not isinstance(cell, dict) or cell.get("id") != expected[index]["id"]
        for index, cell in enumerate(cells)
    ):
        raise ValueError("write-path parity cell order changed")
    expected_by_id = {cell["id"]: cell for cell in expected}
    if len(expected_by_id) != len(expected):
        raise AssertionError("internal duplicate write-path parity cell ID")
    actual_by_id = {}
    identity_fields = (
        "sequence",
        "matrix",
        "round",
        "workload",
        "label",
        "side",
        "binary_role",
        "engine",
        "case",
        "modern_write_batch_ownership",
        "modern_wal_creation",
    )
    for cell in cells:
        identifier = cell.get("id") if isinstance(cell, dict) else None
        if identifier not in expected_by_id or identifier in actual_by_id:
            raise ValueError("write-path parity cells are missing or duplicated")
        if any(cell.get(field) != expected_by_id[identifier][field] for field in identity_fields):
            raise ValueError(f"write-path parity cell identity changed: {identifier}")
        actual_by_id[identifier] = cell
    if set(actual_by_id) != set(expected_by_id):
        raise ValueError("write-path parity cells are missing or duplicated")
    return actual_by_id


def validate_completion_identity(cells):
    by_workload = {}
    for cell in cells:
        completion = cell["completion"]
        if (not isinstance(completion, dict)
                or any(field not in completion for field in COMPLETION_IDENTITY_FIELDS)):
            raise ValueError("matrix completion identity is incomplete")
        validate_residuals(completion)
        identity = tuple(completion[field] for field in COMPLETION_IDENTITY_FIELDS)
        existing = by_workload.setdefault(cell["workload"], identity)
        if identity != existing:
            raise ValueError(
                f"completion fingerprint or fixed operation count differs: {cell['workload']}"
            )


def ratio_delta(candidate, reference):
    if candidate <= 0 or reference <= 0:
        raise ValueError("matrix timing ratios require positive values")
    return candidate / reference - 1.0


def pair_report(cells, matrix, workload, rounds):
    selected = [
        cell for cell in cells
        if cell["matrix"] == matrix and cell["workload"] == workload
    ]
    if len(selected) != rounds * 2:
        raise ValueError(f"matrix pair is incomplete: {matrix}/{workload}")
    round_reports = []
    reference_wall = []
    candidate_wall = []
    reference_cpu = []
    candidate_cpu = []
    for round_number in range(1, rounds + 1):
        pair = [cell for cell in selected if cell["round"] == round_number]
        sides = {cell["side"]: cell for cell in pair}
        if len(pair) != 2 or set(sides) != {"reference", "candidate"}:
            raise ValueError(f"matrix round is incomplete: {matrix}/{workload}/{round_number}")
        reference = sides["reference"]
        candidate = sides["candidate"]
        ref_wall = reference["measurement"]["wall_ns_per_iteration"][0]
        cand_wall = candidate["measurement"]["wall_ns_per_iteration"][0]
        ref_cpu = reference["measurement"]["process_cpu_ns_per_iteration"][0]
        cand_cpu = candidate["measurement"]["process_cpu_ns_per_iteration"][0]
        reference_wall.append(ref_wall)
        candidate_wall.append(cand_wall)
        reference_cpu.append(ref_cpu)
        candidate_cpu.append(cand_cpu)
        round_report = {
            "round": round_number,
            "reference_cell": reference["id"],
            "candidate_cell": candidate["id"],
            "reference_wall_ns_per_iteration": ref_wall,
            "candidate_wall_ns_per_iteration": cand_wall,
            "wall_delta": ratio_delta(cand_wall, ref_wall),
            "reference_process_cpu_ns_per_iteration": ref_cpu,
            "candidate_process_cpu_ns_per_iteration": cand_cpu,
            "process_cpu_delta": ratio_delta(cand_cpu, ref_cpu),
        }
        if workload == "writebatch":
            round_report.update(
                reference_wall_ns_per_written_key=ref_wall / 32,
                candidate_wall_ns_per_written_key=cand_wall / 32,
                reference_process_cpu_ns_per_written_key=ref_cpu / 32,
                candidate_process_cpu_ns_per_written_key=cand_cpu / 32,
            )
        round_reports.append(round_report)
    report = {
        "rounds": round_reports,
        "reference_median_wall_ns_per_iteration": statistics.median(reference_wall),
        "candidate_median_wall_ns_per_iteration": statistics.median(candidate_wall),
        "aggregate_wall_delta": ratio_delta(
            statistics.median(candidate_wall), statistics.median(reference_wall)
        ),
        "reference_median_process_cpu_ns_per_iteration": statistics.median(reference_cpu),
        "candidate_median_process_cpu_ns_per_iteration": statistics.median(candidate_cpu),
        "aggregate_process_cpu_delta": ratio_delta(
            statistics.median(candidate_cpu), statistics.median(reference_cpu)
        ),
    }
    if workload == "writebatch":
        report.update(
            reference_median_wall_ns_per_written_key=report[
                "reference_median_wall_ns_per_iteration"
            ] / 32,
            candidate_median_wall_ns_per_written_key=report[
                "candidate_median_wall_ns_per_iteration"
            ] / 32,
            reference_median_process_cpu_ns_per_written_key=report[
                "reference_median_process_cpu_ns_per_iteration"
            ] / 32,
            candidate_median_process_cpu_ns_per_written_key=report[
                "candidate_median_process_cpu_ns_per_iteration"
            ] / 32,
        )
    return report


def evaluation_report(evidence):
    evidence_valid = (
        isinstance(evidence, dict) and all(evidence.get(gate) is True for gate in EVIDENCE_GATES)
    )
    return {
        "performance_policy": "diagnostic-only",
        "preset_performance_thresholds": False,
        "correctness_and_review_evidence": evidence_valid,
    }


def aggregate_cells(cells, evidence):
    validate_cells(cells)
    validate_completion_identity(cells)
    primary = {
        workload: pair_report(cells, "primary", workload, 5)
        for workload in WORKLOADS
    }
    production = {
        workload: pair_report(cells, "production", workload, 5)
        for workload in WORKLOADS
    }
    batch = pair_report(cells, "batch_ownership", "writebatch", 3)
    crc = {
        workload: pair_report(cells, "crc_continuity", workload, 3)
        for workload in WORKLOADS
    }
    wal_durability = {}
    for workload in WORKLOADS:
        if workload == "writebatch":
            continue
        matched = primary[workload]
        durable = production[workload]
        wal_durability[workload] = {
            "wall_delta": ratio_delta(
                durable["candidate_median_wall_ns_per_iteration"],
                matched["candidate_median_wall_ns_per_iteration"],
            ),
            "process_cpu_delta": ratio_delta(
                durable["candidate_median_process_cpu_ns_per_iteration"],
                matched["candidate_median_process_cpu_ns_per_iteration"],
            ),
            "causal_estimate": False,
        }
    return {
        "schema_version": 2,
        "matrix": MATRIX_NAME,
        "processes": len(cells),
        "primary": primary,
        "production": production,
        "batch_ownership": batch,
        "crc_continuity": crc,
        "wal_durability": wal_durability,
        "evaluation": evaluation_report(evidence),
    }


def run_matrix(plan_path, output):
    # Predeclare roles, fixed work, controls, and ordering before measuring.
    # Recheck binary/source identity around each fresh-process cell so an apparent
    # improvement cannot silently come from changed work or a replaced executable.
    plan_path = Path(plan_path).resolve(strict=True)
    plan_sha256 = file_digest(plan_path)
    plan = load_plan(plan_path)
    if file_digest(plan_path) != plan_sha256:
        raise ValueError("write-path parity plan changed while it was being validated")
    verify_all_role_files(plan)
    output = Path(output).absolute()
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    (output / "cells").mkdir()
    write_json(output / "plan.json", plan)
    source_patches = retain_source_patches(plan, output)
    matrix_manifest = {
        "schema_version": 1,
        "matrix": MATRIX_NAME,
        "status": "running",
        "source_plan": str(plan_path),
        "source_plan_sha256": plan_sha256,
        "retained_plan_sha256": file_digest(output / "plan.json"),
        "cells": [],
        "artifacts": {
            "plan": "plan.json",
            "report": "report.json",
            "source_patches": source_patches,
        },
    }
    manifest_path = output / "manifest.json"
    write_json(manifest_path, matrix_manifest)
    try:
        for cell in enumerate_cells():
            verify_all_role_files(plan)
            role = plan["roles"][cell["binary_role"]]
            cell_output = output / "cells" / cell["id"]
            run_case(
                role["executable"],
                cell["case"],
                cell_output,
                repetitions=1,
                modern_write_batch_ownership=cell["modern_write_batch_ownership"],
                modern_wal_creation=cell["modern_wal_creation"],
            )
            verify_all_role_files(plan)
            retained = validate_cell_manifest(
                cell, read_json(cell_output / "manifest.json"), plan
            )
            matrix_manifest["cells"].append(retained)
            write_json(manifest_path, matrix_manifest)
        report = aggregate_cells(matrix_manifest["cells"], plan["evidence"])
        write_json(output / "report.json", report)
        matrix_manifest["status"] = "complete"
        matrix_manifest["performance_policy"] = report["evaluation"]["performance_policy"]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        matrix_manifest["status"] = "failed"
        matrix_manifest["error"] = str(error)
        raise
    except KeyboardInterrupt:
        matrix_manifest["status"] = "cancelled"
        matrix_manifest["error"] = "interrupted by user"
        raise
    finally:
        write_json(manifest_path, matrix_manifest)
    return matrix_manifest


def main():
    parser = argparse.ArgumentParser(
        description="Run the predeclared ADR-0060 fixed write-path parity matrix."
    )
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        run_matrix(args.plan, args.output)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"write-path parity matrix failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
