import copy
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_benchmark import bind_run, check_report, main, publication_lock


def report():
    return {
        "schema_version": 1,
        "entries": 4096,
        "trials": 3,
        "samples": {
            engine: {phase: [10.0, 12.0, 11.0] for phase in ("write", "read", "scan")}
            for engine in ("modern", "leveldb")
        },
    }

def policy():
    return {
        "schema_version": 1, "samples_schema_version": 1,
        "platform": "Windows", "compiler_id": "MSVC", "compiler_version": "19.51",
        "build_configuration": "Release", "target_architecture": "native_x64",
        "reference_revision": "7ee830d02b623e8ffe0b95d59a74db1e58da04c5",
        "reference_source": "git-source-override", "reference_dirty": "clean",
        "modern_file_access": "copied", "reference_file_access": "platform_default",
        "modern_namespace_policy": "explicit_weak", "sync_wal_creation": True,
        "workload_writes": "unsynced", "performance_policy": "diagnostic-only",
    }


class BenchmarkGateTest(unittest.TestCase):
    def test_accepts_valid_report(self):
        self.assertEqual(check_report(report()), {"read": 1.0, "scan": 1.0, "write": 1.0})

    def test_checks_exact_twenty_times_threshold(self):
        data = report()
        data["samples"]["modern"]["write"] = [220.0] * 3
        self.assertEqual(check_report(data)["write"], 20.0)
        data["samples"]["modern"]["write"] = [220.0001] * 3
        with self.assertRaises(ValueError):
            check_report(data)

    def test_uses_median_not_the_fastest_sample(self):
        data = report()
        data["samples"]["modern"]["read"] = [1.0, 1000.0, 1000.0]
        with self.assertRaises(ValueError):
            check_report(data)

    def test_diagnostic_mode_reports_finite_slowdown_without_downgrading_default(self):
        data = report()
        data["samples"]["modern"]["write"] = [1100.0] * 3
        self.assertEqual(check_report(data, diagnostic_only=True)["write"], 100.0)
        with self.assertRaises(ValueError):
            check_report(data)

    def test_diagnostic_mode_still_rejects_invalid_and_overflowing_measurements(self):
        data = report()
        data["samples"]["modern"]["write"] = [1e308] * 3
        data["samples"]["leveldb"]["write"] = [1e-308] * 3
        with self.assertRaises(ValueError):
            check_report(data, diagnostic_only=True)
        for invalid in (0, -1, float("nan"), float("inf"), True, "10"):
            data = report()
            data["samples"]["modern"]["scan"][0] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                check_report(data, diagnostic_only=True)

    def test_rejects_invalid_values_and_sample_counts(self):
        for invalid in (0, -1, float("nan"), float("inf"), True, "10", None):
            data = report()
            data["samples"]["modern"]["scan"][0] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                check_report(data)
        data = report()
        data["samples"]["modern"]["write"].pop()
        with self.assertRaises(ValueError):
            check_report(data)

    def test_rejects_missing_and_extra_metrics(self):
        for key in ("modern", "leveldb"):
            data = report()
            del data["samples"][key]
            with self.assertRaises(ValueError):
                check_report(data)
        for phase in ("write", "read", "scan"):
            data = report()
            del data["samples"]["modern"][phase]
            with self.assertRaises(ValueError):
                check_report(data)
        data = report()
        data["samples"]["modern"]["extra"] = [1.0] * 3
        with self.assertRaises(ValueError):
            check_report(data)

    def test_rejects_invalid_schema_and_counts(self):
        for field, values in {
            "schema_version": (True, 2, "1"),
            "entries": (0, True, 1_000_001),
            "trials": (1, 2, 32, True),
        }.items():
            for value in values:
                data = copy.deepcopy(report())
                data[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    check_report(data)
        for invalid in (None, [], {}, {**report(), "extra": 1}):
            with self.assertRaises(ValueError):
                check_report(invalid)

    def test_binding_hashes_exact_binary_samples_and_policy_without_paths(self):
        with tempfile.TemporaryDirectory(prefix="modern-benchmark-binding-") as root:
            root = Path(root)
            binary = root / "benchmark"
            samples = root / "results.json"
            sidecar = root / "policy.json"
            binary.write_bytes(b"compiled benchmark identity")
            samples.write_text(json.dumps(report()), encoding="utf-8")
            sidecar.write_text(json.dumps(policy()), encoding="utf-8")
            binding = bind_run(samples, binary, sidecar, True)
            for key, path in (("binary_sha256", binary), ("samples_sha256", samples),
                              ("policy_sha256", sidecar)):
                self.assertEqual(binding[key], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertNotIn(str(root), json.dumps(binding))
            self.assertEqual(json.loads(samples.with_suffix(".provenance.json").read_text()),
                             binding)
            with self.assertRaises(ValueError):
                bind_run(samples, binary, sidecar, False)

    def test_binding_accepts_explicit_native_mapped_default_without_selecting_policy(self):
        with tempfile.TemporaryDirectory(prefix="modern-mapped-policy-") as root:
            root = Path(root)
            binary, samples, sidecar = root / "benchmark", root / "samples.json", root / "policy.json"
            binary.write_bytes(b"native mapped binary")
            samples.write_text(json.dumps(report()), encoding="utf-8")
            data = policy()
            data["modern_file_access"] = "mapped_default"
            sidecar.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(bind_run(samples, binary, sidecar, True)["performance_policy"],
                             "diagnostic-only")
            with self.assertRaises(ValueError):
                bind_run(samples, binary, sidecar, False)

    def test_diagnostic_sidecar_cannot_downgrade_default_admission(self):
        with tempfile.TemporaryDirectory(prefix="modern-benchmark-policy-") as root:
            root = Path(root)
            samples = root / "results.json"
            sidecar = root / "policy.json"
            data = report()
            data["samples"]["modern"]["write"] = [1100.0] * 3
            samples.write_text(json.dumps(data), encoding="utf-8")
            sidecar.write_text(json.dumps(policy()), encoding="utf-8")
            with mock.patch.object(sys, "argv",
                                   ["check_benchmark", str(samples), "--policy-sidecar", str(sidecar)]), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(), 1)
            self.assertFalse(samples.with_suffix(".provenance.json").exists())

    def test_snapshots_reject_replaced_artifacts_before_binding_publication(self):
        with tempfile.TemporaryDirectory(prefix="modern-benchmark-snapshots-") as root:
            root = Path(root)
            binary = root / "benchmark"
            samples = root / "results.json"
            sidecar = root / "policy.json"
            binary.write_bytes(b"executed binary")
            samples.write_text(json.dumps(report()), encoding="utf-8")
            sidecar.write_text(json.dumps(policy()), encoding="utf-8")
            original = tuple(path.read_bytes() for path in (binary, samples, sidecar))
            for changed in (binary, samples, sidecar):
                changed.write_bytes(changed.read_bytes() + b" ")
                with self.subTest(path=changed.name), self.assertRaises(ValueError):
                    bind_run(samples, binary, sidecar, True, snapshots=original)
                self.assertFalse(samples.with_suffix(".provenance.json").exists())
                for path, data in zip((binary, samples, sidecar), original):
                    path.write_bytes(data)

    def test_same_destination_cannot_be_published_by_two_runs(self):
        with tempfile.TemporaryDirectory(prefix="modern-benchmark-lock-") as root:
            samples = Path(root) / "results.json"
            with publication_lock(samples):
                with self.assertRaises(OSError):
                    with publication_lock(samples):
                        self.fail("concurrent publication was admitted")
            with publication_lock(samples):
                pass

    def test_interrupted_run_releases_destination_for_retry(self):
        with tempfile.TemporaryDirectory(prefix="modern-benchmark-interrupted-") as root:
            root = Path(root)
            samples = root / "results.json"
            ready = root / "ready"
            script = (
                "import sys, time\n"
                "from pathlib import Path\n"
                "sys.path.insert(0, sys.argv[1])\n"
                "from check_benchmark import publication_lock\n"
                "with publication_lock(Path(sys.argv[2])):\n"
                "    Path(sys.argv[3]).write_bytes(b'ready')\n"
                "    time.sleep(60)\n"
            )
            child = subprocess.Popen(
                [sys.executable, "-c", script,
                 str(Path(__file__).resolve().parents[2] / "tools"), str(samples), str(ready)],
                stdout=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 10
                while not ready.exists():
                    if child.poll() is not None:
                        self.fail(f"lock fixture exited before readiness: {child.returncode}")
                    if time.monotonic() >= deadline:
                        self.fail("lock fixture did not become ready within its deadline")
                    time.sleep(0.01)
                with self.assertRaises(OSError):
                    with publication_lock(samples):
                        self.fail("live owner did not exclude a concurrent publisher")
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=10)
            with publication_lock(samples):
                pass


if __name__ == "__main__":
    unittest.main()
