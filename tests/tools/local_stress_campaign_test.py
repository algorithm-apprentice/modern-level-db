from pathlib import Path
import ctypes
import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock
import tempfile
import textwrap
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import local_stress_campaign as run_local_stress_campaign


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
        stress_dir = config.stress_dir.resolve(strict=False)
        attempt = Path("/attempt")
        self.assertEqual(
            run_local_stress_campaign.command_for_attempt(plan.jobs[0], attempt),
            [
                str(stress_dir / f"modern_leveldb_concurrent_stress{suffix}"),
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
                str(stress_dir / f"modern_leveldb_option_fault_stress{suffix}"),
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
                with contextlib.redirect_stderr(io.StringIO()):
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


class LocalStressCampaignStateTest(unittest.TestCase):
    def fake_job(self, root: Path) -> Path:
        script = root / "fake_campaign_job.py"
        script.write_text(
            textwrap.dedent(
                """
                import argparse
                from pathlib import Path
                import sys

                parser = argparse.ArgumentParser()
                parser.add_argument("--artifacts", type=Path, required=True)
                parser.add_argument("--control", type=Path, required=True)
                parser.add_argument("--revision", required=True)
                parser.add_argument("--dirty", required=True)
                parser.add_argument("--label", required=True)
                args = parser.parse_args()

                behavior = args.control.read_text(encoding="utf-8").strip()
                print(f"start {args.label}", flush=True)
                if behavior == "fail":
                    print(f"failed {args.label}", file=sys.stderr, flush=True)
                    raise SystemExit(7)

                args.artifacts.mkdir()
                if behavior != "no-run":
                    (args.artifacts / "run.txt").write_text(
                        "\\n".join(
                            [
                                "format=modern-leveldb-stress-run-v1",
                                f"revision={args.revision}",
                                f"dirty={args.dirty}",
                                "",
                            ]
                        ),
                        encoding="utf-8",
                    )
                if behavior != "no-completion":
                    (args.artifacts / "completion.txt").write_text(
                        "format=modern-leveldb-stress-result-v1\\n",
                        encoding="utf-8",
                    )
                """
            ).lstrip(),
            encoding="utf-8",
        )
        return script

    def config(self, root: Path) -> run_local_stress_campaign.CampaignConfig:
        return run_local_stress_campaign.CampaignConfig(
            stress_dir=root,
            workspace=root / "campaign",
            seeds=(17,),
            tiers=("v1",),
            v1_operations=1,
            v2_rounds=1,
            v3_rounds=1,
            v3_inflight_probes=0,
            v3_timeout_seconds=1.0,
            v4_operations=1,
            v4_liveness_writes=1,
            job_timeout_seconds=5.0,
            max_duration_seconds=0.0,
            resume=False,
        )

    def job(
        self,
        index: int,
        script: Path,
        control: Path,
        job_id: str,
        revision: str = "fake-revision",
    ) -> run_local_stress_campaign.CampaignJob:
        return run_local_stress_campaign.CampaignJob(
            index=index,
            job_id=job_id,
            tier="v1",
            seed=17,
            argv_template=(
                sys.executable,
                str(script),
                "--artifacts",
                "{artifacts}",
                "--control",
                str(control),
                "--revision",
                revision,
                "--dirty",
                "false",
                "--label",
                job_id,
            ),
            run_artifact="artifacts/run.txt",
            completion_artifact="artifacts/completion.txt",
        )

    def plan(
        self, root: Path, jobs: list[run_local_stress_campaign.CampaignJob]
    ) -> run_local_stress_campaign.CampaignPlan:
        return run_local_stress_campaign.CampaignPlan(
            stress_dir=str(root),
            seeds=(17,),
            tiers=("v1",),
            v1_operations=1,
            v2_rounds=1,
            v3_rounds=1,
            v3_inflight_probes=0,
            v3_timeout_seconds=1.0,
            v4_operations=1,
            v4_liveness_writes=1,
            jobs=tuple(jobs),
        )

    def control(self, root: Path, name: str, behavior: str) -> Path:
        path = root / f"{name}.control"
        path.write_text(behavior, encoding="utf-8")
        return path

    def test_campaign_publishes_completion_state_summary_and_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            first = self.control(root, "first", "pass")
            second = self.control(root, "second", "pass")
            plan = self.plan(
                root,
                [
                    self.job(0, script, first, "job-one"),
                    self.job(1, script, second, "job-two"),
                ],
            )
            config = self.config(root)

            summary = run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )

            self.assertEqual(summary.status, "completed")
            self.assertEqual(summary.total, 2)
            self.assertEqual(summary.passed, 2)
            self.assertEqual(summary.failed, 0)
            self.assertEqual(summary.pending, 0)
            self.assertEqual(summary.revision, "fake-revision")
            self.assertFalse(summary.dirty)
            self.assertIsNone(summary.blocking_job)
            self.assertTrue((config.workspace / "completion.txt").is_file())
            self.assertTrue((config.workspace / "plan.json").is_file())
            self.assertTrue((config.workspace / "state.json").is_file())
            self.assertTrue((config.workspace / "summary.json").is_file())
            self.assertTrue((config.workspace / "summary.txt").is_file())
            self.assertTrue((config.workspace / "events.jsonl").is_file())
            state = run_local_stress_campaign.read_json(
                config.workspace / "state.json"
            )
            self.assertEqual(
                [job["status"] for job in state["jobs"]],
                ["passed", "passed"],
            )
            self.assertEqual(
                len(state["jobs"][0]["attempts"]),
                1,
            )
            published_summary = run_local_stress_campaign.read_json(
                config.workspace / "summary.json"
            )
            self.assertEqual(
                published_summary["by_tier"]["v1"]["passed"], 2
            )
            self.assertEqual(
                published_summary["by_seed"]["17"]["passed"], 2
            )
            self.assertGreaterEqual(
                published_summary["duration_seconds"], 0.0
            )

    def test_campaign_stops_on_failure_and_resume_retries_in_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            controls = [
                self.control(root, "first", "pass"),
                self.control(root, "second", "fail"),
                self.control(root, "third", "pass"),
            ]
            plan = self.plan(
                root,
                [
                    self.job(0, script, controls[0], "job-one"),
                    self.job(1, script, controls[1], "job-two"),
                    self.job(2, script, controls[2], "job-three"),
                ],
            )
            config = self.config(root)

            failed = run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )

            self.assertEqual(failed.status, "failed")
            self.assertEqual(failed.passed, 1)
            self.assertEqual(failed.failed, 1)
            self.assertEqual(failed.pending, 1)
            self.assertEqual(failed.blocking_job, "job-two")
            self.assertFalse((config.workspace / "completion.txt").exists())
            first_attempt = (
                config.workspace / "jobs" / "000-job-one" / "attempts" / "001"
            )
            failed_attempt = (
                config.workspace / "jobs" / "001-job-two" / "attempts" / "001"
            )
            pending_attempts = (
                config.workspace / "jobs" / "002-job-three" / "attempts"
            )
            self.assertTrue((first_attempt / "result.json").is_file())
            self.assertEqual(
                run_local_stress_campaign.read_json(
                    failed_attempt / "result.json"
                )["category"],
                "nonzero_exit",
            )
            self.assertEqual(list(pending_attempts.iterdir()), [])

            controls[1].write_text("pass", encoding="utf-8")
            config.resume = True
            completed = run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )

            self.assertEqual(completed.status, "completed")
            self.assertEqual(completed.passed, 3)
            self.assertEqual(
                sorted(path.name for path in (first_attempt.parent).iterdir()),
                ["001"],
            )
            self.assertEqual(
                sorted(
                    path.name
                    for path in (
                        config.workspace
                        / "jobs"
                        / "001-job-two"
                        / "attempts"
                    ).iterdir()
                ),
                ["001", "002"],
            )
            self.assertEqual(
                sorted(
                    path.name
                    for path in (
                        config.workspace
                        / "jobs"
                        / "002-job-three"
                        / "attempts"
                    ).iterdir()
                ),
                ["001"],
            )
            self.assertTrue((config.workspace / "completion.txt").is_file())

    def test_campaign_rejects_missing_artifacts_and_mixed_identity(self):
        cases = [
            ("no-completion", "same", "missing_completion"),
            ("pass", "different-revision", "identity_mismatch"),
        ]
        for behavior, revision, category in cases:
            with self.subTest(category=category):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    script = self.fake_job(root)
                    first = self.control(root, "first", "pass")
                    second = self.control(root, "second", behavior)
                    plan = self.plan(
                        root,
                        [
                            self.job(
                                0, script, first, "job-one", "same"
                            ),
                            self.job(
                                1, script, second, "job-two", revision
                            ),
                        ],
                    )
                    config = self.config(root)

                    summary = run_local_stress_campaign.run_campaign(
                        config, plan=plan, working_directory=root
                    )

                    self.assertEqual(summary.status, "failed")
                    self.assertEqual(summary.blocking_job, "job-two")
                    state = run_local_stress_campaign.read_json(
                        config.workspace / "state.json"
                    )
                    self.assertEqual(
                        state["jobs"][1]["attempts"][-1]["category"],
                        category,
                    )
                    self.assertFalse(
                        (config.workspace / "completion.txt").exists()
                    )

    def test_resume_rejects_plan_mismatch_before_new_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            control = self.control(root, "first", "fail")
            initial = self.plan(
                root, [self.job(0, script, control, "job-one")]
            )
            config = self.config(root)
            run_local_stress_campaign.run_campaign(
                config, plan=initial, working_directory=root
            )
            attempts = (
                config.workspace / "jobs" / "000-job-one" / "attempts"
            )
            changed = self.plan(
                root,
                [self.job(0, script, control, "job-one", "changed")],
            )
            config.resume = True

            with self.assertRaisesRegex(ValueError, "plan does not match"):
                run_local_stress_campaign.run_campaign(
                    config, plan=changed, working_directory=root
                )

            self.assertEqual(
                sorted(path.name for path in attempts.iterdir()), ["001"]
            )

    def test_resume_converts_stale_running_and_revalidates_passed_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            first = self.control(root, "first", "pass")
            second = self.control(root, "second", "fail")
            plan = self.plan(
                root,
                [
                    self.job(0, script, first, "job-one"),
                    self.job(1, script, second, "job-two"),
                ],
            )
            config = self.config(root)
            run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )

            first_attempt = (
                config.workspace / "jobs" / "000-job-one" / "attempts" / "001"
            )
            (first_attempt / "artifacts" / "completion.txt").unlink()
            state_path = config.workspace / "state.json"
            state = run_local_stress_campaign.read_json(state_path)
            state["status"] = "running"
            state["jobs"][1]["status"] = "running"
            state["jobs"][1]["attempts"][-1]["status"] = "running"
            state["jobs"][1]["attempts"][-1]["category"] = "running"
            state_path.write_text(
                json.dumps(state, ensure_ascii=False),
                encoding="utf-8",
            )
            second.write_text("pass", encoding="utf-8")
            config.resume = True

            summary = run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )

            self.assertEqual(summary.status, "completed")
            state = run_local_stress_campaign.read_json(state_path)
            self.assertEqual(
                state["jobs"][1]["attempts"][0]["category"],
                "orchestrator_interrupted",
            )
            self.assertEqual(len(state["jobs"][0]["attempts"]), 2)
            self.assertEqual(len(state["jobs"][1]["attempts"]), 2)

    def test_duration_budget_stops_before_next_job_and_resume_finishes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            first = self.control(root, "first", "pass")
            second = self.control(root, "second", "pass")
            plan = self.plan(
                root,
                [
                    self.job(0, script, first, "job-one"),
                    self.job(1, script, second, "job-two"),
                ],
            )
            config = self.config(root)
            config.max_duration_seconds = 1.0
            times = iter([0.0, 0.0, 2.0])

            exhausted = run_local_stress_campaign.run_campaign(
                config,
                plan=plan,
                working_directory=root,
                monotonic=lambda: next(times),
            )

            self.assertEqual(exhausted.status, "budget_exhausted")
            self.assertEqual(exhausted.passed, 1)
            self.assertEqual(exhausted.pending, 1)
            self.assertFalse((config.workspace / "completion.txt").exists())

            config.resume = True
            config.max_duration_seconds = 0.0
            completed = run_local_stress_campaign.run_campaign(
                config, plan=plan, working_directory=root
            )
            self.assertEqual(completed.status, "completed")

    def test_workspace_modes_reject_existing_new_and_missing_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = self.fake_job(root)
            control = self.control(root, "first", "pass")
            plan = self.plan(
                root, [self.job(0, script, control, "job-one")]
            )
            config = self.config(root)
            config.workspace.mkdir()
            sentinel = config.workspace / "sentinel"
            sentinel.write_text("keep", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "already exists"):
                run_local_stress_campaign.run_campaign(
                    config, plan=plan, working_directory=root
                )
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")

            config.workspace = root / "missing"
            config.resume = True
            with self.assertRaisesRegex(ValueError, "does not exist"):
                run_local_stress_campaign.run_campaign(
                    config, plan=plan, working_directory=root
                )

    def test_artifact_identity_normalizes_boolean_and_worker_dirty_names(self):
        cases = [("false", False), ("clean", False), ("true", True), ("dirty", True)]
        for dirty, expected in cases:
            with self.subTest(dirty=dirty):
                with tempfile.TemporaryDirectory() as temporary:
                    attempt = Path(temporary)
                    artifacts = attempt / "artifacts"
                    artifacts.mkdir()
                    (artifacts / "run.txt").write_text(
                        "\n".join(
                            [
                                "format=modern-leveldb-crash-run-v1",
                                "revision=revision",
                                f"dirty={dirty}",
                                "",
                            ]
                        ),
                        encoding="utf-8",
                    )
                    (artifacts / "completion.txt").write_text(
                        "format=modern-leveldb-crash-result-v1\n",
                        encoding="utf-8",
                    )
                    job = run_local_stress_campaign.CampaignJob(
                        index=0,
                        job_id="v3",
                        tier="v3",
                        seed=17,
                        argv_template=(sys.executable, "-c", ""),
                        run_artifact="artifacts/run.txt",
                        completion_artifact="artifacts/completion.txt",
                    )

                    identity = run_local_stress_campaign.validate_job_artifacts(
                        job, attempt
                    )

                    self.assertEqual(identity.revision, "revision")
                    self.assertEqual(identity.dirty, expected)


class LocalStressCampaignCliTest(unittest.TestCase):
    def arguments(self) -> list[str]:
        return [
            "--stress-dir",
            "stress",
            "--workspace",
            "workspace",
            "--seeds",
            "17",
            "--tiers",
            "v4",
        ]

    def summary(self, status: str) -> run_local_stress_campaign.CampaignSummary:
        return run_local_stress_campaign.CampaignSummary(
            status=status,
            total=1,
            passed=1 if status == "completed" else 0,
            failed=1 if status == "failed" else 0,
            pending=1 if status == "budget_exhausted" else 0,
            revision="revision",
            dirty=False,
            blocking_job="job" if status == "failed" else None,
        )

    def test_main_maps_completed_failed_and_budget_statuses(self):
        cases = [("completed", 0), ("failed", 1), ("budget_exhausted", 3)]
        for status, expected in cases:
            with self.subTest(status=status):
                output = io.StringIO()
                with mock.patch.object(
                    run_local_stress_campaign,
                    "run_campaign",
                    return_value=self.summary(status),
                ), contextlib.redirect_stdout(output):
                    result = run_local_stress_campaign.main(self.arguments())
                self.assertEqual(result, expected)
                self.assertIn(f"status={status}", output.getvalue())

    def test_main_maps_usage_runtime_and_interrupt_failures(self):
        cases = [
            (ValueError("bad usage"), 2, "bad usage"),
            (RuntimeError("broken"), 1, "broken"),
            (KeyboardInterrupt(), 130, "interrupted"),
        ]
        for error, expected, message in cases:
            with self.subTest(expected=expected):
                output = io.StringIO()
                with mock.patch.object(
                    run_local_stress_campaign,
                    "run_campaign",
                    side_effect=error,
                ), contextlib.redirect_stderr(output):
                    result = run_local_stress_campaign.main(self.arguments())
                self.assertEqual(result, expected)
                self.assertIn(message, output.getvalue())


if __name__ == "__main__":
    unittest.main()
