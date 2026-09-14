// SPDX-License-Identifier: GPL-3.0-or-later
//
// The documentation_consistency_property_test cases that stayed here after the split.
//
// Suites here: DocumentationConsistencyProperties.
// Same CMake target as its siblings, so every case keeps the name CTest already knew.

#include "documentation_consistency_property_test_support3.hpp"

namespace palmier {
namespace {

using namespace palmier::
    testsupport_documentationconsistencypropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 76: Documentation and the
// running system agree on every name — for all CMake options defined by the build
// system, all tools in the Tool_Surface and all declared tool arguments, the
// documented name set and the actual name set are equal in both directions, and
// each documented argument records the JSON type and the required/optional marking
// the `ToolSchema` declares, and each documented tool records every field its
// success result returns.
//
// The document set quantified over is {the checked-in `docs/BUILD.md`,
// `docs/TOOLS.md`, `docs/REMOTE_ACCESS.md`} union {generated documentation sets
// whose names are the running system's and whose presentation is drawn}. The
// checked-in set is asserted on every case, so this property is a statement about
// the real documentation as well as about the checker; the generated half is what
// stops a pass being an accident of one layout.
//
// **Validates: Requirements 16.4, 16.7**
// ===========================================================================
RC_GTEST_PROP(DocumentationConsistencyProperties,
              EveryWellFormedDocumentationSetAgreesWithTheRunningSystem,
              ()) {
    assertCheckedInDocumentationAgreesWithTheRunningSystem();

    const DocSet  doc = drawDocSet();
    const Checked generated = check(doc);

    // --- non-vacuity: every generated document was read back -------------------
    RC_ASSERT(generated.buildBytes > 0);
    RC_ASSERT(generated.toolsBytes > 0);
    RC_ASSERT(generated.remoteBytes > 0);
    RC_ASSERT(generated.extraction.empty());

    // Options: count, and each row's name, type and `###` section.
    RC_ASSERT(generated.options.size() == modelOptionCount(doc));
    RC_ASSERT(generated.options.size() == live().options.size());
    for (std::size_t g = 0; g < doc.optionGroups.size(); ++g) {
        for (const OptionRow& row : doc.optionGroups[g].rows) {
            const DocumentedOption* parsed = findOption(generated, row.name);
            RC_ASSERT(parsed != nullptr);
            RC_ASSERT(parsed->type == row.type);
            RC_ASSERT(parsed->section == sectionOf(doc, g));
        }
    }
    // The decoy row inside the region and the whole table outside it were ignored.
    RC_ASSERT(findOption(generated, "CMAKE_BUILD_TYPE") == nullptr);
    RC_ASSERT(findOption(generated, "PALMIER_OUTSIDE_THE_MARKED_REGION") == nullptr);

    // Tools: count, ORDER, the no-arguments claim, and every argument's name, type,
    // uuid marker and required marking. The prose `##` sections must not have been
    // read as tools, which the count assertion is what catches.
    RC_ASSERT(generated.tools.size() == doc.tools.size());
    RC_ASSERT(generated.tools.size() == live().tools.size());
    std::size_t withArguments = 0;
    std::size_t withoutArguments = 0;
    std::size_t uuidArguments = 0;
    std::size_t plainArguments = 0;
    std::size_t requiredArguments = 0;
    std::size_t optionalArguments = 0;
    std::size_t conditionalFields = 0;
    std::size_t unobservedConditionalFields = 0;
    for (std::size_t i = 0; i < doc.tools.size(); ++i) {
        const ToolModel& model = doc.tools[i];
        RC_ASSERT(generated.tools[i].name == model.name);  // documented order
        const DocumentedTool* parsed = findTool(generated, model.name);
        RC_ASSERT(parsed != nullptr);
        RC_ASSERT(parsed->declaresNoArguments == model.declaresNoArguments);
        RC_ASSERT(parsed->commandResult == model.commandResult);
        RC_ASSERT(parsed->arguments.size() == model.arguments.size());
        for (const ArgumentModel& argument : model.arguments) {
            const DocumentedArgument* read = findArgument(*parsed, argument.name);
            RC_ASSERT(read != nullptr);
            RC_ASSERT(read->type == argument.type);
            RC_ASSERT(read->required == argument.required);
            RC_ASSERT(read->uuid == argument.uuid);
            argument.uuid ? ++uuidArguments : ++plainArguments;
            argument.required ? ++requiredArguments : ++optionalArguments;
        }
        model.arguments.empty() ? ++withoutArguments : ++withArguments;

        // Result fields, both conditionalities. A *(command result)* tool also
        // carries the status/noOp/indication trio, so compare by containment of the
        // model's claims rather than by equality of the whole conditional list.
        const std::vector<std::string> all = parsed->allResultFields();
        for (const std::string& field : model.resultFields) {
            RC_ASSERT(std::find(parsed->resultFields.begin(), parsed->resultFields.end(), field)
                      != parsed->resultFields.end());
        }
        for (const std::string& field : model.conditionalResultFields) {
            RC_ASSERT(std::find(parsed->conditionalResultFields.begin(),
                                parsed->conditionalResultFields.end(), field)
                      != parsed->conditionalResultFields.end());
            ++conditionalFields;
            if (std::find(model.observedFields.begin(), model.observedFields.end(), field)
                == model.observedFields.end()) {
                ++unobservedConditionalFields;
            }
        }
        for (const std::string& field : model.observedFields) {
            RC_ASSERT(std::find(all.begin(), all.end(), field) != all.end());
        }
        if (model.commandResult) {
            for (const char* field : {"status", "noOp", "indication"}) {
                RC_ASSERT(std::find(all.begin(), all.end(), field) != all.end());
            }
        }
    }

    // Settings: count, and each row's key, environment variable, flag and section.
    RC_ASSERT(generated.settings.size() == modelSettingCount(doc));
    RC_ASSERT(generated.settings.size() == live().settings.size());
    for (const SettingGroup& group : doc.settingGroups) {
        for (const SettingRow& row : group.rows) {
            const DocumentedSetting* parsed = findSetting(generated, row.key);
            RC_ASSERT(parsed != nullptr);
            RC_ASSERT(parsed->environmentVariable == row.environmentVariable);
            RC_ASSERT(parsed->flag == row.flag);
            RC_ASSERT(parsed->section == group.heading);
        }
    }

    // --- the generator's guarantees, asserted rather than assumed --------------
    RC_ASSERT(withArguments > 0);
    RC_ASSERT(withoutArguments > 0);
    RC_ASSERT(uuidArguments > 0);
    RC_ASSERT(plainArguments > 0);
    RC_ASSERT(requiredArguments > 0);
    RC_ASSERT(optionalArguments > 0);
    RC_ASSERT(conditionalFields > 0);
    RC_ASSERT(unobservedConditionalFields > 0);
    RC_ASSERT(std::any_of(doc.tools.begin(), doc.tools.end(),
                          [](const ToolModel& tool) { return tool.resultTable; }));
    RC_ASSERT(std::any_of(doc.tools.begin(), doc.tools.end(),
                          [](const ToolModel& tool) { return !tool.resultTable; }));
    RC_ASSERT(std::any_of(doc.tools.begin(), doc.tools.end(),
                          [](const ToolModel& tool) { return tool.commandResult; }));

    // --- and only then: every check accepts it ---------------------------------
    RC_ASSERT(generated.defects.empty());
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 77: The documentation check
// reports every mismatch and modifies nothing — for any documentation text with an
// injected renamed, absent or extra option, tool or argument name, the check fails,
// names each mismatched name together with the documentation section in which it
// appears, and leaves the documentation file's bytes unchanged.
//
// Quantified over (generated well-formed documentation set) x (29 mutation kinds
// spanning the option, tool, argument, result-field and settings name classes and
// the rename / delete / add / retype / restructure fault classes) x (the target row,
// tool, argument or variant within the kind). The mutation is applied to the MODEL
// and the three documents re-rendered, so a mutation is a value rather than a text
// edit and the same 29 kinds apply to every generated set.
//
// Non-vacuity: the unmutated rendering of the same case is asserted clean; the
// mutant is asserted to have been extracted in full (option, tool, argument and
// settings counts equal the mutated model's, every name read back) except for the
// four structural faults, which declare themselves and are asserted to produce
// exactly the `SectionMissing` the extraction contract promises; and only then is
// the expected defect asserted against the offending NAME, DOCUMENT and SECTION.
//
// The "modifies nothing" clause is asserted directly: every `.md` file in `docs/` is
// fingerprinted (byte count plus an FNV-1a hash) before and after a full check run
// over the checked-in documents.
//
// **Validates: Requirements 16.8**
// ===========================================================================
RC_GTEST_PROP(DocumentationConsistencyProperties,
              TheDocumentationCheckReportsEveryMismatchAndModifiesNothing,
              ()) {
    assertCheckedInDocumentationAgreesWithTheRunningSystem();

    // --- Requirement 16.8: the check leaves the documentation unmodified -------
    const std::vector<DocumentFingerprint> before = fingerprintTheDocumentation();
    RC_ASSERT(before.size() >= 3);  // at least the three documents the checker reads
    for (const DocumentFingerprint& fingerprint : before) {
        RC_ASSERT(fingerprint.bytes > 0);
    }
    const Checked realRun = checkCheckedInDocuments();
    RC_ASSERT(realRun.defects.empty());
    RC_ASSERT(fingerprintTheDocumentation() == before);

    // --- the baseline passes ---------------------------------------------------
    const DocSet base = drawDocSet();
    RC_ASSERT(check(base).defects.empty());

    const Mutation    kind = kMutations[drawIndex(kMutations.size())];
    const std::size_t choice = drawIndex(64);
    RC_TAG(std::string(describe(kind)));

    const Mutant mutant = mutate(base, kind, choice);
    RC_ASSERT(mutant.applicable);  // every kind has a target in a generated set
    RC_ASSERT(!mutant.name.empty());

    const Checked mutated =
        checkDocuments(mutant.text.build, mutant.text.tools, mutant.text.remote,
                       observationsOf(mutant.doc));

    // --- non-vacuity: the mutant is still the document set it was --------------
    if (!mutant.structural) {
        RC_ASSERT(mutated.extraction.empty());
        RC_ASSERT(mutated.options.size() == modelOptionCount(mutant.doc));
        RC_ASSERT(mutated.tools.size() == mutant.doc.tools.size());
        RC_ASSERT(mutated.settings.size() == modelSettingCount(mutant.doc));
        for (const OptionGroup& group : mutant.doc.optionGroups) {
            for (const OptionRow& row : group.rows) {
                RC_ASSERT(findOption(mutated, row.name) != nullptr);
            }
        }
        for (std::size_t i = 0; i < mutant.doc.tools.size(); ++i) {
            RC_ASSERT(mutated.tools[i].name == mutant.doc.tools[i].name);
            for (const ArgumentModel& argument : mutant.doc.tools[i].arguments) {
                RC_ASSERT(findArgument(mutated.tools[i], argument.name) != nullptr);
            }
        }
        for (const SettingGroup& group : mutant.doc.settingGroups) {
            for (const SettingRow& row : group.rows) {
                RC_ASSERT(findSetting(mutated, row.key) != nullptr);
            }
        }
    } else {
        // The structural faults must be REPORTED as missing sections, not silently
        // turned into an empty, vacuously-consistent name set.
        RC_ASSERT(!mutated.extraction.empty());
    }

    // --- the defect, named against the offending name and its section ----------
    RC_ASSERT(!mutated.defects.empty());
    RC_ASSERT(hasDefect(mutated.defects, mutant.expected, mutant.name, mutant.document,
                        mutant.section));
}

}  // namespace
}  // namespace palmier
