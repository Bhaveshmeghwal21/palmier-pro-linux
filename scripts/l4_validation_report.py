#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/l4_validation_report.py — task 12.12 of the end-to-end-editor-integration
# spec; Requirements 8.5, 8.10 and 16.5.
#
# Requirement 8.5:
#
#   "THE L4_Validation_Job SHALL export the fixture timeline of at least 300
#    frames at 1920x1080 and 30 frames per second on an NVIDIA L4 device using
#    `h264_nvenc` and SHALL record, as job output, the selected encoder name, the
#    elapsed wall-clock time in milliseconds and the output file size in bytes."
#
# Requirement 8.10:
#
#   "IF the L4_Validation_Job records a selected encoder other than `h264_nvenc`,
#    a software-fallback flag of true, or an output file size of 0 bytes, THEN THE
#    L4_Validation_Job SHALL exit with a failure status and SHALL retain the
#    recorded measurements as job output."
#
# The export itself is `palmier_services_export_hw_sw_comparison_tests`, which
# already exports exactly that fixture (300 frames, 1920x1080, 30 fps, H.264)
# through the production encode path and gates itself on real hardware. Its
# hardware run prints a measurement block; this script lifts the block out of the
# `ctest -V` log, publishes the values as GitHub Actions job outputs, and then
# applies Requirement 8.10's verdict.
#
# The ORDER matters and is the whole reason this is a script rather than three
# greps: the outputs are written FIRST and the verdict is applied SECOND, so a
# failing validation still retains its measurements as job output.
#
# ## Usage
#
#     scripts/l4_validation_report.py ctest-l4.log \
#         --summary l4-summary.md            # Markdown report for the job summary
#
# `$GITHUB_OUTPUT` is used when set (that is how Actions collects job outputs);
# `--github-output PATH` overrides it for local runs. Nothing else in the
# environment is read.
#
# ## Exit status
#
#   0  every recorded value passed: encoder `h264_nvenc`, no software fallback,
#      a non-empty output.
#   1  the measurements were found and one of Requirement 8.10's conditions
#      holds. Outputs are still written.
#   2  no measurement block in the log at all — the test skipped, crashed or was
#      filtered out, so nothing was validated. A skip on the L4 host is a failed
#      validation, not a pass.
#
# Self-tests: `python3 scripts/test_l4_validation_report.py`.

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Dict, List, Optional, Sequence, Tuple

#: Must match kL4BlockBegin / kL4BlockEnd in
#: tests/services/export_hardware_software_comparison_test.cpp.
BLOCK_BEGIN = "--- BEGIN PALMIER L4 MEASUREMENTS ---"
BLOCK_END = "--- END PALMIER L4 MEASUREMENTS ---"

#: `ctest -V` prefixes each line of a test's output with e.g. `1204: `.
_CTEST_LINE_PREFIX = re.compile(r"^\s*\d+:\s?")

_KEY_PREFIX = "PALMIER_L4_"

#: The encoder Requirement 8.5 names.
DEFAULT_EXPECTED_ENCODER = "h264_nvenc"


class NoMeasurements(Exception):
    """The log carries no measurement block, so nothing was validated."""


def _strip_ctest_prefix(line: str) -> str:
    return _CTEST_LINE_PREFIX.sub("", line.rstrip("\n"))


def parse_measurements(log_text: str) -> Dict[str, str]:
    """The measurement block in `log_text`, as a key -> value mapping.

    Keys are lower-cased with the `PALMIER_L4_` prefix removed, so
    `PALMIER_L4_ELAPSED_MS` arrives as `elapsed_ms`. The LAST block wins: a log
    that somehow carries two runs is reporting on the most recent one.
    """
    blocks: List[List[str]] = []
    current: Optional[List[str]] = None
    for raw in log_text.splitlines():
        line = _strip_ctest_prefix(raw)
        if line.strip() == BLOCK_BEGIN:
            current = []
            continue
        if line.strip() == BLOCK_END:
            if current is not None:
                blocks.append(current)
            current = None
            continue
        if current is not None:
            current.append(line)

    if not blocks:
        raise NoMeasurements(
            "no '{0}' block in the log: the export measured by Requirement 8.5 did not "
            "run (the test skipped for want of hardware, was filtered out, or the run "
            "crashed)".format(BLOCK_BEGIN)
        )

    values: Dict[str, str] = {}
    for line in blocks[-1]:
        stripped = line.strip()
        if not stripped.startswith(_KEY_PREFIX) or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        values[key[len(_KEY_PREFIX) :].strip().lower()] = value.strip()
    return values


def _as_int(values: Dict[str, str], key: str) -> Optional[int]:
    try:
        return int(values[key])
    except (KeyError, ValueError):
        return None


def _as_bool(values: Dict[str, str], key: str) -> Optional[bool]:
    raw = values.get(key, "").lower()
    if raw in ("true", "1", "yes"):
        return True
    if raw in ("false", "0", "no"):
        return False
    return None


def evaluate(
    values: Dict[str, str], expected_encoder: str = DEFAULT_EXPECTED_ENCODER
) -> List[str]:
    """Requirement 8.10's conditions that hold for `values`, as failure reasons.

    An empty list is a passing validation.
    """
    failures: List[str] = []

    encoder = values.get("encoder_name", "")
    if encoder != expected_encoder:
        failures.append(
            "the selected encoder is {0}, not {1}".format(
                "'{0}'".format(encoder) if encoder else "unrecorded", expected_encoder
            )
        )

    fallback = _as_bool(values, "software_fallback")
    if fallback is None:
        failures.append("the software-fallback flag was not recorded")
    elif fallback:
        reason = values.get("fallback_reason", "")
        failures.append(
            "the export fell back to software encoding"
            + (": {0}".format(reason) if reason else "")
        )

    size = _as_int(values, "output_bytes")
    if size is None:
        failures.append("the output file size was not recorded")
    elif size == 0:
        failures.append("the output file is 0 bytes")

    # Not one of the three named conditions, but no honest validation can pass
    # without it: the flag says whether a hardware encoder was used at all.
    used_hardware = _as_bool(values, "used_hardware_encode")
    if used_hardware is False:
        failures.append("the export reports that it did not use a hardware encoder")

    if _as_int(values, "elapsed_ms") is None:
        failures.append("the elapsed wall-clock time was not recorded")

    return failures


# ---------------------------------------------------------------------------
# Publishing
# ---------------------------------------------------------------------------

#: The job outputs written for every run, passing or failing. The first three are
#: Requirement 8.5's recorded values.
OUTPUT_KEYS: Sequence[Tuple[str, str]] = (
    ("encoder-name", "encoder_name"),
    ("elapsed-ms", "elapsed_ms"),
    ("output-bytes", "output_bytes"),
    ("used-hardware-encode", "used_hardware_encode"),
    ("software-fallback", "software_fallback"),
    ("fallback-reason", "fallback_reason"),
    ("frames-encoded", "frames_encoded"),
)


def _github_output_lines(outputs: Dict[str, str]) -> str:
    """`outputs` in the `$GITHUB_OUTPUT` file format.

    Every value is written with a heredoc delimiter rather than `key=value`, so a
    value that ever grows a newline cannot forge extra outputs.
    """
    lines: List[str] = []
    for key, value in outputs.items():
        delimiter = "PALMIER_EOF_{0}".format(re.sub(r"[^A-Za-z0-9]", "_", key).upper())
        lines.append("{0}<<{1}".format(key, delimiter))
        lines.append(value)
        lines.append(delimiter)
    return "\n".join(lines) + "\n"


def publish(
    values: Dict[str, str], failures: Sequence[str], github_output: Optional[str]
) -> Dict[str, str]:
    """Write the job outputs and return exactly what was written."""
    outputs = {name: values.get(key, "") for name, key in OUTPUT_KEYS}
    outputs["validation-status"] = "failed" if failures else "passed"
    outputs["validation-detail"] = "; ".join(failures) if failures else "all recorded values pass"
    if github_output:
        with open(github_output, "a", encoding="utf-8") as handle:
            handle.write(_github_output_lines(outputs))
    return outputs


def render_markdown(
    outputs: Dict[str, str], failures: Sequence[str], expected_encoder: str
) -> str:
    verdict = "FAILED" if failures else "PASSED"
    lines = [
        "# NVIDIA L4 hardware-encode validation: {0}".format(verdict),
        "",
        "Fixture: 300 frames, 1920x1080, 30 fps, H.264 (Requirement 8.5).",
        "",
        "| Recorded value | |",
        "| --- | --- |",
        "| Selected encoder | `{0}` |".format(outputs.get("encoder-name") or "(unrecorded)"),
        "| Elapsed wall-clock | {0} ms |".format(outputs.get("elapsed-ms") or "(unrecorded)"),
        "| Output size | {0} bytes |".format(outputs.get("output-bytes") or "(unrecorded)"),
        "| Used hardware encode | {0} |".format(
            outputs.get("used-hardware-encode") or "(unrecorded)"
        ),
        "| Software fallback | {0} |".format(outputs.get("software-fallback") or "(unrecorded)"),
        "| Frames encoded | {0} |".format(outputs.get("frames-encoded") or "(unrecorded)"),
        "",
    ]
    if outputs.get("fallback-reason"):
        lines += ["Fallback reason: {0}".format(outputs["fallback-reason"]), ""]
    if failures:
        lines += ["## Why this run failed (Requirement 8.10)", ""]
        lines += ["- {0}".format(reason) for reason in failures]
        lines += [
            "",
            "The measurements above are retained as job output regardless of this verdict.",
            "",
        ]
    else:
        lines += [
            "The encoder is `{0}`, no software fallback was used, and the output is "
            "non-empty.".format(expected_encoder),
            "",
        ]
    return "\n".join(lines)


def _missing_measurements_markdown(message: str) -> str:
    return "\n".join(
        [
            "# NVIDIA L4 hardware-encode validation: FAILED",
            "",
            "No measurements were recorded, so nothing was validated.",
            "",
            message,
            "",
            "A skip on the L4 host is a failed validation, not a pass: read the recorded skip "
            "reason in the uploaded log, fix the named cause (NVENC path not compiled in, "
            "device not selected, or no software H.264 encoder present) and re-run.",
            "",
        ]
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Turn the L4 measurement block printed by the hardware/software "
        "comparison test into GitHub Actions job outputs, then apply Requirement "
        "8.10's pass/fail verdict."
    )
    parser.add_argument("log", help="the `ctest -V` log of the comparison test")
    parser.add_argument(
        "--github-output",
        default=os.environ.get("GITHUB_OUTPUT"),
        help="file to append job outputs to (defaults to $GITHUB_OUTPUT)",
    )
    parser.add_argument("--summary", help="write a Markdown report here as well")
    parser.add_argument(
        "--expected-encoder",
        default=DEFAULT_EXPECTED_ENCODER,
        help="the encoder name the validation requires (default: %(default)s)",
    )
    args = parser.parse_args(argv)

    def write_summary(text: str) -> None:
        sys.stdout.write(text)
        if args.summary:
            with open(args.summary, "w", encoding="utf-8") as handle:
                handle.write(text)

    try:
        with open(args.log, encoding="utf-8", errors="replace") as handle:
            log_text = handle.read()
    except OSError as error:
        # No log at all — the run never got as far as the export. Still publish a
        # verdict and a summary, so the step that renders them has something to
        # read and the failure is legible on the run page.
        message = "cannot read {0}: {1}".format(args.log, error)
        publish({}, [message], args.github_output)
        write_summary(_missing_measurements_markdown(message))
        sys.stderr.write("error: {0}\n".format(message))
        return 2

    try:
        values = parse_measurements(log_text)
    except NoMeasurements as error:
        # Nothing to retain, but the verdict still has to be published.
        publish({}, [str(error)], args.github_output)
        write_summary(_missing_measurements_markdown(str(error)))
        sys.stderr.write("error: {0}\n".format(error))
        return 2

    failures = evaluate(values, args.expected_encoder)
    # Outputs first, verdict second — Requirement 8.10 retains the measurements.
    outputs = publish(values, failures, args.github_output)
    write_summary(render_markdown(outputs, failures, args.expected_encoder))

    if failures:
        for reason in failures:
            sys.stderr.write("::error::L4 validation failed: {0}\n".format(reason))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
