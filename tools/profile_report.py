"""Owned-process collection plus typed macOS and native Windows profile support."""

from collections import Counter
from dataclasses import dataclass
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

SUBSYSTEM = "modern_leveldb.profiling"


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    parent: int
    group: int
    started: str
    executable: Path


class TargetDiscoveryError(RuntimeError):
    def __init__(self, message, targets=(), complete=False):
        super().__init__(message)
        self.targets = tuple(targets)
        self.complete = complete


def process_identity(pid):
    result = subprocess.run(
        ["ps", "-p", str(pid), "-o", "ppid=,pgid=,stat=,lstart=,comm="],
        capture_output=True, text=True, timeout=3,
    )
    if result.returncode == 1 and not result.stdout.strip():
        return None
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect owned process {pid}: {result.stderr.strip()}")
    fields = result.stdout.strip().split(None, 8)
    if len(fields) != 9:
        raise RuntimeError(f"cannot identify owned process {pid}")
    if fields[2].startswith("Z"):
        return None
    executable = Path(fields[8])
    if sys.platform.startswith("linux"):
        try:
            executable = Path(os.readlink(f"/proc/{pid}/exe"))
        except FileNotFoundError:
            return None
    return ProcessIdentity(pid, int(fields[0]), int(fields[1]), " ".join(fields[3:8]),
                           executable.resolve())


def same_process(identity):
    current = process_identity(identity.pid)
    return (current is not None and current.started == identity.started
            and current.executable == identity.executable)


def find_target(parent, executable):
    matches = []
    try:
        result = subprocess.run(
            ["pgrep", "-P", str(parent)], capture_output=True, text=True, timeout=3
        )
        if result.returncode not in (0, 1):
            raise RuntimeError(f"cannot inspect collector children: {result.stderr.strip()}")
        for value in result.stdout.split():
            identity = process_identity(int(value))
            if (identity is not None and identity.parent == parent
                    and identity.executable == executable):
                matches.append(identity)
        if len(matches) > 1:
            raise TargetDiscoveryError(
                "collector launched more than one matching workload", matches, complete=True
            )
        return matches[0] if matches else None
    except TargetDiscoveryError:
        raise
    except Exception as error:
        raise TargetDiscoveryError(str(error), matches) from error


def signal_target(identity, sig):
    if identity is not None and same_process(identity):
        try:
            os.kill(identity.pid, sig)
        except ProcessLookupError:
            pass


def remember_targets(targets, discovered):
    for target in discovered:
        if target not in targets:
            targets.append(target)


def stop_owned(process, targets, target_executable, discovery_complete, grace=5):
    # xctrace's launched workload is a direct child in a different process group.
    targets = list(targets)
    if os.name == "nt":
        if target_executable is not None:
            return targets, False, RuntimeError(
                "Windows target discovery is owned by the native collector"
            )
        try:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=grace)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=grace)
        except Exception as error:
            return targets, False, error
        return targets, process.poll() is not None, None
    cleanup_error = None
    target_checks_complete = True

    def remember_error(error):
        nonlocal cleanup_error
        if cleanup_error is None:
            cleanup_error = error

    def discover_target():
        nonlocal discovery_complete
        try:
            target = find_target(process.pid, target_executable)
        except TargetDiscoveryError as error:
            remember_targets(targets, error.targets)
            discovery_complete = error.complete
            remember_error(error)
        except Exception as error:
            discovery_complete = False
            remember_error(error)
        else:
            discovery_complete = True
            if target is not None:
                remember_targets(targets, [target])

    def signal_targets(sig):
        nonlocal target_checks_complete
        for target in targets:
            try:
                signal_target(target, sig)
            except Exception as error:
                target_checks_complete = False
                remember_error(error)

    def signal_collector(sig):
        if process.poll() is None:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            except Exception as error:
                remember_error(error)

    if (target_executable is not None and process.poll() is None
            and (not targets or not discovery_complete)):
        discover_target()

    signal_targets(signal.SIGTERM)
    if target_executable is None:
        signal_collector(signal.SIGTERM)
    elif not targets:
        signal_collector(signal.SIGINT)

    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        signal_targets(signal.SIGKILL)
        signal_collector(signal.SIGINT)
        try:
            process.wait(timeout=grace * 2)
        except subprocess.TimeoutExpired:
            signal_targets(signal.SIGKILL)
            signal_collector(signal.SIGKILL)
            process.wait(timeout=grace)

    targets_stopped = True
    for target in targets:
        try:
            alive = same_process(target)
        except Exception as error:
            target_checks_complete = False
            targets_stopped = False
            remember_error(error)
            continue
        if not alive:
            continue
        try:
            signal_target(target, signal.SIGKILL)
            deadline = time.monotonic() + grace
            while same_process(target) and time.monotonic() < deadline:
                time.sleep(0.05)
            alive = same_process(target)
        except Exception as error:
            target_checks_complete = False
            targets_stopped = False
            remember_error(error)
            continue
        if alive:
            targets_stopped = False
            remember_error(
                RuntimeError("workload termination is unverified; preserve the scratch directory")
            )

    cleanup_verified = (
        target_executable is None
        or (bool(targets) and discovery_complete and target_checks_complete and targets_stopped)
    )
    return targets, cleanup_verified, cleanup_error


def run_owned(command, log, timeout, journal, target_executable=None, grace=5):
    """Run only owned processes; record status and verify cleanup before returning."""
    targets = []
    target_executable = Path(target_executable).resolve() if target_executable else None
    discovery_complete = target_executable is None
    cleanup_error = None
    record = {"argv": [str(arg) for arg in command], "log": Path(log).name, "returncode": None,
              "cleanup_verified": True, "timed_out": False}
    journal.append(record)
    with Path(log).open("wb") as output:
        process = subprocess.Popen(
            record["argv"], stdout=output, stderr=subprocess.STDOUT, start_new_session=True
        )
        record.update(pid=process.pid, cleanup_verified=False)
        try:
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                if target_executable is not None and not targets:
                    try:
                        target = find_target(process.pid, target_executable)
                    except TargetDiscoveryError as error:
                        remember_targets(targets, error.targets)
                        discovery_complete = error.complete
                        raise
                    except Exception:
                        discovery_complete = False
                        raise
                    discovery_complete = True
                    if target is not None:
                        remember_targets(targets, [target])
                        record["target_pid"] = target.pid
                if time.monotonic() >= deadline:
                    record["timed_out"] = True
                    raise subprocess.TimeoutExpired(record["argv"], timeout)
                time.sleep(0.05)
            if any(same_process(target) for target in targets):
                raise RuntimeError("collector exited before its workload")
        finally:
            if process.poll() is None or targets:
                targets, cleanup_verified, cleanup_error = stop_owned(
                    process, targets, target_executable, discovery_complete, grace
                )
            else:
                cleanup_verified = target_executable is None
            if len(targets) == 1:
                record["target_pid"] = targets[0].pid
            record["returncode"] = process.wait(timeout=grace)
            record["cleanup_verified"] = cleanup_verified
    if cleanup_error is not None:
        raise cleanup_error
    if not record["cleanup_verified"]:
        raise RuntimeError("workload identity or termination is unverified; preserve scratch")
    if record["returncode"] != 0:
        raise subprocess.CalledProcessError(record["returncode"], record["argv"])
    return record


class Export:
    def __init__(self, path):
        self.root = ET.parse(path).getroot()
        self.ids = {}
        for element in self.root.iter():
            identifier = element.get("id")
            if identifier is not None:
                if identifier in self.ids:
                    raise ValueError("duplicate profiler XML identifier")
                self.ids[identifier] = element

    def resolve(self, element):
        if element is None:
            raise ValueError("missing profiler XML field")
        visited = set()
        while element.get("ref") is not None:
            reference = element.get("ref")
            if reference in visited or reference not in self.ids:
                raise ValueError("invalid profiler XML reference")
            visited.add(reference)
            element = self.ids[reference]
        return element

    def text(self, element):
        text = self.resolve(element).text
        if text is None:
            raise ValueError("missing raw profiler XML value")
        return text

    def integer(self, element):
        text = self.text(element)
        if not text.isdigit():
            raise ValueError("invalid raw profiler integer")
        return int(text)

    def child(self, element, name):
        return self.resolve(element).find(name)


def target_status(toc):
    root = ET.parse(toc).getroot()
    runs = root.findall("run")
    if len(runs) != 1:
        raise ValueError("a CPU capture must contain exactly one run")
    process = runs[0].find("./info/target/process")
    if process is None or process.get("type") != "launched":
        raise ValueError("capture does not describe the launched target")
    if process.get("return-exit-status") != "0" or process.get("termination-reason") != "exit(0)":
        raise ValueError("profile target failed or was terminated")
    pid = process.get("pid", "")
    if not pid.isdigit() or int(pid) <= 0:
        raise ValueError("invalid profile target PID")
    return int(pid)


def measured_window(markers, pid, case, iterations):
    export = Export(markers)
    events = {}
    for row in export.root.findall(".//row"):
        if row.find("subsystem") is None:
            continue
        if export.text(row.find("subsystem")) != SUBSYSTEM:
            continue
        if export.text(row.find("signpost-name")) != "workload":
            continue
        row_pid = export.integer(export.child(row.find("process"), "pid"))
        if row_pid != pid:
            continue
        metadata = export.resolve(row.find("os-log-metadata"))
        strings = metadata.findall("string")
        integers = metadata.findall("uint64")
        if len(strings) != 1 or len(integers) != 1:
            raise ValueError("unexpected workload marker fields")
        marker_case = export.text(strings[0])
        if marker_case != case:
            raise ValueError("capture contains a different workload case")
        identifier = export.integer(row.find("os-signpost-identifier"))
        kind = export.text(row.find("event-type"))
        if kind not in ("Begin", "End"):
            raise ValueError("unexpected workload marker event")
        value = (export.integer(row.find("event-time")), export.integer(integers[0]),
                 export.integer(export.child(row.find("thread"), "tid")))
        key = (identifier, kind)
        if key in events and events[key] != value:
            raise ValueError("conflicting duplicate workload markers")
        events[key] = value
    identifiers = {key[0] for key in events}
    if not identifiers:
        raise ValueError("no workload measurement markers")
    intervals = []
    for identifier in identifiers:
        if (identifier, "Begin") not in events or (identifier, "End") not in events:
            raise ValueError("unpaired workload markers")
        start, planned, thread = events[(identifier, "Begin")]
        end, completed, end_thread = events[(identifier, "End")]
        if start >= end or planned != completed or thread != end_thread or completed <= 0:
            raise ValueError("inconsistent workload interval")
        intervals.append((start, end, completed, thread))
    intervals.sort(key=lambda interval: interval[0])
    for previous, current in zip(intervals, intervals[1:]):
        if previous[1] > current[0]:
            raise ValueError("overlapping workload intervals")
    start, end, completed, thread = intervals[-1]
    if completed != iterations:
        raise ValueError("final measured interval does not match benchmark iterations")
    return {"start_ns": start, "end_ns": end, "iterations": completed,
            "foreground_thread": thread, "calibration_intervals": len(intervals) - 1}


def summarize_trace(toc, markers, samples, case, iterations):
    pid = target_status(toc)
    window = measured_window(markers, pid, case, iterations)
    export = Export(samples)
    self_weights = Counter()
    inclusive_weights = Counter()
    threads = {}
    total_weight = 0
    sample_count = 0
    unresolved = 0
    seen = set()
    workload_symbols = {
        "readrandom": "RunReadRandom", "readmissing": "RunReadMissing",
        "scan": "RunScan", "seek_reuse": "RunSeekReuse",
        "overwrite": "RunOverwrite", "writebatch": "RunWriteBatch",
        "writesync": "RunWriteSync", "mixed50": "RunMixed50",
    }
    parts = case.split("/")
    if len(parts) != 3 or parts[1] not in workload_symbols:
        raise ValueError("unknown profile workload")
    workload_symbol = workload_symbols[parts[1]]
    mutable = parts[1] in ("overwrite", "writebatch", "writesync", "mixed50")
    if mutable and window["calibration_intervals"] != 0:
        raise ValueError("mutable CPU captures cannot contain calibration or warmup intervals")
    background = [{"compaction_samples": 0, "flush_samples": 0} for _ in range(4)]
    activity_symbols = {
        "modern": ("modern_leveldb::RunCompaction(", "modern_leveldb::FlushMemTable("),
        "leveldb": ("leveldb::DBImpl::DoCompactionWork(", "leveldb::DBImpl::CompactMemTable("),
    }
    if parts[0] not in activity_symbols:
        raise ValueError("unknown profile engine")
    resolved_workload = False
    for row in export.root.findall(".//row"):
        timestamp = export.integer(row.find("sample-time"))
        if not window["start_ns"] <= timestamp < window["end_ns"]:
            continue
        if export.integer(export.child(row.find("process"), "pid")) != pid:
            continue
        thread = export.integer(export.child(row.find("thread"), "tid"))
        weight = export.integer(row.find("weight"))
        if weight <= 0:
            raise ValueError("nonpositive CPU sample weight")
        stack = []
        trace = export.resolve(row.find("tagged-backtrace"))
        for entry in trace.findall("frame"):
            frame = export.resolve(entry)
            name = frame.get("name", "")
            binary = frame.find("binary")
            module = Path(export.resolve(binary).get("name", "unknown")).name if binary is not None else "unknown"
            stack.append((module, name or "<unresolved>"))
        if not stack:
            stack.append(("unknown", "<unresolved>"))
        identity = (timestamp, thread, weight, tuple(stack))
        if identity in seen:
            continue
        seen.add(identity)
        if thread == window["foreground_thread"]:
            resolved_workload = resolved_workload or any(workload_symbol in name for _, name in stack)
        elif mutable:
            quarter = (timestamp - window["start_ns"]) * 4 // (window["end_ns"] - window["start_ns"])
            for category, symbol in zip(("compaction_samples", "flush_samples"), activity_symbols[parts[0]]):
                background[quarter][category] += any(symbol in name for _, name in stack)
        sample_count += 1
        total_weight += weight
        unresolved += any(name.startswith("0x") or name == "<unresolved>" for _, name in stack)
        self_weights[stack[0]] += weight
        for symbol in set(stack):
            inclusive_weights[symbol] += weight
        counter = threads.setdefault(thread, {"samples": 0, "sample_weight_ns": 0})
        counter["samples"] += 1
        counter["sample_weight_ns"] += weight
    if sample_count == 0:
        raise ValueError("no CPU samples inside the verified measurement window")
    if not resolved_workload:
        raise ValueError("no symbolized foreground workload samples; check debug symbols or capture duration")

    def top(counter):
        return [
            {"module": module, "symbol": name, "sample_weight_ns": weight,
             "percent": weight * 100.0 / total_weight}
            for (module, name), weight in counter.most_common(50)
        ]

    result = {
        "schema_version": 1,
        "case": case,
        "measurement": "sampled_cpu_not_operation_latency",
        "window": window,
        "sample_count": sample_count,
        "sample_weight_ns": total_weight,
        "samples_with_unresolved_frames": unresolved,
        "low_confidence": sample_count < 100,
        "resolved_workload_symbol": workload_symbol,
        "threads": [
            {"tid": thread, "role": "foreground" if thread == window["foreground_thread"] else "background",
             **counts}
            for thread, counts in sorted(threads.items())
        ],
        "self": top(self_weights),
        "inclusive": top(inclusive_weights),
        "inclusive_percentages_overlap": True,
    }
    if mutable:
        result["background_activity"] = {
            "measurement": "sampled_stacks_not_completed_jobs", "quarters": background,
            "sustained_compaction_observed": all(part["compaction_samples"] > 0 for part in background),
            "steady_state_proven": False,
        }
    return result


def collector_version(path):
    text = Path(path).read_text(encoding="utf-8").strip()
    match = re.fullmatch(r"xctrace version ([^\s]+) \(([^()\r\n]+)\)", text)
    if match is None:
        raise ValueError("cannot identify the xctrace version; see collector-version.log")
    return {"name": "xctrace", "version": match.group(1), "build": match.group(2)}


def capture(binary, arguments, output, journal, timeout=180):
    if sys.platform != "darwin":
        raise ValueError("automated CPU capture currently requires macOS Xcode Time Profiler")
    output = Path(output)
    run_owned(["xcrun", "xctrace", "version"], output / "collector-version.log", 15, journal)
    collector_version(output / "collector-version.log")
    run_owned(["dsymutil", str(binary)], output / "symbols.log", 60, journal)
    command = [
        "xcrun", "xctrace", "record", "--template", "Time Profiler",
        "--time-limit", "120s", "--output", str(output / "profile.trace"),
        "--no-prompt", "--target-stdout", str(output / "target.log"),
        "--launch", "--", str(binary), *arguments,
    ]
    run_owned(command, output / "collector.log", timeout, journal, target_executable=binary)
    exports = {
        "toc.xml": ["--toc"],
        "markers.xml": ["--xpath", '/trace-toc/run[@number="1"]/data/table[@schema="os-signpost"]'],
        "samples.xml": ["--xpath", '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'],
    }
    for filename, selection in exports.items():
        run_owned(
            ["xcrun", "xctrace", "export", "--input", str(output / "profile.trace"),
             *selection, "--output", str(output / filename)],
            output / f"{filename}.log", 120, journal,
        )
