#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-tests for scripts/check_repo_consistency.py.
#
# The point of these is the lesson that a checker reporting "clean" is worthless until
# it has been shown to report "dirty" for the defect it claims to catch. So every check
# is exercised twice: once against a tree that is correct, and once against a tree
# carrying exactly the defect that check exists to find.
#
# The last test runs the checks against this repository, which is what makes a passing
# run evidence about the real tree rather than only about fixtures.
#
#     python3 scripts/test_check_repo_consistency.py

from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_repo_consistency as checker

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A minimal well-formed tree: one documented variable that the fake source reads.
GOOD_ENV = "PALMIER_MCP_PORT=\n"
GOOD_SOURCE = 'constexpr std::string_view k = "PALMIER_MCP_PORT";\n'

GOOD_MAKEFILE = """\
.PHONY: build test

build: ## Compile
\tcmake --build build

test: ## Run the suite
\tctest
"""


def write(root: str, relative: str, text: str) -> None:
    path = os.path.join(root, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


class EnvironmentExample(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        for source in checker.ENV_SOURCES:
            write(self.root, source, GOOD_SOURCE)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_a_matching_pair_is_clean(self):
        write(self.root, ".env.example", GOOD_ENV)
        self.assertEqual(checker.check_environment_example(self.root), [])

    def test_documenting_a_variable_nothing_reads_is_a_failure(self):
        write(self.root, ".env.example", GOOD_ENV + "PALMIER_INVENTED_SETTING=\n")
        failures = checker.check_environment_example(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("PALMIER_INVENTED_SETTING", failures[0])
        self.assertIn("no source", failures[0])

    def test_a_variable_read_but_undocumented_is_a_failure(self):
        write(
            self.root,
            checker.ENV_SOURCES[0],
            GOOD_SOURCE + 'constexpr auto x = "PALMIER_NEW_THING";\n',
        )
        write(self.root, ".env.example", GOOD_ENV)
        failures = checker.check_environment_example(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("PALMIER_NEW_THING", failures[0])
        self.assertIn("not mentioned", failures[0])

    def test_a_commented_mention_counts_as_documented(self):
        # The file documents derivation-only variables in comments; that must not
        # register as undocumented.
        write(
            self.root,
            checker.ENV_SOURCES[0],
            GOOD_SOURCE + 'constexpr auto x = "PALMIER_DERIVED";\n',
        )
        write(self.root, ".env.example", GOOD_ENV + "# PALMIER_DERIVED=\n")
        self.assertEqual(checker.check_environment_example(self.root), [])

    def test_a_tooling_variable_is_exempt(self):
        name = sorted(checker.TOOLING_VARIABLES)[0]
        write(self.root, ".env.example", GOOD_ENV + f"{name}=\n")
        self.assertEqual(checker.check_environment_example(self.root), [])

    def test_a_missing_example_file_is_reported(self):
        failures = checker.check_environment_example(self.root)
        self.assertEqual(failures, [".env.example is missing"])


class RelativeLinks(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_a_link_to_a_present_file_is_clean(self):
        write(self.root, "docs/BUILD.md", "build\n")
        write(self.root, "README.md", "See [build](docs/BUILD.md).\n")
        self.assertEqual(checker.check_relative_links(self.root), [])

    def test_a_link_to_an_absent_file_is_a_failure(self):
        write(self.root, "README.md", "See [gone](docs/GONE.md).\n")
        failures = checker.check_relative_links(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("docs/GONE.md", failures[0])

    def test_absolute_urls_are_not_checked(self):
        write(self.root, "README.md", "[ci](https://example.invalid/x) [m](mailto:a@b.c)\n")
        self.assertEqual(checker.check_relative_links(self.root), [])

    def test_a_fragment_is_stripped_before_resolving(self):
        write(self.root, "docs/BUILD.md", "build\n")
        write(self.root, "README.md", "See [opts](docs/BUILD.md#options).\n")
        self.assertEqual(checker.check_relative_links(self.root), [])

    def test_an_html_image_to_an_absent_file_is_a_failure(self):
        # A centred image has to be an <img> tag, because Markdown image syntax cannot be
        # aligned. The check must still see it, or the README's hero image can 404 silently.
        write(self.root, "README.md", '<p align="center"><img src="docs/media/gone.gif"></p>\n')
        failures = checker.check_relative_links(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("docs/media/gone.gif", failures[0])

    def test_an_html_image_to_a_present_file_is_clean(self):
        write(self.root, "docs/media/logo.svg", "<svg/>\n")
        write(self.root, "README.md", '<img width="96" src=\'docs/media/logo.svg\' alt="x">\n')
        self.assertEqual(checker.check_relative_links(self.root), [])

    def test_an_absolute_html_image_is_not_checked(self):
        write(self.root, "README.md", '<img src="https://example.invalid/badge.svg">\n')
        self.assertEqual(checker.check_relative_links(self.root), [])


class MakefileTargets(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_matching_sets_are_clean(self):
        write(self.root, "Makefile", GOOD_MAKEFILE)
        self.assertEqual(checker.check_makefile_targets(self.root), [])

    def test_a_documented_target_missing_from_phony_is_a_failure(self):
        write(self.root, "Makefile", GOOD_MAKEFILE.replace(".PHONY: build test", ".PHONY: build"))
        failures = checker.check_makefile_targets(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'test'", failures[0])
        self.assertIn(".PHONY", failures[0])

    def test_a_phony_target_with_no_description_is_a_failure(self):
        write(
            self.root,
            "Makefile",
            GOOD_MAKEFILE.replace(".PHONY: build test", ".PHONY: build test deploy"),
        )
        failures = checker.check_makefile_targets(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("'deploy'", failures[0])
        self.assertIn("help", failures[0])

    def test_a_backslash_continued_phony_list_is_parsed(self):
        write(
            self.root,
            "Makefile",
            GOOD_MAKEFILE.replace(".PHONY: build test", ".PHONY: build \\\n        test"),
        )
        self.assertEqual(checker.check_makefile_targets(self.root), [])


class ReleaseVersionAgreement(unittest.TestCase):
    """A clean result only means something if a drift is detected, so test both."""

    CMAKE = "project(PalmierProLinux\n    VERSION 0.2.0\n    LANGUAGES CXX)\n"
    CPACK = 'set(CPACK_PACKAGE_VERSION "0.2.0")\n'
    HEADER = (
        "class McpProtocolHandler {\n"
        '    static constexpr std::string_view kDefaultServerVersion = "0.2.0";\n};\n'
    )
    CHANGELOG = (
        "# Changelog\n\n## [Unreleased]\n\n## [0.2.0] - 2026-09-09\n\n## [0.1.0] - 2026-08-31\n"
    )
    # Newest release first, which is both the AppStream convention and what the check
    # relies on to read the current version rather than the oldest one.
    METAINFO = (
        "<component>\n  <releases>\n"
        '    <release version="0.2.0" date="2026-09-09"/>\n'
        '    <release version="0.1.0" date="2026-08-31"/>\n'
        "  </releases>\n</component>\n"
    )
    # `version: 1` is the recipe format, not the app version; the check must not read it.
    APPIMAGE = "version: 1\n\nAppDir:\n  app_info:\n    version: 0.2.0\n"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self._write_all()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_all(
        self, cmake=None, cpack=None, header=None, changelog=None, metainfo=None, appimage=None
    ) -> None:
        write(self.root, "CMakeLists.txt", cmake if cmake is not None else self.CMAKE)
        write(self.root, "packaging/CPackConfig.cmake", cpack if cpack is not None else self.CPACK)
        write(
            self.root,
            "src/services/McpProtocolHandler.hpp",
            header if header is not None else self.HEADER,
        )
        write(self.root, "CHANGELOG.md", changelog if changelog is not None else self.CHANGELOG)
        write(
            self.root,
            "packaging/shared/io.palmier.PalmierPro.metainfo.xml",
            metainfo if metainfo is not None else self.METAINFO,
        )
        write(
            self.root,
            "packaging/appimage/AppImageBuilder.yml",
            appimage if appimage is not None else self.APPIMAGE,
        )

    def test_all_six_agreeing_is_clean(self):
        self.assertEqual(checker.check_version_agreement(self.root), [])

    def test_the_packaging_config_lagging_behind_is_a_failure(self):
        # Exactly the real drift: the tag and CMake moved, the standalone cpack
        # config did not, so the .deb would be stamped with the old version.
        self._write_all(cpack='set(CPACK_PACKAGE_VERSION "0.1.0")\n')
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("disagrees", failures[0])
        self.assertIn("0.1.0", failures[0])
        self.assertIn("CPackConfig.cmake", failures[0])

    def test_the_mcp_server_version_lagging_behind_is_a_failure(self):
        self._write_all(header=self.HEADER.replace("0.2.0", "0.1.0"))
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("McpProtocolHandler.hpp", failures[0])

    def test_a_changelog_without_the_released_version_is_a_failure(self):
        self._write_all(changelog="# Changelog\n\n## [Unreleased]\n\n## [0.1.0] - 2026-08-31\n")
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("CHANGELOG", failures[0])

    def test_the_unreleased_heading_is_not_mistaken_for_a_version(self):
        # [Unreleased] carries no version, so the newest RELEASED heading is the
        # one that must agree. A checker that read the first heading blindly would
        # find no version at all and pass vacuously.
        self.assertEqual(checker.check_version_agreement(self.root), [])
        self._write_all(changelog="# Changelog\n\n## [Unreleased]\n\n## [0.3.0] - 2026-10-01\n")
        self.assertNotEqual(checker.check_version_agreement(self.root), [])

    def test_a_missing_version_line_is_reported_rather_than_ignored(self):
        self._write_all(cpack="# no version here at all\n")
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("no version matching", failures[0])

    def test_the_appstream_metainfo_lagging_behind_is_a_failure(self):
        # The real drift this source was added for: at project version 0.3.0 the metainfo
        # still advertised 0.1.0, so a software centre would name the wrong release.
        self._write_all(metainfo=self.METAINFO.replace("0.2.0", "0.1.0"))
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("metainfo.xml", failures[0])

    def test_the_appimage_recipe_lagging_behind_is_a_failure(self):
        self._write_all(appimage=self.APPIMAGE.replace("    version: 0.2.0", "    version: 0.1.0"))
        failures = checker.check_version_agreement(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("AppImageBuilder.yml", failures[0])

    def test_the_appimage_recipe_format_version_is_not_read_as_the_app_version(self):
        # `version: 1` sits at column 0; the app version is indented under app_info. A
        # pattern that matched either would read 1 and report a spurious disagreement.
        self.assertEqual(checker.check_version_agreement(self.root), [])
        self._write_all(appimage="version: 2\n\nAppDir:\n  app_info:\n    version: 0.2.0\n")
        self.assertEqual(checker.check_version_agreement(self.root), [])


# A README fragment carrying every figure the check inspects, and the two sources it compares them
# against. Two distinct add_executable names plus a repeat of one inside an if/else and one in a
# comment, so the counter is exercised on the shape that made the real README claim 94 for 90.
GOOD_README = """\
The suite is **more than 1800 GoogleTest and RapidCheck cases across 2 test executables**, and
CI runs all of them on every push.

**Line coverage is around 81%**, measured by a job that fails the build below a
floor of 78% and publishes Cobertura XML. Branch coverage is around 46% and is not gated.
"""

GOOD_TEST_CMAKE = """\
add_executable(palmier_one
    one.cpp)

if(SOMETHING)
    add_executable(palmier_two
        two_a.cpp)
else()
    add_executable(palmier_two
        two_b.cpp)
endif()

# A comment that mentions add_executable(palmier_not_real) in prose.
"""

GOOD_WORKFLOW = """\
jobs:
  coverage:
    env:
      COVERAGE_MIN_LINE: 78
"""


class ReadmeMeasuredClaims(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        write(self.root, "README.md", GOOD_README)
        write(self.root, "tests/CMakeLists.txt", GOOD_TEST_CMAKE)
        write(self.root, ".github/workflows/ci.yml", GOOD_WORKFLOW)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_a_well_formed_readme_is_clean(self):
        self.assertEqual(checker.check_readme_measurements(self.root), [])

    def test_distinct_targets_are_counted_not_occurrences(self):
        # The if/else declares one target twice and a comment names a third; the answer is 2.
        self.assertEqual(checker._declared_test_executables(self.root), 2)

    def test_a_wrong_executable_count_is_a_failure(self):
        write(
            self.root, "README.md", GOOD_README.replace("2 test executables", "9 test executables")
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("claims 9 test executables", failures[0])
        self.assertIn("declares 2", failures[0])

    def test_omitting_the_executable_count_is_a_failure(self):
        write(
            self.root, "README.md", GOOD_README.replace("across 2 test executables", "across many")
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("states no test executable count", failures[0])

    def test_a_floor_that_disagrees_with_the_workflow_is_a_failure(self):
        write(self.root, ".github/workflows/ci.yml", GOOD_WORKFLOW.replace("78", "70"))
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("floor of 78%", failures[0])
        self.assertIn("enforces 70%", failures[0])

    def test_a_claim_wrapped_across_lines_is_still_understood(self):
        # README.md wraps its prose, so "90 test executables" and "floor of 78%" can each straddle a
        # line break -- the real README wraps the first of those on purpose. A pattern anchored to one
        # line would find nothing here and report success, which is the one failure mode a
        # consistency check must not have.
        wrapped = GOOD_README.replace("2 test executables", "2 test\nexecutables").replace(
            "floor of 78%", "floor\nof 78%"
        )
        write(self.root, "README.md", wrapped)
        self.assertEqual(checker.check_readme_measurements(self.root), [])

    def test_a_wrapped_claim_that_disagrees_is_still_caught(self):
        write(
            self.root, "README.md", GOOD_README.replace("2 test executables", "9 test\nexecutables")
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("claims 9 test executables", failures[0])

    def test_a_wrapped_brittle_claim_is_still_caught(self):
        write(
            self.root,
            "README.md",
            GOOD_README.replace("around 81%**", "around 81%** (16814\nof 20812 lines)"),
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("exact covered-line count", failures[0])

    def test_an_unhedged_case_count_is_a_failure(self):
        write(
            self.root,
            "README.md",
            GOOD_README.replace("more than 1800 GoogleTest", "1849 GoogleTest"),
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("unhedged test-case count", failures[0])

    def test_every_approximation_word_satisfies_the_hedge(self):
        for word in checker.APPROXIMATION_WORDS:
            write(
                self.root,
                "README.md",
                GOOD_README.replace("more than 1800 GoogleTest", f"{word} 1800 GoogleTest"),
            )
            self.assertEqual(checker.check_readme_measurements(self.root), [], word)

    def test_an_approximation_word_merely_nearby_does_not_satisfy_the_hedge(self):
        # Regression: the first version of this check searched the preceding 40 characters for an
        # approximation word, and README.md says "line-coverage report" just before the count --
        # "coverage" contains "over", so an exact count passed. The hedge has to qualify the number.
        write(
            self.root,
            "README.md",
            GOOD_README.replace(
                "The suite is **more than 1800 GoogleTest",
                "A gcovr line-coverage report is published. The suite is **1849 GoogleTest",
            ),
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("unhedged test-case count", failures[0])

    def test_a_hedge_directly_qualifying_the_number_is_accepted(self):
        write(
            self.root,
            "README.md",
            GOOD_README.replace(
                "The suite is **more than 1800 GoogleTest",
                "A gcovr line-coverage report is published. The suite is **more than 1800 GoogleTest",
            ),
        )
        self.assertEqual(checker.check_readme_measurements(self.root), [])

    def test_an_exact_covered_line_count_is_a_failure(self):
        write(
            self.root,
            "README.md",
            GOOD_README.replace("around 81%**", "around 81%** (16814 of 20812 lines)"),
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("exact covered-line count", failures[0])

    def test_a_one_decimal_coverage_figure_is_a_failure(self):
        write(
            self.root,
            "README.md",
            GOOD_README.replace("coverage is around 81%", "coverage is 80.8%"),
        )
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("one decimal place", failures[0])

    def test_a_missing_readme_is_reported_rather_than_ignored(self):
        os.remove(os.path.join(self.root, "README.md"))
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("README.md is missing", failures[0])

    def test_a_missing_workflow_is_reported_rather_than_ignored(self):
        os.remove(os.path.join(self.root, ".github/workflows/ci.yml"))
        failures = checker.check_readme_measurements(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("coverage floor cannot be checked", failures[0])


class PackagingManifestAgreement(unittest.TestCase):
    """Three formats restate the same identity, and no CI job builds two of them.

    CI builds only the .deb, and only on a version tag, so a rename in the Flatpak or
    AppImage recipe cannot fail any job. These are the checks that would notice.
    """

    DESKTOP = (
        "[Desktop Entry]\nType=Application\nName=Palmier Pro\n"
        "Exec=palmier-pro %F\nTryExec=palmier-pro\nIcon=io.palmier.PalmierPro\n"
        "MimeType=application/x-palmier-project;video/mp4;\n"
    )
    METAINFO = (
        "<component>\n  <id>io.palmier.PalmierPro</id>\n"
        "  <project_license>GPL-3.0-or-later</project_license>\n"
        "  <provides><binary>palmier-pro</binary></provides>\n"
        '  <releases><release version="0.3.0" date="2026-09-10"/></releases>\n'
        "</component>\n"
    )
    MIME = (
        '<mime-info>\n  <mime-type type="application/x-palmier-project">\n'
        '    <glob pattern="*.palmier"/>\n  </mime-type>\n</mime-info>\n'
    )
    FLATPAK = (
        "app-id: io.palmier.PalmierPro\nruntime: org.kde.Platform\nsdk: org.kde.Sdk\n"
        "command: palmier-pro\nrename-desktop-file: io.palmier.PalmierPro.desktop\n"
        "rename-appdata-file: io.palmier.PalmierPro.metainfo.xml\nmodules:\n"
        "  - name: palmier-pro\n    build-commands:\n"
        "      - install -Dm644 packaging/shared/io.palmier.PalmierPro.desktop /app/x\n"
        "      - install -Dm644 packaging/shared/io.palmier.PalmierPro.svg /app/y\n"
    )
    APPIMAGE = (
        "version: 1\n\nAppDir:\n  app_info:\n    id: io.palmier.PalmierPro\n"
        "    name: Palmier Pro\n    version: 0.3.0\n    exec: usr/bin/palmier-pro\n"
    )
    CONTROL = "Source: palmier-pro\n\nPackage: palmier-pro\nArchitecture: amd64\n"
    COPYRIGHT = "Upstream-Name: palmier-pro\n\nFiles: *\nLicense: GPL-3.0-or-later\n"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self._write_all()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_all(self, **overrides) -> None:
        files = {
            "packaging/shared/io.palmier.PalmierPro.desktop": overrides.get(
                "desktop", self.DESKTOP
            ),
            "packaging/shared/io.palmier.PalmierPro.metainfo.xml": overrides.get(
                "metainfo", self.METAINFO
            ),
            "packaging/shared/io.palmier.PalmierPro.xml": overrides.get("mime", self.MIME),
            "packaging/shared/io.palmier.PalmierPro.svg": overrides.get("icon", "<svg/>\n"),
            "packaging/flatpak/io.palmier.PalmierPro.yaml": overrides.get("flatpak", self.FLATPAK),
            "packaging/appimage/AppImageBuilder.yml": overrides.get("appimage", self.APPIMAGE),
            "packaging/deb/debian/control": overrides.get("control", self.CONTROL),
            "packaging/deb/debian/copyright": overrides.get("copyright", self.COPYRIGHT),
        }
        for relative, content in files.items():
            if content is not None:
                write(self.root, relative, content)

    def test_a_consistent_set_of_manifests_is_clean(self):
        self.assertEqual(checker.check_packaging_manifests(self.root), [])

    def test_a_renamed_flatpak_app_id_is_a_failure(self):
        self._write_all(
            flatpak=self.FLATPAK.replace(
                "app-id: io.palmier.PalmierPro", "app-id: io.palmier.Other"
            )
        )
        failures = checker.check_packaging_manifests(self.root)
        self.assertTrue(any("Flatpak app-id" in f for f in failures), failures)

    def test_a_renamed_executable_in_one_format_is_a_failure(self):
        # The AppImage entry point is a path; only its basename is the executable name.
        self._write_all(appimage=self.APPIMAGE.replace("usr/bin/palmier-pro", "usr/bin/palmier"))
        failures = checker.check_packaging_manifests(self.root)
        self.assertTrue(any("AppImage exec" in f for f in failures), failures)

    def test_a_relicensed_debian_package_is_a_failure(self):
        self._write_all(copyright=self.COPYRIGHT.replace("GPL-3.0-or-later", "MIT"))
        failures = checker.check_packaging_manifests(self.root)
        self.assertTrue(any("Debian copyright" in f for f in failures), failures)

    def test_a_desktop_file_that_drops_the_project_mime_type_is_a_failure(self):
        # The editor would still launch, but double-clicking a .palmier file would not
        # open it -- a defect no build or test would otherwise notice.
        self._write_all(desktop=self.DESKTOP.replace("application/x-palmier-project;", ""))
        failures = checker.check_packaging_manifests(self.root)
        self.assertTrue(any("MimeType=" in f for f in failures), failures)

    def test_installing_an_asset_that_does_not_exist_is_a_failure(self):
        self._write_all(flatpak=self.FLATPAK.replace("PalmierPro.svg", "PalmierPro.svgg"))
        failures = checker.check_packaging_manifests(self.root)
        self.assertTrue(any("does not exist" in f for f in failures), failures)

    def test_a_missing_manifest_is_reported_and_stops_the_comparison(self):
        # With a file absent, comparing identities would report noise about the file that
        # is not there; the absence is the finding.
        os.remove(os.path.join(self.root, "packaging/flatpak/io.palmier.PalmierPro.yaml"))
        failures = checker.check_packaging_manifests(self.root)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("is missing", failures[0])

    def test_the_real_packaging_tree_agrees(self):
        self.assertEqual(checker.check_packaging_manifests(REPO_ROOT), [])


class TheRealRepository(unittest.TestCase):
    """What makes a passing run evidence about this tree and not only about fixtures."""

    def test_every_check_passes_against_this_repository(self):
        failures = checker.run_all(REPO_ROOT)
        self.assertEqual(failures, [], "\n".join(failures))

    def test_the_checks_actually_ran_against_a_populated_tree(self):
        # Guards against the checks silently passing because the root was wrong.
        self.assertTrue(os.path.isfile(os.path.join(REPO_ROOT, ".env.example")))
        self.assertTrue(os.path.isfile(os.path.join(REPO_ROOT, "Makefile")))
        # Named rather than only counted: a bare count tells a future reader that
        # something changed but not what, and it was the count alone that failed
        # when the version check was added.
        self.assertEqual(
            [name for name, _check in checker.CHECKS],
            [
                "environment example",
                "relative links",
                "Makefile targets",
                "release version agreement",
                "README measured claims",
                "packaging manifest agreement",
            ],
        )

    def test_main_reports_success_on_this_repository(self):
        self.assertEqual(checker.main(["--root", REPO_ROOT]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
