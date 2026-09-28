#!/usr/bin/env python3
"""Compare frozen pread and mmap read-diagnostic reports."""

import argparse
import json
from pathlib import Path
import sys

from run_performance import (
    compare_read_diagnostics,
    read_json,
    validate_read_diagnostics,
    write_json,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        baseline = read_json(args.baseline)
        candidate = validate_read_diagnostics(
            read_json(args.candidate), args.case, modern_file_access="mmap"
        )
        if baseline.get("schema_version") == 2:
            baseline = validate_read_diagnostics(
                baseline, args.case, modern_file_access="default"
            )
        comparison = compare_read_diagnostics(baseline, candidate, args.case)
        if args.output is not None:
            if args.output.exists():
                raise FileExistsError(args.output)
            write_json(args.output, comparison)
    except (OSError, ValueError) as error:
        print(f"read diagnostic comparison failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(comparison, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
