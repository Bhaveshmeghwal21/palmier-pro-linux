#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_repo_consistency.py â€” assert the repository's cross-file claims are true.
#
# The C++ suite already has documentation-consistency tests, but they can only check
# things reachable from a compiled test binary: CMake options against docs/BUILD.md,
# agent tools against docs/TOOLS.md. Three claims live outside that reach because they
# are made by files no test compiles, and each is the kind that rots silently:
#
#   1. .env.example says it documents every environment variable the editor reads. If a
#      new PALMIER_* setting is added to src/app/AppSettings.cpp and not documented, or
#      a documented one is renamed in code, the file becomes actively misleading â€” worse
#      than absent, because a reader trusts it.
#
#   2. README.md, SECURITY.md and CHANGELOG.md link to files by relative path. A
#      renamed document turns the project's front door into a 404.
#
#   3. The Makefile's `help` target lists targets by scraping its own `##` comments, and
#      .PHONY has to be maintained by hand beside them. A target in one and not the
#      other either vanishes from the help or breaks when a same-named file exists.
#
#   4. README.md quotes figures it measured once: how many cases the suite holds, how many test
#      executables the build declares, what line coverage is, and what floor CI gates on. Two
#      are facts of this repository and are checked against their sources -- the executable
#      count was WRONG, claiming 94 where the build declares 90. The other two are measurements
#      that move with almost every commit, so the check requires them to be hedged rather than
#      exact: the case count was corrected to 1845 and was 1849 one commit later.
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
from typing import List, Optional, Sequence, Tuple

#: Sources whose string literals are the authority on which variables are read.
#: core/Logging.hpp is a header rather than a .cpp because the two logging variables are
#: declared as constants there, so that the application, .env.example and this check all
#: agree with one definition instead of three spellings.
ENV_SOURCES = (
    "src/app/AppSettings.cpp",
    "src/services/LocalizationManager.cpp",
    "src/gpu/GpuSelectionStore.cpp",
    "src/core/Logging.hpp",
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


VERSION_SOURCES = (
    # (path, regex with one capturing group, what the value feeds)
    ("CMakeLists.txt", r"^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$", "PROJECT_VERSION, and so the .deb"),
    (
        "packaging/CPackConfig.cmake",
        r'CPACK_PACKAGE_VERSION\s+"(\d+\.\d+\.\d+)"',
        "the standalone cpack config",
    ),
    (
        "src/services/McpProtocolHandler.hpp",
        r'kDefaultServerVersion\s*=\s*"(\d+\.\d+\.\d+)"',
        "the version MCP clients are told",
    ),
    (
        # AppStream advertises the newest <release> to software centres. Newest first is
        # the AppStream convention, so the first match is the current release.
        "packaging/shared/io.palmier.PalmierPro.metainfo.xml",
        r'<release version="(\d+\.\d+\.\d+)"',
        "the version software centres display",
    ),
    (
        # Indented and three-part, so this cannot match the recipe's own top-level
        # `version: 1` format declaration.
        "packaging/appimage/AppImageBuilder.yml",
        r"^    version:\s*(\d+\.\d+\.\d+)\s*$",
        "the version baked into the AppImage",
    ),
)


# A health-report example carries a concrete version, and those examples drift silently:
# they are prose, so no build reads them. Scanned by shape rather than from a list, so an
# example written later is covered without being registered anywhere.
HEALTH_VERSION_EXAMPLE = re.compile(r'"version":\s*"(\d+\.\d+\.\d+)"')

HEALTH_EXAMPLE_ROOTS = ("docs", "src")


def _health_version_examples(root: str) -> List[Tuple[str, str]]:
    """Every (relative path, version) pair appearing in a health-report example."""
    found: List[Tuple[str, str]] = []
    for top in HEALTH_EXAMPLE_ROOTS:
        for directory, _subdirs, names in os.walk(os.path.join(root, top)):
            for name in names:
                if not name.endswith((".md", ".hpp", ".cpp")):
                    continue
                absolute = os.path.join(directory, name)
                relative = os.path.relpath(absolute, root).replace(os.sep, "/")
                try:
                    with open(absolute, "r", encoding="utf-8") as handle:
                        body = handle.read()
                except OSError:
                    continue
                for version in HEALTH_VERSION_EXAMPLE.findall(body):
                    found.append((relative, version))
    return found


def check_version_agreement(root: str) -> List[str]:
    """Every independent copy of the release version says the same thing.

    Five files carry the version with no build-time relationship between them, and the
    newest CHANGELOG heading is a sixth. Nothing had ever compared them, and they DID
    drift: the v0.2.0 tag produced a package stamped 0.1.0, because the tag does not feed
    PROJECT_VERSION. This turns that into a gate.

    The gate originally covered three files, and the drift promptly survived in the two it
    did not look at: the AppStream metainfo and the appimage-builder recipe were still
    advertising 0.1.0 at project version 0.3.0, so a software centre would have named the
    wrong release and an AppImage would have been stamped two minor versions stale. Any
    file that independently states the version belongs here, or it will drift next.
    """
    failures: List[str] = []
    found = {}
    for relative, pattern, purpose in VERSION_SOURCES:
        if not os.path.isfile(os.path.join(root, relative)):
            failures.append(f"{relative} is missing, so the release version cannot be checked")
            continue
        match = re.search(pattern, _read(root, relative), flags=re.M)
        if match is None:
            failures.append(f"no version matching {pattern!r} found in {relative} ({purpose})")
            continue
        found[relative] = match.group(1)

    changelog = os.path.join(root, "CHANGELOG.md")
    if os.path.isfile(changelog):
        headings = re.findall(r"^## \[(\d+\.\d+\.\d+)\]", _read(root, "CHANGELOG.md"), flags=re.M)
        if headings:
            found["CHANGELOG.md (newest release heading)"] = headings[0]
        else:
            failures.append("CHANGELOG.md declares no released version heading")

    # Documented health-report examples state the version in prose, where nothing reads
    # them. Each is compared against the project version rather than to the others, so the
    # failure names the stale example instead of only reporting a disagreement.
    project_version = found.get("CMakeLists.txt")
    for relative, version in sorted(set(_health_version_examples(root))):
        if project_version is not None and version != project_version:
            failures.append(
                f"{relative} shows a health report for version {version}, but the project "
                f"is {project_version}; the example is prose, so nothing else reads it"
            )

    distinct = set(found.values())
    if len(distinct) > 1:
        failures.append(
            "the release version disagrees across files: "
            + ", ".join(f"{path} says {value}" for path, value in sorted(found.items()))
            + " â€” they have no build-time relationship, so they must be changed together"
        )
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


#: Words that turn a figure into a claim that stays true as the tree grows. A measured quantity in
#: README.md must carry one of these, directly in front of it; see check_readme_measurements.
APPROXIMATION_WORDS = ("more than", "over", "around", "about", "roughly", "nearly", "upwards of")

#: A test-case count in README.md, and the same count with an approximation word directly qualifying
#: it. Both are needed: the first finds every count, the second decides which of them are hedged. A
#: span-containment test between the two is what stops an unrelated word elsewhere in the sentence
#: from standing in for the hedge.
#: Every claim pattern below matches across whitespace, newlines included, because Markdown prose is
#: rewrapped whenever a sentence near it is edited. A pattern anchored to a single line would stop
#: finding its claim the moment that happened, and a check that finds nothing passes -- the one
#: failure mode a consistency check must not have.
TEST_EXECUTABLES_RE = re.compile(r"(\d+)\s+test\s+executables")
COVERAGE_FLOOR_RE = re.compile(r"floor\s+of\s+(\d+)%")
EXACT_LINES_RE = re.compile(r"\(\d[\d,]*\s+of\s+\d[\d,]*\s+lines\)")
DECIMAL_COVERAGE_RE = re.compile(r"coverage\s+is\s+\d+\.\d+%")

CASE_COUNT_RE = re.compile(r"([\d][\d,]{2,})\s+GoogleTest")
HEDGED_CASE_COUNT_RE = re.compile(
    r"\b(?:" + "|".join(APPROXIMATION_WORDS) + r")\s+\**\s*[\d][\d,]{2,}\s+GoogleTest"
)


def _declared_test_executables(root: str) -> int:
    """How many distinct test executables tests/CMakeLists.txt declares.

    Counted as distinct TARGET NAMES rather than as occurrences of add_executable, because
    palmier_ui_shell_tests is declared twice inside an if/else -- one target, two branches -- and
    three further occurrences are prose in comments. Counting occurrences is how README.md came to
    claim 94 when the build produces 90.
    """
    text = _read(root, "tests/CMakeLists.txt")
    names = set()
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        match = re.search(r"add_executable\(\s*([A-Za-z0-9_]+)", line)
        if match is not None:
            names.add(match.group(1))
    return len(names)


def check_readme_measurements(root: str) -> List[str]:
    """README.md's figures are either checkable here or written as approximations.

    README.md is the project's front door, so a figure in it that has quietly stopped being true is
    worse than one never given. Two kinds of figure appear there and they need opposite treatment.

    Static facts -- how many test executables the build declares, and the coverage floor CI gates on
    -- each have one source of truth in this repository, so they are stated exactly and compared
    against that source here. That is not hypothetical tidiness: the executable count was wrong.

    Measurements -- the case count and the coverage percentages -- move with almost every commit, and
    no check can keep an exact value current. They must be hedged, and this refuses an unhedged one,
    because correcting such a figure by hand costs a CI run and is stale again immediately. The case
    count was corrected to 1845 and was 1849 one commit later; two coverage runs over the same tree
    reported 16814 and then 16808 of 20812 lines.
    """
    failures: List[str] = []
    if not os.path.isfile(os.path.join(root, "README.md")):
        return ["README.md is missing, so its claims cannot be checked"]
    readme = _read(root, "README.md")

    # 1. Static fact: the number of test executables.
    if not os.path.isfile(os.path.join(root, "tests/CMakeLists.txt")):
        failures.append(
            "tests/CMakeLists.txt is missing, so the executable count cannot be checked"
        )
    else:
        declared = _declared_test_executables(root)
        claimed = {int(value) for value in TEST_EXECUTABLES_RE.findall(readme)}
        if not claimed:
            failures.append(
                "README.md states no test executable count; it should, because that is a fact of "
                "tests/CMakeLists.txt rather than a measurement"
            )
        for value in sorted(claimed):
            if value != declared:
                failures.append(
                    f"README.md claims {value} test executables but tests/CMakeLists.txt declares "
                    f"{declared} distinct targets"
                )

    # 2. Static fact: the coverage floor CI enforces.
    workflow = ".github/workflows/ci.yml"
    if not os.path.isfile(os.path.join(root, workflow)):
        failures.append(f"{workflow} is missing, so the coverage floor cannot be checked")
    else:
        match = re.search(r"COVERAGE_MIN_LINE:\s*(\d+)", _read(root, workflow))
        if match is None:
            failures.append(
                f"no COVERAGE_MIN_LINE in {workflow}, so the advertised floor is uncheckable"
            )
        else:
            enforced = int(match.group(1))
            for value in {int(v) for v in COVERAGE_FLOOR_RE.findall(readme)}:
                if value != enforced:
                    failures.append(
                        f"README.md advertises a coverage floor of {value}% but {workflow} enforces "
                        f"{enforced}% - a reader trusts the README"
                    )

    # 3. Measurements must be hedged, by a word that DIRECTLY qualifies the number.
    #
    # Checked as one span covering hedge and number rather than by looking for an approximation word
    # somewhere nearby. The nearby version passed an unhedged count in this very file, because the
    # preceding sentence contains "line-coverage" and "coverage" contains "over".
    hedged = [match.span() for match in HEDGED_CASE_COUNT_RE.finditer(readme)]
    for match in CASE_COUNT_RE.finditer(readme):
        qualified = any(start <= match.start() and match.end() <= end for start, end in hedged)
        if not qualified:
            failures.append(
                f"README.md states an unhedged test-case count ({match.group(1)!r}); write "
                '"more than N" with a round N, because the next commit that adds a test falsifies '
                "an exact one and nothing here can keep it current"
            )
    for pattern, description, remedy in (
        (
            EXACT_LINES_RE,
            "an exact covered-line count",
            'write "around N%" instead - two coverage runs over the same tree reported 16814 and '
            "then 16808 of 20812 lines",
        ),
        (
            DECIMAL_COVERAGE_RE,
            "a coverage percentage to one decimal place",
            'write "around N%" instead - the decimal moves between runs of the same tree, and the '
            "figure that does not move is the floor, stated separately",
        ),
    ):
        found = pattern.search(readme)
        if found is not None:
            failures.append(
                f"README.md states {description} ({found.group(0)!r}), which nothing in this "
                f"repository can keep true: {remedy}"
            )

    return failures


# The identity every packaging manifest restates. Each format needs its own copy -- there
# is no build-time relationship between them -- so they are compared instead.
PACKAGING_APP_ID = "io.palmier.PalmierPro"
PACKAGING_BINARY = "palmier-pro"
PACKAGING_LICENSE = "GPL-3.0-or-later"

PACKAGING_FILES = {
    "metainfo": "packaging/shared/io.palmier.PalmierPro.metainfo.xml",
    "desktop": "packaging/shared/io.palmier.PalmierPro.desktop",
    "mime": "packaging/shared/io.palmier.PalmierPro.xml",
    "icon": "packaging/shared/io.palmier.PalmierPro.svg",
    "flatpak": "packaging/flatpak/io.palmier.PalmierPro.yaml",
    "appimage": "packaging/appimage/AppImageBuilder.yml",
    "deb-control": "packaging/deb/debian/control",
    "deb-copyright": "packaging/deb/debian/copyright",
}


def _capture(pattern: str, text: str) -> Optional[str]:
    """The first capturing group of `pattern` in `text`, or None."""
    match = re.search(pattern, text, flags=re.M)
    return match.group(1).strip() if match else None


def check_packaging_manifests(root: str) -> List[str]:
    """Every packaging manifest agrees on the application's identity.

    Flatpak, AppImage and Debian each restate the application id, the executable name and
    the licence, and the two shared freedesktop files restate them again. Nothing had ever
    compared them. That matters more here than for most cross-file claims because CI builds
    only the .deb, and only on a version tag: a rename in the Flatpak or AppImage recipe
    could not fail any job, so it would have been discovered by whoever first tried to
    install the result.
    """
    failures: List[str] = []
    for label, relative in sorted(PACKAGING_FILES.items()):
        if not os.path.isfile(os.path.join(root, relative)):
            failures.append(f"the {label} packaging file {relative} is missing")
    if failures:
        return failures

    text = {label: _read(root, path) for label, path in PACKAGING_FILES.items()}

    # -- the application id, restated by three manifests and the two Flatpak rename keys
    ids = {
        "the metainfo <id>": _capture(r"<id>([^<]+)</id>", text["metainfo"]),
        "the Flatpak app-id": _capture(r"^app-id:\s*(\S+)", text["flatpak"]),
        "the AppImage app_info.id": _capture(r"^\s+id:\s*(\S+)", text["appimage"]),
        "the Flatpak rename-desktop-file": _capture(
            r"^rename-desktop-file:\s*(\S+?)\.desktop\s*$", text["flatpak"]
        ),
        "the Flatpak rename-appdata-file": _capture(
            r"^rename-appdata-file:\s*(\S+?)\.metainfo\.xml\s*$", text["flatpak"]
        ),
    }
    for where, value in sorted(ids.items()):
        if value is None:
            failures.append(f"{where} declares no application id")
        elif value != PACKAGING_APP_ID:
            failures.append(f"{where} says {value}, but the application id is {PACKAGING_APP_ID}")

    # -- the executable name, restated by every format
    binaries = {
        "the desktop Exec": _capture(r"^Exec=(\S+)", text["desktop"]),
        "the desktop TryExec": _capture(r"^TryExec=(\S+)", text["desktop"]),
        "the metainfo <binary>": _capture(r"<binary>([^<]+)</binary>", text["metainfo"]),
        "the Flatpak command": _capture(r"^command:\s*(\S+)", text["flatpak"]),
        "the AppImage exec": _capture(r"^\s+exec:\s*\S*?([^/\s]+)\s*$", text["appimage"]),
        "the Debian Package": _capture(r"^Package:\s*(\S+)", text["deb-control"]),
    }
    for where, value in sorted(binaries.items()):
        if value is None:
            failures.append(f"{where} names no executable")
        elif value != PACKAGING_BINARY:
            failures.append(f"{where} says {value}, but the executable is {PACKAGING_BINARY}")

    # -- the licence, which must match the repository's own GPLv3-or-later grant
    licences = {
        "the metainfo <project_license>": _capture(
            r"<project_license>([^<]+)</project_license>", text["metainfo"]
        ),
        "the Debian copyright": _capture(r"^License:\s*(\S+)", text["deb-copyright"]),
    }
    for where, value in sorted(licences.items()):
        if value is None:
            failures.append(f"{where} states no licence")
        elif value != PACKAGING_LICENSE:
            failures.append(f"{where} says {value}, but the project is {PACKAGING_LICENSE}")

    # -- every shared asset the Flatpak manifest installs actually exists
    for referenced in sorted(set(re.findall(r"(packaging/shared/\S+)", text["flatpak"]))):
        if not os.path.isfile(os.path.join(root, referenced)):
            failures.append(f"the Flatpak manifest installs {referenced}, which does not exist")

    # -- the .desktop advertises the MIME type the shared MIME definition declares
    mime_type = _capture(r'<mime-type type="([^"]+)"', text["mime"])
    declared = _capture(r"^MimeType=(.*)$", text["desktop"]) or ""
    if mime_type is None:
        failures.append("the shared MIME definition declares no mime-type")
    elif mime_type not in [entry for entry in declared.split(";") if entry]:
        failures.append(
            f"the MIME definition declares {mime_type}, which the .desktop MimeType= omits, "
            "so double-clicking a project file would not open the editor"
        )

    return failures


CHECKS = (
    ("environment example", check_environment_example),
    ("relative links", check_relative_links),
    ("Makefile targets", check_makefile_targets),
    ("release version agreement", check_version_agreement),
    ("README measured claims", check_readme_measurements),
    ("packaging manifest agreement", check_packaging_manifests),
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
