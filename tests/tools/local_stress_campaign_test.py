from pathlib import Path
import os
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
