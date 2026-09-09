#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_repo_consistency.py — assert the repository's cross-file claims are true.
#
# The C++ suite already has documentation-consistency tests, but they can only check
# things reachable from a compiled test binary: CMake options against docs/BUILD.md,
# agent tools against docs/TOOLS.md. Three claims live outside that reach because they
# are made by files no test compiles, and each is the kind that rots silently:
#
#   1. .env.example says it documents every environment variable the editor reads. If a
#      new PALMIER_* setting is added to src/app/AppSettings.cpp and not documented, or
#      a documented one is renamed in code, the file becomes actively misleading — worse
#      than absent, because a reader trusts it.
#
#   2. README.md, SECURITY.md and CHANGELOG.md link to files by relative path. A
#      renamed document turns the project's front door into a 404.
#
#   3. The Makefile's `help` target lists targets by scraping its own `##` comments, and
#      .PHONY has to be maintained by hand beside them. A target in one and not the
#      other either vanishes from the help or breaks when a same-named file exists.
#
# Exit status is 0 when every claim holds and 1 otherwise, with each failure naming the
# file, the claim and what would fix it.
#
# Self-tests: scripts/test_check_repo_consistency.py

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import List, Optional, Sequence

#: Sources whose string literals are the authority on which variables are read.
ENV_SOURCES = (
    "src/app/AppSettings.cpp",
    "src/services/LocalizationManager.cpp",
    "src/gpu/GpuSelectionStore.cpp",
)

#: Root documents whose relative links must resolve.
LINKED_DOCUMENTS = ("README.md", "SECURITY.md", "CHANGELOG.md", "CONTRIBUTING.md")

#: Variables that appear in .env.example but are consumed by tooling rather than by the
#: editor, so they are legitimately absent from the C++ sources.
TOOLING_VARIABLES = frozenset({"PALMIER_MEDIA_DIR"})


def _read(root: str, relative: str) -> str:
    with open(os.path.join(root, relative), encoding="utf-8", errors="replace") as handle:
        return handle.read()


def check_environment_example(root: str) -> List[str]:
    """Every PALMIER_* in .env.example is read by the code, and vice versa."""
    failures: List[str] = []
    example_path = os.path.join(root, ".env.example")
    if not os.path.isfile(example_path):
        return [".env.example is missing"]
    example = _read(root, ".env.example")

    # An uncommented `NAME=` line is a claim that NAME is settable.
    documented = {
        name
        for name in re.findall(r"^([A-Z][A-Z0-9_]*)=", example, flags=re.M)
        if name.startswith("PALMIER_")
    }
    # Anywhere in the file, including commented context, counts as "mentioned".
    mentioned = set(re.findall(r"\b(PALMIER_[A-Z0-9_]+)\b", example))

    code = ""
    for source in ENV_SOURCES:
        if not os.path.isfile(os.path.join(root, source)):
            failures.append(f".env.example check: expected source {source} is missing")
            continue
        code += _read(root, source)
    read_by_code = set(re.findall(r'"(PALMIER_[A-Z0-9_]+)"', code))

    for name in sorted(documented - read_by_code - TOOLING_VARIABLES):
        failures.append(
            f".env.example documents {name} as settable, but no source in "
            f"{', '.join(ENV_SOURCES)} reads it. Remove it, or add it to "
            f"TOOLING_VARIABLES in this script if tooling consumes it."
        )
    for name in sorted(read_by_code - mentioned):
        failures.append(
            f"{name} is read by the code but is not mentioned in .env.example. "
            f"Document it, so the file remains a complete list."
        )
    return failures


def check_relative_links(root: str) -> List[str]:
    """Every relative Markdown link in the root documents resolves to a real path."""
    failures: List[str] = []
    for document in LINKED_DOCUMENTS:
        if not os.path.isfile(os.path.join(root, document)):
            continue
        text = _read(root, document)
        for match in re.finditer(r"\[[^\]]*\]\(([^)#\s]+)(?:#[^)]*)?\)", text):
            target = match.group(1)
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            resolved = os.path.normpath(os.path.join(root, os.path.dirname(document), target))
            if not os.path.exists(resolved):
                failures.append(f"{document} links to {target}, which does not exist")
    return failures


def check_makefile_targets(root: str) -> List[str]:
    """Documented Makefile targets and .PHONY entries are the same set."""
    makefile = os.path.join(root, "Makefile")
    if not os.path.isfile(makefile):
        return ["Makefile is missing"]
    text = _read(root, "Makefile")

    documented = set(re.findall(r"^([a-zA-Z][a-zA-Z0-9_-]*):.*?## ", text, flags=re.M))
    match = re.search(r"^\.PHONY:((?:[^\n]*\\\n)*[^\n]*)", text, flags=re.M)
    if match is None:
        return ["Makefile declares no .PHONY targets"]
    phony = set(match.group(1).replace("\\", " ").split())

    failures = []
    for name in sorted(documented - phony):
        failures.append(
            f"Makefile target '{name}' is documented with ## but missing from .PHONY, "
            f"so it would break if a file of that name existed"
        )
    for name in sorted(phony - documented):
        failures.append(
            f"Makefile target '{name}' is in .PHONY but has no ## description, so "
            f"`make help` will not list it"
        )
    return failures


CHECKS = (
    ("environment example", check_environment_example),
    ("relative links", check_relative_links),
    ("Makefile targets", check_makefile_targets),
)


def run_all(root: str) -> List[str]:
    """Every failure from every check, in declaration order."""
    failures: List[str] = []
    for _name, check in CHECKS:
        failures.extend(check(root))
    return failures


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Assert this repository's cross-file claims agree with its code."
    )
    parser.add_argument(
        "--root",
        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        help="repository root to check (default: the one containing this script)",
    )
    args = parser.parse_args(argv)

    total = 0
    all_failures: List[str] = []
    for name, check in CHECKS:
        failures = check(args.root)
        total += 1
        status = "FAIL" if failures else "ok"
        sys.stdout.write(f"[{status:>4}] {name}\n")
        all_failures.extend(failures)

    if all_failures:
        sys.stdout.write(f"\n{len(all_failures)} problem(s) found:\n")
        for failure in all_failures:
            sys.stdout.write(f"  - {failure}\n")
        return 1

    sys.stdout.write(f"\nall {total} consistency checks passed\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
