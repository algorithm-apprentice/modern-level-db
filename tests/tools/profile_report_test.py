from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from profile_report import summarize_trace

CASE = "modern/readrandom/4096"

TOC = """<trace-toc><run number="1"><info><target>
<process type="launched" pid="42" return-exit-status="0" termination-reason="exit(0)"/>
</target></info></run></trace-toc>"""

MARKERS = f"""<trace-query-result><node/>
<node><row>
<event-time id="b">10</event-time>
<thread id="thread"><tid>7</tid></thread>
<process id="process"><pid>42</pid></process>
<subsystem id="subsystem">modern_leveldb.profiling</subsystem>
<signpost-name id="name">workload</signpost-name>
<event-type id="begin">Begin</event-type><os-signpost-identifier id="i">1</os-signpost-identifier>
<os-log-metadata id="small"><string id="case">{CASE}</string><uint64>10</uint64></os-log-metadata>
</row><row>
<event-time>20</event-time><thread ref="thread"/><process ref="process"/>
<subsystem ref="subsystem"/><signpost-name ref="name"/>
<event-type id="end">End</event-type><os-signpost-identifier ref="i"/>
<os-log-metadata ref="small"/>
</row></node>
<node><row>
<event-time id="measured_begin">100</event-time><thread ref="thread"/><process ref="process"/>
<subsystem ref="subsystem"/><signpost-name ref="name"/>
<event-type ref="begin"/><os-signpost-identifier id="j">2</os-signpost-identifier>
<os-log-metadata id="large" fmt="iterations=1,000">
<string ref="case"/><uint64 fmt="1,000">1000</uint64></os-log-metadata>
</row><row>
<event-time>200</event-time><thread ref="thread"/><process ref="process"/>
<subsystem ref="subsystem"/><signpost-name ref="name"/>
<event-type ref="end"/><os-signpost-identifier ref="j"/><os-log-metadata ref="large"/>
</row></node><node><row>
<event-time ref="measured_begin"/><thread ref="thread"/><process ref="process"/>
<subsystem ref="subsystem"/><signpost-name ref="name"/>
<event-type ref="begin"/><os-signpost-identifier ref="j"/><os-log-metadata ref="large"/>
</row></node></trace-query-result>"""

SAMPLES = """<trace-query-result><node>
<row><sample-time>50</sample-time><process id="p"><pid>42</pid></process>
<thread id="t"><tid>7</tid></thread><weight id="weight">1000000</weight>
<tagged-backtrace><frame name="Prepare"><binary id="bin" name="profile-program"/></frame></tagged-backtrace></row>
<row><sample-time id="inside">150</sample-time><process ref="p"/><thread ref="t"/>
<weight ref="weight"/><tagged-backtrace id="stack"><frame id="f" name="Read">
<binary ref="bin"/></frame><frame name="RunReadRandom"><binary ref="bin"/></frame>
</tagged-backtrace></row>
<row><sample-time ref="inside"/><process ref="p"/><thread ref="t"/>
<weight ref="weight"/><tagged-backtrace ref="stack"/></row>
<row><sample-time>170</sample-time><process ref="p"/><thread><tid>8</tid></thread>
<weight ref="weight"/><tagged-backtrace><frame name="Compaction"><binary ref="bin"/></frame></tagged-backtrace></row>
<row><sample-time>180</sample-time><process><pid>99</pid></process><thread ref="t"/>
<weight ref="weight"/><tagged-backtrace ref="stack"/></row>
<row><sample-time>200</sample-time><process ref="p"/><thread ref="t"/>
<weight ref="weight"/><tagged-backtrace ref="stack"/></row>
</node></trace-query-result>"""


class ProfileReportTest(unittest.TestCase):
    def report(self, toc=TOC, markers=MARKERS, samples=SAMPLES, iterations=1000):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, text in (("toc", toc), ("markers", markers), ("samples", samples)):
                (root / name).write_text(text)
            return summarize_trace(root / "toc", root / "markers", root / "samples", CASE, iterations)

    def test_resolves_references_deduplicates_and_excludes_preparation_and_other_processes(self):
        report = self.report()
        self.assertEqual(report["window"]["start_ns"], 100)
        self.assertEqual(report["window"]["iterations"], 1000)
        self.assertEqual(report["window"]["calibration_intervals"], 1)
        self.assertEqual(report["sample_count"], 2)
        self.assertEqual(report["sample_weight_ns"], 2_000_000)
        self.assertTrue(report["low_confidence"])
        self.assertNotIn("Prepare", {entry["symbol"] for entry in report["inclusive"]})
        self.assertEqual({thread["role"] for thread in report["threads"]}, {"foreground", "background"})
        self.assertEqual(sum(entry["percent"] for entry in report["self"]), 100.0)
        self.assertGreater(sum(entry["percent"] for entry in report["inclusive"]), 100.0)

    def test_rejects_failed_or_unlaunched_targets_and_multiple_runs(self):
        for toc in (
            TOC.replace('return-exit-status="0"', 'return-exit-status="7"'),
            TOC.replace('termination-reason="exit(0)"', 'termination-reason="SIGKILL"'),
            TOC.replace('type="launched"', 'type="attached"'),
            TOC.replace("</trace-toc>", '<run number="2"/></trace-toc>'),
        ):
            with self.subTest(toc=toc), self.assertRaises(ValueError):
                self.report(toc=toc)

    def test_rejects_unpaired_conflicting_overlapping_and_mismatched_intervals(self):
        for markers in (
            MARKERS.replace('<event-time>200</event-time>', '<event-time>99</event-time>'),
            MARKERS.replace('<event-time ref="measured_begin"/>', '<event-time>101</event-time>'),
            MARKERS.replace('<os-signpost-identifier ref="j"/>', '<os-signpost-identifier>3</os-signpost-identifier>', 1),
            MARKERS.replace(">1000</uint64>", ">999</uint64>"),
            MARKERS.replace("<event-time>20</event-time>", "<event-time>110</event-time>"),
            MARKERS.replace(CASE, "leveldb/scan/4096"),
        ):
            with self.subTest(markers=markers), self.assertRaises(ValueError):
                self.report(markers=markers)
        with self.assertRaises(ValueError):
            self.report(iterations=10)

    def test_rejects_missing_or_cyclic_references_and_empty_samples(self):
        for samples in (
            SAMPLES.replace('<process ref="p"/>', '<process ref="absent"/>'),
            "<trace-query-result><node/></trace-query-result>",
            SAMPLES.replace('<sample-time id="inside">150</sample-time>',
                            '<sample-time id="inside" ref="inside"/>'),
            SAMPLES.replace('<weight id="weight">1000000</weight>', '<weight id="weight">0</weight>'),
        ):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                self.report(samples=samples)
        with self.assertRaises(ET.ParseError):
            self.report(toc="<broken")

    def test_reports_unresolved_frames_without_fabricating_symbols(self):
        report = self.report(samples=SAMPLES.replace('name="Read"', 'name="0x1234"'))
        self.assertEqual(report["samples_with_unresolved_frames"], 1)

    def test_rejects_captures_without_a_symbolized_workload(self):
        with self.assertRaises(ValueError):
            self.report(samples=SAMPLES.replace('name="RunReadRandom"', 'name="0xabcd"'))


if __name__ == "__main__":
    unittest.main()
