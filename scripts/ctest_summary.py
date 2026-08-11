#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/ctest_summary.py — task 12.11 of the end-to-end-editor-integration
# spec; Requirements 15.2, 15.5 and 15.7.
#
# Requirement 15.7 asks for one artifact per CI run carrying "the complete test
# log together with a summary listing every test's name and its outcome of
# passed, failed, or skipped with its recorded skip reason". The log half is
# `ctest --output-on-failure` tee'd to a file plus `build/Testing/**`. This
# script is the summary half: it turns the JUnit XML that
# `ctest --output-junit` writes into a Markdown report.
#
# ## Why the skip reason has to be dug out of `<system-out>`
#
# This is the part that is easy to get wrong. Tests in this suite record their
# skip reason with `GTEST_SKIP() << reason` (Requirement 15.5 requires the
# reason to name the absent SDK or device), and CTest classifies the test as
# skipped through the target's `SKIP_REGULAR_EXPRESSION` property. CTest's JUnit
# writer therefore emits:
#
#     <testcase name="..." status="notrun">
#       <skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/>
#       <system-out>...gtest output...</system-out>
#     </testcase>
#
# The `message` attribute is the literal string `SKIP_REGULAR_EXPRESSION_MATCHED`
# for every skipped test — it carries no reason at all. The reason the test
# actually recorded is inside `<system-out>`, in GoogleTest's own layout:
#
#     /path/to/test.cpp:465: Skipped
#     <the reason, which may run over several lines>
#
#     [  SKIPPED ] SuiteName.CaseName (0 ms)
#
# so this script parses `<system-out>` for that block, keeps the file:line the
# skip came from, and preserves multi-line reasons verbatim (the hardware-encode
# comparison records a bulleted reason naming both missing halves). Reading only
# the `message` attribute would produce a summary in which all four expected
# skips read `SKIP_REGULAR_EXPRESSION_MATCHED`, which satisfies the letter of
# "reports skipped" and none of Requirement 15.5.
#
# ## Usage
#
#     scripts/ctest_summary.py ctest-results.xml                 # Markdown to stdout
#     scripts/ctest_summary.py ctest-results.xml -o summary.md   # ...to a file
#     scripts/ctest_summary.py ctest-results.xml --fail-on-failure
#
# `--fail-on-failure` is for local use; CI deliberately does not pass it,
# because the summary step runs with `if: always()` and must publish the
# measurements rather than re-fail a job the test step has already failed.
#
# Self-tests live in scripts/test_ctest_summary.py (`python3
# scripts/test_ctest_summary.py`) and run against XML captured from real ctest
# output.

from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

PASSED = "passed"
FAILED = "failed"
SKIPPED = "skipped"
NOT_RUN = "not run"

#: The placeholder CTest puts in `<skipped message="...">` when the skip came
#: from a SKIP_REGULAR_EXPRESSION match. It names no cause, so it is never
#: reported as if it were the test's recorded reason.
_UNINFORMATIVE_SKIP_MESSAGES = frozenset(
    {"", "SKIP_REGULAR_EXPRESSION_MATCHED", "SKIP_RETURN_CODE_MATCHED"}
)

#: `/path/to/test.cpp:465: Skipped` — GoogleTest's header line for GTEST_SKIP().
_GTEST_SKIP_HEADER = re.compile(r"^(?P<location>.*?:\d+): Skipped\s*$")

#: Lines that end a GoogleTest reason block: the banner lines and the result
#: markers. A reason never contains one of these at the start of a line.
_GTEST_BLOCK_END = re.compile(r"^(\[\s*(SKIPPED|OK|FAILED|PASSED)\s*\]|\[-{4,}\]|\[={4,}\])")


@dataclass
class SkipReason:
    """One recorded GTEST_SKIP(), with where it was recorded."""

    location: Optional[str]
    text: str


@dataclass
class TestResult:
    name: str
    outcome: str
    seconds: float = 0.0
    #: Every reason the test recorded. Normally one; a skip in a fixture's
    #: SetUp() plus one in the body would give two, and both are reported.
    skip_reasons: List[SkipReason] = field(default_factory=list)
    #: The `<failure message="...">` attribute, when it says anything.
    failure_message: str = ""
    #: The test's captured stdout, used for the tail shown under a failure.
    system_out: str = ""

    @property
    def skip_reason_text(self) -> str:
        """The recorded skip reason(s) as one displayable block."""
        if not self.skip_reasons:
            return ""
        parts = []
        for reason in self.skip_reasons:
            if reason.location:
                parts.append("{0}: {1}".format(reason.location, reason.text))
            else:
                parts.append(reason.text)
        return "\n".join(parts)


@dataclass
class Summary:
    results: List[TestResult] = field(default_factory=list)

    def of(self, outcome: str) -> List[TestResult]:
        return [r for r in self.results if r.outcome == outcome]

    def count(self, outcome: str) -> int:
        return len(self.of(outcome))

    @property
    def total(self) -> int:
        return len(self.results)

    @property
    def unsuccessful(self) -> int:
        """Failures plus tests that neither ran nor recorded a skip."""
        return self.count(FAILED) + self.count(NOT_RUN)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


def extract_skip_reasons(system_out: str) -> List[SkipReason]:
    """Every `GTEST_SKIP()` reason recorded in `system_out`.

    A reason starts on the line after `<file>:<line>: Skipped` and continues
    until GoogleTest's next banner or result marker. Trailing blank lines are
    dropped; interior blank lines and indentation are kept, because the
    hardware-encode skip reason is a bulleted list whose shape carries meaning.
    """
    reasons: List[SkipReason] = []
    lines = system_out.splitlines()
    index = 0
    while index < len(lines):
        header = _GTEST_SKIP_HEADER.match(lines[index])
        if header is None:
            index += 1
            continue
        body: List[str] = []
        index += 1
        while index < len(lines) and not _GTEST_BLOCK_END.match(lines[index]):
            body.append(lines[index])
            index += 1
        while body and not body[-1].strip():
            body.pop()
        text = "\n".join(body).strip("\n")
        if text:
            reasons.append(SkipReason(location=header.group("location"), text=text))
    return reasons


def _parse_testcase(element: ET.Element) -> TestResult:
    name = element.get("name") or element.get("classname") or "<unnamed test>"
    try:
        seconds = float(element.get("time") or 0.0)
    except ValueError:
        seconds = 0.0

    system_out_element = element.find("system-out")
    system_out = (system_out_element.text or "") if system_out_element is not None else ""

    failure = element.find("failure")
    skipped = element.find("skipped")

    result = TestResult(name=name, seconds=seconds, outcome=PASSED, system_out=system_out)

    if failure is not None:
        result.outcome = FAILED
        result.failure_message = (failure.get("message") or "").strip()
        return result

    if skipped is not None:
        result.outcome = SKIPPED
        result.skip_reasons = extract_skip_reasons(system_out)
        if not result.skip_reasons:
            message = (skipped.get("message") or "").strip()
            if message in _UNINFORMATIVE_SKIP_MESSAGES:
                text = (
                    "no reason recorded — the test printed no GTEST_SKIP() reason"
                    + (" (ctest reported {0})".format(message) if message else "")
                )
            else:
                text = message
            result.skip_reasons = [SkipReason(location=None, text=text)]
        return result

    if (element.get("status") or "run") != "run":
        result.outcome = NOT_RUN
        result.failure_message = "ctest reported status={0} with no <failure> or <skipped>".format(
            element.get("status")
        )
    return result


def parse_junit(path: str) -> Summary:
    """Read a `ctest --output-junit` file into a Summary."""
    root = ET.parse(path).getroot()
    # ctest writes a bare <testsuite>; tolerate a <testsuites> wrapper too.
    suites = [root] if root.tag == "testsuite" else list(root.iter("testsuite"))
    summary = Summary()
    for suite in suites:
        for case in suite.findall("testcase"):
            summary.results.append(_parse_testcase(case))
    return summary


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def _fence(text: str) -> str:
    """`text` in a fenced block, with a fence long enough to survive its content."""
    longest = max((len(run) for run in re.findall(r"`+", text)), default=0)
    fence = "`" * max(3, longest + 1)
    return "{0}text\n{1}\n{0}".format(fence, text)


def _table_cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def _tail(text: str, lines: int) -> str:
    if lines <= 0:
        return ""
    kept = text.splitlines()[-lines:]
    return "\n".join(kept).strip("\n")


def render_markdown(
    summary: Summary,
    title: str = "CTest summary",
    failure_log_lines: int = 40,
) -> str:
    out: List[str] = ["# {0}".format(title), ""]

    out += [
        "| Outcome | Count |",
        "| --- | --- |",
        "| Passed | {0} |".format(summary.count(PASSED)),
        "| Failed | {0} |".format(summary.count(FAILED)),
        "| Skipped | {0} |".format(summary.count(SKIPPED)),
    ]
    if summary.count(NOT_RUN):
        out.append("| Not run | {0} |".format(summary.count(NOT_RUN)))
    out += ["| **Total** | **{0}** |".format(summary.total), ""]

    if summary.total == 0:
        out += ["No test cases were recorded in the JUnit results file.", ""]

    failed = summary.of(FAILED) + summary.of(NOT_RUN)
    if failed:
        out += ["## Failed ({0})".format(len(failed)), ""]
        for result in failed:
            out.append("### `{0}`".format(result.name))
            out.append("")
            if result.failure_message:
                out += ["ctest reported: {0}".format(result.failure_message), ""]
            tail = _tail(result.system_out, failure_log_lines)
            if tail:
                out += [
                    "Last {0} lines of this test's output (the complete log is in the "
                    "uploaded artifact):".format(failure_log_lines),
                    "",
                    _fence(tail),
                    "",
                ]

    skipped = summary.of(SKIPPED)
    if skipped:
        out += [
            "## Skipped ({0})".format(len(skipped)),
            "",
            "Each skip is listed with the reason the test recorded (Requirement 15.5).",
            "",
        ]
        for result in skipped:
            out += ["### `{0}`".format(result.name), "", _fence(result.skip_reason_text), ""]

    out += [
        "## Every test",
        "",
        "| Test | Outcome | Time (s) | Recorded skip reason |",
        "| --- | --- | --- | --- |",
    ]
    for result in summary.results:
        reason = _table_cell(result.skip_reason_text) if result.outcome == SKIPPED else ""
        if result.outcome == FAILED and result.failure_message:
            reason = _table_cell(result.failure_message)
        out.append(
            "| `{0}` | {1} | {2:.2f} | {3} |".format(
                _table_cell(result.name), result.outcome, result.seconds, reason
            )
        )
    out.append("")
    return "\n".join(out)


def render_text(summary: Summary) -> str:
    """A one-line-per-test rendering, for reading in a terminal."""
    out = [
        "passed={0} failed={1} skipped={2}{3} total={4}".format(
            summary.count(PASSED),
            summary.count(FAILED),
            summary.count(SKIPPED),
            " notrun={0}".format(summary.count(NOT_RUN)) if summary.count(NOT_RUN) else "",
            summary.total,
        ),
        "",
    ]
    for result in summary.results:
        out.append("{0:<8} {1}".format(result.outcome.upper(), result.name))
        if result.outcome == SKIPPED:
            for line in result.skip_reason_text.splitlines():
                out.append("         | {0}".format(line))
        elif result.outcome in (FAILED, NOT_RUN) and result.failure_message:
            out.append("         | {0}".format(result.failure_message))
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def _missing_input_report(path: str, title: str) -> str:
    return "\n".join(
        [
            "# {0}".format(title),
            "",
            "No JUnit results file was produced at `{0}`.".format(path),
            "",
            "CTest writes it at the end of the run, so an absent file means the run did not "
            "reach that point — it crashed, was cancelled, or never started. The uploaded "
            "test log is the record of what did happen.",
            "",
        ]
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Summarise a ctest --output-junit XML file as Markdown, "
        "listing every test's name, outcome and recorded skip reason."
    )
    parser.add_argument("junit_xml", help="the file ctest --output-junit wrote")
    parser.add_argument(
        "-o", "--output", help="write the summary here instead of standard output"
    )
    parser.add_argument(
        "--format", choices=("markdown", "text"), default="markdown", help="output format"
    )
    parser.add_argument("--title", default="CTest summary", help="heading for the report")
    parser.add_argument(
        "--failure-log-lines",
        type=int,
        default=40,
        help="lines of a failing test's output to inline (0 to inline none)",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="when the XML file is absent, write a report saying so and succeed "
        "(for a CI step that must publish something even after a crashed run)",
    )
    parser.add_argument(
        "--fail-on-failure",
        action="store_true",
        help="exit 1 when any test failed or did not run",
    )
    args = parser.parse_args(argv)

    if not os.path.isfile(args.junit_xml):
        if not args.allow_missing:
            sys.stderr.write("error: no such JUnit file: {0}\n".format(args.junit_xml))
            return 2
        report = _missing_input_report(args.junit_xml, args.title)
        _emit(report, args.output)
        return 0

    try:
        summary = parse_junit(args.junit_xml)
    except ET.ParseError as error:
        sys.stderr.write("error: {0} is not parseable XML: {1}\n".format(args.junit_xml, error))
        return 2

    if args.format == "text":
        report = render_text(summary)
    else:
        report = render_markdown(
            summary, title=args.title, failure_log_lines=args.failure_log_lines
        )
    _emit(report, args.output)

    if args.fail_on_failure and summary.unsuccessful:
        return 1
    return 0


def _emit(report: str, output: Optional[str]) -> None:
    if output:
        directory = os.path.dirname(os.path.abspath(output))
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(output, "w", encoding="utf-8") as handle:
            handle.write(report)
        sys.stderr.write("wrote {0}\n".format(output))
    else:
        try:
            sys.stdout.write(report)
        except BrokenPipeError:
            # `... | head` closed the pipe. Not an error worth a traceback.
            try:
                sys.stdout.close()
            except BrokenPipeError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
