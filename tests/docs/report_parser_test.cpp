// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/report_parser_test.cpp — the parity and port-backlog checkers running
// against the CHECKED-IN documents, and proven falsifiable (task 12.3;
// Requirements 13.8, 14.11).
//
// Requirements 13.8 and 14.11 name the Verification_Suite, so the checks run here
// rather than in a CI script. This file is the task-12.3 half of that: the
// example-based cases that prove the parser and the two checkers work at all.
// Tasks 12.4 and 12.5 add the quantified halves (Properties 69-72) over generated
// documents and generated mutations; they consume the very same pure functions.
//
// WHAT MAKES THIS NON-DECORATIVE
// ---------------------------------------------------------------------------
// 1. THE REAL DOCUMENTS ARE CHECKED, and the checks are shown to have SEEN them.
//    `docs/UPSTREAM_PARITY.md` and `docs/PORT_BACKLOG.md` are read from
//    PALMIER_DOCS_DIR and must yield an empty defect list — but every such case
//    first asserts what the parser found (34 entries, 22 + 12 per table, 31
//    build-order items, ten backlog entries, nine acceptance checks). A parser
//    that silently understood nothing would produce an empty defect list too, so
//    "no defects" is only worth asserting beside "this much was parsed".
//
// 2. THE CHECKERS ARE PROVEN TO ACCEPT AND TO REJECT. A synthetic well-formed
//    document of each kind passes with no defect, which rules out a checker that
//    always fails; and each of the nine defect kinds is then produced by a
//    targeted mutation of the real document, which rules out a checker that always
//    passes. Both halves are needed: either alone is satisfiable by a constant
//    function.
//
// 3. TOTALITY IS ASSERTED, NOT ASSUMED. The empty document, a truncated document,
//    a document of unrelated prose and a document whose tables have the wrong
//    column count each yield defects and no exception. This is the property the
//    task calls out: a parser that threw would abort the suite, and a parser that
//    returned an empty report would let `check(parse(doc)).empty()` pass
//    vacuously.
//
// 4. THE FIXED NAME SETS ARE CROSS-CHECKED AGAINST THE REQUIREMENTS. The 22 tool
//    categories, the 12 capability areas and the ten upstream identifiers are
//    transcribed into ReportParser.cpp because the requirements document — not the
//    document under check — is their source of record. `RequiredNamesMatch*` reads
//    the parenthesised lists straight out of `requirements.md` and compares, so a
//    transcription slip fails here instead of quietly shrinking the check.
//
// _Requirements: 13.8, 14.11_

#include "support/ReportParser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_SPEC_DIR
#error "PALMIER_SPEC_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace {

using testsupport::BacklogEntry;
using testsupport::Defect;
using testsupport::DefectKind;
using testsupport::ParityReport;
using testsupport::ParityTable;
using testsupport::PortBacklog;

// ===========================================================================
// Inputs
// ===========================================================================

[[nodiscard]] std::string parityMarkdown() {
    return testsupport::readReportDocument(std::string{PALMIER_DOCS_DIR} + "/UPSTREAM_PARITY.md");
}

[[nodiscard]] std::string backlogMarkdown() {
    return testsupport::readReportDocument(std::string{PALMIER_DOCS_DIR} + "/PORT_BACKLOG.md");
}

[[nodiscard]] std::string requirementsMarkdown() {
    return testsupport::readReportDocument(std::string{PALMIER_SPEC_DIR}
                                           + "/end-to-end-editor-integration/requirements.md");
}

/// One targeted mutation. Every falsifiability case below is a single edit to the
/// real document, so the defect it produces is attributable to that edit.
[[nodiscard]] std::string mutated(std::string_view markdown,
                                  std::string_view from,
                                  std::string_view to) {
    std::string text{markdown};
    const std::size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << "the mutation anchor is absent: " << from;
    if (at == std::string::npos) {
        return text;
    }
    text.replace(at, from.size(), to);
    return text;
}

/// Parse and check in one step, the way a CI gate and the 12.4/12.5 properties do.
[[nodiscard]] std::vector<Defect> checkParity(std::string_view markdown) {
    std::vector<Defect> defects;
    const ParityReport report = testsupport::parseParityReport(markdown, defects);
    const std::vector<Defect> checked = testsupport::checkParityReport(report);
    defects.insert(defects.end(), checked.begin(), checked.end());
    return defects;
}

[[nodiscard]] std::vector<Defect> checkBacklog(std::string_view markdown) {
    std::vector<Defect> defects;
    const PortBacklog backlog = testsupport::parsePortBacklog(markdown, defects);
    const std::vector<Defect> checked = testsupport::checkPortBacklog(backlog);
    defects.insert(defects.end(), checked.begin(), checked.end());
    return defects;
}

// ===========================================================================
// Synthetic well-formed documents — the "the checker can accept" half
// ===========================================================================

/// A minimal Parity_Report that satisfies every rule: all 34 entries `present`,
/// so Requirement 13.3 asks for no priority and no rationale and Requirement
/// 13.9's projection is empty. Built from the required name sets, so it stays
/// well-formed if those sets ever change.
[[nodiscard]] std::string wellFormedParity() {
    std::string text =
        "# Synthetic parity report\n\n"
        "## Provenance\n\n"
        "- upstream-repository: https://github.com/palmier-io/palmier-pro\n"
        "- upstream-ref: v1.2.3\n"
        "- linux-ref: 0123456789abcdef\n"
        "- comparison-date: 2026-08-04\n\n"
        "## Table 1\n\n"
        "| category | status | linux-components | priority | rationale | macos-framework | "
        "linux-replacement |\n|---|---|---|---|---|---|---|\n";
    for (const std::string& name : testsupport::requiredToolCategories()) {
        text += "| " + name + " | present | core::Thing | - | - | - | - |\n";
    }
    text +=
        "\n## Table 2\n\n"
        "| area | status | linux-components | priority | rationale | macos-framework | "
        "linux-replacement |\n|---|---|---|---|---|---|---|\n";
    for (const std::string& name : testsupport::requiredCapabilityAreas()) {
        text += "| " + name + " | present | ui::Thing | - | - | - | - |\n";
    }
    text += "\n## Build order\n\nEvery entry is `present`, so this projection is empty.\n";
    return text;
}

/// A minimal Port_Backlog: one `port` entry per required identifier, each with a
/// complete acceptance check.
[[nodiscard]] std::string wellFormedBacklog() {
    std::string text =
        "# Synthetic port backlog\n\n"
        "## Provenance\n\n"
        "- upstream-repository: https://github.com/palmier-io/palmier-pro\n"
        "- upstream-range: aaaa..bbbb\n"
        "- window: 2026-06-25..2026-07-25\n\n";
    for (const std::string& identifier : testsupport::requiredBacklogIdentifiers()) {
        text += "### " + identifier + " \u2014 a synthetic change\n\n";
        text += "identifier: " + identifier + "\n";
        text += "summary: A one-line summary of the synthetic change under this identifier.\n";
        text += "disposition: port\n";
        text += "linux-component: core::TimelineEngine\n";
        text += "rationale: The change lands in core::TimelineEngine, which is why it ports.\n";
        text += "check:\n";
        text += "  given: a project with one clip on one track\n";
        text += "  when:  the documented action is performed once\n";
        text += "  then:  the documented observable outcome holds\n";
        text += "status: not-started\n\n";
    }
    return text;
}

// ===========================================================================
// The checked-in documents
// ===========================================================================

TEST(ParityReportDocument, ParsesEveryEntryOfTheCheckedInReport) {
    const std::string markdown = parityMarkdown();
    ASSERT_FALSE(markdown.empty()) << "docs/UPSTREAM_PARITY.md could not be read";

    std::vector<Defect> parseDefects;
    const ParityReport report = testsupport::parseParityReport(markdown, parseDefects);

    EXPECT_TRUE(parseDefects.empty()) << "parse defects:\n" << testsupport::toString(parseDefects);
    EXPECT_TRUE(report.categoryTableFound);
    EXPECT_TRUE(report.areaTableFound);
    EXPECT_TRUE(report.buildOrderSectionFound);

    // Non-vacuity: the counts the requirements fix, so "no defects" below is a
    // statement about 34 parsed entries rather than about an empty report.
    EXPECT_EQ(report.entriesIn(ParityTable::ToolCategory).size(),
              testsupport::requiredToolCategories().size());
    EXPECT_EQ(report.entriesIn(ParityTable::CapabilityArea).size(),
              testsupport::requiredCapabilityAreas().size());
    EXPECT_EQ(report.entries.size(), 34U);

    const std::size_t needingPriority =
        static_cast<std::size_t>(std::count_if(report.entries.begin(), report.entries.end(),
                                               [](const testsupport::ParityEntry& entry) {
                                                   return entry.requiresPriority();
                                               }));
    EXPECT_GT(needingPriority, 0U);
    EXPECT_EQ(report.buildOrder.size(), needingPriority)
        << "Requirement 13.9's list is a projection of the absent and partial entries";

    EXPECT_FALSE(report.provenance.upstreamRepository.empty());
    EXPECT_FALSE(report.provenance.upstreamRef.empty());
    EXPECT_FALSE(report.provenance.linuxRef.empty());
    EXPECT_EQ(report.provenance.comparisonDate.size(), 10U);
}

TEST(ParityReportDocument, TheCheckedInReportHasNoDefects) {
    const std::vector<Defect> defects = checkParity(parityMarkdown());
    EXPECT_TRUE(defects.empty()) << "docs/UPSTREAM_PARITY.md (Requirement 13.8):\n"
                                 << testsupport::toString(defects);
}

TEST(PortBacklogDocument, ParsesEveryEntryOfTheCheckedInBacklog) {
    const std::string markdown = backlogMarkdown();
    ASSERT_FALSE(markdown.empty()) << "docs/PORT_BACKLOG.md could not be read";

    std::vector<Defect> parseDefects;
    const PortBacklog backlog = testsupport::parsePortBacklog(markdown, parseDefects);

    EXPECT_TRUE(parseDefects.empty()) << "parse defects:\n" << testsupport::toString(parseDefects);
    EXPECT_EQ(backlog.entries.size(), testsupport::requiredBacklogIdentifiers().size());

    // Every entry carries every field, and every port/adapt entry a complete
    // check — asserted here so the clean run below cannot be a parse that found
    // headings and no fields.
    std::size_t withChecks = 0;
    for (const BacklogEntry& entry : backlog.entries) {
        EXPECT_FALSE(entry.identifier.empty()) << entry.heading;
        EXPECT_FALSE(entry.summary.empty()) << entry.heading;
        EXPECT_FALSE(entry.disposition.empty()) << entry.heading;
        EXPECT_FALSE(entry.rationale.empty()) << entry.heading;
        EXPECT_FALSE(entry.status.empty()) << entry.heading;
        if (entry.requiresCheck()) {
            EXPECT_TRUE(entry.check.complete()) << entry.heading;
            ++withChecks;
        }
    }
    EXPECT_EQ(withChecks, 9U) << "nine of the ten entries are port or adapt; PR 401 is "
                                 "not-applicable and carries no check";

    EXPECT_FALSE(backlog.provenance.upstreamRepository.empty());
    EXPECT_FALSE(backlog.provenance.upstreamRange.empty());
    EXPECT_EQ(backlog.provenance.window, "2026-06-25..2026-07-25");
}

TEST(PortBacklogDocument, TheCheckedInBacklogHasNoDefects) {
    const std::vector<Defect> defects = checkBacklog(backlogMarkdown());
    EXPECT_TRUE(defects.empty()) << "docs/PORT_BACKLOG.md (Requirement 14.11):\n"
                                 << testsupport::toString(defects);
}

// ===========================================================================
// The checkers accept a well-formed document
// ===========================================================================

TEST(ReportCheckers, AcceptAWellFormedSyntheticDocument) {
    const std::vector<Defect> parity = checkParity(wellFormedParity());
    EXPECT_TRUE(parity.empty()) << testsupport::toString(parity);

    const std::vector<Defect> backlog = checkBacklog(wellFormedBacklog());
    EXPECT_TRUE(backlog.empty()) << testsupport::toString(backlog);
}

// ===========================================================================
// Totality — a malformed document yields defects, never an exception
// ===========================================================================

TEST(ReportParserTotality, AnEmptyDocumentIsDefectiveRatherThanClean) {
    const std::vector<Defect> parity = checkParity("");
    EXPECT_FALSE(parity.empty());
    EXPECT_TRUE(testsupport::hasDefect(parity, DefectKind::MissingEntry));

    const std::vector<Defect> backlog = checkBacklog("");
    EXPECT_FALSE(backlog.empty());
    EXPECT_TRUE(testsupport::hasDefect(backlog, DefectKind::MissingEntry));
    for (const std::string& identifier : testsupport::requiredBacklogIdentifiers()) {
        EXPECT_TRUE(testsupport::hasDefect(backlog, DefectKind::MissingEntry, identifier));
    }
}

TEST(ReportParserTotality, UnrelatedProseIsDefectiveRatherThanClean) {
    constexpr std::string_view kProse =
        "# Not a report\n\nSome prose, a | pipe | or two, a `- key: value` in code, and\n"
        "1. a numbered list item that is not a build-order item.\n";
    EXPECT_FALSE(checkParity(kProse).empty());
    EXPECT_FALSE(checkBacklog(kProse).empty());
}

TEST(ReportParserTotality, ATruncatedDocumentYieldsDefectsAndDoesNotThrow) {
    const std::string parity = parityMarkdown();
    const std::string backlog = backlogMarkdown();
    ASSERT_FALSE(parity.empty());
    ASSERT_FALSE(backlog.empty());

    for (const std::size_t fraction : {2U, 3U, 7U}) {
        EXPECT_FALSE(checkParity(parity.substr(0, parity.size() / fraction)).empty());
        EXPECT_FALSE(checkBacklog(backlog.substr(0, backlog.size() / fraction)).empty());
    }
}

TEST(ReportParserTotality, ATableWithTheWrongColumnCountIsReported) {
    const std::string text = mutated(
        wellFormedParity(),
        "| category | status | linux-components | priority | rationale | macos-framework | "
        "linux-replacement |",
        "| category | status | linux-components |");
    const std::vector<Defect> defects = checkParity(text);
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingField))
        << testsupport::toString(defects);
}

// ===========================================================================
// Falsifiability — every defect kind, produced by one edit to the real document
// ===========================================================================

TEST(ParityCheckFalsifiability, DetectsAMissingEntry) {
    const std::vector<Defect> defects =
        checkParity(mutated(parityMarkdown(), "| denoise | absent |", "| | absent |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingEntry, "denoise"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsADuplicatedEntry) {
    const std::vector<Defect> defects =
        checkParity(mutated(parityMarkdown(), "| search | absent | none | later |",
                            "| denoise | absent | none | later | duplicated on purpose to prove "
                            "the check detects it. | - | - |\n| search | absent | none | later |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::DuplicateEntry, "denoise"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAStatusOutsideItsValueSet) {
    const std::vector<Defect> defects =
        checkParity(mutated(parityMarkdown(), "| clips | partial |", "| clips | mostly |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidStatus, "clips"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAPriorityOutsideItsValueSet) {
    const std::vector<Defect> defects = checkParity(
        mutated(parityMarkdown(), "| texts | absent | none | should |",
                "| texts | absent | none | urgent |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidPriority, "texts"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAPriorityOnAPresentEntry) {
    const std::vector<Defect> defects = checkParity(
        mutated(parityMarkdown(), "| import | present |", "| import | present | x | must |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidPriority, "import"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAnEmptyRationaleWhereOneIsRequired) {
    const std::string markdown = parityMarkdown();
    std::vector<Defect> parseDefects;
    const ParityReport report = testsupport::parseParityReport(markdown, parseDefects);
    const testsupport::ParityEntry* subject = nullptr;
    for (const testsupport::ParityEntry& entry : report.entries) {
        if (entry.requiresPriority()) {
            subject = &entry;
            break;
        }
    }
    ASSERT_NE(subject, nullptr);

    const std::vector<Defect> defects =
        checkParity(mutated(markdown, "| " + subject->rationale + " |", "| - |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingRationale, subject->name))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAnOverlongRationale) {
    const std::string markdown = parityMarkdown();
    std::vector<Defect> parseDefects;
    const ParityReport report = testsupport::parseParityReport(markdown, parseDefects);
    ASSERT_FALSE(report.entries.empty());
    const testsupport::ParityEntry& subject = *report.entriesIn(ParityTable::ToolCategory).front();
    ASSERT_TRUE(subject.requiresPriority());

    const std::vector<Defect> defects = checkParity(
        mutated(markdown, "| " + subject.rationale + " |", "| " + std::string(201, 'x') + " |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingRationale, subject.name))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAMissingProvenanceFieldAndAMalformedDate) {
    const std::vector<Defect> removed =
        checkParity(mutated(parityMarkdown(), "- comparison-date: ", "- comparison-taken: "));
    EXPECT_TRUE(testsupport::hasDefect(removed, DefectKind::MissingField, "comparison-date"))
        << testsupport::toString(removed);

    const std::vector<Defect> malformed =
        checkParity(mutated(parityMarkdown(), "comparison-date: 2026-08-04",
                            "comparison-date: 4 August 2026"));
    EXPECT_TRUE(testsupport::hasDefect(malformed, DefectKind::MissingField, "comparison-date"))
        << testsupport::toString(malformed);
}

TEST(ParityCheckFalsifiability, DetectsAnUnsortedBuildOrderList) {
    // Swap the first `should` item ahead of the two `must` items.
    const std::vector<Defect> defects = checkParity(
        mutated(parityMarkdown(), "1. timeline editing (capability area) \u2014 must",
                "1. clips (tool category) \u2014 should"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::OutOfOrder))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAnEntryMissingFromTheBuildOrderList) {
    const std::vector<Defect> defects =
        checkParity(mutated(parityMarkdown(), "22. denoise (tool category) \u2014 later\n", ""));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingEntry, "denoise"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsABuildOrderPriorityThatDisagreesWithItsTable) {
    const std::vector<Defect> defects = checkParity(
        mutated(parityMarkdown(), "31. auto-update (capability area) \u2014 later",
                "31. auto-update (capability area) \u2014 should"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidPriority, "auto-update"))
        << testsupport::toString(defects);
}

TEST(ParityCheckFalsifiability, DetectsAMacosFrameworkWithNoLinuxReplacement) {
    const std::vector<Defect> defects =
        checkParity(mutated(parityMarkdown(), "| SwiftUI | Qt 6 Widgets |", "| SwiftUI | - |"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingField))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAMissingRequiredEntry) {
    const std::vector<Defect> defects =
        checkBacklog(mutated(backlogMarkdown(), "identifier: PR 397", "identifier: PR 3971"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingEntry, "PR 397"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsADuplicateIdentifier) {
    const std::vector<Defect> defects =
        checkBacklog(mutated(backlogMarkdown(), "identifier: PR 405", "identifier: PR 403"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::DuplicateIdentifier, "PR 403"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAMissingIdentifierSummaryAndRationale) {
    const std::string markdown = backlogMarkdown();

    const std::vector<Defect> noIdentifier =
        checkBacklog(mutated(markdown, "identifier: PR 408", "upstream: PR 408"));
    EXPECT_TRUE(testsupport::hasDefect(noIdentifier, DefectKind::MissingField))
        << testsupport::toString(noIdentifier);

    const std::vector<Defect> noSummary =
        checkBacklog(mutated(markdown, "summary: Add a per-channel colour inversion effect",
                             "summary-line: Add a per-channel colour inversion effect"));
    EXPECT_TRUE(testsupport::hasDefect(noSummary, DefectKind::MissingField, "PR 408"))
        << testsupport::toString(noSummary);

    const std::vector<Defect> noRationale =
        checkBacklog(mutated(markdown, "rationale: The effect itself ports directly",
                             "note2: The effect itself ports directly"));
    EXPECT_TRUE(testsupport::hasDefect(noRationale, DefectKind::MissingRationale, "PR 408"))
        << testsupport::toString(noRationale);
}

TEST(BacklogCheckFalsifiability, DetectsADispositionOutsideItsValueSet) {
    const std::vector<Defect> defects =
        checkBacklog(mutated(backlogMarkdown(), "disposition: not-applicable", "disposition: skip"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidStatus, "PR 401"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAStatusOutsideItsValueSet) {
    const std::vector<Defect> defects =
        checkBacklog(mutated(backlogMarkdown(), "status: in-progress", "status: nearly"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::InvalidStatus, "PR 399"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAPortEntryWithNoAcceptanceCheck) {
    // PR 397 is `port`; drop its whole check block, legs and all.
    std::string markdown = backlogMarkdown();
    markdown = mutated(markdown, "check:\n  given: a project with two clips on different tracks",
                       "  given: a project with two clips on different tracks");
    const std::vector<Defect> defects = checkBacklog(markdown);
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingCheck, "PR 397"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAnAcceptanceCheckMissingALeg) {
    const std::vector<Defect> defects = checkBacklog(mutated(
        backlogMarkdown(), "  then:  both grouped clips move by exactly that duration",
        "  outcome:  both grouped clips move by exactly that duration"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingCheck, "PR 397"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAnAcceptanceCheckOnANotApplicableEntry) {
    const std::vector<Defect> defects =
        checkBacklog(mutated(backlogMarkdown(),
                             "status: not-started\nnote: `not-started` here is a formality",
                             "check:\n  given: nothing\n  when:  nothing\n  then:  nothing\n"
                             "status: not-started\nnote: `not-started` here is a formality"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingCheck, "PR 401"))
        << testsupport::toString(defects);
}

TEST(BacklogCheckFalsifiability, DetectsAMissingProvenanceFieldAndAMalformedWindow) {
    const std::vector<Defect> removed =
        checkBacklog(mutated(backlogMarkdown(), "- window: ", "- period: "));
    EXPECT_TRUE(testsupport::hasDefect(removed, DefectKind::MissingField, "window"))
        << testsupport::toString(removed);

    const std::vector<Defect> malformed = checkBacklog(
        mutated(backlogMarkdown(), "window: 2026-06-25..2026-07-25", "window: June to July 2026"));
    EXPECT_TRUE(testsupport::hasDefect(malformed, DefectKind::MissingField, "window"))
        << testsupport::toString(malformed);
}

TEST(BacklogCheckFalsifiability, DetectsAnOverlongSummary) {
    const std::vector<Defect> defects = checkBacklog(
        mutated(backlogMarkdown(), "summary: Refresh pinned dependency versions",
                "summary: " + std::string(201, 'x') + " \u2014 Refresh pinned dependency versions"));
    EXPECT_TRUE(testsupport::hasDefect(defects, DefectKind::MissingField, "PR 399"))
        << testsupport::toString(defects);
}

// ===========================================================================
// The fixed name sets against their source of record
// ===========================================================================

/// The parenthesised list that follows `phrase` in `requirements.md`, split on
/// ", ". Returns an empty vector when the phrase or the parentheses are absent,
/// which the caller asserts against.
[[nodiscard]] std::vector<std::string> parenthesisedListAfter(std::string_view markdown,
                                                             std::string_view phrase) {
    const std::size_t at = markdown.find(phrase);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t open = markdown.find('(', at);
    const std::size_t close = markdown.find(')', open);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return {};
    }
    const std::string_view list = markdown.substr(open + 1, close - open - 1);

    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        std::string_view piece =
            comma == std::string_view::npos ? list.substr(start) : list.substr(start, comma - start);
        while (!piece.empty() && piece.front() == ' ') {
            piece.remove_prefix(1);
        }
        while (!piece.empty() && piece.back() == ' ') {
            piece.remove_suffix(1);
        }
        if (!piece.empty()) {
            names.emplace_back(piece);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

TEST(RequiredNamesMatchTheRequirements, TheToolCategoriesAreRequirement131s) {
    const std::string requirements = requirementsMarkdown();
    ASSERT_FALSE(requirements.empty()) << "requirements.md could not be read";

    const std::vector<std::string> stated =
        parenthesisedListAfter(requirements, "22 upstream agent tool categories");
    ASSERT_EQ(stated.size(), 22U) << "Requirement 13.1's parenthesised list was not found";
    EXPECT_EQ(stated, testsupport::requiredToolCategories());
}

TEST(RequiredNamesMatchTheRequirements, TheCapabilityAreasAreRequirement132s) {
    const std::string requirements = requirementsMarkdown();
    ASSERT_FALSE(requirements.empty());

    const std::vector<std::string> stated =
        parenthesisedListAfter(requirements, "12 upstream user-facing capability areas");
    ASSERT_EQ(stated.size(), 12U) << "Requirement 13.2's parenthesised list was not found";
    EXPECT_EQ(stated, testsupport::requiredCapabilityAreas());
}

TEST(RequiredNamesMatchTheRequirements, TheBacklogIdentifiersAreRequirement142s) {
    const std::string requirements = requirementsMarkdown();
    ASSERT_FALSE(requirements.empty());

    const std::size_t at =
        requirements.find("THE Port_Backlog SHALL include the following identified upstream");
    ASSERT_NE(at, std::string::npos) << "Requirement 14.2 was not found";
    const std::size_t end = requirements.find('\n', at);
    const std::string criterion = requirements.substr(at, end - at);

    std::set<std::string> stated;
    for (std::size_t cursor = criterion.find("PR "); cursor != std::string::npos;
         cursor = criterion.find("PR ", cursor + 1)) {
        std::size_t digits = cursor + 3;
        while (digits < criterion.size()
               && (std::isdigit(static_cast<unsigned char>(criterion[digits])) != 0)) {
            ++digits;
        }
        if (digits > cursor + 3) {
            stated.insert(criterion.substr(cursor, digits - cursor));
        }
    }
    const std::set<std::string> transcribed{testsupport::requiredBacklogIdentifiers().begin(),
                                            testsupport::requiredBacklogIdentifiers().end()};
    ASSERT_EQ(stated.size(), 10U) << "Requirement 14.2 names ten upstream changes";
    EXPECT_EQ(stated, transcribed);
}

}  // namespace
}  // namespace palmier
