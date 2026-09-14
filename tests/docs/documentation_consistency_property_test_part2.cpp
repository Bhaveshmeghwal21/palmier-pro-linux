// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the documentation_consistency_property_test cases.
//
// Suites here: DocumentationCheckMutations.
// Same CMake target as its siblings, so every case keeps the name CTest already knew.

#include "documentation_consistency_property_test_support3.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_documentationconsistencypropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Full mutation-kind coverage on every run, independent of sampling
// ===========================================================================

TEST(DocumentationCheckMutations, EveryMutationKindIsDetectedAndNamesItsSection) {
    const DocSet base = fixedDocSet();
    const Checked baseline = check(base);
    ASSERT_TRUE(baseline.defects.empty())
        << "the fixed well-formed documentation set is not clean:\n"
        << testsupport::toString(baseline.defects);
    ASSERT_EQ(baseline.options.size(), live().options.size());
    ASSERT_EQ(baseline.tools.size(), live().tools.size());
    ASSERT_EQ(baseline.settings.size(), live().settings.size());

    std::set<DocDefectKind> produced;
    for (const Mutation kind : kMutations) {
        for (const std::size_t choice :
             {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{13}}) {
            const Mutant mutant = mutate(base, kind, choice);
            ASSERT_TRUE(mutant.applicable) << describe(kind) << " found no target";
            const Checked mutated = checkDocuments(mutant.text.build, mutant.text.tools,
                                                   mutant.text.remote, observationsOf(mutant.doc));
            EXPECT_TRUE(hasDefect(mutated.defects, mutant.expected, mutant.name, mutant.document,
                                  mutant.section))
                << describe(kind) << " (choice " << choice << ", name `" << mutant.name
                << "`, section `" << mutant.section << "`) was not reported:\n"
                << testsupport::toString(mutated.defects);
            if (!mutant.structural) {
                // Still a readable document set, so the defect is the injected one.
                EXPECT_EQ(mutated.options.size(), modelOptionCount(mutant.doc)) << describe(kind);
                EXPECT_EQ(mutated.tools.size(), mutant.doc.tools.size()) << describe(kind);
                EXPECT_EQ(mutated.settings.size(), modelSettingCount(mutant.doc)) << describe(kind);
            }
            produced.insert(mutant.expected);
        }
    }

    // The whole defect vocabulary this checker can report, minus `InputUnreadable`,
    // which is not a document fault but a missing input — task 12.7's
    // `ReportsAnEmptyOptionsManifestRatherThanAgreeingWithEverything` and
    // `ReportsAMissingResultRenderingFunctionInsteadOfAnEmptySet` own that kind.
    const std::set<DocDefectKind> expected{
        DocDefectKind::UndocumentedName, DocDefectKind::UnknownDocumentedName,
        DocDefectKind::TypeMismatch,     DocDefectKind::RequiredMismatch,
        DocDefectKind::OrderMismatch,    DocDefectKind::SectionMissing};
    EXPECT_EQ(produced, expected)
        << "the mutation set no longer covers this checker's document-fault kinds";

    std::set<DocDefectKind> whole = produced;
    whole.insert(DocDefectKind::InputUnreadable);
    EXPECT_EQ(whole.size(), 7U);
}

}  // namespace
}  // namespace palmier
