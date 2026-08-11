#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/test_ctest_summary.py — self-tests for scripts/ctest_summary.py
# (task 12.11; Requirements 15.5, 15.7).
#
# Run with:  python3 scripts/test_ctest_summary.py
#
# Every XML fixture below is *captured from real ctest output* on this tree
# (`ctest --test-dir build-nogui -R ... --output-junit`), not invented, because
# the one thing this generator must get right is the shape CTest actually emits:
# `<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>` with the reason the test
# recorded buried in `<system-out>`. A fixture written from memory would happily
# agree with a generator that reads the useless attribute.

import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ctest_summary as cs  # noqa: E402

# Captured verbatim from `ctest --test-dir build-nogui -R
# "RejectsAnUnwritableParentDirectory|HardwareAndSoftware" --output-junit`,
# trimmed only by removing gtest banner lines that carry no information here.
REAL_SKIPS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux-c++" tests="2" failures="0" disabled="0" skipped="2" time="0">
\t<testcase name="ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory" \
classname="ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory" time="0.00791232" \
status="notrun">
\t\t<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>
\t\t<system-out>[ RUN      ] ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory
/src/tests/services/export_coordinator_test.cpp:465: Skipped
this user can write to a read-only directory; the writability rejection is not observable

[  SKIPPED ] ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory (0 ms)
[----------] 1 test from ExportCoordinatorValidate (0 ms total)
</system-out>
\t</testcase>
\t<testcase name="ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixture" \
classname="ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixture" \
time="0.0282396" status="notrun">
\t\t<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>
\t\t<system-out>[ RUN      ] ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixture
/src/tests/services/export_hardware_software_comparison_test.cpp:428: Skipped
Requirement 8.6's hardware-versus-software comparison cannot run on this host, because \
neither half of the comparison is available:
  * no hardware encoder: no vendor hardware encode path is compiled in (PALMIER_HAVE_NVENC, \
PALMIER_HAVE_VAAPI and PALMIER_HAVE_QSV are all undefined)
  * no software encoder to compare against: libavcodec on this host carries no software \
H.264 encoder ("libx264")
The comparison needs BOTH, so it is reported as skipped rather than failed (Requirement 15.5).

[  SKIPPED ] ExportHardwareSoftwareComparisonTest.HardwareAndSoftwareExportsOfTheFixture (18 ms)
</system-out>
\t</testcase>
</testsuite>
"""

# Captured verbatim from a throwaway ctest project exercising one passing, one
# failing and one skipped test, to pin the pass/fail shapes CTest emits.
REAL_MIXED_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="4" failures="1" disabled="0" skipped="1" time="0">
\t<testcase name="passing" classname="passing" time="0.00539728" status="run">
\t\t<system-out>hello
</system-out>
\t</testcase>
\t<testcase name="failing" classname="failing" time="0.00536362" status="fail">
\t\t<failure message=""/>
\t\t<system-out>a line of output
the assertion that failed
</system-out>
\t</testcase>
\t<testcase name="timing-out" classname="timing-out" time="600.1" status="fail">
\t\t<failure message="Timeout"/>
\t\t<system-out></system-out>
\t</testcase>
\t<testcase name="skipped-without-a-reason" classname="skipped-without-a-reason" time="0.004" \
status="notrun">
\t\t<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>
\t\t<system-out>Skipped
</system-out>
\t</testcase>
</testsuite>
"""


def write(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


class SkipReasonExtraction(unittest.TestCase):
    def test_a_single_line_reason_is_extracted_with_its_location(self):
        reasons = cs.extract_skip_reasons(
            "[ RUN      ] X.Y\n"
            "/src/tests/x_test.cpp:12: Skipped\n"
            "the widget is absent\n"
            "\n"
            "[  SKIPPED ] X.Y (0 ms)\n"
        )
        self.assertEqual(len(reasons), 1)
        self.assertEqual(reasons[0].location, "/src/tests/x_test.cpp:12")
        self.assertEqual(reasons[0].text, "the widget is absent")

    def test_a_multi_line_bulleted_reason_survives_intact(self):
        summary = cs.Summary()
        reasons = cs.extract_skip_reasons(
            "/src/x.cpp:1: Skipped\n"
            "two halves are missing:\n"
            "  * no hardware encoder: PALMIER_HAVE_NVENC undefined\n"
            "  * no software encoder: no libx264\n"
            "so it skips.\n"
            "\n"
            "[  SKIPPED ] X.Y (1 ms)\n"
        )
        del summary
        self.assertEqual(len(reasons), 1)
        self.assertEqual(
            reasons[0].text,
            "two halves are missing:\n"
            "  * no hardware encoder: PALMIER_HAVE_NVENC undefined\n"
            "  * no software encoder: no libx264\n"
            "so it skips.",
        )

    def test_two_recorded_skips_are_both_reported(self):
        reasons = cs.extract_skip_reasons(
            "/src/a.cpp:1: Skipped\nfirst reason\n\n[  SKIPPED ] A.B (0 ms)\n"
            "/src/b.cpp:2: Skipped\nsecond reason\n\n[  SKIPPED ] C.D (0 ms)\n"
        )
        self.assertEqual([r.text for r in reasons], ["first reason", "second reason"])

    def test_output_with_no_recorded_reason_yields_none(self):
        self.assertEqual(cs.extract_skip_reasons("[  SKIPPED ] X.Y (0 ms)\n"), [])


class ParsingRealCtestOutput(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmpdir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def test_skips_are_classified_and_carry_their_recorded_reason(self):
        summary = cs.parse_junit(write(self.tmpdir, "skips.xml", REAL_SKIPS_XML))
        self.assertEqual(summary.total, 2)
        self.assertEqual(summary.count(cs.SKIPPED), 2)
        self.assertEqual(summary.count(cs.PASSED), 0)
        self.assertEqual(summary.count(cs.FAILED), 0)
        # The reason must be the test's own, never the attribute placeholder.
        first = summary.of(cs.SKIPPED)[0]
        self.assertIn("this user can write to a read-only directory", first.skip_reason_text)
        self.assertNotIn("SKIP_REGULAR_EXPRESSION_MATCHED", first.skip_reason_text)
        second = summary.of(cs.SKIPPED)[1]
        self.assertIn("no hardware encoder:", second.skip_reason_text)
        self.assertIn("no software encoder to compare against:", second.skip_reason_text)

    def test_pass_fail_timeout_and_reasonless_skip_are_classified(self):
        summary = cs.parse_junit(write(self.tmpdir, "mixed.xml", REAL_MIXED_XML))
        self.assertEqual(summary.total, 4)
        self.assertEqual(summary.count(cs.PASSED), 1)
        self.assertEqual(summary.count(cs.FAILED), 2)
        self.assertEqual(summary.count(cs.SKIPPED), 1)
        self.assertEqual(summary.count(cs.NOT_RUN), 0)
        self.assertEqual(summary.of(cs.FAILED)[1].failure_message, "Timeout")
        # A skip with no recorded reason says exactly that, rather than passing
        # the placeholder off as a reason.
        reasonless = summary.of(cs.SKIPPED)[0].skip_reason_text
        self.assertIn("no reason recorded", reasonless)

    def test_a_notrun_case_with_neither_element_is_not_counted_as_passed(self):
        xml = (
            '<?xml version="1.0"?><testsuite name="x" tests="1">'
            '<testcase name="vanished" classname="vanished" time="0" status="notrun">'
            "<system-out></system-out></testcase></testsuite>"
        )
        summary = cs.parse_junit(write(self.tmpdir, "notrun.xml", xml))
        self.assertEqual(summary.count(cs.PASSED), 0)
        self.assertEqual(summary.count(cs.NOT_RUN), 1)
        self.assertEqual(summary.unsuccessful, 1)


class MarkdownRendering(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmpdir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def test_the_report_lists_counts_every_test_and_the_skip_reasons(self):
        summary = cs.parse_junit(write(self.tmpdir, "skips.xml", REAL_SKIPS_XML))
        report = cs.render_markdown(summary)
        self.assertIn("| Passed | 0 |", report)
        self.assertIn("| Skipped | 2 |", report)
        self.assertIn("| **Total** | **2** |", report)
        self.assertIn("## Skipped (2)", report)
        self.assertIn("## Every test", report)
        for result in summary.results:
            self.assertIn(result.name, report)
        self.assertIn("the writability rejection is not observable", report)
        self.assertIn("PALMIER_HAVE_NVENC", report)

    def test_a_failure_inlines_the_tail_of_its_output(self):
        summary = cs.parse_junit(write(self.tmpdir, "mixed.xml", REAL_MIXED_XML))
        report = cs.render_markdown(summary, failure_log_lines=5)
        self.assertIn("## Failed (2)", report)
        self.assertIn("the assertion that failed", report)
        self.assertIn("ctest reported: Timeout", report)

    def test_table_cells_do_not_break_the_table(self):
        xml = (
            '<?xml version="1.0"?><testsuite name="x" tests="1">'
            '<testcase name="pipes|and" classname="pipes|and" time="0" status="notrun">'
            '<skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>'
            "<system-out>/a.cpp:1: Skipped\nline one|piped\nline two\n\n"
            "[  SKIPPED ] pipes|and (0 ms)\n</system-out></testcase></testsuite>"
        )
        summary = cs.parse_junit(write(self.tmpdir, "pipes.xml", xml))
        report = cs.render_markdown(summary)
        row = [
            line
            for line in report.splitlines()
            if line.startswith("| `pipes") and "skipped" in line
        ]
        self.assertEqual(len(row), 1)
        # Five unescaped pipes: the four column delimiters plus the trailing one.
        delimiters = re.findall(r"(?<!\\)\|", row[0])
        self.assertEqual(len(delimiters), 5, row[0])
        self.assertNotIn("line one|piped", row[0])
        self.assertIn("line one\\|piped", row[0])

    def test_text_format_lists_every_test_with_its_reason(self):
        summary = cs.parse_junit(write(self.tmpdir, "skips.xml", REAL_SKIPS_XML))
        text = cs.render_text(summary)
        self.assertIn("passed=0 failed=0 skipped=2 total=2", text)
        self.assertIn("SKIPPED  ExportCoordinatorValidate.RejectsAnUnwritableParentDirectory", text)
        self.assertIn("this user can write to a read-only directory", text)


class CommandLine(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmpdir = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def test_writing_to_a_file_succeeds_when_nothing_failed(self):
        source = write(self.tmpdir, "skips.xml", REAL_SKIPS_XML)
        target = os.path.join(self.tmpdir, "nested", "summary.md")
        self.assertEqual(cs.main([source, "-o", target, "--fail-on-failure"]), 0)
        with open(target, encoding="utf-8") as handle:
            self.assertIn("## Skipped (2)", handle.read())

    def test_fail_on_failure_exits_nonzero_only_with_a_failure(self):
        source = write(self.tmpdir, "mixed.xml", REAL_MIXED_XML)
        target = os.path.join(self.tmpdir, "summary.md")
        self.assertEqual(cs.main([source, "-o", target]), 0)
        self.assertEqual(cs.main([source, "-o", target, "--fail-on-failure"]), 1)

    def test_a_missing_file_is_an_error_unless_allowed(self):
        missing = os.path.join(self.tmpdir, "absent.xml")
        target = os.path.join(self.tmpdir, "summary.md")
        self.assertEqual(cs.main([missing, "-o", target]), 2)
        self.assertEqual(cs.main([missing, "-o", target, "--allow-missing"]), 0)
        with open(target, encoding="utf-8") as handle:
            self.assertIn("No JUnit results file was produced", handle.read())

    def test_unparseable_xml_is_reported_rather_than_crashing(self):
        broken = write(self.tmpdir, "broken.xml", "<testsuite><testcase")
        self.assertEqual(cs.main([broken, "-o", os.path.join(self.tmpdir, "s.md")]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
