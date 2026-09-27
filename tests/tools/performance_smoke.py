"""Run all selected-case contracts without installing a throughput threshold."""

import argparse
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_performance import CASES, MUTATION_CASES, run_case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--full-mutations", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # Repeated CTest invocations preserve older reports rather than replacing them.
    root = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    passed = []
    for case in MUTATION_CASES if args.full_mutations else CASES:
        result = run_case(args.binary, case, root / case.replace("/", "-"),
                          smoke=not args.full_mutations)
        passed.append(result["case"])
        print(f"passed {case}", flush=True)
    (root / "summary.json").write_text(json.dumps({"cases": passed}, indent=2) + "\n")
    print(f"{len(passed)} workload cases completed; artifacts: {root}")


if __name__ == "__main__":
    main()
