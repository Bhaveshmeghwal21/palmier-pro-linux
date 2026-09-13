// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the parity_report_property_test cases, split out of parity_report_property_test.cpp.
//
// Suites here: ParityCheckMutations.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "parity_report_property_test_support.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_parityreportpropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Full mutation-kind coverage on every run, independent of sampling
// ===========================================================================

TEST(ParityCheckMutations, EveryMutationKindIsDetectedAndNamesItsEntry) {
    const Doc base = fixedDoc();
    const Checked baseline = checkMarkdown(render(base));
    ASSERT_TRUE(baseline.defects.empty()) << "the fixed well-formed document is not clean:\n"
                                          << testsupport::toString(baseline.defects);
    ASSERT_EQ(baseline.report.entries.size(), requiredEntryCount());
    ASSERT_EQ(baseline.report.buildOrder.size(), priorityBearingEntries(baseline.report));

    std::set<DefectKind> produced;
    for (const Mutation kind : kMutations) {
        for (const std::size_t choice :
             {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{13}}) {
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
    EXPECT_EQ(produced, expected)
        << "the mutation set no longer covers this grammar's defect kinds";

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
