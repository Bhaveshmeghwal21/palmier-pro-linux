// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the documentation_consistency_test cases, split out of documentation_consistency_test.cpp.
//
// Suites here: DocumentationConsistency.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "documentation_consistency_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_documentationconsistencytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// The checked-in documentation agrees with the running system
// ===========================================================================

TEST(DocumentationConsistency, EveryDocumentAndTheRunningSystemAgreeOnEveryName) {
    const CheckRun& run = checkRun();
    EXPECT_TRUE(run.defects.empty()) << run.defects.size() << " documentation inconsistency(ies):\n"
                                     << testsupport::toString(run.defects);
}
TEST(DocumentationConsistency, TheDocumentedCMakeOptionsAreTheOptionsTheTreeDefines) {
    const CheckRun& run = checkRun();
    ASSERT_FALSE(run.liveOptions.empty()) << "palmier_options.txt carried no PALMIER_* records";
    ASSERT_FALSE(run.options.empty()) << "docs/BUILD.md carried no option rows";
    EXPECT_EQ(run.options.size(), run.liveOptions.size());
    const std::vector<DocDefect> defects = testsupport::checkOptions(run.options, run.liveOptions);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}
TEST(DocumentationConsistency, TheDocumentedToolSurfaceIsWhatToolsListPublishes) {
    const CheckRun& run = checkRun();
    ASSERT_FALSE(run.liveTools.empty()) << "the tool registry advertised nothing";
    EXPECT_EQ(run.tools.size(), run.liveTools.size());
    const std::vector<DocDefect> defects = testsupport::checkTools(run.tools, run.liveTools);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}
TEST(DocumentationConsistency, TheDocumentedConfigurationInputsAreTheRecognisedKeys) {
    const CheckRun& run = checkRun();
    const std::vector<LiveSetting> live = liveSettings();
    ASSERT_FALSE(live.empty()) << "AppSettings recognised no keys";
    const std::vector<DocDefect> defects = testsupport::checkSettings(run.settings, live);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}
TEST(DocumentationConsistency, TheDocumentedResultFieldsAreTheFieldsTheHandlersReturn) {
    const CheckRun& run = checkRun();
    const std::vector<DocDefect> defects = testsupport::checkResultFields(run.tools, run.observed);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}
// Non-vacuity: the scenario must really have exercised the surface. A silently
// empty observation set would make the result check above pass by checking nothing.
TEST(DocumentationConsistency, EveryToolWithARegistryOwnedResultWasActuallyInvoked) {
    const CheckRun& run = checkRun();
    std::vector<std::string> invoked;
    for (const ObservedResult& result : run.observed) {
        if (std::find(invoked.begin(), invoked.end(), result.toolName) == invoked.end()) {
            invoked.push_back(result.toolName);
        }
    }

    // The only tools allowed to be missing are the two whose payload the registry
    // does not render; they are checked against their rendering function instead.
    std::vector<std::string> missing;
    for (const LiveTool& tool : run.liveTools) {
        if (std::find(invoked.begin(), invoked.end(), tool.name) == invoked.end()) {
            missing.push_back(tool.name);
        }
    }
    const std::vector<std::string> expectedMissing{"generation.generate", "timeline.export"};
    std::sort(missing.begin(), missing.end());
    EXPECT_EQ(missing, expectedMissing)
        << "the result scenario must invoke every tool whose success payload the registry itself "
           "renders";
}
// Requirement 16.8's third clause, asserted rather than assumed.
TEST(DocumentationConsistency, LeavesTheDocumentationUnmodified) {
    const std::vector<std::string> documents{docsPath("BUILD.md"), docsPath("TOOLS.md"),
                                             docsPath("REMOTE_ACCESS.md")};
    std::vector<std::string> before;
    for (const std::string& path : documents) {
        before.push_back(testsupport::readWholeFile(path));
        ASSERT_FALSE(before.back().empty()) << path;
    }

    (void)checkRun();

    for (std::size_t i = 0; i < documents.size(); ++i) {
        EXPECT_EQ(testsupport::readWholeFile(documents[i]), before[i])
            << documents[i] << " was modified by the check";
    }
}

}  // namespace
}  // namespace palmier
