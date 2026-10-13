#!/usr/bin/env python3
"""Plan and run resumable local V1 through V4 stress campaigns."""

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Optional, Sequence


PLAN_FORMAT = "modern-leveldb-stress-plan-v1"
COMMAND_FORMAT = "modern-leveldb-stress-command-v1"
ATTEMPT_RESULT_FORMAT = "modern-leveldb-stress-attempt-result-v1"
STATE_FORMAT = "modern-leveldb-stress-state-v1"
SUMMARY_FORMAT = "modern-leveldb-stress-summary-v1"
CAMPAIGN_RESULT_FORMAT = "modern-leveldb-stress-campaign-result-v1"
UINT64_MAX = (1 << 64) - 1
TIER_ORDER = ("v1", "v2", "v3", "v4")
COMPRESSIONS = ("none", "snappy", "zstd")
V2_PROFILES = ((2, "none"), (4, "snappy"), (8, "zstd"))


@dataclass
class CampaignConfig:
    stress_dir: Path
    workspace: Path
    seeds: tuple[int, ...] = (1, 17, 301)
    tiers: tuple[str, ...] = TIER_ORDER
    v1_operations: int = 5000
    v2_rounds: int = 300
    v3_rounds: int = 25
    v3_inflight_probes: int = 3
    v3_timeout_seconds: float = 30.0
    v4_operations: int = 250
    v4_liveness_writes: int = 2000
    job_timeout_seconds: float = 3600.0
    max_duration_seconds: float = 0.0
    resume: bool = False


@dataclass(frozen=True)
class CampaignJob:
    index: int
    job_id: str
    tier: str
    seed: int
    argv_template: tuple[str, ...]
    run_artifact: str
    completion_artifact: str


@dataclass(frozen=True)
class CampaignPlan:
    stress_dir: str
    seeds: tuple[int, ...]
    tiers: tuple[str, ...]
    v1_operations: int
    v2_rounds: int
    v3_rounds: int
    v3_inflight_probes: int
    v3_timeout_seconds: float
    v4_operations: int
    v4_liveness_writes: int
    jobs: tuple[CampaignJob, ...]


@dataclass(frozen=True)
class AttemptResult:
    category: str
    returncode: Optional[int]
    termination_sent: bool
    duration_seconds: float
    error: Optional[str]


@dataclass(frozen=True)
class SourceIdentity:
    revision: str
    dirty: bool


@dataclass(frozen=True)
class CampaignSummary:
    status: str
    total: int
    passed: int
    failed: int
    pending: int
    revision: Optional[str]
    dirty: Optional[bool]
    blocking_job: Optional[str]


class ArtifactError(RuntimeError):
    def __init__(self, category: str, message: str):
        super().__init__(message)
        self.category = category


def _positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("value must be an integer") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def _nonnegative_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("value must be an integer") from error
    if value < 0:
        raise argparse.ArgumentTypeError("value cannot be negative")
    return value


def _positive_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("value must be a number") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def _nonnegative_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("value must be a number") from error
    if value < 0:
        raise argparse.ArgumentTypeError("value cannot be negative")
    return value


def _parse_seeds(text: str) -> tuple[int, ...]:
    if not text:
        raise argparse.ArgumentTypeError("seeds cannot be empty")
    values = []
    for token in text.split(","):
        if not token:
            raise argparse.ArgumentTypeError("seed cannot be empty")
        try:
            seed = int(token)
        except ValueError as error:
            raise argparse.ArgumentTypeError("seed must be an integer") from error
        if seed < 0 or seed > UINT64_MAX:
            raise argparse.ArgumentTypeError("seed must fit uint64")
        if seed in values:
            raise argparse.ArgumentTypeError("seeds cannot repeat")
        values.append(seed)
    return tuple(values)


def _parse_tiers(text: str) -> tuple[str, ...]:
    if not text:
        raise argparse.ArgumentTypeError("tiers cannot be empty")
    selected = text.split(",")
    if len(set(selected)) != len(selected):
        raise argparse.ArgumentTypeError("tiers cannot repeat")
    unknown = set(selected) - set(TIER_ORDER)
    if unknown:
        raise argparse.ArgumentTypeError("unknown tier")
    return tuple(tier for tier in TIER_ORDER if tier in selected)


def parse_arguments(arguments: Sequence[str]) -> CampaignConfig:
    parser = argparse.ArgumentParser(
        description="Run resumable local Modern LevelDB stress campaigns."
    )
    parser.add_argument("--stress-dir", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--seeds", type=_parse_seeds, default=(1, 17, 301))
    parser.add_argument("--tiers", type=_parse_tiers, default=TIER_ORDER)
    parser.add_argument("--v1-operations", type=_positive_int, default=5000)
    parser.add_argument("--v2-rounds", type=_positive_int, default=300)
    parser.add_argument("--v3-rounds", type=_positive_int, default=25)
    parser.add_argument(
        "--v3-inflight-probes", type=_nonnegative_int, default=3
    )
    parser.add_argument(
        "--v3-timeout-seconds", type=_positive_float, default=30.0
    )
    parser.add_argument("--v4-operations", type=_positive_int, default=250)
    parser.add_argument(
        "--v4-liveness-writes", type=_positive_int, default=2000
    )
    parser.add_argument(
        "--job-timeout-seconds", type=_positive_float, default=3600.0
    )
    parser.add_argument(
        "--max-duration-seconds", type=_nonnegative_float, default=0.0
    )
    parser.add_argument("--resume", action="store_true")
    parsed = parser.parse_args(arguments)
    return CampaignConfig(
        stress_dir=parsed.stress_dir,
        workspace=parsed.workspace,
        seeds=parsed.seeds,
        tiers=parsed.tiers,
        v1_operations=parsed.v1_operations,
        v2_rounds=parsed.v2_rounds,
        v3_rounds=parsed.v3_rounds,
        v3_inflight_probes=parsed.v3_inflight_probes,
        v3_timeout_seconds=parsed.v3_timeout_seconds,
        v4_operations=parsed.v4_operations,
        v4_liveness_writes=parsed.v4_liveness_writes,
        job_timeout_seconds=parsed.job_timeout_seconds,
        max_duration_seconds=parsed.max_duration_seconds,
        resume=parsed.resume,
    )


def _executable(stress_dir: Path, name: str) -> str:
    suffix = ".exe" if os.name == "nt" else ""
    return str(stress_dir / f"{name}{suffix}")


def _float_text(value: float) -> str:
    return format(value, ".15g")


def _validate_config(config: CampaignConfig) -> tuple[str, ...]:
    if not config.seeds:
        raise ValueError("seeds cannot be empty")
    if len(set(config.seeds)) != len(config.seeds):
        raise ValueError("seeds cannot repeat")
    if any(seed < 0 or seed > UINT64_MAX for seed in config.seeds):
        raise ValueError("seed must fit uint64")
    if not config.tiers:
        raise ValueError("tiers cannot be empty")
    if len(set(config.tiers)) != len(config.tiers):
        raise ValueError("tiers cannot repeat")
    unknown = set(config.tiers) - set(TIER_ORDER)
    if unknown:
        raise ValueError("unknown tier")
    positive = (
        config.v1_operations,
        config.v2_rounds,
        config.v3_rounds,
        config.v3_timeout_seconds,
        config.v4_operations,
        config.v4_liveness_writes,
        config.job_timeout_seconds,
    )
    if any(value <= 0 for value in positive):
        raise ValueError("campaign counts and timeouts must be positive")
    if config.v3_inflight_probes < 0:
        raise ValueError("inflight probes cannot be negative")
    if config.max_duration_seconds < 0:
        raise ValueError("maximum duration cannot be negative")
    return tuple(tier for tier in TIER_ORDER if tier in config.tiers)


def build_plan(config: CampaignConfig) -> CampaignPlan:
    tiers = _validate_config(config)
    crash_runner = Path(__file__).resolve().with_name("run_crash_stress.py")
    jobs = []

    def add(
        job_id: str,
        tier: str,
        seed: int,
        argv: list[str],
        run_artifact: str = "artifacts/run.txt",
        completion_artifact: str = "artifacts/completion.txt",
    ) -> None:
        jobs.append(
            CampaignJob(
                index=len(jobs),
                job_id=job_id,
                tier=tier,
                seed=seed,
                argv_template=tuple(argv),
                run_artifact=run_artifact,
                completion_artifact=completion_artifact,
            )
        )

    for seed in config.seeds:
        if "v1" in tiers:
            for compression in COMPRESSIONS:
                add(
                    f"seed-{seed}-v1-{compression}",
                    "v1",
                    seed,
                    [
                        _executable(config.stress_dir, "modern_leveldb_stress"),
                        "--db",
                        "{database}",
                        "--output",
                        "{artifacts}",
                        "--seed",
                        str(seed),
                        "--operations",
                        str(config.v1_operations),
                        "--compression",
                        compression,
                    ],
                )
        if "v2" in tiers:
            for threads, compression in V2_PROFILES:
                add(
                    f"seed-{seed}-v2-t{threads}-{compression}",
                    "v2",
                    seed,
                    [
                        _executable(
                            config.stress_dir,
                            "modern_leveldb_concurrent_stress",
                        ),
                        "--db",
                        "{database}",
                        "--output",
                        "{artifacts}",
                        "--seed",
                        str(seed),
                        "--threads",
                        str(threads),
                        "--rounds",
                        str(config.v2_rounds),
                        "--compression",
                        compression,
                    ],
                )
        if "v3" in tiers:
            for compression in COMPRESSIONS:
                add(
                    f"seed-{seed}-v3-{compression}",
                    "v3",
                    seed,
                    [
                        sys.executable,
                        str(crash_runner),
                        "--worker",
                        _executable(
                            config.stress_dir,
                            "modern_leveldb_crash_stress_worker",
                        ),
                        "--db",
                        "{database}",
                        "--output",
                        "{artifacts}",
                        "--rounds",
                        str(config.v3_rounds),
                        "--seed",
                        str(seed),
                        "--compression",
                        compression,
                        "--timeout-seconds",
                        _float_text(config.v3_timeout_seconds),
                        "--inflight-probes",
                        str(config.v3_inflight_probes),
                    ],
                )
        if "v4" in tiers:
            add(
                f"seed-{seed}-v4",
                "v4",
                seed,
                [
                    _executable(
                        config.stress_dir,
                        "modern_leveldb_option_fault_stress",
                    ),
                    "--workspace",
                    "{workspace}",
                    "--seed",
                    str(seed),
                    "--operations",
                    str(config.v4_operations),
                    "--liveness-writes",
                    str(config.v4_liveness_writes),
                ],
                run_artifact="workspace/artifacts/run.txt",
                completion_artifact="workspace/artifacts/completion.txt",
            )

    return CampaignPlan(
        stress_dir=str(config.stress_dir),
        seeds=config.seeds,
        tiers=tiers,
        v1_operations=config.v1_operations,
        v2_rounds=config.v2_rounds,
        v3_rounds=config.v3_rounds,
        v3_inflight_probes=config.v3_inflight_probes,
        v3_timeout_seconds=config.v3_timeout_seconds,
        v4_operations=config.v4_operations,
        v4_liveness_writes=config.v4_liveness_writes,
        jobs=tuple(jobs),
    )


def command_for_attempt(job: CampaignJob, attempt: Path) -> list[str]:
    replacements = {
        "{attempt}": str(attempt),
        "{database}": str(attempt / "database"),
        "{artifacts}": str(attempt / "artifacts"),
        "{workspace}": str(attempt / "workspace"),
    }
    return [replacements.get(argument, argument) for argument in job.argv_template]


def plan_document(plan: CampaignPlan) -> dict:
    return {
        "format": PLAN_FORMAT,
        "stress_dir": plan.stress_dir,
        "seeds": list(plan.seeds),
        "tiers": list(plan.tiers),
        "budgets": {
            "v1_operations": plan.v1_operations,
            "v2_rounds": plan.v2_rounds,
            "v3_rounds": plan.v3_rounds,
            "v3_inflight_probes": plan.v3_inflight_probes,
            "v3_timeout_seconds": plan.v3_timeout_seconds,
            "v4_operations": plan.v4_operations,
            "v4_liveness_writes": plan.v4_liveness_writes,
        },
        "jobs": [
            {
                "index": job.index,
                "id": job.job_id,
                "tier": job.tier,
                "seed": job.seed,
                "argv": list(job.argv_template),
                "run_artifact": job.run_artifact,
                "completion_artifact": job.completion_artifact,
            }
            for job in plan.jobs
        ],
    }


def plan_digest(plan: CampaignPlan) -> str:
    encoded = json.dumps(
        plan_document(plan),
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _json_text(document: dict) -> str:
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def _fsync_directory(path: Path) -> None:
    if os.name == "nt":
        return
    descriptor = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _atomic_write_text(path: Path, text: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        output.write(text)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    _fsync_directory(path.parent)


def _atomic_write_json(path: Path, document: dict) -> None:
    _atomic_write_text(path, _json_text(document))


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as source:
        document = json.load(source)
    if not isinstance(document, dict):
        raise RuntimeError(f"JSON document is not an object: {path}")
    return document


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _terminate_owned_process(
    process: subprocess.Popen, grace_seconds: float
) -> bool:
    if process.poll() is not None:
        return False
    termination_sent = True
    if os.name == "nt":
        try:
            process.send_signal(signal.CTRL_BREAK_EVENT)
        except OSError:
            process.terminate()
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=grace_seconds)
        return termination_sent
    except subprocess.TimeoutExpired:
        pass

    if os.name == "nt":
        process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=grace_seconds)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("owned process group could not be reaped") from error
    return termination_sent


def execute_attempt(
    job: CampaignJob,
    attempt: Path,
    timeout_seconds: float,
    *,
    working_directory: Optional[Path] = None,
    termination_grace_seconds: float = 5.0,
) -> AttemptResult:
    if timeout_seconds <= 0:
        raise ValueError("attempt timeout must be positive")
    if termination_grace_seconds <= 0:
        raise ValueError("termination grace must be positive")
    if attempt.exists():
        raise ValueError(f"attempt path already exists: {attempt}")
    if not attempt.parent.is_dir():
        raise ValueError("attempt parent directory does not exist")
    attempt.mkdir()

    command = command_for_attempt(job, attempt)
    cwd = Path.cwd() if working_directory is None else working_directory
    _atomic_write_json(
        attempt / "command.json",
        {
            "format": COMMAND_FORMAT,
            "job_id": job.job_id,
            "argv": command,
            "working_directory": str(cwd),
        },
    )

    started_at = _utc_now()
    started = time.monotonic()
    category = "launch_failure"
    returncode = None
    termination_sent = False
    error_text = None
    interrupted = False
    process = None
    with (attempt / "stdout.log").open("wb") as stdout, (
        attempt / "stderr.log"
    ).open("wb") as stderr:
        try:
            options = {
                "stdin": subprocess.DEVNULL,
                "stdout": stdout,
                "stderr": stderr,
                "cwd": str(cwd),
            }
            if os.name == "nt":
                options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                options["start_new_session"] = True
            process = subprocess.Popen(command, **options)
            try:
                returncode = process.wait(timeout=timeout_seconds)
                category = "exit_zero" if returncode == 0 else "nonzero_exit"
            except subprocess.TimeoutExpired:
                category = "timed_out"
                termination_sent = _terminate_owned_process(
                    process, termination_grace_seconds
                )
                returncode = process.returncode
            except KeyboardInterrupt:
                category = "interrupted"
                interrupted = True
                termination_sent = _terminate_owned_process(
                    process, termination_grace_seconds
                )
                returncode = process.returncode
        except OSError as error:
            category = "launch_failure"
            error_text = str(error)
        except BaseException as error:
            if process is not None and process.poll() is None:
                try:
                    termination_sent = _terminate_owned_process(
                        process, termination_grace_seconds
                    )
                    returncode = process.returncode
                except BaseException as termination_error:
                    error_text = (
                        f"{error}; process containment failed: "
                        f"{termination_error}"
                    )
            if error_text is None:
                error_text = str(error)
            category = "execution_failure"

    duration = time.monotonic() - started
    result = AttemptResult(
        category=category,
        returncode=returncode,
        termination_sent=termination_sent,
        duration_seconds=duration,
        error=error_text,
    )
    _atomic_write_json(
        attempt / "result.json",
        {
            "format": ATTEMPT_RESULT_FORMAT,
            "job_id": job.job_id,
            "category": result.category,
            "returncode": result.returncode,
            "termination_sent": result.termination_sent,
            "duration_seconds": result.duration_seconds,
            "error": result.error,
            "started_at": started_at,
            "ended_at": _utc_now(),
        },
    )
    if interrupted:
        raise KeyboardInterrupt
    return result


def _relative_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _job_root(workspace: Path, job: CampaignJob) -> Path:
    return workspace / "jobs" / f"{job.index:03d}-{job.job_id}"


def _validate_file(path: Path, name: str, executable: bool = False) -> None:
    if not path.is_file():
        raise ValueError(f"{name} does not exist: {path}")
    if executable and os.name != "nt" and not os.access(path, os.X_OK):
        raise ValueError(f"{name} is not executable: {path}")


def _validate_plan_commands(plan: CampaignPlan) -> None:
    expected_indices = list(range(len(plan.jobs)))
    if [job.index for job in plan.jobs] != expected_indices:
        raise ValueError("plan job indices are not contiguous")
    identifiers = [job.job_id for job in plan.jobs]
    if len(set(identifiers)) != len(identifiers):
        raise ValueError("plan job IDs are not unique")
    for job in plan.jobs:
        if not re.fullmatch(r"[A-Za-z0-9._-]+", job.job_id):
            raise ValueError("plan job ID is not path-safe")
        if not job.argv_template:
            raise ValueError("plan job command is empty")
        _validate_file(Path(job.argv_template[0]), "job executable", True)
        if (
            len(job.argv_template) > 1
            and job.argv_template[0] == sys.executable
            and job.argv_template[1].endswith(".py")
        ):
            _validate_file(Path(job.argv_template[1]), "Python job script")
        if job.tier == "v3":
            try:
                worker_index = job.argv_template.index("--worker") + 1
                worker = Path(job.argv_template[worker_index])
            except (ValueError, IndexError) as error:
                raise ValueError("V3 job does not name a worker") from error
            _validate_file(worker, "V3 worker", True)


def _plan_file_document(plan: CampaignPlan) -> dict:
    document = plan_document(plan)
    document["digest"] = plan_digest(plan)
    return document


def _initial_state(plan: CampaignPlan) -> dict:
    return {
        "format": STATE_FORMAT,
        "plan_digest": plan_digest(plan),
        "status": "pending",
        "identity": None,
        "jobs": [
            {
                "index": job.index,
                "id": job.job_id,
                "tier": job.tier,
                "seed": job.seed,
                "status": "pending",
                "attempts": [],
            }
            for job in plan.jobs
        ],
    }


def _append_event(workspace: Path, event: dict) -> None:
    record = dict(event)
    record["timestamp"] = _utc_now()
    text = json.dumps(
        record, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    )
    with (workspace / "events.jsonl").open(
        "a", encoding="utf-8", newline="\n"
    ) as output:
        output.write(text + "\n")
        output.flush()
        os.fsync(output.fileno())


def _publish_state(workspace: Path, state: dict) -> None:
    _atomic_write_json(workspace / "state.json", state)
    for job in state["jobs"]:
        job_path = (
            workspace
            / "jobs"
            / f"{job['index']:03d}-{job['id']}"
            / "state.json"
        )
        _atomic_write_json(
            job_path,
            {
                "format": STATE_FORMAT,
                "plan_digest": state["plan_digest"],
                "campaign_status": state["status"],
                "job": job,
            },
        )


def _identity_document(identity: Optional[SourceIdentity]) -> Optional[dict]:
    if identity is None:
        return None
    return {"revision": identity.revision, "dirty": identity.dirty}


def _summary_from_state(state: dict) -> CampaignSummary:
    statuses = [job["status"] for job in state["jobs"]]
    failed_statuses = {"failed", "timed_out", "interrupted"}
    identity_document = state.get("identity")
    identity = (
        None
        if identity_document is None
        else SourceIdentity(
            identity_document["revision"], identity_document["dirty"]
        )
    )
    blocking = next(
        (
            job["id"]
            for job in state["jobs"]
            if job["status"] in failed_statuses
        ),
        None,
    )
    return CampaignSummary(
        status=state["status"],
        total=len(statuses),
        passed=statuses.count("passed"),
        failed=sum(status in failed_statuses for status in statuses),
        pending=sum(status in {"pending", "running"} for status in statuses),
        revision=None if identity is None else identity.revision,
        dirty=None if identity is None else identity.dirty,
        blocking_job=blocking,
    )


def _publish_summary(workspace: Path, state: dict) -> CampaignSummary:
    summary = _summary_from_state(state)
    _atomic_write_json(
        workspace / "summary.json",
        {
            "format": SUMMARY_FORMAT,
            "plan_digest": state["plan_digest"],
            "status": summary.status,
            "total": summary.total,
            "passed": summary.passed,
            "failed": summary.failed,
            "pending": summary.pending,
            "revision": summary.revision,
            "dirty": summary.dirty,
            "blocking_job": summary.blocking_job,
            "jobs": state["jobs"],
        },
    )
    _atomic_write_text(
        workspace / "summary.txt",
        "\n".join(
            [
                f"format={SUMMARY_FORMAT}",
                f"plan_digest={state['plan_digest']}",
                f"status={summary.status}",
                f"total={summary.total}",
                f"passed={summary.passed}",
                f"failed={summary.failed}",
                f"pending={summary.pending}",
                f"revision={summary.revision or ''}",
                (
                    "dirty="
                    if summary.dirty is None
                    else f"dirty={'true' if summary.dirty else 'false'}"
                ),
                f"blocking_job={summary.blocking_job or ''}",
                "",
            ]
        ),
    )
    return summary


def _parse_text_artifact(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ArtifactError("unreadable_artifact", str(error)) from error
    result = {}
    for line in lines:
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in result:
            raise ArtifactError(
                "invalid_artifact", f"duplicate artifact key: {key}"
            )
        result[key] = value
    return result


def _expected_formats(tier: str) -> tuple[str, str]:
    formats = {
        "v1": (
            "modern-leveldb-stress-run-v1",
            "modern-leveldb-stress-result-v1",
        ),
        "v2": (
            "modern-leveldb-concurrent-stress-run-v1",
            "modern-leveldb-concurrent-result-v1",
        ),
        "v3": (
            "modern-leveldb-crash-run-v1",
            "modern-leveldb-crash-result-v1",
        ),
        "v4": (
            "modern-leveldb-option-fault-run-v1",
            "modern-leveldb-option-fault-result-v1",
        ),
    }
    try:
        return formats[tier]
    except KeyError as error:
        raise ArtifactError("unknown_tier", f"unknown tier: {tier}") from error


def validate_job_artifacts(
    job: CampaignJob, attempt: Path
) -> SourceIdentity:
    run_path = attempt / job.run_artifact
    completion_path = attempt / job.completion_artifact
    if not run_path.is_file():
        raise ArtifactError("missing_run", f"run artifact is missing: {run_path}")
    if not completion_path.is_file():
        raise ArtifactError(
            "missing_completion",
            f"completion artifact is missing: {completion_path}",
        )
    if (completion_path.parent / "failure.txt").exists():
        raise ArtifactError(
            "conflicting_failure",
            "failure artifact exists beside completion",
        )
    expected_run, expected_result = _expected_formats(job.tier)
    run = _parse_text_artifact(run_path)
    completion = _parse_text_artifact(completion_path)
    if run.get("format") != expected_run:
        raise ArtifactError("bad_run_format", "run artifact format is invalid")
    if completion.get("format") != expected_result:
        raise ArtifactError(
            "bad_completion_format", "completion artifact format is invalid"
        )
    revision = run.get("revision", "")
    if not revision:
        raise ArtifactError("missing_identity", "run revision is missing")
    dirty_text = run.get("dirty")
    if dirty_text not in {"true", "false"}:
        raise ArtifactError("missing_identity", "run dirty state is invalid")
    return SourceIdentity(revision, dirty_text == "true")


def _attempt_path(workspace: Path, job: CampaignJob, number: int) -> Path:
    return _job_root(workspace, job) / "attempts" / f"{number:03d}"


def _prepare_new_workspace(workspace: Path, plan: CampaignPlan) -> dict:
    if workspace.exists():
        raise ValueError(f"workspace path already exists: {workspace}")
    if not workspace.parent.is_dir():
        raise ValueError("workspace parent directory does not exist")
    workspace.mkdir()
    (workspace / "jobs").mkdir()
    for job in plan.jobs:
        root = _job_root(workspace, job)
        root.mkdir()
        (root / "attempts").mkdir()
    _atomic_write_json(workspace / "plan.json", _plan_file_document(plan))
    state = _initial_state(plan)
    _publish_state(workspace, state)
    _append_event(
        workspace,
        {
            "event": "campaign_created",
            "plan_digest": state["plan_digest"],
        },
    )
    _publish_summary(workspace, state)
    return state


def _load_resume_workspace(workspace: Path, plan: CampaignPlan) -> dict:
    if not workspace.is_dir():
        raise ValueError("resume workspace does not exist")
    stored_plan = read_json(workspace / "plan.json")
    if stored_plan != _plan_file_document(plan):
        raise ValueError("resume plan does not match recorded plan")
    state = read_json(workspace / "state.json")
    if state.get("format") != STATE_FORMAT:
        raise ValueError("resume state format is invalid")
    if state.get("plan_digest") != plan_digest(plan):
        raise ValueError("resume state plan digest is invalid")
    if len(state.get("jobs", [])) != len(plan.jobs):
        raise ValueError("resume state job count is invalid")
    for planned, recorded in zip(plan.jobs, state["jobs"]):
        if (
            recorded.get("index") != planned.index
            or recorded.get("id") != planned.job_id
        ):
            raise ValueError("resume state job identity is invalid")
        if recorded.get("status") == "running":
            if recorded.get("attempts"):
                attempt = recorded["attempts"][-1]
                attempt["status"] = "interrupted"
                attempt["category"] = "orchestrator_interrupted"
            recorded["status"] = "interrupted"
            _append_event(
                workspace,
                {
                    "event": "stale_running_interrupted",
                    "job_id": planned.job_id,
                },
            )
    return state


def _identity_from_state(state: dict) -> Optional[SourceIdentity]:
    document = state.get("identity")
    if document is None:
        return None
    return SourceIdentity(document["revision"], document["dirty"])


def _revalidate_passed_jobs(
    workspace: Path, plan: CampaignPlan, state: dict
) -> Optional[SourceIdentity]:
    identity = None
    completion_exists = (workspace / "completion.txt").exists()
    for job, recorded in zip(plan.jobs, state["jobs"]):
        if recorded["status"] != "passed":
            continue
        if not recorded["attempts"]:
            raise ValueError("passed job has no attempt")
        attempt = workspace / recorded["attempts"][-1]["path"]
        try:
            found = validate_job_artifacts(job, attempt)
            if identity is None:
                identity = found
            elif found != identity:
                raise ArtifactError(
                    "identity_mismatch",
                    "passed job source identity differs from campaign",
                )
        except ArtifactError as error:
            if completion_exists:
                raise ValueError(
                    f"completed campaign artifact validation failed: {error}"
                ) from error
            recorded["status"] = "failed"
            recorded["attempts"][-1]["revalidation_error"] = str(error)
            recorded["attempts"][-1]["category"] = error.category
    return identity


def _write_campaign_completion(
    workspace: Path, state: dict, identity: SourceIdentity
) -> None:
    _atomic_write_text(
        workspace / "completion.txt",
        "\n".join(
            [
                f"format={CAMPAIGN_RESULT_FORMAT}",
                f"plan_digest={state['plan_digest']}",
                f"revision={identity.revision}",
                f"dirty={'true' if identity.dirty else 'false'}",
                f"jobs={len(state['jobs'])}",
                "",
            ]
        ),
    )


def _record_attempt_result(
    workspace: Path,
    job: CampaignJob,
    recorded: dict,
    attempt_record: dict,
    result: AttemptResult,
    identity: Optional[SourceIdentity],
) -> tuple[str, Optional[SourceIdentity]]:
    attempt = workspace / attempt_record["path"]
    attempt_record.update(
        {
            "returncode": result.returncode,
            "duration_seconds": result.duration_seconds,
            "termination_sent": result.termination_sent,
            "error": result.error,
        }
    )
    if result.category != "exit_zero":
        status = "timed_out" if result.category == "timed_out" else "failed"
        attempt_record["status"] = status
        attempt_record["category"] = result.category
        recorded["status"] = status
        return status, identity
    try:
        found = validate_job_artifacts(job, attempt)
        if identity is not None and found != identity:
            raise ArtifactError(
                "identity_mismatch",
                "job source identity differs from campaign",
            )
        identity = found if identity is None else identity
        _atomic_write_json(
            attempt / "validation.json",
            {
                "format": ATTEMPT_RESULT_FORMAT,
                "category": "passed",
                "revision": found.revision,
                "dirty": found.dirty,
            },
        )
        attempt_record["status"] = "passed"
        attempt_record["category"] = "passed"
        recorded["status"] = "passed"
        return "passed", identity
    except ArtifactError as error:
        _atomic_write_json(
            attempt / "validation.json",
            {
                "format": ATTEMPT_RESULT_FORMAT,
                "category": error.category,
                "error": str(error),
            },
        )
        attempt_record["status"] = "failed"
        attempt_record["category"] = error.category
        attempt_record["error"] = str(error)
        recorded["status"] = "failed"
        return "failed", identity


def run_campaign(
    config: CampaignConfig,
    *,
    plan: Optional[CampaignPlan] = None,
    working_directory: Optional[Path] = None,
    monotonic=time.monotonic,
) -> CampaignSummary:
    selected_plan = build_plan(config) if plan is None else plan
    _validate_plan_commands(selected_plan)
    workspace = config.workspace.resolve(strict=False)
    state = (
        _load_resume_workspace(workspace, selected_plan)
        if config.resume
        else _prepare_new_workspace(workspace, selected_plan)
    )
    identity = _revalidate_passed_jobs(workspace, selected_plan, state)
    recorded_identity = _identity_from_state(state)
    if recorded_identity is not None and identity is not None:
        if recorded_identity != identity:
            raise ValueError("recorded campaign identity is inconsistent")
    if identity is None:
        identity = recorded_identity
    state["identity"] = _identity_document(identity)
    _publish_state(workspace, state)

    if all(job["status"] == "passed" for job in state["jobs"]):
        if identity is None:
            raise RuntimeError("completed campaign has no source identity")
        state["status"] = "completed"
        _write_campaign_completion(workspace, state, identity)
        _publish_state(workspace, state)
        return _publish_summary(workspace, state)

    invocation_start = monotonic()
    deadline = (
        None
        if config.max_duration_seconds == 0
        else invocation_start + config.max_duration_seconds
    )
    state["status"] = "running"
    _publish_state(workspace, state)
    _append_event(workspace, {"event": "invocation_started"})

    for job, recorded in zip(selected_plan.jobs, state["jobs"]):
        if recorded["status"] == "passed":
            continue
        now = monotonic()
        if deadline is not None and now >= deadline:
            state["status"] = "budget_exhausted"
            _append_event(
                workspace,
                {"event": "budget_exhausted", "next_job": job.job_id},
            )
            _publish_state(workspace, state)
            return _publish_summary(workspace, state)

        number = len(recorded["attempts"]) + 1
        attempt = _attempt_path(workspace, job, number)
        attempt_record = {
            "number": number,
            "path": _relative_path(attempt, workspace),
            "status": "running",
            "category": "running",
        }
        recorded["attempts"].append(attempt_record)
        recorded["status"] = "running"
        _publish_state(workspace, state)
        _append_event(
            workspace,
            {
                "event": "job_started",
                "job_id": job.job_id,
                "attempt": number,
            },
        )
        timeout = config.job_timeout_seconds
        if deadline is not None:
            timeout = min(timeout, max(deadline - now, 1e-6))
        try:
            result = execute_attempt(
                job,
                attempt,
                timeout,
                working_directory=working_directory,
            )
        except KeyboardInterrupt:
            published = read_json(attempt / "result.json")
            attempt_record.update(
                {
                    "status": "interrupted",
                    "category": "interrupted",
                    "returncode": published.get("returncode"),
                    "duration_seconds": published.get("duration_seconds"),
                    "termination_sent": published.get("termination_sent"),
                    "error": published.get("error"),
                }
            )
            recorded["status"] = "interrupted"
            state["status"] = "interrupted"
            _publish_state(workspace, state)
            _publish_summary(workspace, state)
            _append_event(
                workspace,
                {
                    "event": "job_interrupted",
                    "job_id": job.job_id,
                    "attempt": number,
                },
            )
            raise

        status, identity = _record_attempt_result(
            workspace,
            job,
            recorded,
            attempt_record,
            result,
            identity,
        )
        state["identity"] = _identity_document(identity)
        _publish_state(workspace, state)
        _append_event(
            workspace,
            {
                "event": "job_finished",
                "job_id": job.job_id,
                "attempt": number,
                "status": status,
                "category": attempt_record["category"],
            },
        )
        if status != "passed":
            state["status"] = "failed"
            _publish_state(workspace, state)
            return _publish_summary(workspace, state)

    if identity is None:
        raise RuntimeError("successful campaign has no source identity")
    state["status"] = "completed"
    state["identity"] = _identity_document(identity)
    _write_campaign_completion(workspace, state, identity)
    _append_event(workspace, {"event": "campaign_completed"})
    _publish_state(workspace, state)
    return _publish_summary(workspace, state)


def main(arguments: Sequence[str]) -> int:
    try:
        config = parse_arguments(arguments)
        summary = run_campaign(config)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    except Exception as error:
        print(error, file=sys.stderr)
        return 1

    print(
        " ".join(
            [
                f"status={summary.status}",
                f"total={summary.total}",
                f"passed={summary.passed}",
                f"failed={summary.failed}",
                f"pending={summary.pending}",
                f"revision={summary.revision or ''}",
                (
                    "dirty="
                    if summary.dirty is None
                    else f"dirty={'true' if summary.dirty else 'false'}"
                ),
                f"blocking_job={summary.blocking_job or ''}",
            ]
        )
    )
    if summary.status == "completed":
        return 0
    if summary.status == "budget_exhausted":
        return 3
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
