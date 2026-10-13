from pathlib import Path
import sys
import tempfile
import textwrap
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_crash_stress


class CrashStressTest(unittest.TestCase):
    def fake_worker(self, root: Path) -> Path:
        worker = root / "fake_worker.py"
        worker.write_text(
            textwrap.dedent(
                """
                import argparse
                from pathlib import Path
                import sys
                import time

                behavior = "normal"
                if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
                    behavior = sys.argv.pop(1)

                parser = argparse.ArgumentParser()
                parser.add_argument("--mode", required=True)
                parser.add_argument("--db")
                parser.add_argument("--journal")
                parser.add_argument("--seed")
                parser.add_argument("--index", type=int)
                parser.add_argument("--compression")
                args = parser.parse_args()

                if args.mode == "identity":
                    print("identity\\tfake-revision\\tclean", flush=True)
                elif args.mode == "write-hold":
                    if behavior == "malformed":
                        print("bad", flush=True)
                    else:
                        print(f"ack\\t{args.index}", flush=True)
                    time.sleep(60)
                elif args.mode == "inflight-hold":
                    print(f"ready\\t{args.index}", flush=True)
                    time.sleep(60)
                elif args.mode == "initialize":
                    print("initialized", flush=True)
                elif args.mode == "verify-prefix":
                    lines = Path(args.journal).read_text(encoding="utf-8").splitlines()
                    print(f"verified\\t{len(lines) - 1}", flush=True)
                elif args.mode == "verify-optional":
                    print("optional\\tabsent", flush=True)
                else:
                    raise SystemExit(2)
                """
            ).lstrip(),
            encoding="utf-8",
        )
        return worker

    def config(self, root: Path, worker_command: list[str]) -> run_crash_stress.Config:
        return run_crash_stress.Config(
            worker_command=worker_command,
            database=root / "db",
            output=root / "output",
            rounds=3,
            seed=17,
            compression="none",
            timeout_seconds=2.0,
            inflight_probes=2,
        )

    def test_holding_child_validates_protocol_kills_and_reaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            worker = self.fake_worker(root)
            command = [sys.executable, str(worker), "--mode", "write-hold", "--index", "0"]
            callback_ran = False

            def after_record():
                nonlocal callback_ran
                callback_ran = True

            result = run_crash_stress.run_holding_child(
                command, "ack\t0", 2.0, after_record
            )

            self.assertTrue(callback_ran)
            self.assertTrue(result.terminated_by_parent)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.trailing_stdout, "")
            self.assertEqual(result.stderr, "")

    def test_holding_child_rejects_timeout_early_exit_and_trailing_output(self):
        cases = [
            (
                [sys.executable, "-c", "import time; time.sleep(60)"],
                "ack\t0",
                "timed out",
            ),
            (
                [sys.executable, "-c", "print('ack\\t0', flush=True)"],
                "ack\t0",
                "exited before termination",
            ),
            (
                [
                    sys.executable,
                    "-c",
                    "import sys,time; sys.stdout.write('ack\\t0\\ntail\\n'); "
                    "sys.stdout.flush(); time.sleep(60)",
                ],
                "ack\t0",
                "trailing stdout",
            ),
        ]
        for command, expected, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(RuntimeError, message):
                    run_crash_stress.run_holding_child(command, expected, 0.2)

    def test_holding_child_rejects_exit_during_acknowledgement_callback(self):
        command = [
            sys.executable,
            "-c",
            "import sys,time; print('ack\\t0', flush=True); time.sleep(0.1); sys.exit(7)",
        ]

        with self.assertRaisesRegex(RuntimeError, "before parent termination"):
            run_crash_stress.run_holding_child(
                command,
                "ack\t0",
                1.0,
                lambda: time.sleep(0.2),
            )

    def test_journal_is_contiguous_and_fsynced(self):
        with tempfile.TemporaryDirectory() as temporary:
            journal = Path(temporary) / "acknowledged.log"
            run_crash_stress.create_journal(journal)
            run_crash_stress.append_acknowledgement(journal, 0)
            run_crash_stress.append_acknowledgement(journal, 1)

            self.assertEqual(
                journal.read_text(encoding="utf-8"),
                "format=modern-leveldb-crash-journal-v1\nack\t0\nack\t1\n",
            )
            with self.assertRaisesRegex(RuntimeError, "contiguous"):
                run_crash_stress.append_acknowledgement(journal, 3)

    def test_campaign_publishes_completion_and_process_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            worker = self.fake_worker(root)
            config = self.config(root, [sys.executable, str(worker)])

            summary = run_crash_stress.run_campaign(config)

            self.assertEqual(summary.acknowledged, 3)
            self.assertEqual(summary.inflight_absent, 2)
            self.assertEqual(summary.inflight_complete, 0)
            self.assertTrue((config.output / "completion.txt").is_file())
            self.assertFalse((config.output / "failure.txt").exists())
            process_log = (config.output / "process.log").read_text(encoding="utf-8")
            self.assertIn("journal\tack\t0", process_log)
            self.assertIn("kill\tack\t0", process_log)
            self.assertLess(
                process_log.index("journal\tack\t0"),
                process_log.index("kill\tack\t0"),
            )
            self.assertIn("verify\tprefix\t3", process_log)
            self.assertIn("initialize\tinflight\t0", process_log)
            self.assertIn("verify\toptional\tabsent", process_log)

    def test_campaign_failure_preserves_artifacts_without_completion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            worker = self.fake_worker(root)
            config = self.config(root, [sys.executable, str(worker), "malformed"])

            with self.assertRaisesRegex(RuntimeError, "protocol"):
                run_crash_stress.run_campaign(config)

            self.assertTrue((config.output / "run.txt").is_file())
            self.assertTrue((config.output / "failure.txt").is_file())
            self.assertFalse((config.output / "completion.txt").exists())
            self.assertEqual(
                (config.output / "acknowledged.log").read_text(encoding="utf-8"),
                "format=modern-leveldb-crash-journal-v1\n",
            )

    def test_rejects_existing_equal_nested_and_missing_parent_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            worker = self.fake_worker(root)
            base = self.config(root, [sys.executable, str(worker)])

            existing_db = self.config(root, base.worker_command)
            existing_db.database.mkdir()
            with self.assertRaisesRegex(ValueError, "already exists"):
                run_crash_stress.validate_paths(existing_db)
            existing_db.database.rmdir()

            equal = self.config(root, base.worker_command)
            equal.output = equal.database
            with self.assertRaisesRegex(ValueError, "non-nested"):
                run_crash_stress.validate_paths(equal)

            nested = self.config(root, base.worker_command)
            nested.output = nested.database / "artifacts"
            with self.assertRaisesRegex(ValueError, "non-nested"):
                run_crash_stress.validate_paths(nested)

            missing_parent = self.config(root, base.worker_command)
            missing_parent.database = root / "missing" / "db"
            with self.assertRaisesRegex(ValueError, "parent"):
                run_crash_stress.validate_paths(missing_parent)


if __name__ == "__main__":
    unittest.main()
