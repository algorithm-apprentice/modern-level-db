import bisect
from fractions import Fraction
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "docs" / "learning" / "examples"
sys.path.insert(0, str(EXAMPLES))
import restart_search


class RestartSearchModelTest(unittest.TestCase):
    def test_literal_encoding_distinguishes_restarts_from_other_complete_keys(self):
        block = restart_search.encode_block(restart_search.DEMO_KEYS, 4)
        self.assertEqual(block.restarts, (0, 4, 8))
        self.assertEqual(block.entries, (
            (0, b"ant"), (3, b"eater"), (4, b"lope"), (0, b"bat"),
            (0, b"batch"), (3, b"h"), (0, b"cat"), (3, b"alog"),
            (0, b"cater"), (0, b"dog"), (2, b"ve"), (0, b"eel"),
        ))
        self.assertEqual(block.entries[3][0], 0)
        self.assertNotIn(3, block.restarts)

    def test_exact_next_restart_needs_the_k_plus_one_boundary(self):
        block = restart_search.encode_block(restart_search.DEMO_KEYS, 4)
        found = restart_search.fresh_lower_bound(block, b"cater")
        self.assertEqual((found.index, found.key, found.restart_entry), (8, b"cater", 4))
        self.assertEqual((found.restart_comparisons, found.decoded_entries), (2, 5))

    def test_before_first_after_last_and_empty_blocks(self):
        block = restart_search.encode_block(restart_search.DEMO_KEYS, 4)
        first = restart_search.fresh_lower_bound(block, b"aardvark")
        self.assertEqual((first.index, first.key, first.decoded_entries), (0, b"ant", 1))
        last = restart_search.fresh_lower_bound(block, b"zoo")
        self.assertEqual((last.index, last.key, last.restart_entry, last.decoded_entries),
                         (None, None, 8, 4))
        empty = restart_search.fresh_lower_bound(restart_search.encode_block([], 4), b"a")
        self.assertEqual(empty, restart_search.SearchResult(None, None, None, 0, 0))

    def test_all_small_binary_subsets_match_independent_lower_bound_and_cost_bounds(self):
        universe = (b"", b"\x00", b"\x00a", b"a", b"aa", b"ab", b"b", b"\xff")
        queries = (*universe, b"\x00\x00", b"\x01", b"a\x00", b"ac", b"ba", b"\xff\x00")
        for mask in range(1 << len(universe)):
            keys = tuple(key for i, key in enumerate(universe) if mask & (1 << i))
            for interval in range(1, 11):
                block = restart_search.encode_block(keys, interval)
                self.assertEqual(len(block.restarts),
                                 (len(keys) + interval - 1) // interval)
                for query in queries:
                    actual = restart_search.fresh_lower_bound(block, query)
                    expected = bisect.bisect_left(keys, query)
                    self.assertEqual(actual.index, expected if expected < len(keys) else None)
                    self.assertEqual(actual.key, keys[expected] if expected < len(keys) else None)
                    self.assertLessEqual(actual.decoded_entries, min(len(keys), interval + 1))
                    comparisons = (len(block.restarts) - 1).bit_length() if keys else 0
                    self.assertLessEqual(actual.restart_comparisons, comparisons)

    def test_long_prefixes_do_not_change_the_entry_count_bound(self):
        prefix = b"x" * 1024
        keys = tuple(prefix + i.to_bytes(2, "big") for i in range(0, 200, 2))
        for interval in (1, 4, 16, 101):
            block = restart_search.encode_block(keys, interval)
            for value in range(201):
                query = prefix + value.to_bytes(2, "big")
                actual = restart_search.fresh_lower_bound(block, query)
                expected = bisect.bisect_left(keys, query)
                self.assertEqual(actual.index, expected if expected < len(keys) else None)
                self.assertLessEqual(actual.decoded_entries, min(len(keys), interval + 1))

    def test_uniform_interior_region_keeps_the_exact_restart_cost(self):
        for interval in range(1, 9):
            keys = tuple(bytes([i]) for i in range(3 * interval))
            block = restart_search.encode_block(keys, interval)
            interior = sum(restart_search.fresh_lower_bound(block, key).decoded_entries
                           for key in keys[interval:2 * interval])
            first = sum(restart_search.fresh_lower_bound(block, key).decoded_entries
                        for key in keys[:interval])
            self.assertEqual(Fraction(interior, interval), Fraction(interval + 3, 2))
            self.assertEqual(Fraction(first, interval), Fraction(interval + 1, 2))

    def test_model_rejects_invalid_builder_inputs(self):
        for interval in (0, -1, 1.5, True):
            with self.assertRaises(ValueError):
                restart_search.encode_block([b"a"], interval)
        for keys in ([b"b", b"a"], [b"a", b"a"]):
            with self.assertRaises(ValueError):
                restart_search.encode_block(keys, 4)
        with self.assertRaises(TypeError):
            restart_search.encode_block(["a"], 4)
        with self.assertRaises(TypeError):
            restart_search.fresh_lower_bound(restart_search.encode_block([b"a"], 4), "a")

    def test_changing_upper_midpoint_to_floor_can_prevent_progress(self):
        restarts = (b"ant", b"batch")
        left, right = 0, 1
        middle = (left + right) // 2
        self.assertLess(restarts[middle], b"zoo")
        new_left = middle
        self.assertEqual((new_left, right), (left, right))

    def test_demo_is_runnable_without_an_engine_build(self):
        completed = subprocess.run([sys.executable, str(EXAMPLES / "restart_search.py")],
                                   check=True, capture_output=True, text=True, timeout=10)
        self.assertEqual(len(completed.stdout.splitlines()), 3)
        self.assertIn("decoded=5", completed.stdout)
        self.assertIn("key=None", completed.stdout)


class RestartSearchLessonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = (ROOT / "docs" / "learning" / "05a-restart-search.md").read_text(
            encoding="utf-8")

    def test_actual_worked_encoding_table_matches_the_independent_literal(self):
        rows = re.findall(
            r"^\| (\d+) \| `([^`]*)` \| (\d+) \| `([^`]*)` \| (yes|no) \|$",
            self.text, re.MULTILINE)
        self.assertEqual(len(rows), 12)
        self.assertEqual([int(row[0]) for row in rows], list(range(12)))
        keys = tuple(row[1].encode("ascii") for row in rows)
        self.assertEqual(keys, restart_search.DEMO_KEYS)
        block = restart_search.encode_block(keys, 4)
        self.assertEqual(tuple((int(row[2]), row[3].encode("ascii")) for row in rows),
                         block.entries)
        self.assertEqual(tuple(int(row[0]) for row in rows if row[4] == "yes"),
                         block.restarts)

    def test_actual_query_table_has_the_claimed_answers_and_counts(self):
        rows = re.findall(
            r"^\| `([^`]*)` \| `([^`]*)` \| (\d+) \| (\d+) \| (\d+) \|$",
            self.text, re.MULTILINE)
        self.assertEqual(len(rows), 5)
        block = restart_search.encode_block(restart_search.DEMO_KEYS, 4)
        for query, key, restart, comparisons, decoded in rows:
            result = restart_search.fresh_lower_bound(block, query.encode("ascii"))
            self.assertEqual(result.key, None if key == "none" else key.encode("ascii"))
            self.assertEqual((result.restart_entry, result.restart_comparisons,
                              result.decoded_entries),
                             (int(restart), int(comparisons), int(decoded)))

    def test_actual_cost_table_and_budget_exercise_match_the_declared_model(self):
        rows = re.findall(r"^\| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \|$",
                          self.text, re.MULTILINE)
        self.assertEqual(len(rows), 4)
        feasible = []
        for interval, count, metadata, binary, decoded, total in map(
                lambda row: tuple(map(int, row)), rows):
            restarts = (12 + interval - 1) // interval
            self.assertEqual((count, metadata, binary, decoded, total),
                             (restarts, 4 * restarts + 4, (restarts - 1).bit_length(),
                              min(12, interval + 1),
                              (restarts - 1).bit_length() + min(12, interval + 1)))
            if metadata <= 20:
                feasible.append((total, interval))
        self.assertEqual(min(feasible)[1], 4)


if __name__ == "__main__":
    unittest.main()
