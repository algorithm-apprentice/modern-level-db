import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_benchmark import check_report


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


if __name__ == "__main__":
    unittest.main()
