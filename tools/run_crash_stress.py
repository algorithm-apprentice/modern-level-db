#!/usr/bin/env python3
"""Run local process-crash stress with an external acknowledgement journal."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
from typing import Callable, Optional


JOURNAL_FORMAT = "format=modern-leveldb-crash-journal-v1\n"
RUN_FORMAT = "modern-leveldb-crash-run-v1"
RESULT_FORMAT = "modern-leveldb-crash-result-v1"


@dataclass
class Config:
    worker_command: list[str]
    database: Path
    output: Path
    rounds: int = 100
    seed: int = 301
    compression: str = "none"
    timeout_seconds: float = 30.0
    inflight_probes: int = 3


@dataclass
class HoldingResult:
    returncode: int
    trailing_stdout: str
    stderr: str
    terminated_by_parent: bool


@dataclass
class Summary:
    acknowledged: int
    inflight_absent: int
    inflight_complete: int


def _resolved(path: Path) -> Path:
    return path.resolve(strict=False)


def _nested(left: Path, right: Path) -> bool:
    return left == right or left.is_relative_to(right) or right.is_relative_to(left)


def validate_paths(config: Config) -> tuple[Path, Path]:
    if not config.worker_command:
        raise ValueError("worker command is empty")
    database = _resolved(config.database)
    output = _resolved(config.output)
    if _nested(database, output):
        raise ValueError("database and output paths must be distinct and non-nested")
    for name, path in (("database", database), ("output", output)):
        if path.exists():
            raise ValueError(f"{name} path already exists")
        if not path.parent.is_dir():
            raise ValueError(f"{name} parent directory does not exist")
    if config.rounds < 1:
        raise ValueError("rounds must be positive")
    if config.seed < 0 or config.seed > (1 << 64) - 1:
        raise ValueError("seed must fit uint64")
    if config.compression not in {"none", "snappy", "zstd"}:
        raise ValueError("unknown compression")
    if config.timeout_seconds <= 0:
        raise ValueError("timeout must be positive")
    if config.inflight_probes < 0:
        raise ValueError("inflight probes cannot be negative")
    return database, output


def _write_fsync(path: Path, text: str, mode: str = "w") -> None:
    with path.open(mode, encoding="utf-8", newline="\n") as output:
        output.write(text)
        output.flush()
        os.fsync(output.fileno())


def create_journal(path: Path) -> None:
    _write_fsync(path, JOURNAL_FORMAT)


def _journal_count(path: Path) -> int:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != JOURNAL_FORMAT.rstrip("\n"):
        raise RuntimeError("journal format is invalid")
    for expected, line in enumerate(lines[1:]):
        if line != f"ack\t{expected}":
            raise RuntimeError("journal is not a contiguous acknowledged prefix")
    return len(lines) - 1


def append_acknowledgement(path: Path, index: int) -> None:
    if _journal_count(path) != index:
        raise RuntimeError("journal acknowledgement is not contiguous")
    _write_fsync(path, f"ack\t{index}\n", mode="a")


def _read_one_line(pipe, result: queue.Queue) -> None:
    try:
        result.put(("line", pipe.readline()))
    except BaseException as error:
        result.put(("error", error))


def _read_and_close_streams(process: subprocess.Popen) -> tuple[str, str]:
    assert process.stdout is not None
    assert process.stderr is not None
    try:
        return process.stdout.read(), process.stderr.read()
    finally:
        process.stdout.close()
        process.stderr.close()


def _kill_and_collect(process: subprocess.Popen, reader: threading.Thread) -> HoldingResult:
    terminated_by_parent = process.poll() is None
    if terminated_by_parent:
        process.kill()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("owned child could not be reaped") from error
    reader.join(timeout=5)
    if reader.is_alive():
        raise RuntimeError("owned child protocol reader did not finish")
    trailing_stdout, stderr = _read_and_close_streams(process)
    return HoldingResult(
        process.returncode, trailing_stdout, stderr, terminated_by_parent
    )


def run_holding_child(
    command: list[str],
    expected_record: str,
    timeout_seconds: float,
    after_record: Optional[Callable[[], None]] = None,
) -> HoldingResult:
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
    )
    assert process.stdout is not None
    result: queue.Queue = queue.Queue(maxsize=1)
    reader = threading.Thread(target=_read_one_line, args=(process.stdout, result), daemon=True)
    reader.start()
    try:
        try:
            kind, payload = result.get(timeout=timeout_seconds)
        except queue.Empty as error:
            collected = _kill_and_collect(process, reader)
            raise RuntimeError(
                f"child protocol timed out; stderr={collected.stderr!r}"
            ) from error
        if kind == "error":
            collected = _kill_and_collect(process, reader)
            raise RuntimeError(
                f"child protocol read failed; stderr={collected.stderr!r}"
            ) from payload
        record = payload.removesuffix("\n")
        if record != expected_record:
            collected = _kill_and_collect(process, reader)
            raise RuntimeError(
                f"child protocol mismatch: expected {expected_record!r}, got {record!r}; "
                f"stderr={collected.stderr!r}"
            )
        try:
            process.wait(timeout=0.05)
        except subprocess.TimeoutExpired:
            pass
        else:
            reader.join(timeout=5)
            trailing_stdout, stderr = _read_and_close_streams(process)
            raise RuntimeError(
                "child exited before termination; "
                f"code={process.returncode} trailing={trailing_stdout!r} stderr={stderr!r}"
            )
        if after_record is not None:
            after_record()
        collected = _kill_and_collect(process, reader)
        if collected.trailing_stdout:
            raise RuntimeError(
                f"child emitted trailing stdout: {collected.trailing_stdout!r}"
            )
        if collected.stderr:
            raise RuntimeError(f"child emitted stderr: {collected.stderr!r}")
        if not collected.terminated_by_parent:
            raise RuntimeError(
                f"child exited before parent termination with code {collected.returncode}"
            )
        if collected.returncode == 0:
            raise RuntimeError("killed child reported successful exit")
        return collected
    except BaseException:
        if process.poll() is None:
            process.kill()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        raise


def run_completed_child(command: list[str], expected_record: str,
                        timeout_seconds: float) -> None:
    completed = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="strict",
        timeout=timeout_seconds,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"worker failed with code {completed.returncode}: {completed.stderr!r}"
        )
    if completed.stderr:
        raise RuntimeError(f"worker emitted stderr: {completed.stderr!r}")
    if completed.stdout != expected_record + "\n":
        raise RuntimeError(
            f"worker protocol mismatch: expected {expected_record!r}, "
            f"got {completed.stdout!r}"
        )


def _worker_command(config: Config, mode: str, database: Path,
                    *, index: Optional[int] = None,
                    journal: Optional[Path] = None) -> list[str]:
    command = list(config.worker_command)
    command.extend(
        [
            "--mode",
            mode,
            "--db",
            str(database),
            "--seed",
            str(config.seed),
            "--compression",
            config.compression,
        ]
    )
    if index is not None:
        command.extend(["--index", str(index)])
    if journal is not None:
        command.extend(["--journal", str(journal)])
    return command


def _identity(config: Config) -> tuple[str, str]:
    completed = subprocess.run(
        [*config.worker_command, "--mode", "identity"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="strict",
        timeout=config.timeout_seconds,
        check=False,
    )
    if completed.returncode != 0 or completed.stderr:
        raise RuntimeError(
            f"worker identity failed: code={completed.returncode} stderr={completed.stderr!r}"
        )
    fields = completed.stdout.removesuffix("\n").split("\t")
    if len(fields) != 3 or fields[0] != "identity" or fields[2] not in {"clean", "dirty"}:
        raise RuntimeError("worker identity protocol is malformed")
    return fields[1], fields[2]


def _append_process_log(path: Path, record: str) -> None:
    _write_fsync(path, record + "\n", mode="a")


def run_campaign(config: Config) -> Summary:
    database, output = validate_paths(config)
    output.mkdir()
    process_log = output / "process.log"
    process_log.touch()
    try:
        revision, dirty = _identity(config)
        _write_fsync(
            output / "run.txt",
            "\n".join(
                [
                    f"format={RUN_FORMAT}",
                    f"revision={revision}",
                    f"dirty={dirty}",
                    f"seed={config.seed}",
                    f"rounds={config.rounds}",
                    f"compression={config.compression}",
                    f"timeout_seconds={config.timeout_seconds}",
                    f"inflight_probes={config.inflight_probes}",
                    "",
                ]
            ),
        )
        journal = output / "acknowledged.log"
        create_journal(journal)
        for index in range(config.rounds):
            _append_process_log(process_log, f"launch\tack\t{index}")
            def persist_acknowledgement(selected=index):
                append_acknowledgement(journal, selected)
                _append_process_log(process_log, f"journal\tack\t{selected}")

            run_holding_child(
                _worker_command(config, "write-hold", database, index=index),
                f"ack\t{index}",
                config.timeout_seconds,
                persist_acknowledgement,
            )
            _append_process_log(process_log, f"kill\tack\t{index}")
            run_completed_child(
                _worker_command(config, "verify-prefix", database, journal=journal),
                f"verified\t{index + 1}",
                config.timeout_seconds,
            )
            _append_process_log(process_log, f"verify\tprefix\t{index + 1}")

        inflight_root = output / "inflight"
        inflight_root.mkdir()
        absent = 0
        complete = 0
        inflight_records = [f"format={RESULT_FORMAT}"]
        for probe in range(config.inflight_probes):
            probe_root = inflight_root / str(probe)
            probe_root.mkdir()
            probe_database = probe_root / "db"
            run_completed_child(
                _worker_command(config, "initialize", probe_database),
                "initialized",
                config.timeout_seconds,
            )
            _append_process_log(process_log, f"initialize\tinflight\t{probe}")
            _append_process_log(process_log, f"launch\tinflight\t{probe}")
            run_holding_child(
                _worker_command(config, "inflight-hold", probe_database, index=probe),
                f"ready\t{probe}",
                config.timeout_seconds,
            )
            _append_process_log(process_log, f"kill\tinflight\t{probe}")
            command = _worker_command(
                config, "verify-optional", probe_database, index=probe
            )
            completed = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="strict",
                timeout=config.timeout_seconds,
                check=False,
            )
            if completed.returncode != 0 or completed.stderr:
                raise RuntimeError(
                    f"optional verifier failed: code={completed.returncode} "
                    f"stderr={completed.stderr!r}"
                )
            record = completed.stdout.removesuffix("\n")
            if record == "optional\tabsent":
                absent += 1
                state = "absent"
            elif record == "optional\tcomplete":
                complete += 1
                state = "complete"
            else:
                raise RuntimeError(f"optional verifier protocol mismatch: {record!r}")
            inflight_records.append(f"probe\t{probe}\t{state}")
            _append_process_log(process_log, f"verify\toptional\t{state}")
        _write_fsync(output / "inflight.txt", "\n".join(inflight_records) + "\n")
        summary = Summary(config.rounds, absent, complete)
        _write_fsync(
            output / "completion.txt",
            "\n".join(
                [
                    f"format={RESULT_FORMAT}",
                    f"acknowledged={summary.acknowledged}",
                    f"inflight_absent={summary.inflight_absent}",
                    f"inflight_complete={summary.inflight_complete}",
                    "",
                ]
            ),
        )
        return summary
    except BaseException as error:
        _write_fsync(
            output / "failure.txt",
            f"format={RESULT_FORMAT}\nmessage={str(error).replace(chr(10), ' ')}\n",
        )
        raise


def parse_args(argv: Optional[list[str]] = None) -> Config:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--seed", type=int, default=301)
    parser.add_argument("--compression", choices=("none", "snappy", "zstd"), default="none")
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--inflight-probes", type=int, default=3)
    args = parser.parse_args(argv)
    return Config(
        worker_command=[str(args.worker.resolve())],
        database=args.db,
        output=args.output,
        rounds=args.rounds,
        seed=args.seed,
        compression=args.compression,
        timeout_seconds=args.timeout_seconds,
        inflight_probes=args.inflight_probes,
    )


def main(argv: Optional[list[str]] = None) -> int:
    try:
        summary = run_campaign(parse_args(argv))
        print(
            f"completed acknowledged={summary.acknowledged} "
            f"inflight_absent={summary.inflight_absent} "
            f"inflight_complete={summary.inflight_complete}"
        )
        return 0
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    except BaseException as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
