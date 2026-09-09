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

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self._write_all()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_all(self, cmake=None, cpack=None, header=None, changelog=None) -> None:
        write(self.root, "CMakeLists.txt", cmake if cmake is not None else self.CMAKE)
        write(self.root, "packaging/CPackConfig.cmake", cpack if cpack is not None else self.CPACK)
        write(
            self.root,
            "src/services/McpProtocolHandler.hpp",
            header if header is not None else self.HEADER,
        )
        write(self.root, "CHANGELOG.md", changelog if changelog is not None else self.CHANGELOG)

    def test_all_four_agreeing_is_clean(self):
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
            ],
        )

    def test_main_reports_success_on_this_repository(self):
        self.assertEqual(checker.main(["--root", REPO_ROOT]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
