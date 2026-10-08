"""Validate native owned-process stack sampling and calibration contracts."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_performance import run_case
import profile_report

COLLECTOR = CALIBRATION = CALIBRATION_PDB = PERFORMANCE = PERFORMANCE_PDB = None
OUTPUT = None


def capture(mode, duration=1000, allow_zero=True, timeout=30_000, symbols=None):
    root = Path(tempfile.mkdtemp(prefix=f"{mode}-", dir=OUTPUT))
    command = [
        str(COLLECTOR), "--sample",
        "--binary", str(CALIBRATION),
        "--symbols", str(symbols or CALIBRATION_PDB),
        "--log", str(root / "workload.log"),
        "--report", str(root / "profile.json"),
        "--epochs", str(root / "epochs.json"),
        "--timeout-ms", str(timeout),
    ]
    if allow_zero:
        command.append("--allow-zero-cpu")
    command.extend(["--", "--mode", mode, "--duration-ms", str(duration)])
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    profile = (
        json.loads((root / "profile.json").read_text(encoding="utf-8"))
        if (root / "profile.json").is_file() else None
    )
    epochs = (
        json.loads((root / "epochs.json").read_text(encoding="utf-8"))
        if (root / "epochs.json").is_file() else None
    )
    return root, result, profile, epochs


class WindowsProfileTest(unittest.TestCase):
    def test_outer_runner_timeout_terminates_and_reaps_its_collector(self):
        journal = []
        with tempfile.TemporaryDirectory(prefix="outer-timeout-", dir=OUTPUT) as root:
            with self.assertRaises(subprocess.TimeoutExpired):
                profile_report.run_owned(
                    [sys.executable, "-c", "import time;time.sleep(60)"],
                    Path(root) / "collector.log", 0.1, journal, grace=0.5,
                )
        self.assertEqual(len(journal), 1)
        self.assertTrue(journal[0]["timed_out"])
        self.assertTrue(journal[0]["cleanup_verified"])

    def test_opened_thread_ownership_rejects_a_foreign_pid(self):
        result = subprocess.run(
            [str(COLLECTOR), "--self-test-foreign-thread"],
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hot_wait_and_periodic_calibration(self):
        reports = {}
        coverage = {}
        for mode in ("hot", "wait", "alternating", "alternating-offset"):
            _, result, profile, epochs = capture(mode, duration=1500)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIsNotNone(profile)
            self.assertIsNotNone(epochs)
            self.assertEqual(profile["sample_schedule"],
                             "high-resolution-jitter-5-7-11-13-17-v1")
            reports[mode] = profile
            final = epochs["epochs"][-1]
            self.assertEqual(final["expected_iterations"], final["completed_iterations"])
            self.assertLessEqual(profile["total_cpu_100ns"], final["cpu_100ns"])
            coverage[mode] = {
                "ledger_cpu_100ns": final["cpu_100ns"],
                "sampled_cpu_100ns": profile["total_cpu_100ns"],
                "sampled_fraction": (
                    profile["total_cpu_100ns"] / final["cpu_100ns"]
                    if final["cpu_100ns"] else 0.0
                ),
                "attributed_fraction": (
                    profile["attributed_cpu_100ns"] / profile["total_cpu_100ns"]
                    if profile["total_cpu_100ns"] else 0.0
                ),
            }
        hot = reports["hot"]["total_cpu_100ns"]
        self.assertGreater(hot, 0)
        self.assertGreaterEqual(reports["hot"]["attributed_cpu_100ns"] / hot, 0.5)
        for mode in ("alternating", "alternating-offset"):
            total = reports[mode]["total_cpu_100ns"]
            self.assertGreater(total, 0)
            self.assertGreaterEqual(reports[mode]["attributed_cpu_100ns"] / total, 0.1)
            self.assertGreaterEqual(coverage[mode]["sampled_fraction"], 0.05)
        self.assertGreaterEqual(coverage["hot"]["sampled_fraction"], 0.05)
        self.assertLessEqual(reports["wait"]["total_cpu_100ns"], hot // 5)
        (OUTPUT / "calibration-summary.json").write_text(
            json.dumps(
                {"schema_version": 1, "sample_schedule":
                 "high-resolution-jitter-5-7-11-13-17-v1", "modes": coverage},
                indent=2, sort_keys=True,
            ) + "\n",
            encoding="utf-8",
        )

    def test_mismatched_symbols_and_readiness_timeout_are_not_success(self):
        root, result, profile, epochs = capture("hot", symbols=PERFORMANCE_PDB)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(profile)
        self.assertIsNone(epochs)
        self.assertFalse((root / "epochs.json").exists())
        root, result, profile, epochs = capture("stall", duration=1000, timeout=250)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(profile)
        self.assertIsNone(epochs)
        self.assertFalse((root / "epochs.json").exists())

    def test_trusted_runner_reconciles_the_final_real_epoch(self):
        output = Path(tempfile.mkdtemp(prefix="runner-parent-", dir=OUTPUT)) / "runner"
        manifest = run_case(
            PERFORMANCE, "modern/readrandom/4096", output, capture_cpu=True,
            repetitions=1, min_time=0.5, timeout=90,
            native_collector=COLLECTOR, native_symbols=PERFORMANCE_PDB,
        )
        self.assertEqual(manifest["status"], "complete")
        self.assertTrue(manifest["process_cleanup_verified"])
        self.assertGreater(manifest["profile_summary"]["attributed_cpu_100ns"], 0)
        self.assertGreater(manifest["profile_summary"]["attributed_fraction"], 0)
        for name, digest in manifest["artifact_sha256"].items():
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
        original = (output / "profile.json").read_bytes()
        (output / "profile.json").write_bytes(original + b" ")
        self.assertNotEqual(
            hashlib.sha256((output / "profile.json").read_bytes()).hexdigest(),
            manifest["artifact_sha256"]["profile.json"],
        )
        (output / "profile.json").write_bytes(original)
        profile = json.loads((output / "profile.json").read_text(encoding="utf-8"))
        for stack in profile["stacks"]:
            addresses = [
                (frame["module"], frame["module_offset"]) for frame in stack["frames"]
            ]
            self.assertTrue(all(left != right for left, right in zip(
                addresses, addresses[1:]
            )))
            for frame in stack["frames"]:
                self.assertGreaterEqual(frame["module_offset"], 0)
                self.assertFalse(re.search(r"0x[0-9a-f]{8,}", frame["symbol"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collector", type=Path, required=True)
    parser.add_argument("--calibration", type=Path, required=True)
    parser.add_argument("--calibration-pdb", type=Path, required=True)
    parser.add_argument("--performance", type=Path, required=True)
    parser.add_argument("--performance-pdb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    global COLLECTOR, CALIBRATION, CALIBRATION_PDB
    global PERFORMANCE, PERFORMANCE_PDB, OUTPUT
    COLLECTOR = args.collector
    CALIBRATION, CALIBRATION_PDB = args.calibration, args.calibration_pdb
    PERFORMANCE, PERFORMANCE_PDB = args.performance, args.performance_pdb
    OUTPUT = args.output
    OUTPUT.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
