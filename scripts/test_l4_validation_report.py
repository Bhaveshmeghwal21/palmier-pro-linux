#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/test_l4_validation_report.py — self-tests for
# scripts/l4_validation_report.py (task 12.12; Requirements 8.5, 8.10).
#
# Run with:  python3 scripts/test_l4_validation_report.py
#
# The measurement block can only be produced by an export that really used
# `h264_nvenc`, which needs an NVIDIA L4 — so unlike the ctest summary
# generator, this parser cannot be fed real output on a GPU-less host. Two things
# are done about that instead of pretending otherwise:
#
#   1. `MeasurementBlockMatchesTheEmittingTest` reads
#      tests/services/export_hardware_software_comparison_test.cpp and checks the
#      sentinels and key names this script looks for against the ones that file
#      actually prints, so the two cannot drift apart unnoticed.
#   2. The log fixtures below are wrapped in `ctest -V`'s real line prefix
#      (`1204: `), because that is what the job feeds in.

import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import l4_validation_report as l4  # noqa: E402

EMITTING_TEST_SOURCE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "tests",
    "services",
    "export_hardware_software_comparison_test.cpp",
)


def ctest_log(
    encoder="h264_nvenc",
    elapsed_ms=8421,
    output_bytes=1234567,
    used_hardware="true",
    fallback="false",
    fallback_reason="",
    frames=300,
    prefix="1204: ",
):
    """A `ctest -V` log carrying one measurement block."""
    body = [
        l4.BLOCK_BEGIN,
        "PALMIER_L4_ENCODER_NAME={0}".format(encoder),
        "PALMIER_L4_ELAPSED_MS={0}".format(elapsed_ms),
        "PALMIER_L4_OUTPUT_BYTES={0}".format(output_bytes),
        "PALMIER_L4_USED_HARDWARE_ENCODE={0}".format(used_hardware),
        "PALMIER_L4_SOFTWARE_FALLBACK={0}".format(fallback),
        "PALMIER_L4_FALLBACK_REASON={0}".format(fallback_reason),
        "PALMIER_L4_FRAMES_ENCODED={0}".format(frames),
        "PALMIER_L4_OUTPUT_PATH=/tmp/palmier_export_hw_sw_42/hardware.mp4",
        l4.BLOCK_END,
    ]
    lines = [
        prefix + "[ RUN      ] ExportHardwareSoftwareComparisonTest.HardwareAndSoftware",
    ]
    lines += [prefix + line for line in body]
    lines.append(prefix + "[       OK ] ExportHardwareSoftwareComparisonTest.HardwareAndSoftware")
    lines.append("100% tests passed, 0 tests failed out of 2")
    return "\n".join(lines) + "\n"


class MeasurementBlockMatchesTheEmittingTest(unittest.TestCase):
    """The parser and the test that prints the block must agree, exactly."""

    def setUp(self):
        with open(EMITTING_TEST_SOURCE, encoding="utf-8") as handle:
            self.source = handle.read()

    def test_the_sentinels_are_the_ones_the_test_prints(self):
        self.assertIn('"{0}"'.format(l4.BLOCK_BEGIN), self.source)
        self.assertIn('"{0}"'.format(l4.BLOCK_END), self.source)

    def test_every_key_the_script_reads_is_a_key_the_test_prints(self):
        emitted = {
            match.lower()
            for match in re.findall(r'"PALMIER_L4_([A-Z_]+)=', self.source)
        }
        self.assertTrue(emitted, "no PALMIER_L4_* keys found in the emitting test")
        for _output_name, key in l4.OUTPUT_KEYS:
            self.assertIn(key, emitted)
        for key in ("encoder_name", "elapsed_ms", "output_bytes", "software_fallback"):
            self.assertIn(key, emitted)


class Parsing(unittest.TestCase):
    def test_values_are_lifted_out_of_a_verbose_ctest_log(self):
        values = l4.parse_measurements(ctest_log())
        self.assertEqual(values["encoder_name"], "h264_nvenc")
        self.assertEqual(values["elapsed_ms"], "8421")
        self.assertEqual(values["output_bytes"], "1234567")
        self.assertEqual(values["software_fallback"], "false")
        self.assertEqual(values["frames_encoded"], "300")

    def test_an_unprefixed_log_parses_too(self):
        values = l4.parse_measurements(ctest_log(prefix=""))
        self.assertEqual(values["encoder_name"], "h264_nvenc")

    def test_the_last_block_wins(self):
        text = ctest_log(encoder="h264_vaapi") + ctest_log(encoder="h264_nvenc")
        self.assertEqual(l4.parse_measurements(text)["encoder_name"], "h264_nvenc")

    def test_a_log_without_a_block_raises(self):
        skipped = (
            "1204: [ RUN      ] ExportHardwareSoftwareComparisonTest.HardwareAndSoftware\n"
            "1204: /src/x.cpp:428: Skipped\n"
            "1204: no vendor hardware encode path is compiled in\n"
        )
        with self.assertRaises(l4.NoMeasurements):
            l4.parse_measurements(skipped)


class Verdict(unittest.TestCase):
    def test_a_good_run_has_no_failures(self):
        self.assertEqual(l4.evaluate(l4.parse_measurements(ctest_log())), [])

    def test_the_wrong_encoder_fails(self):
        failures = l4.evaluate(l4.parse_measurements(ctest_log(encoder="libx264")))
        self.assertEqual(len(failures), 1)
        self.assertIn("libx264", failures[0])
        self.assertIn("h264_nvenc", failures[0])

    def test_a_software_fallback_fails_and_names_the_reason(self):
        failures = l4.evaluate(
            l4.parse_measurements(
                ctest_log(
                    encoder="libx264",
                    used_hardware="false",
                    fallback="true",
                    fallback_reason="hardware encoder initialization failed",
                )
            )
        )
        joined = "; ".join(failures)
        self.assertIn("fell back to software", joined)
        self.assertIn("hardware encoder initialization failed", joined)
        self.assertIn("did not use a hardware encoder", joined)

    def test_a_zero_byte_output_fails(self):
        failures = l4.evaluate(l4.parse_measurements(ctest_log(output_bytes=0)))
        self.assertEqual(failures, ["the output file is 0 bytes"])

    def test_unrecorded_values_fail_rather_than_pass_silently(self):
        failures = l4.evaluate({})
        joined = "; ".join(failures)
        self.assertIn("unrecorded", joined)
        self.assertIn("software-fallback flag was not recorded", joined)
        self.assertIn("output file size was not recorded", joined)
        self.assertIn("elapsed wall-clock time was not recorded", joined)


class CommandLine(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmpdir = self._tmp.name
        self.outputs = os.path.join(self.tmpdir, "github-output")
        self.summary = os.path.join(self.tmpdir, "summary.md")

    def tearDown(self):
        self._tmp.cleanup()

    def _log(self, text):
        path = os.path.join(self.tmpdir, "ctest.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def _written_outputs(self):
        with open(self.outputs, encoding="utf-8") as handle:
            text = handle.read()
        # Parse the heredoc form back: key<<DELIM \n value \n DELIM
        found = {}
        lines = text.splitlines()
        index = 0
        while index < len(lines):
            head = lines[index]
            self.assertIn("<<", head)
            key, _, delimiter = head.partition("<<")
            value_lines = []
            index += 1
            while index < len(lines) and lines[index] != delimiter:
                value_lines.append(lines[index])
                index += 1
            index += 1
            found[key] = "\n".join(value_lines)
        return found

    def test_a_passing_run_publishes_the_three_recorded_values_and_exits_zero(self):
        status = l4.main(
            [
                self._log(ctest_log()),
                "--github-output",
                self.outputs,
                "--summary",
                self.summary,
            ]
        )
        self.assertEqual(status, 0)
        written = self._written_outputs()
        self.assertEqual(written["encoder-name"], "h264_nvenc")
        self.assertEqual(written["elapsed-ms"], "8421")
        self.assertEqual(written["output-bytes"], "1234567")
        self.assertEqual(written["validation-status"], "passed")
        with open(self.summary, encoding="utf-8") as handle:
            report = handle.read()
        self.assertIn("validation: PASSED", report)
        self.assertIn("8421 ms", report)

    def test_a_failing_run_still_publishes_the_measurements(self):
        """Requirement 8.10: exit non-zero, retain the measurements."""
        status = l4.main(
            [
                self._log(ctest_log(encoder="libx264", used_hardware="false", fallback="true")),
                "--github-output",
                self.outputs,
                "--summary",
                self.summary,
            ]
        )
        self.assertEqual(status, 1)
        written = self._written_outputs()
        self.assertEqual(written["encoder-name"], "libx264")
        self.assertEqual(written["elapsed-ms"], "8421")
        self.assertEqual(written["output-bytes"], "1234567")
        self.assertEqual(written["software-fallback"], "true")
        self.assertEqual(written["validation-status"], "failed")
        with open(self.summary, encoding="utf-8") as handle:
            report = handle.read()
        self.assertIn("validation: FAILED", report)
        self.assertIn("Requirement 8.10", report)
        self.assertIn("1234567 bytes", report)

    def test_a_zero_byte_output_exits_nonzero(self):
        status = l4.main(
            [self._log(ctest_log(output_bytes=0)), "--github-output", self.outputs]
        )
        self.assertEqual(status, 1)
        self.assertEqual(self._written_outputs()["output-bytes"], "0")

    def test_a_skipped_run_exits_two_and_says_nothing_was_validated(self):
        status = l4.main(
            [
                self._log("1204: /src/x.cpp:428: Skipped\n1204: no NVENC path compiled in\n"),
                "--github-output",
                self.outputs,
                "--summary",
                self.summary,
            ]
        )
        self.assertEqual(status, 2)
        written = self._written_outputs()
        self.assertEqual(written["validation-status"], "failed")
        self.assertEqual(written["encoder-name"], "")
        with open(self.summary, encoding="utf-8") as handle:
            self.assertIn("nothing was validated", handle.read())

    def test_a_missing_log_exits_two_and_still_writes_a_summary(self):
        """The CI step renders the summary file, so it must exist even here."""
        self.assertEqual(
            l4.main(
                [
                    os.path.join(self.tmpdir, "absent.log"),
                    "--github-output",
                    self.outputs,
                    "--summary",
                    self.summary,
                ]
            ),
            2,
        )
        self.assertEqual(self._written_outputs()["validation-status"], "failed")
        with open(self.summary, encoding="utf-8") as handle:
            self.assertIn("nothing was validated", handle.read())

    def test_a_value_carrying_a_newline_cannot_forge_an_output(self):
        outputs = l4._github_output_lines({"fallback-reason": "line one\nvalidation-status=passed"})
        self.assertTrue(outputs.startswith("fallback-reason<<PALMIER_EOF_FALLBACK_REASON\n"))
        self.assertIn("line one\nvalidation-status=passed\n", outputs)
        self.assertTrue(outputs.rstrip().endswith("PALMIER_EOF_FALLBACK_REASON"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
