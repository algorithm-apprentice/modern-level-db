"""Run and validate one fixed-count point-read diagnostic capture."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_performance import COPIED_FILE_ACCESS, run_read_diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    rejected = subprocess.run(
        [
            str(args.binary),
            "--case", "leveldb/readrandom/4096",
            "--database", str(root / "rejected-db"),
            "--diagnostic-report", str(root / "rejected.json"),
        ],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if rejected.returncode == 0 or (root / "rejected-db").exists():
        raise RuntimeError("diagnostic executable accepted a reference-engine case")
    cases = (
        ("modern/readrandom/4096", "default"),
        ("modern/readmissing/65536", COPIED_FILE_ACCESS),
    )
    for case, file_access in cases:
        output = root / f"{case.replace('/', '-')}-{file_access}"
        result = run_read_diagnostics(
            args.binary, case, output, modern_file_access=file_access
        )
        print(
            f"{result['diagnostics']['operations']} diagnostic Gets completed for {case}; "
            f"artifacts: {output}"
        )


if __name__ == "__main__":
    main()
