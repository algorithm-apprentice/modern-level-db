#!/usr/bin/env python3
"""Run a deterministic libFuzzer smoke campaign and preserve its artifacts."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--kind", choices=("formats", "database"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=1000)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    corpus = args.output / "corpus"
    artifacts = args.output / "artifacts"
    corpus.mkdir(parents=True, exist_ok=True)
    artifacts.mkdir(parents=True, exist_ok=True)
    seeds = {
        "formats": {
            "manifest": "00020103020400",
            "batch": "01" + "00" * 12,
            "wal": "02" + "00" * 7,
            "block": "030001016b760000000001000000",
            "footer": "04" + "00" * 40 + "57fb808b247547db",
            "key": "056b0101000000000000",
            "snappy": "0e051068656c6c6f",
            "zstd": "1628b52ffd240529000068656c6c6fa36d9f88",
            "compression": "07" + "78" * 64,
        },
        "database": {
            "lifecycle": "01f00061f10162f20263f30364f50400f60600f70700",
            "snapshots": "020000610501000301620e00000401000f0000",
        },
    }
    for name, encoded in seeds[args.kind].items():
        (corpus / name).write_bytes(bytes.fromhex(encoded))
    subprocess.run(
        [
            str(args.binary.resolve()),
            str(corpus.resolve()),
            f"-runs={args.runs}",
            "-seed=301",
            "-max_len=4096",
            "-timeout=15",
            "-rss_limit_mb=1024",
            "-malloc_limit_mb=256",
            f"-artifact_prefix={artifacts.resolve()}/",
            "-print_final_stats=1",
        ],
        check=True,
        timeout=1800,
    )


if __name__ == "__main__":
    main()
