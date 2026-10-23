from fractions import Fraction
import itertools
import math
from pathlib import Path
import statistics
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "docs" / "learning" / "examples"))
import retention


def visible(records, sequence):
    eligible = [record for record in records if record.sequence <= sequence]
    return max(eligible, key=lambda record: record.sequence).value if eligible else None


def table_rows(filename, marker):
    text = (ROOT / "docs" / "learning" / filename).read_text(encoding="utf-8")
    table = text.split("<!-- " + marker + " -->", 1)[1].strip().split("\n\n", 1)[0]
    return [[cell.strip().strip("`") for cell in line.strip("|").split("|")]
            for line in table.splitlines()[2:]]


class RetentionModelTest(unittest.TestCase):
    def test_worked_history_and_tombstone_cases(self):
        version = retention.Version
        values = tuple(version(s, str(s).encode()) for s in (105, 101, 99, 70))
        self.assertEqual(retention.compact_key(values, 100, False), values[:3])
        deleted = (version(90, None), version(70, b"old"))
        self.assertEqual(retention.compact_key(deleted, 80, False), deleted)
        self.assertEqual(retention.compact_key(deleted, 100, True), deleted[:1])
        self.assertEqual(retention.compact_key(deleted, 100, False), ())

    def test_all_small_histories_preserve_every_protected_read(self):
        version = retention.Version
        for count in range(6):
            for sequences in itertools.combinations(range(1, 6), count):
                ordered = tuple(reversed(sequences))
                for deletions in itertools.product((False, True), repeat=count):
                    records = tuple(version(s, None if deleted else str(s).encode())
                                    for s, deleted in zip(ordered, deletions))
                    for oldest in range(7):
                        for deeper_present in (False, True):
                            deeper = (version(0, b"deeper"),) if deeper_present else ()
                            kept = retention.compact_key(records, oldest, deeper_present)
                            for query in range(oldest, 7):
                                self.assertEqual(visible((*records, *deeper), query),
                                                 visible((*kept, *deeper), query))

    def test_wrong_base_level_assumption_resurrects_a_deeper_value(self):
        records = (retention.Version(3, None),)
        deeper = (retention.Version(0, b"old"),)
        wrong = retention.compact_key(records, 3, False)
        self.assertIsNone(visible((*records, *deeper), 3))
        self.assertEqual(visible((*wrong, *deeper), 3), b"old")

    def test_wrong_oldest_snapshot_can_destroy_a_registered_view(self):
        records = (retention.Version(4, b"new"), retention.Version(2, b"old"))
        wrong = retention.compact_key(records, 4, False)
        self.assertEqual(visible(records, 2), b"old")
        self.assertIsNone(visible(wrong, 2))

    def test_empty_value_is_not_an_obsolete_deletion(self):
        record = retention.Version(0, b"")
        kept = retention.compact_key((record,), 0, False)
        self.assertEqual(kept, (record,))
        self.assertEqual(visible(kept, 0), b"")

    def test_invalid_model_domains_are_explicit(self):
        for records in ((retention.Version(1, b"a"), retention.Version(2, b"b")),
                        (retention.Version(1, b"a"), retention.Version(1, b"b"))):
            with self.assertRaises(ValueError):
                retention.compact_key(records, 0, False)
        with self.assertRaises(ValueError):
            retention.compact_key((), -1, False)


class CourseWorkedExamplesTest(unittest.TestCase):
    def test_foreground_example_does_not_include_hidden_background_work(self):
        rows = table_rows("01-the-engine-map.md", "foreground-cost-example")
        self.assertEqual(rows, [["Rewrite sorted file", "4", "8", "32"],
                                ["Append update log", "4", "1", "4"]])
        for _, count, per_update, total in rows:
            self.assertEqual(int(count) * int(per_update), int(total))

    def test_canonical_varint_table_uses_independent_base128_expansion(self):
        rows = table_rows("02-bytes-and-formats.md", "canonical-varint-examples")
        self.assertEqual(len(rows), 7)
        for value, encoded, length in rows:
            number = int(value)
            remaining = number
            expected = []
            while remaining >= 128:
                expected.append((remaining % 128) + 128)
                remaining //= 128
            expected.append(remaining)
            self.assertEqual(bytes.fromhex(encoded), bytes(expected))
            self.assertEqual(int(length), max(1, (number.bit_length() + 6) // 7))

    def test_skip_height_table_has_the_declared_probability_and_population(self):
        rows = table_rows("03-memory-and-mvcc.md", "skip-height-model")
        self.assertEqual(len(rows), 6)
        for level, probability, expected_nodes in rows:
            self.assertEqual(Fraction(probability), Fraction(1, 4 ** int(level)))
            self.assertEqual(Fraction(expected_nodes), 1024 * Fraction(probability))

    def test_durability_cuts_separate_guarantees_from_unsynced_possibility(self):
        rows = table_rows("04-wal-and-recovery.md", "install-crash-cuts")
        self.assertEqual(len(rows), 5)
        self.assertEqual(rows[3][1:], ["yes", "yes", "no", "old or new"])
        self.assertEqual(rows[4][1:], ["yes", "yes", "yes", "new"])
        self.assertTrue(all(row[4] == "old" for row in rows[:3]))

    def test_bloom_table_is_an_explicit_approximation_not_an_exact_rate(self):
        rows = table_rows("05-tables-filters-and-caches.md", "bloom-model-costs")
        self.assertEqual(len(rows), 4)
        for bits_per_key, probes, percentage in rows:
            ratio = int(bits_per_key)
            k = max(1, min(30, ratio * 69 // 100))
            self.assertEqual(int(probes), k)
            probability = (1 - math.exp(-k / ratio)) ** k
            self.assertAlmostEqual(float(percentage), probability * 100, delta=0.0051)

    def test_retention_table_uses_the_same_explicit_snapshot_and_base_contract(self):
        rows = table_rows("07-writes-and-compaction.md", "retention-worked-cases")
        self.assertEqual(len(rows), 4)
        for oldest, deeper, source, expected in rows:
            records = tuple(retention.Version(int(token[:-1]),
                                               None if token[-1] == "D" else b"value")
                            for token in source.split(","))
            actual = retention.compact_key(records, int(oldest), deeper == "yes")
            labels = ",".join(str(r.sequence) + ("D" if r.value is None else "V")
                              for r in actual) or "none"
            self.assertEqual(labels, expected)

    def test_paired_reporting_example_distinguishes_noninterchangeable_medians(self):
        rows = table_rows("09-verification-and-performance.md", "paired-cost-example")
        self.assertEqual(len(rows), 2)
        before = [Fraction(row[1]) for row in rows]
        after = [Fraction(row[2]) for row in rows]
        paired = statistics.median(a / b for a, b in zip(after, before)) - 1
        ratio_of_medians = statistics.median(after) / statistics.median(before) - 1
        self.assertEqual(paired, Fraction(-1, 40))
        self.assertEqual(ratio_of_medians, 0)
        self.assertNotEqual(statistics.median(1 / value for value in before),
                            1 / statistics.median(before))


if __name__ == "__main__":
    unittest.main()
