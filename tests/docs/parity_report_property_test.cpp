// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/parity_report_property_test.cpp — the quantified half of the
// Parity_Report checks: Property 69 (well-formedness) and Property 70 (the check
// detects every malformation) (task 12.4; Requirements 13.1, 13.2, 13.3, 13.5,
// 13.6, 13.8, 13.9).
//
// HOW THIS FILE RELATES TO TASK 12.3's `tests/docs/report_parser_test.cpp`
// ---------------------------------------------------------------------------
// That file is the EXAMPLE-BASED half: it checks the two checked-in documents,
// proves the parser total, and produces each defect kind by a single targeted TEXT
// EDIT of the real document. This file is the QUANTIFIED half over the very same
// pure functions of `tests/support/ReportParser.hpp`. It therefore does not restate
// those single-edit cases. Instead it:
//
//   * GENERATES whole Parity_Report revisions to the grammar the header documents —
//     34 rows across the two tables, every status/priority/rationale/framework
//     shape, a build-order projection derived from the rows — and asserts the check
//     accepts every one of them (Property 69); and
//   * MUTATES a generated revision at the MODEL level (drop a row, duplicate a row,
//     blank a rationale, unsort the projection, ...), re-renders it, and asserts the
//     check reports the corresponding defect against the offending entry's name
//     (Property 70).
//
// Mutating the model and re-rendering, rather than editing text, is what makes the
// mutation space quantifiable: every mutation is a pure function of (generated
// document, mutation kind, choice), so the property draws over all three.
//
// WHY THESE PROPERTIES CANNOT PASS VACUOUSLY
// ---------------------------------------------------------------------------
// The one failure mode a document check cannot afford is a generator that emits
// something the parser cannot read: `check(parse(unreadable)).empty()` would be a
// green tick over nothing. Four guards, all asserted on every case:
//
//   1. THE CHECKED-IN DOCUMENT IS A CASE OF EVERY RUN. Both properties assert that
//      `docs/UPSTREAM_PARITY.md` was found (non-zero bytes), that both tables and
//      the build-order section were recognised in it, that it holds exactly the 34
//      required entries and a projection of exactly its `absent`/`partial` ones,
//      and that it has no defects. This is design.md's "primary case" for Property
//      69 and the baseline Property 70's mutations are measured against.
//   2. EVERY GENERATED DOCUMENT IS ASSERTED FIELD-FOR-FIELD AGAINST ITS MODEL. Not
//      "no defects" alone: each of the 34 rows is located in the parsed report by
//      (table, name) and its seven cells compared with the model's, the build-order
//      list is compared item for item including its table label and priority, and
//      the provenance block is compared key for key. A parser that returned an
//      empty report, or a renderer that emitted a document the parser skipped, fails
//      here rather than passing quietly.
//   3. THE MUTANT IS ASSERTED TO STILL BE A READABLE DOCUMENT. Property 70 asserts
//      the mutant's parsed row count and build-order count equal the mutated
//      model's, that every row name and every item name in the mutated model was
//      read back, that both tables and the build-order heading were still
//      recognised, and that the defect list does NOT contain the parser's
//      "the report holds no entries at all" defect. So the defect being asserted is
//      attributable to the injected fault and not to a garbled document.
//   4. THE UNMUTATED DOCUMENT OF THE SAME CASE IS ASSERTED CLEAN. Property 70 checks
//      the base rendering first: a mutation is only evidence if the thing it was
//      applied to passed.
//
// THE DEFECT KINDS THIS DOCUMENT'S GRAMMAR CAN EXPRESS
// ---------------------------------------------------------------------------
// Seven of the nine: `MissingEntry`, `DuplicateEntry`, `InvalidStatus`,
// `InvalidPriority`, `MissingRationale`, `MissingField` and `OutOfOrder`. The other
// two — `MissingCheck` and `DuplicateIdentifier` — are Port_Backlog kinds
// (Requirements 14.3, 14.10) and are covered by Property 72 in
// `tests/docs/port_backlog_property_test.cpp`; the union of the two files is the
// whole nine-kind vocabulary, which
// `ParityCheckMutations.EveryMutationKindIsDetectedAndNamesItsEntry` states and
// asserts. That test also drives EVERY mutation kind on a fixed document on every
// run, so full kind coverage does not depend on the sampling of the property.
//
// COST
// ---------------------------------------------------------------------------
// The checked-in document is read and checked ONCE per process (a function-local
// static); each generated case builds a ~40-row Markdown string and parses it,
// which is microseconds. The whole binary stays far inside the 600 s per-test limit.
//
// _Requirements: 13.1, 13.2, 13.3, 13.5, 13.6, 13.8, 13.9_

#include "support/ReportParser.hpp"

#include <gtest/gtest.h>

#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace {

using testsupport::Defect;
using testsupport::DefectKind;
using testsupport::ParityEntry;
using testsupport::ParityReport;
using testsupport::ParityTable;

// ===========================================================================
// Parse and check in one step — exactly what a CI gate does
// ===========================================================================

struct Checked {
    std::size_t         markdownBytes{0};
    ParityReport        report;
    std::vector<Defect> defects;  ///< parse defects followed by check defects
};

[[nodiscard]] Checked checkMarkdown(std::string_view markdown) {
    Checked outcome;
    outcome.markdownBytes = markdown.size();
    outcome.report = testsupport::parseParityReport(markdown, outcome.defects);
    const std::vector<Defect> checked = testsupport::checkParityReport(outcome.report);
    outcome.defects.insert(outcome.defects.end(), checked.begin(), checked.end());
    return outcome;
}

/// `docs/UPSTREAM_PARITY.md`, read and checked once per process. Read from the
/// SOURCE tree via PALMIER_DOCS_DIR: ctest's working directory is the build tree,
/// where the document does not exist, and a check that cannot find its input would
/// pass vacuously — hence the byte-count assertion at every use.
const Checked& checkedInReport() {
    static const Checked outcome = checkMarkdown(
        testsupport::readReportDocument(std::string{PALMIER_DOCS_DIR} + "/UPSTREAM_PARITY.md"));
    return outcome;
}

[[nodiscard]] std::size_t requiredEntryCount() {
    return testsupport::requiredToolCategories().size()
           + testsupport::requiredCapabilityAreas().size();
}

[[nodiscard]] std::size_t priorityBearingEntries(const ParityReport& report) {
    return static_cast<std::size_t>(std::count_if(
        report.entries.begin(), report.entries.end(),
        [](const ParityEntry& entry) { return entry.requiresPriority(); }));
}

[[nodiscard]] const ParityEntry* findEntry(const ParityReport& report,
                                          ParityTable table,
                                          const std::string& name) {
    for (const ParityEntry& entry : report.entries) {
        if (entry.table == table && entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

// ===========================================================================
// The document model — a Parity_Report as values, plus a renderer
// ===========================================================================
//
// The generator builds this model and renders it to Markdown; the mutations of
// Property 70 edit the model and re-render. The renderer writes the grammar
// `ReportParser.hpp` documents: four provenance bullets, two 7-column tables
// recognised by their first header cell, and a `Build order` heading over `N. `
// items.

struct Row {
    ParityTable table{ParityTable::ToolCategory};
    std::string name;
    std::string status{"present"};
    std::string components{"core::TimelineEngine"};
    std::string priority;     ///< empty unless the status is absent or partial
    std::string rationale;    ///< empty unless the status is absent or partial
    std::string framework;    ///< a macOS framework, or empty
    std::string replacement;  ///< a Linux technology, `out-of-scope: <reason>`, or empty
    /// Render this row with its last cell missing — the wrong-column-count fault.
    bool dropLastCell{false};

    [[nodiscard]] bool requiresPriority() const {
        return status == "absent" || status == "partial";
    }
};

struct Item {
    std::string name;
    ParityTable table{ParityTable::ToolCategory};
    std::string priority;
    /// Render this item without its `(tool category)` / `(capability area)` label.
    bool omitTableLabel{false};
};

struct Doc {
    std::string      repository{"https://github.com/palmier-io/palmier-pro"};
    std::string      upstreamRef{"v2026.07.1"};
    std::string      linuxRef{"16274d51b77ac9ffcf8db49592d2a90411610ab5"};
    std::string      comparisonDate{"2026-08-04"};
    std::vector<Row> rows;
    std::vector<Item> buildOrder;
};

[[nodiscard]] std::string_view tableLabel(ParityTable table) {
    return table == ParityTable::ToolCategory ? "tool category" : "capability area";
}

/// `-` is how both documents spell "no value", and `normalizeValue` reads it back
/// as the empty string.
[[nodiscard]] std::string cell(const std::string& value) {
    return value.empty() ? std::string{"-"} : value;
}

[[nodiscard]] std::string renderTable(const Doc& doc, ParityTable table) {
    const std::string first =
        table == ParityTable::ToolCategory ? std::string{"category"} : std::string{"area"};
    std::string text = "| " + first
                       + " | status | linux-components | priority | rationale | macos-framework "
                         "| linux-replacement |\n|---|---|---|---|---|---|---|\n";
    for (const Row& row : doc.rows) {
        if (row.table != table) {
            continue;
        }
        text += "| " + row.name + " | " + cell(row.status) + " | " + cell(row.components) + " | "
                + cell(row.priority) + " | " + cell(row.rationale) + " | " + cell(row.framework);
        if (!row.dropLastCell) {
            text += " | " + cell(row.replacement);
        }
        text += " |\n";
    }
    return text;
}

[[nodiscard]] std::string render(const Doc& doc) {
    // A provenance value left empty is rendered as no bullet at all, which is how
    // the "omitted provenance field" mutation is expressed.
    std::string text = "# A generated parity report\n\n## Provenance\n\n";
    const auto bullet = [&text](std::string_view key, const std::string& value) {
        if (!value.empty()) {
            text += "- ";
            text += key;
            text += ": " + value + "\n";
        }
    };
    bullet("upstream-repository", doc.repository);
    bullet("upstream-ref", doc.upstreamRef);
    bullet("linux-ref", doc.linuxRef);
    bullet("comparison-date", doc.comparisonDate);

    text += "\n## Tool categories\n\n" + renderTable(doc, ParityTable::ToolCategory);
    text += "\n## Capability areas\n\n" + renderTable(doc, ParityTable::CapabilityArea);

    text += "\n## Build order (Requirement 13.9)\n\n";
    if (doc.buildOrder.empty()) {
        text += "Every entry is `present`, so this projection is empty.\n";
    }
    for (std::size_t i = 0; i < doc.buildOrder.size(); ++i) {
        const Item& item = doc.buildOrder[i];
        text += std::to_string(i + 1) + ". " + item.name;
        if (!item.omitTableLabel) {
            text += " (" + std::string(tableLabel(item.table)) + ")";
        }
        text += " \u2014 " + item.priority + "\n";
    }
    return text;
}

/// Requirement 13.9's projection: exactly the `absent` and `partial` rows, `must`
/// before `should` before `later`, document order within a priority.
void rebuildBuildOrder(Doc& doc) {
    doc.buildOrder.clear();
    for (const std::string& priority : testsupport::parityPriorityValues()) {
        for (const Row& row : doc.rows) {
            if (row.requiresPriority() && row.priority == priority) {
                doc.buildOrder.push_back(Item{row.name, row.table, row.priority, false});
            }
        }
    }
}

// ===========================================================================
// Generation
// ===========================================================================

const std::vector<std::string>& componentPool() {
    static const std::vector<std::string> pool{"core::TimelineEngine",
                                               "services::ToolRegistry",
                                               "ui::MainWindow, ui::TimelinePanel",
                                               "media::ExportEngine",
                                               "gpu::Compositor, gpu::EffectKernels"};
    return pool;
}

const std::vector<std::string>& frameworkPool() {
    static const std::vector<std::string> pool{"AVFoundation", "SwiftUI", "Core Image",
                                               "VideoToolbox", "Speech"};
    return pool;
}

const std::vector<std::string>& replacementPool() {
    static const std::vector<std::string> pool{"FFmpeg (libavcodec)", "Qt 6 Widgets", "Vulkan",
                                               "whisper.cpp"};
    return pool;
}

/// A uniform index in [0, bound), shrinking towards 0.
///
/// `rc::gen::inRange` SCALES its range with RapidCheck's `size` parameter, which
/// rises across a run — so a bare `inRange` would draw only low indices in the early
/// cases. For a categorical choice (a status, a priority, a pool entry, a mutation
/// kind) that means whole categories never being exercised: measured over a 100-case
/// run, five of the twenty mutation kinds were never drawn. Resizing to the nominal
/// size makes every draw uniform over the whole range while keeping `inRange`'s
/// shrinking, so the reported counterexample is still the small one.
[[nodiscard]] std::size_t drawIndex(std::size_t bound) {
    return *rc::gen::resize(rc::kNominalSize, rc::gen::inRange<std::size_t>(0, bound));
}

[[nodiscard]] bool drawFlag() { return drawIndex(2) == 1; }

[[nodiscard]] const std::string& drawFrom(const std::vector<std::string>& pool) {
    return pool[drawIndex(pool.size())];
}

/// ASCII text of exactly `length` characters (so a code-point count and a byte
/// count agree), never ending in a space, never containing `|`. Used for the
/// 1-200 character rationale bound of Requirements 13.3 and 13.5, whose boundaries
/// the generator draws over.
[[nodiscard]] std::string textOfLength(std::string seed, std::size_t length) {
    static constexpr std::string_view kFiller =
        " and the blocking dependency is recorded beside it";
    while (seed.size() < length) {
        seed.append(kFiller);
    }
    seed.resize(length);
    if (!seed.empty() && seed.back() == ' ') {
        seed.back() = '.';
    }
    return seed;
}

[[nodiscard]] std::string drawRationale(const std::string& name) {
    return textOfLength("the " + name + " operations are not reachable from the Tool_Surface",
                        drawIndex(200) + 1);
}

[[nodiscard]] std::string drawOutOfScope() {
    return "out-of-scope: "
           + textOfLength("no Linux equivalent of this macOS framework is in scope",
                          drawIndex(200) + 1);
}

[[nodiscard]] Row drawRow(ParityTable table, std::string name) {
    Row row;
    row.table = table;
    row.name = std::move(name);
    row.status = testsupport::parityStatusValues()[drawIndex(3)];
    // `linux-components: none` states that no Linux component exists, which the
    // checker reads as implying `absent`; so only an `absent` row may say `none`.
    row.components = (row.status == "absent" && drawFlag()) ? std::string{"none"}
                                                           : drawFrom(componentPool());
    if (row.requiresPriority()) {
        row.priority = testsupport::parityPriorityValues()[drawIndex(3)];
        row.rationale = drawRationale(row.name);
    }
    if (drawFlag()) {
        row.framework = drawFrom(frameworkPool());
        row.replacement = drawFlag() ? drawFrom(replacementPool()) : drawOutOfScope();
    }
    return row;
}

/// Force `row` to `present`: no priority, no rationale, a named component.
void makePresent(Row& row) {
    row.status = "present";
    row.priority.clear();
    row.rationale.clear();
    if (row.components == "none") {
        row.components = componentPool().front();
    }
}

/// Force `row` to a priority-bearing status carrying `priority`.
void makePriority(Row& row, const std::string& priority) {
    if (!row.requiresPriority()) {
        row.status = "partial";
        row.components = componentPool().front();
    }
    row.priority = priority;
    if (row.rationale.empty()) {
        row.rationale = "the " + row.name + " workflow is blocked and this entry records why";
    }
}

/// A generated well-formed revision.
///
/// The row ORDER within each table is shuffled (no requirement fixes it), and the
/// statuses, components, priorities, rationale lengths and macOS-framework shapes
/// are drawn per row. Six positions are then overwritten to GUARANTEE that every
/// mutation of Property 70 has a target and that every clause of Property 69 is
/// actually exercised: a `present` row, one row of each priority, a row whose macOS
/// framework carries a named replacement, a row whose framework is `out-of-scope`,
/// and a row with no framework at all. The names at those positions vary with the
/// shuffle, so the guarantee fixes shapes, not entries.
[[nodiscard]] Doc drawDoc() {
    Doc doc;
    const auto appendTable = [&doc](ParityTable table, const std::vector<std::string>& names) {
        const std::size_t begin = doc.rows.size();
        for (const std::string& name : names) {
            doc.rows.push_back(drawRow(table, name));
        }
        // Fisher-Yates over the drawn segment, so row order is generated too.
        for (std::size_t i = doc.rows.size() - 1; i > begin; --i) {
            std::swap(doc.rows[i], doc.rows[begin + drawIndex(i - begin + 1)]);
        }
    };
    appendTable(ParityTable::ToolCategory, testsupport::requiredToolCategories());
    appendTable(ParityTable::CapabilityArea, testsupport::requiredCapabilityAreas());

    makePresent(doc.rows[0]);
    makePriority(doc.rows[1], "must");
    makePriority(doc.rows[2], "should");
    makePriority(doc.rows[3], "later");

    doc.rows[4].framework = frameworkPool().front();
    doc.rows[4].replacement = replacementPool().front();
    doc.rows[5].framework = frameworkPool().back();
    doc.rows[5].replacement = drawOutOfScope();
    doc.rows[6].framework.clear();
    doc.rows[6].replacement.clear();

    rebuildBuildOrder(doc);
    return doc;
}

/// The same shape with nothing drawn, for the exhaustive mutation-coverage test:
/// statuses cycle present / partial / absent and priorities cycle must / should /
/// later, so all three of each appear, and rows 1, 2 and 6 carry the three
/// macOS-framework shapes.
[[nodiscard]] Doc fixedDoc() {
    Doc doc;
    const auto appendTable = [&doc](ParityTable table, const std::vector<std::string>& names) {
        std::size_t i = 0;
        for (const std::string& name : names) {
            Row row;
            row.table = table;
            row.name = name;
            row.status = testsupport::parityStatusValues()[i % 3];
            row.components = row.status == "absent" ? std::string{"none"}
                                                    : componentPool()[i % componentPool().size()];
            if (row.requiresPriority()) {
                row.priority = testsupport::parityPriorityValues()[(i / 3) % 3];
                row.rationale = "the " + name + " operations are unreachable from the product "
                                                "surface, which this entry records";
            }
            doc.rows.push_back(std::move(row));
            ++i;
        }
    };
    appendTable(ParityTable::ToolCategory, testsupport::requiredToolCategories());
    appendTable(ParityTable::CapabilityArea, testsupport::requiredCapabilityAreas());

    makePresent(doc.rows[0]);
    makePriority(doc.rows[1], "must");
    makePriority(doc.rows[2], "should");
    makePriority(doc.rows[3], "later");

    doc.rows[1].framework = frameworkPool().front();
    doc.rows[1].replacement = replacementPool().front();
    doc.rows[2].framework = frameworkPool().back();
    doc.rows[2].replacement = "out-of-scope: no Linux equivalent of this framework is in scope";
    doc.rows[6].framework.clear();
    doc.rows[6].replacement.clear();

    rebuildBuildOrder(doc);
    return doc;
}

// ===========================================================================
// Mutation — every malformation Requirement 13.8 names, and then some
// ===========================================================================

enum class Mutation {
    OmitARequiredEntry,
    AddAnEntryOutsideTheRequiredSet,
    DuplicateAnEntry,
    StatusOutsideItsValueSet,
    PriorityOutsideItsValueSet,
    PriorityOnAPresentEntry,
    BlankARequiredRationale,
    OverlongRationale,
    RationaleOnAPresentEntry,
    DropAColumnFromARow,
    OmitAProvenanceField,
    MalformedComparisonDate,
    MacosFrameworkWithNoReplacement,
    ReplacementWithNoMacosFramework,
    OmitABuildOrderItem,
    BuildOrderItemWithNoTableLabel,
    BuildOrderPriorityDisagreesWithTheTable,
    DuplicateABuildOrderItem,
    UnsortedBuildOrder,
    BuildOrderListsAPresentEntry
};

constexpr std::array<Mutation, 20> kMutations{
    Mutation::OmitARequiredEntry,
    Mutation::AddAnEntryOutsideTheRequiredSet,
    Mutation::DuplicateAnEntry,
    Mutation::StatusOutsideItsValueSet,
    Mutation::PriorityOutsideItsValueSet,
    Mutation::PriorityOnAPresentEntry,
    Mutation::BlankARequiredRationale,
    Mutation::OverlongRationale,
    Mutation::RationaleOnAPresentEntry,
    Mutation::DropAColumnFromARow,
    Mutation::OmitAProvenanceField,
    Mutation::MalformedComparisonDate,
    Mutation::MacosFrameworkWithNoReplacement,
    Mutation::ReplacementWithNoMacosFramework,
    Mutation::OmitABuildOrderItem,
    Mutation::BuildOrderItemWithNoTableLabel,
    Mutation::BuildOrderPriorityDisagreesWithTheTable,
    Mutation::DuplicateABuildOrderItem,
    Mutation::UnsortedBuildOrder,
    Mutation::BuildOrderListsAPresentEntry};

[[nodiscard]] std::string_view describe(Mutation kind) {
    switch (kind) {
    case Mutation::OmitARequiredEntry:
        return "omit a required entry";
    case Mutation::AddAnEntryOutsideTheRequiredSet:
        return "add an entry outside the required set";
    case Mutation::DuplicateAnEntry:
        return "duplicate an entry";
    case Mutation::StatusOutsideItsValueSet:
        return "a status outside its value set";
    case Mutation::PriorityOutsideItsValueSet:
        return "a priority outside its value set";
    case Mutation::PriorityOnAPresentEntry:
        return "a priority on a present entry";
    case Mutation::BlankARequiredRationale:
        return "blank a required rationale";
    case Mutation::OverlongRationale:
        return "a rationale over the 200-character bound";
    case Mutation::RationaleOnAPresentEntry:
        return "a rationale on a present entry";
    case Mutation::DropAColumnFromARow:
        return "drop a column from a row";
    case Mutation::OmitAProvenanceField:
        return "omit a provenance field";
    case Mutation::MalformedComparisonDate:
        return "a comparison date outside YYYY-MM-DD";
    case Mutation::MacosFrameworkWithNoReplacement:
        return "a macOS framework with no Linux replacement";
    case Mutation::ReplacementWithNoMacosFramework:
        return "a Linux replacement with no macOS framework";
    case Mutation::OmitABuildOrderItem:
        return "omit a build-order item";
    case Mutation::BuildOrderItemWithNoTableLabel:
        return "a build-order item naming no table";
    case Mutation::BuildOrderPriorityDisagreesWithTheTable:
        return "a build-order priority disagreeing with the table";
    case Mutation::DuplicateABuildOrderItem:
        return "duplicate a build-order item";
    case Mutation::UnsortedBuildOrder:
        return "an unsorted build-order list";
    case Mutation::BuildOrderListsAPresentEntry:
        return "a build-order list holding a present entry";
    }
    return "unknown mutation";
}

/// One applied mutation: the mutated model, its rendering, and the defect the
/// check must report against which entry name (Requirement 13.8's "each offending
/// entry by category or capability-area name and the specific defect").
struct Mutant {
    bool        applicable{true};
    Doc         doc;
    std::string markdown;
    DefectKind  expected{DefectKind::MissingField};
    std::string entry;
};

template <typename Predicate>
[[nodiscard]] std::vector<std::size_t> rowsWhere(const Doc& doc, Predicate predicate) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < doc.rows.size(); ++i) {
        if (predicate(doc.rows[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

/// Pure in (base, kind, choice): `choice` selects the target row, item or variant
/// by modulo, so the property draws it and the coverage test enumerates it.
[[nodiscard]] Mutant mutate(const Doc& base, Mutation kind, std::size_t choice) {
    Mutant mutant;
    mutant.doc = base;
    Doc& doc = mutant.doc;

    const auto target = [&](const std::vector<std::size_t>& candidates) -> std::size_t {
        if (candidates.empty()) {
            mutant.applicable = false;  // a generator guarantee was broken; asserted by callers
            return 0;
        }
        return candidates[choice % candidates.size()];
    };
    const auto anyRow = [](const Row&) { return true; };

    switch (kind) {
    case Mutation::OmitARequiredEntry: {
        const std::size_t i = target(rowsWhere(doc, anyRow));
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = doc.rows[i].name;
        doc.rows.erase(doc.rows.begin() + static_cast<std::ptrdiff_t>(i));
        rebuildBuildOrder(doc);  // the projection follows the rows it projects
        break;
    }
    case Mutation::AddAnEntryOutsideTheRequiredSet: {
        Row row;
        row.table = (choice % 2 == 0) ? ParityTable::ToolCategory : ParityTable::CapabilityArea;
        row.name = (choice % 2 == 0) ? "holograms" : "colour management";
        makePresent(row);
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = row.name;
        doc.rows.push_back(std::move(row));
        break;
    }
    case Mutation::DuplicateAnEntry: {
        const std::size_t i = target(rowsWhere(doc, anyRow));
        mutant.expected = DefectKind::DuplicateEntry;
        mutant.entry = doc.rows[i].name;
        const Row copy = doc.rows[i];
        doc.rows.insert(doc.rows.begin() + static_cast<std::ptrdiff_t>(i) + 1, copy);
        break;
    }
    case Mutation::StatusOutsideItsValueSet: {
        const std::size_t i = target(rowsWhere(doc, anyRow));
        static const std::array<std::string_view, 3> kBogus{"mostly", "PRESENT", "unknown"};
        mutant.expected = DefectKind::InvalidStatus;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].status = std::string(kBogus[choice % kBogus.size()]);
        rebuildBuildOrder(doc);  // isolate the fault: the projection follows the statuses
        break;
    }
    case Mutation::PriorityOutsideItsValueSet: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return row.requiresPriority(); }));
        mutant.expected = DefectKind::InvalidPriority;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].priority = (choice % 2 == 0) ? "urgent" : "MUST";
        break;
    }
    case Mutation::PriorityOnAPresentEntry: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return !row.requiresPriority(); }));
        mutant.expected = DefectKind::InvalidPriority;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].priority = testsupport::parityPriorityValues()[choice % 3];
        break;
    }
    case Mutation::BlankARequiredRationale: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return row.requiresPriority(); }));
        mutant.expected = DefectKind::MissingRationale;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].rationale.clear();
        break;
    }
    case Mutation::OverlongRationale: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return row.requiresPriority(); }));
        mutant.expected = DefectKind::MissingRationale;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].rationale = textOfLength("this rationale is one character over the bound",
                                             201 + (choice % 5));
        break;
    }
    case Mutation::RationaleOnAPresentEntry: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return !row.requiresPriority(); }));
        mutant.expected = DefectKind::MissingRationale;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].rationale = "a rationale where the field rules permit none";
        break;
    }
    case Mutation::DropAColumnFromARow: {
        const std::size_t i = target(rowsWhere(doc, anyRow));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].dropLastCell = true;
        break;
    }
    case Mutation::OmitAProvenanceField: {
        static const std::array<std::string_view, 4> kKeys{"upstream-repository", "upstream-ref",
                                                           "linux-ref", "comparison-date"};
        const std::size_t which = choice % kKeys.size();
        mutant.expected = DefectKind::MissingField;
        mutant.entry = std::string(kKeys[which]);
        switch (which) {
        case 0:
            doc.repository.clear();
            break;
        case 1:
            doc.upstreamRef.clear();
            break;
        case 2:
            doc.linuxRef.clear();
            break;
        default:
            doc.comparisonDate.clear();
            break;
        }
        break;
    }
    case Mutation::MalformedComparisonDate: {
        static const std::array<std::string_view, 3> kDates{"4 August 2026", "2026-8-4",
                                                            "2026/08/04"};
        mutant.expected = DefectKind::MissingField;
        mutant.entry = "comparison-date";
        doc.comparisonDate = std::string(kDates[choice % kDates.size()]);
        break;
    }
    case Mutation::MacosFrameworkWithNoReplacement: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return !row.framework.empty(); }));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].replacement.clear();
        break;
    }
    case Mutation::ReplacementWithNoMacosFramework: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return row.framework.empty(); }));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.rows[i].name;
        doc.rows[i].replacement = replacementPool()[choice % replacementPool().size()];
        break;
    }
    case Mutation::OmitABuildOrderItem: {
        if (doc.buildOrder.empty()) {
            mutant.applicable = false;
            break;
        }
        const std::size_t i = choice % doc.buildOrder.size();
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = doc.buildOrder[i].name;
        doc.buildOrder.erase(doc.buildOrder.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    case Mutation::BuildOrderItemWithNoTableLabel: {
        if (doc.buildOrder.empty()) {
            mutant.applicable = false;
            break;
        }
        const std::size_t i = choice % doc.buildOrder.size();
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.buildOrder[i].name;
        doc.buildOrder[i].omitTableLabel = true;
        break;
    }
    case Mutation::BuildOrderPriorityDisagreesWithTheTable: {
        if (doc.buildOrder.empty()) {
            mutant.applicable = false;
            break;
        }
        const std::size_t i = choice % doc.buildOrder.size();
        Item& item = doc.buildOrder[i];
        mutant.expected = DefectKind::InvalidPriority;
        mutant.entry = item.name;
        for (const std::string& priority : testsupport::parityPriorityValues()) {
            if (priority != item.priority) {
                item.priority = priority;
                break;
            }
        }
        break;
    }
    case Mutation::DuplicateABuildOrderItem: {
        if (doc.buildOrder.empty()) {
            mutant.applicable = false;
            break;
        }
        const std::size_t i = choice % doc.buildOrder.size();
        mutant.expected = DefectKind::DuplicateEntry;
        mutant.entry = doc.buildOrder[i].name;
        const Item copy = doc.buildOrder[i];
        doc.buildOrder.insert(doc.buildOrder.begin() + static_cast<std::ptrdiff_t>(i) + 1, copy);
        break;
    }
    case Mutation::UnsortedBuildOrder: {
        // Rotate the last item to the front. The generator guarantees one `must`
        // and one `later` item, so the front and back ranks differ and the item
        // that ends up second is the one the checker names as out of order.
        if (doc.buildOrder.size() < 2 || doc.buildOrder.front().priority
                                             == doc.buildOrder.back().priority) {
            mutant.applicable = false;
            break;
        }
        const Item moved = doc.buildOrder.back();
        doc.buildOrder.pop_back();
        doc.buildOrder.insert(doc.buildOrder.begin(), moved);
        mutant.expected = DefectKind::OutOfOrder;
        mutant.entry = doc.buildOrder[1].name;
        break;
    }
    case Mutation::BuildOrderListsAPresentEntry: {
        const std::size_t i =
            target(rowsWhere(doc, [](const Row& row) { return !row.requiresPriority(); }));
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = doc.rows[i].name;
        // At the FRONT with `must`, so the list's ordering rule stays satisfied and
        // the only fault is the membership one.
        doc.buildOrder.insert(doc.buildOrder.begin(),
                              Item{doc.rows[i].name, doc.rows[i].table, "must", false});
        break;
    }
    }

    mutant.markdown = render(doc);
    return mutant;
}

// ===========================================================================
// The checked-in document, asserted before anything generated
// ===========================================================================

/// Shared by both properties: the checked-in revision is a case of every run, and
/// what was PARSED out of it is asserted alongside "it has no defects".
void assertCheckedInReportIsWellFormed() {
    const Checked& real = checkedInReport();
    RC_ASSERT(real.markdownBytes > 0);  // PALMIER_DOCS_DIR/UPSTREAM_PARITY.md was found
    RC_ASSERT(real.report.categoryTableFound);
    RC_ASSERT(real.report.areaTableFound);
    RC_ASSERT(real.report.buildOrderSectionFound);
    RC_ASSERT(real.report.entries.size() == requiredEntryCount());
    RC_ASSERT(real.report.entriesIn(ParityTable::ToolCategory).size()
              == testsupport::requiredToolCategories().size());
    RC_ASSERT(real.report.entriesIn(ParityTable::CapabilityArea).size()
              == testsupport::requiredCapabilityAreas().size());
    const std::size_t projected = priorityBearingEntries(real.report);
    RC_ASSERT(projected > 0);
    RC_ASSERT(real.report.buildOrder.size() == projected);
    RC_ASSERT(!real.report.provenance.upstreamRepository.empty());
    RC_ASSERT(!real.report.provenance.upstreamRef.empty());
    RC_ASSERT(!real.report.provenance.linuxRef.empty());
    RC_ASSERT(real.report.provenance.comparisonDate.size() == 10);
    RC_ASSERT(real.defects.empty());
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 69: Parity_Report
// well-formedness — for all revisions of the Parity_Report, each of the 34
// required entries (the 22 upstream tool categories and the 12 capability areas)
// is present exactly once, carries exactly one status from
// `present`/`partial`/`absent`, carries exactly one priority from
// `must`/`should`/`later` if and only if its status is `absent` or `partial`,
// carries a rationale of 1-200 characters wherever a priority is required, names
// both a macOS framework and either a Linux replacement or `out-of-scope` with a
// 1-200 character reason wherever a macOS dependency is declared, and appears in
// the build-order list exactly when its status is `absent` or `partial`, with that
// list ordered `must` before `should` before `later`.
//
// The revision set quantified over is {the checked-in `docs/UPSTREAM_PARITY.md`}
// union {generated revisions}: the checked-in document is design.md's primary case
// and is asserted on every generated case, so this property is a statement about
// the real document as well as about the checker. The generated half is what
// stops a pass being an accident of one document — statuses, components,
// priorities, rationale lengths (including both boundaries of the 1-200 bound),
// macOS-framework shapes and row order are all drawn.
//
// **Validates: Requirements 13.1, 13.2, 13.3, 13.5, 13.6, 13.9**
// ===========================================================================
RC_GTEST_PROP(ParityReportProperties, EveryWellFormedRevisionPassesTheParityCheck, ()) {
    assertCheckedInReportIsWellFormed();

    const Doc     doc = drawDoc();
    const Checked generated = checkMarkdown(render(doc));

    // --- non-vacuity: the generated revision was read back field for field ----
    RC_ASSERT(generated.report.categoryTableFound);
    RC_ASSERT(generated.report.areaTableFound);
    RC_ASSERT(generated.report.buildOrderSectionFound);
    RC_ASSERT(generated.report.entries.size() == doc.rows.size());
    RC_ASSERT(generated.report.entries.size() == requiredEntryCount());
    RC_ASSERT(generated.report.provenance.upstreamRepository == doc.repository);
    RC_ASSERT(generated.report.provenance.upstreamRef == doc.upstreamRef);
    RC_ASSERT(generated.report.provenance.linuxRef == doc.linuxRef);
    RC_ASSERT(generated.report.provenance.comparisonDate == doc.comparisonDate);

    std::size_t present = 0;
    std::size_t withFramework = 0;
    std::size_t outOfScope = 0;
    for (const Row& row : doc.rows) {
        const ParityEntry* parsed = findEntry(generated.report, row.table, row.name);
        RC_ASSERT(parsed != nullptr);
        RC_ASSERT(parsed->status == row.status);
        RC_ASSERT(parsed->linuxComponents == row.components);
        RC_ASSERT(parsed->priority == row.priority);
        RC_ASSERT(parsed->rationale == row.rationale);
        RC_ASSERT(parsed->macosFramework == row.framework);
        RC_ASSERT(parsed->linuxReplacement == row.replacement);
        RC_ASSERT(parsed->requiresPriority() == row.requiresPriority());
        // The clauses of Property 69, restated over the model the checker accepted.
        if (row.requiresPriority()) {
            RC_ASSERT(!row.priority.empty());
            RC_ASSERT(!row.rationale.empty());
            RC_ASSERT(row.rationale.size() <= 200);
        } else {
            ++present;
            RC_ASSERT(row.priority.empty());
            RC_ASSERT(row.rationale.empty());
        }
        if (!row.framework.empty()) {
            ++withFramework;
            RC_ASSERT(!row.replacement.empty());
            if (row.replacement.rfind("out-of-scope", 0) == 0) {
                ++outOfScope;
            }
        }
    }
    // The generator's guarantees, asserted rather than assumed: a document with no
    // `present` row or no macOS framework would exercise fewer clauses than the
    // property claims.
    RC_ASSERT(present > 0);
    RC_ASSERT(withFramework > 0);
    RC_ASSERT(outOfScope > 0);

    // --- the build-order projection, item for item ----------------------------
    RC_ASSERT(generated.report.buildOrder.size() == doc.buildOrder.size());
    RC_ASSERT(generated.report.buildOrder.size() == priorityBearingEntries(generated.report));
    RC_ASSERT(generated.report.buildOrder.size() >= 3);  // one item per priority, guaranteed
    std::set<std::string> priorities;
    for (std::size_t i = 0; i < doc.buildOrder.size(); ++i) {
        const testsupport::BuildOrderItem& parsed = generated.report.buildOrder[i];
        RC_ASSERT(parsed.tableRecognised);
        RC_ASSERT(parsed.name == doc.buildOrder[i].name);
        RC_ASSERT(parsed.table == doc.buildOrder[i].table);
        RC_ASSERT(parsed.priority == doc.buildOrder[i].priority);
        priorities.insert(parsed.priority);
    }
    RC_ASSERT(priorities.size() == 3);

    // --- and only then: the check accepts it ----------------------------------
    RC_ASSERT(generated.defects.empty());
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 70: The parity check detects
// every malformation — for any mutation of a well-formed Parity_Report that omits
// a required entry, duplicates an entry, uses a status or priority outside the
// defined sets, or blanks a required rationale, the parity check fails and its
// output names each offending entry by category or capability-area name together
// with the specific defect.
//
// Quantified over (generated well-formed revision) x (20 mutation kinds) x (target
// entry or item, and the variant within the kind). The mutation is applied to the
// MODEL and the document re-rendered, so a mutation is a value rather than a text
// edit and the same 20 kinds apply to every generated revision. The nine defect
// kinds this document's grammar can express are all reached; `MissingCheck` and
// `DuplicateIdentifier` belong to the Port_Backlog and are Property 72's, as the
// coverage test below states.
//
// Non-vacuity: the unmutated rendering of the same case is asserted clean, the
// mutant is asserted to have been parsed in full (row count, item count, every
// name read back, both tables and the build-order heading recognised, and no
// "the report holds no entries at all" defect), and only then is the expected
// defect kind asserted AGAINST THE OFFENDING ENTRY'S NAME.
//
// **Validates: Requirements 13.8**
// ===========================================================================
RC_GTEST_PROP(ParityReportProperties, TheParityCheckDetectsEveryMalformation, ()) {
    assertCheckedInReportIsWellFormed();

    const Doc base = drawDoc();
    RC_ASSERT(checkMarkdown(render(base)).defects.empty());  // the baseline passes

    const Mutation kind = kMutations[drawIndex(kMutations.size())];
    const std::size_t choice = drawIndex(64);
    RC_TAG(std::string(describe(kind)));

    const Mutant mutant = mutate(base, kind, choice);
    RC_ASSERT(mutant.applicable);  // every kind has a target in a generated revision
    RC_ASSERT(!mutant.entry.empty());

    const Checked mutated = checkMarkdown(mutant.markdown);

    // --- non-vacuity: the mutant is still the document it was ------------------
    RC_ASSERT(mutated.report.categoryTableFound);
    RC_ASSERT(mutated.report.areaTableFound);
    RC_ASSERT(mutated.report.buildOrderSectionFound);
    RC_ASSERT(mutated.report.entries.size() == mutant.doc.rows.size());
    RC_ASSERT(mutated.report.buildOrder.size() == mutant.doc.buildOrder.size());
    for (const Row& row : mutant.doc.rows) {
        RC_ASSERT(findEntry(mutated.report, row.table, row.name) != nullptr);
    }
    for (std::size_t i = 0; i < mutant.doc.buildOrder.size(); ++i) {
        RC_ASSERT(mutated.report.buildOrder[i].name == mutant.doc.buildOrder[i].name);
    }
    RC_ASSERT(!testsupport::hasDefect(mutated.defects, DefectKind::MissingEntry, "parity report"));

    // --- the defect, named against the offending entry ------------------------
    RC_ASSERT(!mutated.defects.empty());
    RC_ASSERT(testsupport::hasDefect(mutated.defects, mutant.expected, mutant.entry));
}

// ===========================================================================
// Full mutation-kind coverage on every run, independent of sampling
// ===========================================================================

TEST(ParityCheckMutations, EveryMutationKindIsDetectedAndNamesItsEntry) {
    const Doc base = fixedDoc();
    const Checked baseline = checkMarkdown(render(base));
    ASSERT_TRUE(baseline.defects.empty())
        << "the fixed well-formed document is not clean:\n"
        << testsupport::toString(baseline.defects);
    ASSERT_EQ(baseline.report.entries.size(), requiredEntryCount());
    ASSERT_EQ(baseline.report.buildOrder.size(), priorityBearingEntries(baseline.report));

    std::set<DefectKind> produced;
    for (const Mutation kind : kMutations) {
        for (const std::size_t choice : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                                         std::size_t{7}, std::size_t{13}}) {
            const Mutant mutant = mutate(base, kind, choice);
            ASSERT_TRUE(mutant.applicable) << describe(kind) << " found no target";
            const Checked mutated = checkMarkdown(mutant.markdown);
            EXPECT_TRUE(testsupport::hasDefect(mutated.defects, mutant.expected, mutant.entry))
                << describe(kind) << " (choice " << choice << ", entry `" << mutant.entry
                << "`) was not reported:\n"
                << testsupport::toString(mutated.defects);
            // Still a readable document, so the defect is the injected one.
            EXPECT_EQ(mutated.report.entries.size(), mutant.doc.rows.size()) << describe(kind);
            produced.insert(mutant.expected);
        }
    }

    const std::set<DefectKind> expected{DefectKind::MissingEntry,     DefectKind::DuplicateEntry,
                                        DefectKind::InvalidStatus,    DefectKind::InvalidPriority,
                                        DefectKind::MissingRationale, DefectKind::MissingField,
                                        DefectKind::OutOfOrder};
    EXPECT_EQ(produced, expected) << "the mutation set no longer covers this grammar's defect kinds";

    // The two kinds a Parity_Report cannot express are the Port_Backlog's, and
    // Property 72 covers them in tests/docs/port_backlog_property_test.cpp. Their
    // union is the whole closed vocabulary of ReportParser.hpp.
    std::set<DefectKind> whole = produced;
    whole.insert(DefectKind::MissingCheck);
    whole.insert(DefectKind::DuplicateIdentifier);
    EXPECT_EQ(whole.size(), 9U);
}

}  // namespace
}  // namespace palmier
