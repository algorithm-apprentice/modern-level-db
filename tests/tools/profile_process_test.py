import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import profile_report


class ProfileProcessTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="modern-perf-process-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.journal = []

    def test_records_success_and_explicit_nonzero_exit(self):
        profile_report.run_owned(
            [sys.executable, "-c", "print('ok')"], self.root / "ok.log", 10, self.journal
        )
        self.assertEqual((self.root / "ok.log").read_text().strip(), "ok")
        with self.assertRaises(subprocess.CalledProcessError):
            profile_report.run_owned(
                [sys.executable, "-c", "raise SystemExit(7)"],
                self.root / "fail.log", 10, self.journal,
            )
        self.assertEqual([entry["returncode"] for entry in self.journal], [0, 7])
        self.assertTrue(all(entry["cleanup_verified"] for entry in self.journal))

    def test_timeout_stops_and_reaps_an_owned_group(self):
        code = ("import signal,time;"
                "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                "signal.signal(signal.SIGINT,signal.SIG_IGN);time.sleep(60)")
        with self.assertRaises(subprocess.TimeoutExpired):
            profile_report.run_owned(
                [sys.executable, "-c", code], self.root / "timeout.log", 0.5,
                self.journal, grace=0.1,
            )
        entry = self.journal[0]
        self.assertTrue(entry["timed_out"])
        self.assertTrue(entry["cleanup_verified"])
        self.assertIsNone(profile_report.process_identity(entry["pid"]))

    def test_timeout_also_stops_a_target_in_a_separate_process_group(self):
        child = ("import signal,time;"
                 "signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)")
        collector = (
            "import subprocess,sys;"
            f"child=subprocess.Popen([sys.executable,'-c',{child!r}],start_new_session=True);"
            "child.wait()"
        )
        with self.assertRaises(subprocess.TimeoutExpired):
            profile_report.run_owned(
                [sys.executable, "-c", collector], self.root / "collector.log", 2,
                self.journal,
                target_executable=profile_report.process_identity(os.getpid()).executable,
                grace=0.2,
            )
        entry = self.journal[0]
        self.assertIn("target_pid", entry)
        self.assertTrue(entry["cleanup_verified"])
        self.assertIsNone(profile_report.process_identity(entry["pid"]))
        self.assertIsNone(profile_report.process_identity(entry["target_pid"]))

    def test_discovery_failure_still_stops_and_reaps_the_collector(self):
        processes = []
        real_popen = subprocess.Popen

        def start_process(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            processes.append(process)
            return process

        try:
            with mock.patch.object(
                profile_report.subprocess, "Popen", side_effect=start_process
            ):
                with mock.patch.object(
                    profile_report, "find_target",
                    side_effect=RuntimeError("target discovery failed"),
                ):
                    with self.assertRaisesRegex(RuntimeError, "target discovery failed"):
                        profile_report.run_owned(
                            [sys.executable, "-c", "import time;time.sleep(60)"],
                            self.root / "discovery-failure.log", 10, self.journal,
                            target_executable=sys.executable, grace=0.1,
                        )
        finally:
            for process in processes:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)

        entry = self.journal[0]
        self.assertIsNotNone(entry["returncode"])
        self.assertFalse(entry["cleanup_verified"])
        self.assertIsNone(profile_report.process_identity(entry["pid"]))

    def test_multiple_discovered_targets_are_stopped_before_failure(self):
        first = profile_report.ProcessIdentity(101, 100, 101, "first", Path("/target"))
        second = profile_report.ProcessIdentity(102, 100, 102, "second", Path("/target"))

        class FakeProcess:
            pid = 100

            def __init__(self):
                self.returncode = None

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                self.returncode = 1
                return self.returncode

        process = FakeProcess()
        discovery_error = profile_report.TargetDiscoveryError(
            "collector launched more than one matching workload",
            [first, second], complete=True,
        )
        with mock.patch.object(profile_report.subprocess, "Popen", return_value=process):
            with mock.patch.object(
                profile_report, "find_target", side_effect=discovery_error
            ):
                with mock.patch.object(
                    profile_report, "signal_target"
                ) as signal_target:
                    with mock.patch.object(
                        profile_report, "same_process", return_value=False
                    ):
                        with self.assertRaisesRegex(
                            RuntimeError, "more than one matching workload"
                        ):
                            profile_report.run_owned(
                                ["collector"], self.root / "multiple-targets.log", 10,
                                self.journal, target_executable="/target", grace=0.1,
                            )

        signal_target.assert_has_calls([
            mock.call(first, signal.SIGTERM),
            mock.call(second, signal.SIGTERM),
        ])
        self.assertEqual(self.journal[0]["returncode"], 1)
        self.assertTrue(self.journal[0]["cleanup_verified"])

    def test_cardinality_error_preserves_every_discovered_identity(self):
        first = profile_report.ProcessIdentity(101, 100, 101, "first", Path("/target"))
        second = profile_report.ProcessIdentity(102, 100, 102, "second", Path("/target"))
        children = mock.Mock(returncode=0, stdout="101\n102\n", stderr="")
        with mock.patch.object(profile_report.subprocess, "run", return_value=children):
            with mock.patch.object(
                profile_report, "process_identity", side_effect=[first, second]
            ):
                with self.assertRaisesRegex(
                    profile_report.TargetDiscoveryError,
                    "more than one matching workload",
                ) as raised:
                    profile_report.find_target(100, Path("/target"))

        self.assertTrue(raised.exception.complete)
        self.assertEqual(raised.exception.targets, (first, second))

    def test_never_signals_a_reused_identity(self):
        old = profile_report.ProcessIdentity(123, 100, 123, "old", Path("/old"))
        new = profile_report.ProcessIdentity(123, 100, 123, "new", Path("/new"))
        with mock.patch.object(profile_report, "process_identity", return_value=new):
            with mock.patch.object(os, "kill") as kill:
                profile_report.signal_target(old, signal.SIGKILL)
                kill.assert_not_called()

    def test_never_claims_cleanup_when_the_target_cannot_be_identified(self):
        with self.assertRaises(RuntimeError):
            profile_report.run_owned(
                [sys.executable, "-c", "pass"], self.root / "no-target.log", 10,
                self.journal, target_executable=self.root / "absent",
            )
        self.assertFalse(self.journal[0]["cleanup_verified"])


if __name__ == "__main__":
    unittest.main()
