// SPDX-License-Identifier: GPL-3.0-or-later
//
// The documentation_consistency_test cases that stayed in this file after it was split.
//
// Suites here: DocumentationChecker.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "documentation_consistency_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_documentationconsistencytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// The checker is falsifiable — each fault it must catch, injected
// ===========================================================================

TEST(DocumentationChecker, ReportsAnOptionTheTreeDefinesAndTheDocumentationOmits) {
    const std::vector<DocumentedOption> documented{{"PALMIER_BUILD_UI", "BOOL", "User-settable"}};
    const std::vector<LiveOption> live{{"PALMIER_BUILD_UI", "BOOL"}, {"PALMIER_WERROR", "BOOL"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "PALMIER_WERROR");
    EXPECT_EQ(defects.front().document, "docs/BUILD.md");
}
TEST(DocumentationChecker, ReportsAnOptionTheDocumentationInventsAndReportsItsSection) {
    const std::vector<DocumentedOption> documented{
        {"PALMIER_ENABLE_TELEPATHY", "BOOL", "User-settable options"}};
    const std::vector<LiveOption> live{{"PALMIER_BUILD_UI", "BOOL"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    const std::vector<DocDefect> unknown =
        ofKind(defects, DocDefectKind::UnknownDocumentedName);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front().name, "PALMIER_ENABLE_TELEPATHY");
    // Requirement 16.8: the report names the section the name appears in.
    EXPECT_EQ(unknown.front().section, "User-settable options");
}
TEST(DocumentationChecker, ReportsAnOptionDocumentedWithTheWrongType) {
    const std::vector<DocumentedOption> documented{{"PALMIER_PBT_MIN_SUCCESS", "BOOL", "table"}};
    const std::vector<LiveOption> live{{"PALMIER_PBT_MIN_SUCCESS", "STRING"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::TypeMismatch);
}
TEST(DocumentationChecker, ReportsAMissingOptionsRegionRatherThanFindingNothing) {
    std::vector<DocDefect> defects;
    const std::vector<DocumentedOption> options = testsupport::extractDocumentedOptions(
        "# Build\n\n| Option | Type |\n|---|---|\n| `PALMIER_BUILD_UI` | BOOL |\n", defects);
    EXPECT_TRUE(options.empty());
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::SectionMissing);
}
TEST(DocumentationChecker, ReportsAnEmptyOptionsManifestRatherThanAgreeingWithEverything) {
    std::vector<DocDefect> defects;
    const std::vector<LiveOption> options =
        testsupport::parseOptionsManifest("# only a comment\n", defects);
    EXPECT_TRUE(options.empty());
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::InputUnreadable);
}
TEST(DocumentationChecker, ReportsARenamedArgumentInBothDirections) {
    const std::string markdown = R"(# Tools

## `timeline.move_clip`

Move a clip.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string *uuid* | **yes** |
| `startNs` | integer | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    ASSERT_TRUE(extraction.empty()) << testsupport::toString(extraction);
    ASSERT_EQ(documented.size(), 1u);

    const std::vector<LiveTool> live{
        {"timeline.move_clip",
         {{"clipId", "string", true, true}, {"timelineStartNs", "integer", true, false}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::UndocumentedName).size(), 1u)
        << testsupport::toString(defects);
    EXPECT_EQ(ofKind(defects, DocDefectKind::UnknownDocumentedName).size(), 1u)
        << testsupport::toString(defects);
}
TEST(DocumentationChecker, ReportsAFlippedRequiredMarkingAndAChangedJsonType) {
    const std::string markdown = R"(# Tools

## `timeline.add_clip`

Add a clip.

| Argument | Type | Required |
|---|---|---|
| `trackId` | string *uuid* | **yes** |
| `opacity` | integer | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    ASSERT_TRUE(extraction.empty());

    const std::vector<LiveTool> live{
        {"timeline.add_clip",
         {{"trackId", "string", true, true}, {"opacity", "number", false, false}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::TypeMismatch).size(), 1u)
        << testsupport::toString(defects);
    EXPECT_EQ(ofKind(defects, DocDefectKind::RequiredMismatch).size(), 1u)
        << testsupport::toString(defects);
}
TEST(DocumentationChecker, ReportsAMissingUuidMarker) {
    const std::string markdown = R"(# Tools

## `timeline.delete_clip`

Delete a clip.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    const std::vector<LiveTool> live{
        {"timeline.delete_clip", {{"clipId", "string", true, /*uuid=*/true}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::TypeMismatch);
    EXPECT_EQ(defects.front().section, "timeline.delete_clip");
}
TEST(DocumentationChecker, ReportsADocumentedToolOrderThatIsNotThePublishedOrder) {
    const std::vector<DocumentedTool> documented{{"edit.redo"}, {"edit.undo"}};
    const std::vector<LiveTool> live{{"edit.undo", {}}, {"edit.redo", {}}};

    // Both sections say "No arguments." in the real document; here they do not,
    // so filter to the ordering finding this case is about.
    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::OrderMismatch).size(), 1u)
        << testsupport::toString(defects);
}
TEST(DocumentationChecker, ReportsAToolThatTakesNoArgumentsWithoutSayingSo) {
    const std::vector<DocumentedTool> documented{{"project.info"}};  // no "No arguments."
    const std::vector<LiveTool> live{{"project.info", {}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
}
TEST(DocumentationChecker, ReportsAResultFieldTheHandlerReturnsAndTheDocumentationOmits) {
    DocumentedTool documented;
    documented.name = "project.info";
    documented.resultFields = {"projectId"};

    const std::vector<ObservedResult> observed{{"project.info", {"projectId", "undoDepth"}}};

    const std::vector<DocDefect> defects = testsupport::checkResultFields({documented}, observed);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "undoDepth");
}
TEST(DocumentationChecker, ReportsAnUnconditionalResultFieldNoInvocationEverReturns) {
    DocumentedTool documented;
    documented.name = "project.info";
    documented.resultFields = {"projectId", "phaseOfTheMoon"};

    const std::vector<ObservedResult> observed{{"project.info", {"projectId"}}};

    const std::vector<DocDefect> defects = testsupport::checkResultFields({documented}, observed);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UnknownDocumentedName);
    EXPECT_EQ(defects.front().name, "phaseOfTheMoon");
}
TEST(DocumentationChecker, AcceptsAConditionalResultFieldThatNoScenarioProduced) {
    DocumentedTool documented;
    documented.name = "media.import";
    documented.resultFields = {"assetId"};
    documented.conditionalResultFields = {"width"};

    const std::vector<ObservedResult> observed{{"media.import", {"assetId"}}};

    EXPECT_TRUE(testsupport::checkResultFields({documented}, observed).empty());
}
TEST(DocumentationChecker, ReportsAWrongEnvironmentVariableOrFlagForAKnownKey) {
    const std::vector<DocumentedSetting> documented{
        {"remote.port", "PALMIER_REMOTE_PORTS", "--remote-port", "Every configuration input"}};
    const std::vector<LiveSetting> live{{"remote.port", "PALMIER_REMOTE_PORT", "--remote-port"}};

    const std::vector<DocDefect> defects = testsupport::checkSettings(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().name, "PALMIER_REMOTE_PORTS");
    EXPECT_EQ(defects.front().section, "Every configuration input");
}
TEST(DocumentationChecker, ReportsASettingsKeyTheDocumentationDoesNotMention) {
    const std::vector<LiveSetting> live{{"agent.interpreter", "PALMIER_AGENT_INTERPRETER",
                                        "--agent-interpreter"}};
    const std::vector<DocDefect> defects = testsupport::checkSettings({}, live);
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "agent.interpreter");
}
TEST(DocumentationChecker, ReportsAMissingResultRenderingFunctionInsteadOfAnEmptySet) {
    const testsupport::FunctionFieldScan scan = testsupport::scanJsonSetFields(
        "Json somethingElse() { Json out; out.set(\"a\", 1); return out; }", "Json renamedAway(");
    EXPECT_FALSE(scan.found);
    EXPECT_TRUE(scan.fields.empty());
}
TEST(DocumentationChecker, ScansTheFieldNamesARenderingFunctionSets) {
    const testsupport::FunctionFieldScan scan = testsupport::scanJsonSetFields(
        R"(Json render(const Outcome& o) {
    Json out = Json::object();
    out.set("outputPath", o.path);
    if (!o.reason.empty()) out.set("fallbackReason", o.reason);
    return out;
}
Json other() { Json x; x.set("notThisOne", 1); return x; })",
        "Json render(");
    ASSERT_TRUE(scan.found);
    EXPECT_EQ(scan.fields, (std::vector<std::string>{"outputPath", "fallbackReason"}));
}
TEST(DocumentationChecker, ExtractsArgumentsResultFieldsAndConditionalFieldsFromASection) {
    const std::string markdown = R"(# Tools

## `media.import`

Import a file.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `path` | string | **yes** | 1-4096 characters |

Result: `assetId` (string), `duplicate` (bool - already registered).

`width` (integer), `height` (integer) and `fps` (number) are present only for a video asset.

## `timeline.read`

Read it.

No arguments.

Result - the whole project:

| Field | Type | Notes |
|---|---|---|
| `id` | string | project UUID |
| `extra` | string | present only when something |
)";
    std::vector<DocDefect> defects;
    const std::vector<DocumentedTool> tools =
        testsupport::extractDocumentedTools(markdown, defects);
    ASSERT_TRUE(defects.empty()) << testsupport::toString(defects);
    ASSERT_EQ(tools.size(), 2u);

    EXPECT_EQ(tools[0].name, "media.import");
    ASSERT_EQ(tools[0].arguments.size(), 1u);
    EXPECT_EQ(tools[0].arguments.front().name, "path");
    EXPECT_TRUE(tools[0].arguments.front().required);
    EXPECT_EQ(tools[0].resultFields, (std::vector<std::string>{"assetId", "duplicate"}));
    EXPECT_EQ(tools[0].conditionalResultFields,
              (std::vector<std::string>{"width", "height", "fps"}));

    EXPECT_EQ(tools[1].name, "timeline.read");
    EXPECT_TRUE(tools[1].declaresNoArguments);
    EXPECT_TRUE(tools[1].arguments.empty());
    EXPECT_EQ(tools[1].resultFields, (std::vector<std::string>{"id"}));
    EXPECT_EQ(tools[1].conditionalResultFields, (std::vector<std::string>{"extra"}));
}
TEST(DocumentationChecker, TreatsTheCommandResultMarkerAsTheStatusOrNoOpTrio) {
    const std::string markdown = R"(# Tools

## `timeline.add_clip`

Add it.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string *uuid* | no |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> defects;
    const std::vector<DocumentedTool> tools =
        testsupport::extractDocumentedTools(markdown, defects);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_TRUE(tools.front().commandResult);
    for (const char* field : {"status", "noOp", "indication"}) {
        EXPECT_NE(std::find(tools.front().conditionalResultFields.begin(),
                            tools.front().conditionalResultFields.end(), field),
                  tools.front().conditionalResultFields.end())
            << field;
    }
}

}  // namespace
}  // namespace palmier
