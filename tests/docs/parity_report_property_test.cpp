// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parity_report_property_test cases that stayed in this file after it was split.
//
// Suites here: ParityReportProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "parity_report_property_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_parityreportpropertytest;  // NOLINT(google-build-using-namespace)

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

}  // namespace
}  // namespace palmier
