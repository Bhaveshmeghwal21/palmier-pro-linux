// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/file_size_property_test.cpp - a ceiling on how long one translation unit may be.
//
// Why this exists rather than a convention: the production tree had already been brought under a
// 1000-line ceiling once, and the invariant then decayed without anyone noticing. core/EditCommands.hpp
// crept back to 1074 lines because every new command was appended to it, and app/ApplicationComposition.cpp
// reached 1052 because every new hook was. Neither showed up in a review, because a diff that adds
// thirty lines to a thousand-line file looks exactly like a diff that adds thirty lines to a short one.
// A limit nobody measures is a limit that decays, so this measures it.
//
// Two rules, because the two trees are in different states:
//
//   * src/ has NO exceptions. Every production header and translation unit is under the ceiling and
//     must stay there.
//
//   * tests/ has a shrinking allowlist. Nineteen test files were already over the ceiling when this
//     was introduced, and each is pinned at the length it had then. They may shrink, and must be
//     removed from the list once they fall under the ceiling, but they cannot grow. That makes the
//     backlog visible and monotonic instead of a note in a document.
//
// The ceiling is a proxy, not a virtue in itself. What it actually catches is a file that has taken on
// more than one job -- EditCommands.hpp was declaring six unrelated command families, and
// ApplicationComposition.cpp was both wiring components and implementing the adapters it wired. Both
// split along seams that were already there.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifndef PALMIER_SOURCE_DIR
#error "PALMIER_SOURCE_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier::docs {
namespace {

/// The ceiling. One file, one job; past about this length a file is usually doing two.
constexpr std::size_t kMaxLines = 1000;

/// Test files that were already over the ceiling when this property was introduced, each pinned at the
/// length it had then. Entries may only shrink: bring a file under kMaxLines and delete its line.
///
/// The method that worked for the production split applies here too -- move a file's preamble and
/// interleaved helpers into a per-file support header in a named namespace with `inline` functions,
/// then partition whole test suites across new .cpp files added to the SAME CMake target, so
/// gtest_discover_tests registers identical names and the suite's case count is preserved.
const std::map<std::string, std::size_t>& allowlist() {
    static const std::map<std::string, std::size_t> pinned = {
        {"tests/ui/shell_unit_test.cpp", 1846},
        {"tests/docs/suite_hygiene_property_test.cpp", 1441},
        {"tests/services/offline_mode_availability_test.cpp", 1414},
        {"tests/services/generative_atomicity_property_test.cpp", 1191},
        {"tests/services/generative_lifecycle_property_test.cpp", 1293},
        {"tests/services/offline_interpreter_property_test.cpp", 1127},
        {"tests/support/ReportParser.cpp", 1202},
    };
    return pinned;
}

/// Newline count, which is what `wc -l` reports and therefore what the numbers above mean.
[[nodiscard]] std::size_t lineCount(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::size_t lines = 0;
    char c{};
    bool sawAny = false;
    bool endsWithNewline = false;
    while (in.get(c)) {
        sawAny = true;
        endsWithNewline = (c == '\n');
        if (c == '\n') ++lines;
    }
    // A final line with no trailing newline still counts as a line.
    if (sawAny && !endsWithNewline) ++lines;
    return lines;
}

struct Measured {
    std::string relative;
    std::size_t lines;
};

/// Every C++ file under `subdirectory`, as repository-relative paths with '/' separators.
[[nodiscard]] std::vector<Measured> measure(std::string_view subdirectory) {
    const std::filesystem::path root{PALMIER_SOURCE_DIR};
    const std::filesystem::path base = root / subdirectory;
    std::vector<Measured> out;
    if (!std::filesystem::exists(base)) return out;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(base)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        out.push_back({std::filesystem::relative(entry.path(), root).generic_string(),
                       lineCount(entry.path())});
    }
    std::sort(out.begin(), out.end(),
              [](const Measured& a, const Measured& b) { return a.lines > b.lines; });
    return out;
}

// ---------------------------------------------------------------------------

TEST(FileSizeLimit, TheTreesAreActuallyBeingWalked) {
    // Guards the rest of this file: a property that silently measures nothing passes forever. If the
    // walk breaks, or PALMIER_SOURCE_DIR points somewhere unexpected, this is what says so.
    const std::vector<Measured> production = measure("src");
    const std::vector<Measured> tests = measure("tests");
    EXPECT_GT(production.size(), 100u)
        << "expected to find the production tree under " << PALMIER_SOURCE_DIR;
    EXPECT_GT(tests.size(), 100u) << "expected to find the test tree under " << PALMIER_SOURCE_DIR;
}

TEST(FileSizeLimit, EveryProductionFileIsUnderTheCeiling) {
    std::vector<std::string> over;
    for (const Measured& file : measure("src")) {
        if (file.lines > kMaxLines) {
            over.push_back("  " + file.relative + " is " + std::to_string(file.lines) +
                           " lines (ceiling " + std::to_string(kMaxLines) + ")");
        }
    }
    EXPECT_TRUE(over.empty()) << over.size() << " production file(s) over the ceiling:\n"
                              <<
        [&over] {
            std::string joined;
            for (const std::string& line : over) joined += line + "\n";
            return joined;
        }()
                              << "\nSplit along the seam that made it long. src/ has no allowlist: "
                                 "a production file that\n"
                                 "needs one is a file doing two jobs.";
}

TEST(FileSizeLimit, EveryTestFileIsUnderTheCeilingOrPinnedOnTheAllowlist) {
    std::vector<std::string> problems;
    for (const Measured& file : measure("tests")) {
        if (file.lines <= kMaxLines) continue;
        const auto pinned = allowlist().find(file.relative);
        if (pinned == allowlist().end()) {
            problems.push_back(
                "  " + file.relative + " is " + std::to_string(file.lines) +
                " lines and is not on the allowlist -- split it rather than adding it");
        } else if (file.lines > pinned->second) {
            problems.push_back(
                "  " + file.relative + " grew from " + std::to_string(pinned->second) + " to " +
                std::to_string(file.lines) + " lines -- the allowlist only allows these to shrink");
        }
    }
    EXPECT_TRUE(problems.empty()) << problems.size() << " problem(s):\n"
                                  << [&problems] {
                                         std::string joined;
                                         for (const std::string& line : problems)
                                             joined += line + "\n";
                                         return joined;
                                     }();
}

TEST(FileSizeLimit, TheAllowlistCarriesNoEntryThatIsNoLongerNeeded) {
    // Keeps the backlog honest. Without this the list would outlive the problem and stop meaning
    // anything, which is how the production ceiling decayed in the first place.
    const std::filesystem::path root{PALMIER_SOURCE_DIR};
    std::vector<std::string> stale;
    for (const auto& [relative, ceiling] : allowlist()) {
        const std::filesystem::path path = root / relative;
        if (!std::filesystem::exists(path)) {
            stale.push_back("  " + relative + " no longer exists -- delete its allowlist entry");
            continue;
        }
        const std::size_t lines = lineCount(path);
        if (lines <= kMaxLines) {
            stale.push_back("  " + relative + " is now " + std::to_string(lines) +
                            " lines, under the ceiling -- delete its allowlist entry");
        }
    }
    EXPECT_TRUE(stale.empty()) << stale.size() << " stale allowlist entr(y/ies):\n"
                               << [&stale] {
                                      std::string joined;
                                      for (const std::string& line : stale) joined += line + "\n";
                                      return joined;
                                  }();
}

}  // namespace
}  // namespace palmier::docs
