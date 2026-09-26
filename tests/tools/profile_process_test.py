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
