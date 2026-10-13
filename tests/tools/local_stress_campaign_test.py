from pathlib import Path
import ctypes
import os
import sys
import unittest
import tempfile
import textwrap
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_local_stress_campaign


class LocalStressCampaignPlannerTest(unittest.TestCase):
    def config(self) -> run_local_stress_campaign.CampaignConfig:
        return run_local_stress_campaign.CampaignConfig(
            stress_dir=Path("/stress"),
            workspace=Path("/campaign"),
            seeds=(1, 17, 301),
            tiers=("v1", "v2", "v3", "v4"),
            v1_operations=5000,
            v2_rounds=300,
            v3_rounds=25,
            v3_inflight_probes=3,
            v3_timeout_seconds=30.0,
            v4_operations=250,
            v4_liveness_writes=2000,
            job_timeout_seconds=3600.0,
            max_duration_seconds=0.0,
            resume=False,
        )

    def test_default_plan_has_stable_complete_order(self):
        plan = run_local_stress_campaign.build_plan(self.config())

        self.assertEqual(len(plan.jobs), 30)
        self.assertEqual(
            [job.job_id for job in plan.jobs[:10]],
            [
                "seed-1-v1-none",
                "seed-1-v1-snappy",
                "seed-1-v1-zstd",
                "seed-1-v2-t2-none",
                "seed-1-v2-t4-snappy",
                "seed-1-v2-t8-zstd",
                "seed-1-v3-none",
                "seed-1-v3-snappy",
                "seed-1-v3-zstd",
                "seed-1-v4",
            ],
        )
        self.assertEqual(plan.jobs[-1].job_id, "seed-301-v4")
        self.assertEqual([job.index for job in plan.jobs], list(range(30)))

    def test_selected_tiers_keep_canonical_profiles_and_commands(self):
        config = self.config()
        config.tiers = ("v4", "v2")
        config.seeds = (17,)
        config.v2_rounds = 41
        config.v4_operations = 42
        config.v4_liveness_writes = 84

        plan = run_local_stress_campaign.build_plan(config)

        self.assertEqual(
            [job.job_id for job in plan.jobs],
            [
                "seed-17-v2-t2-none",
                "seed-17-v2-t4-snappy",
                "seed-17-v2-t8-zstd",
                "seed-17-v4",
            ],
        )
        suffix = ".exe" if os.name == "nt" else ""
        attempt = Path("/attempt")
        self.assertEqual(
            run_local_stress_campaign.command_for_attempt(plan.jobs[0], attempt),
            [
                str(Path("/stress") / f"modern_leveldb_concurrent_stress{suffix}"),
                "--db",
                str(attempt / "database"),
                "--output",
                str(attempt / "artifacts"),
                "--seed",
                "17",
                "--threads",
                "2",
                "--rounds",
                "41",
                "--compression",
                "none",
            ],
        )
        self.assertEqual(
            run_local_stress_campaign.command_for_attempt(plan.jobs[-1], attempt),
            [
                str(Path("/stress") / f"modern_leveldb_option_fault_stress{suffix}"),
                "--workspace",
                str(attempt / "workspace"),
                "--seed",
                "17",
                "--operations",
                "42",
                "--liveness-writes",
                "84",
            ],
        )

    def test_plan_document_and_digest_are_stable_and_sensitive(self):
        config = self.config()
        first = run_local_stress_campaign.build_plan(config)
        second = run_local_stress_campaign.build_plan(self.config())

        self.assertEqual(
            run_local_stress_campaign.plan_document(first),
            run_local_stress_campaign.plan_document(second),
        )
        self.assertEqual(
            run_local_stress_campaign.plan_digest(first),
            run_local_stress_campaign.plan_digest(second),
        )
        self.assertEqual(len(run_local_stress_campaign.plan_digest(first)), 64)
        document = run_local_stress_campaign.plan_document(first)
        self.assertEqual(document["format"], "modern-leveldb-stress-plan-v1")
        self.assertEqual(document["jobs"][0]["run_artifact"], "artifacts/run.txt")
        self.assertEqual(
            document["jobs"][-1]["completion_artifact"],
            "workspace/artifacts/completion.txt",
        )
        self.assertIn("{database}", document["jobs"][0]["argv"])
        self.assertNotIn("/attempt", str(document))

        changed = self.config()
        changed.v1_operations += 1
        self.assertNotEqual(
            run_local_stress_campaign.plan_digest(first),
            run_local_stress_campaign.plan_digest(
                run_local_stress_campaign.build_plan(changed)
            ),
        )

    def test_argument_parser_rejects_duplicate_invalid_and_zero_values(self):
        invalid = [
            ["--stress-dir", "stress", "--workspace", "work", "--seeds", ""],
            ["--stress-dir", "stress", "--workspace", "work", "--seeds", "1,1"],
            ["--stress-dir", "stress", "--workspace", "work", "--seeds", "-1"],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--seeds",
                str(1 << 64),
            ],
            ["--stress-dir", "stress", "--workspace", "work", "--tiers", "v1,v1"],
            ["--stress-dir", "stress", "--workspace", "work", "--tiers", "v5"],
            ["--stress-dir", "stress", "--workspace", "work", "--v1-operations", "0"],
            ["--stress-dir", "stress", "--workspace", "work", "--v2-rounds", "0"],
            ["--stress-dir", "stress", "--workspace", "work", "--v3-rounds", "0"],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--v3-inflight-probes",
                "-1",
            ],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--v3-timeout-seconds",
                "0",
            ],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--v4-operations",
                "0",
            ],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--v4-liveness-writes",
                "0",
            ],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--job-timeout-seconds",
                "0",
            ],
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--max-duration-seconds",
                "-1",
            ],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    run_local_stress_campaign.parse_arguments(arguments)

    def test_argument_parser_applies_defaults_and_canonical_tier_order(self):
        parsed = run_local_stress_campaign.parse_arguments(
            [
                "--stress-dir",
                "stress",
                "--workspace",
                "work",
                "--seeds",
                "17,1",
                "--tiers",
                "v4,v2",
                "--resume",
            ]
        )

        self.assertEqual(parsed.stress_dir, Path("stress"))
        self.assertEqual(parsed.workspace, Path("work"))
        self.assertEqual(parsed.seeds, (17, 1))
        self.assertEqual(parsed.tiers, ("v2", "v4"))
        self.assertEqual(parsed.v1_operations, 5000)
        self.assertEqual(parsed.v2_rounds, 300)
        self.assertEqual(parsed.v3_rounds, 25)
        self.assertEqual(parsed.v3_inflight_probes, 3)
        self.assertEqual(parsed.v3_timeout_seconds, 30.0)
        self.assertEqual(parsed.v4_operations, 250)
        self.assertEqual(parsed.v4_liveness_writes, 2000)
        self.assertEqual(parsed.job_timeout_seconds, 3600.0)
        self.assertEqual(parsed.max_duration_seconds, 0.0)
        self.assertTrue(parsed.resume)


class LocalStressCampaignAttemptTest(unittest.TestCase):
    def script(self, root: Path, source: str) -> Path:
        script = root / "fake_job.py"
        script.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
        return script

    def job(self, command: list[str]) -> run_local_stress_campaign.CampaignJob:
        return run_local_stress_campaign.CampaignJob(
            index=0,
            job_id="fake-job",
            tier="v1",
            seed=17,
            argv_template=tuple(command),
            run_artifact="artifacts/run.txt",
            completion_artifact="artifacts/completion.txt",
        )

    def process_exists(self, process_id: int) -> bool:
        if os.name != "nt":
            try:
                os.kill(process_id, 0)
                return True
            except ProcessLookupError:
                return False
        process_query_limited_information = 0x1000
        still_active = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(
            process_query_limited_information, False, process_id
        )
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return False
            return exit_code.value == still_active
        finally:
            kernel32.CloseHandle(handle)

    def test_attempt_streams_output_and_publishes_success_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.script(
                root,
                """
                import sys
                print("fake stdout", flush=True)
                print("fake stderr", file=sys.stderr, flush=True)
                """,
            )
            attempt = root / "attempt"
            job = self.job([sys.executable, str(script), "{attempt}"])

            result = run_local_stress_campaign.execute_attempt(
                job, attempt, timeout_seconds=5.0
            )

            self.assertEqual(result.category, "exit_zero")
            self.assertEqual(result.returncode, 0)
            self.assertFalse(result.termination_sent)
            self.assertEqual(
                (attempt / "stdout.log").read_text(encoding="utf-8"),
                "fake stdout\n",
            )
            self.assertEqual(
                (attempt / "stderr.log").read_text(encoding="utf-8"),
                "fake stderr\n",
            )
            command = run_local_stress_campaign.read_json(attempt / "command.json")
            self.assertEqual(command["format"], "modern-leveldb-stress-command-v1")
            self.assertEqual(command["argv"][-1], str(attempt))
            published = run_local_stress_campaign.read_json(attempt / "result.json")
            self.assertEqual(published["category"], "exit_zero")
            self.assertEqual(published["returncode"], 0)

    def test_attempt_classifies_nonzero_and_launch_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            failed_script = self.script(
                root,
                """
                import sys
                print("failed", file=sys.stderr, flush=True)
                raise SystemExit(7)
                """,
            )

            nonzero = run_local_stress_campaign.execute_attempt(
                self.job([sys.executable, str(failed_script)]),
                root / "nonzero",
                timeout_seconds=5.0,
            )
            launched = run_local_stress_campaign.execute_attempt(
                self.job([str(root / "missing-executable")]),
                root / "launch",
                timeout_seconds=5.0,
            )

            self.assertEqual(nonzero.category, "nonzero_exit")
            self.assertEqual(nonzero.returncode, 7)
            self.assertIn(
                "failed",
                (root / "nonzero" / "stderr.log").read_text(encoding="utf-8"),
            )
            self.assertEqual(launched.category, "launch_failure")
            self.assertIsNone(launched.returncode)
            self.assertTrue(launched.error)
            self.assertTrue((root / "launch" / "result.json").is_file())

    def test_timeout_terminates_owned_process_group_and_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_pid = root / "child.pid"
            script = self.script(
                root,
                """
                from pathlib import Path
                import subprocess
                import sys
                import time

                child = subprocess.Popen(
                    [sys.executable, "-c", "import time; time.sleep(60)"]
                )
                Path(sys.argv[1]).write_text(str(child.pid), encoding="utf-8")
                print("ready", flush=True)
                time.sleep(60)
                """,
            )

            result = run_local_stress_campaign.execute_attempt(
                self.job([sys.executable, str(script), str(child_pid)]),
                root / "timeout",
                timeout_seconds=0.2,
                termination_grace_seconds=1.0,
            )

            self.assertEqual(result.category, "timed_out")
            self.assertTrue(result.termination_sent)
            self.assertTrue(child_pid.is_file())
            process_id = int(child_pid.read_text(encoding="utf-8"))
            deadline = time.monotonic() + 5.0
            while self.process_exists(process_id) and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(self.process_exists(process_id))

    def test_attempt_rejects_existing_path_without_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            attempt = root / "attempt"
            attempt.mkdir()
            sentinel = attempt / "sentinel"
            sentinel.write_text("keep", encoding="utf-8")
            job = self.job([sys.executable, "-c", "raise SystemExit(0)"])

            with self.assertRaisesRegex(ValueError, "already exists"):
                run_local_stress_campaign.execute_attempt(
                    job, attempt, timeout_seconds=5.0
                )

            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")


if __name__ == "__main__":
    unittest.main()
