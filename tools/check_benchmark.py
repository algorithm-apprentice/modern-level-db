#!/usr/bin/env python3
"""Validate the benchmark schema and the ADR-0041 severe-regression threshold."""

import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys

MAX_SLOWDOWN = 20.0
PHASES = {"write", "read", "scan"}


def check_report(report):
    if not isinstance(report, dict) or set(report) != {
        "schema_version", "entries", "trials", "samples"
    }:
        raise ValueError("benchmark report has an invalid schema")
    if type(report["schema_version"]) is not int or report["schema_version"] != 1:
        raise ValueError("unsupported benchmark schema version")
    entries, trials = report["entries"], report["trials"]
    if type(entries) is not int or not 1 <= entries <= 1_000_000:
        raise ValueError("invalid benchmark entry count")
    if type(trials) is not int or not 3 <= trials <= 31 or trials % 2 != 1:
        raise ValueError("invalid benchmark trial count")
    samples = report["samples"]
    if not isinstance(samples, dict) or set(samples) != {"modern", "leveldb"}:
        raise ValueError("benchmark must report both implementations")
    medians = {}
    for engine, phases in samples.items():
        if not isinstance(phases, dict) or set(phases) != PHASES:
            raise ValueError(f"{engine}: missing or extra benchmark phases")
        medians[engine] = {}
        for phase, values in phases.items():
            if not isinstance(values, list) or len(values) != trials:
                raise ValueError(f"{engine}/{phase}: incorrect sample count")
            for value in values:
                if (type(value) not in (int, float)
                        or not math.isfinite(value) or value <= 0):
                    raise ValueError(f"{engine}/{phase}: samples must be positive finite numbers")
            medians[engine][phase] = statistics.median(values)
    ratios = {
        phase: medians["modern"][phase] / medians["leveldb"][phase]
        for phase in sorted(PHASES)
    }
    for phase, ratio in ratios.items():
        if not math.isfinite(ratio) or ratio > MAX_SLOWDOWN:
            raise ValueError(f"{phase}: slowdown {ratio:.3f}x exceeds {MAX_SLOWDOWN:.0f}x")
    return ratios


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--run", type=Path, help="Run this benchmark before checking its report")
    args = parser.parse_args()
    try:
        if args.run is not None:
            result = subprocess.run(
                [str(args.run.resolve())],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
                timeout=180,
            )
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(result.stdout, encoding="utf-8")
        report = json.loads(args.report.read_text(encoding="utf-8"))
        print(json.dumps({"median_slowdown": check_report(report)}, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"benchmark gate failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
