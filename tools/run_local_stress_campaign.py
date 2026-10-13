#!/usr/bin/env python3
"""Plan and run resumable local V1 through V4 stress campaigns."""

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Optional, Sequence


PLAN_FORMAT = "modern-leveldb-stress-plan-v1"
COMMAND_FORMAT = "modern-leveldb-stress-command-v1"
ATTEMPT_RESULT_FORMAT = "modern-leveldb-stress-attempt-result-v1"
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


def main(arguments: Sequence[str]) -> int:
    parse_arguments(arguments)
    raise RuntimeError("campaign execution is not implemented")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ValueError as error:
        print(error, file=sys.stderr)
        sys.exit(2)
