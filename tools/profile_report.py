"""Owned-process collection and typed macOS Time Profiler report extraction."""

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
    result = subprocess.run(
        ["pgrep", "-P", str(parent)], capture_output=True, text=True, timeout=3
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"cannot inspect collector children: {result.stderr.strip()}")
    matches = []
    for value in result.stdout.split():
        identity = process_identity(int(value))
        if (identity is not None and identity.parent == parent
                and identity.executable == executable):
            matches.append(identity)
    if len(matches) > 1:
        raise RuntimeError("collector launched more than one matching workload")
    return matches[0] if matches else None


def signal_target(identity, sig):
    if identity is not None and same_process(identity):
        try:
            os.kill(identity.pid, sig)
        except ProcessLookupError:
            pass


def stop_owned(process, target, target_executable, grace=5):
    # xctrace's launched workload is a direct child in a different process group.
    if target is None and target_executable is not None and process.poll() is None:
        target = find_target(process.pid, target_executable)
    signal_target(target, signal.SIGTERM)
    if target_executable is None and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        signal_target(target, signal.SIGKILL)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGINT)
        try:
            process.wait(timeout=grace * 2)
        except subprocess.TimeoutExpired:
            signal_target(target, signal.SIGKILL)
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=grace)
    if target is not None and same_process(target):
        signal_target(target, signal.SIGKILL)
        deadline = time.monotonic() + grace
        while same_process(target) and time.monotonic() < deadline:
            time.sleep(0.05)
        if same_process(target):
            raise RuntimeError("workload termination is unverified; preserve the scratch directory")
    return target


def run_owned(command, log, timeout, journal, target_executable=None, grace=5):
    """Run only owned processes; record status and verify cleanup before returning."""
    target = None
    target_executable = Path(target_executable).resolve() if target_executable else None
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
                if target_executable is not None and target is None:
                    target = find_target(process.pid, target_executable)
                    if target is not None:
                        record["target_pid"] = target.pid
                if time.monotonic() >= deadline:
                    record["timed_out"] = True
                    raise subprocess.TimeoutExpired(record["argv"], timeout)
                time.sleep(0.05)
            if target is not None and same_process(target):
                raise RuntimeError("collector exited before its workload")
        finally:
            if process.poll() is None or (target is not None and same_process(target)):
                target = stop_owned(process, target, target_executable, grace)
            if target is not None:
                record["target_pid"] = target.pid
            record["returncode"] = process.wait(timeout=grace)
            record["cleanup_verified"] = (
                target_executable is None or (target is not None and not same_process(target))
            )
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
    }
    parts = case.split("/")
    if len(parts) != 3 or parts[1] not in workload_symbols:
        raise ValueError("unknown profile workload")
    workload_symbol = workload_symbols[parts[1]]
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

    return {
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
