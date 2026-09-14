// SPDX-License-Identifier: GPL-3.0-or-later
//
// Continuation of the documentation_consistency_property_test fixtures; see documentation_consistency_property_test_support1.hpp.

#include "documentation_consistency_property_test_support1.hpp"

// Part 2 of 3 of the shared fixtures for documentation_consistency_property_test, split because the
// fixtures alone exceed the 1000-line ceiling this repository enforces. Cut only at
// points where the brace depth is zero, so no entity is divided.

#ifndef PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT2_HPP_
#define PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT2_HPP_

namespace palmier {
namespace testsupport_documentationconsistencypropertytest {

struct Rendered {
    std::string build;
    std::string tools;
    std::string remote;
};

[[nodiscard]] inline Rendered render(const DocSet& doc) {
    return Rendered{renderBuildDoc(doc), renderToolsDoc(doc), renderRemoteDoc(doc)};
}

/// The observation set a generated document's result fields are checked against:
/// per tool, whatever that tool's model says a real payload carried.
[[nodiscard]] inline std::vector<ObservedResult> observationsOf(const DocSet& doc) {
    std::vector<ObservedResult> observed;
    for (const ToolModel& tool : doc.tools) {
        observed.push_back(ObservedResult{tool.name, tool.observedFields});
    }
    return observed;
}

[[nodiscard]] inline Checked check(const DocSet& doc) {
    const Rendered text = render(doc);
    return checkDocuments(text.build, text.tools, text.remote, observationsOf(doc));
}

// --- model queries used by the readback and by the mutations -----------------

/// The `###` section the extractor will report a row under: its group's heading, or
/// the region's default when the first group carries none.
[[nodiscard]] inline std::string sectionOf(const DocSet& doc, std::size_t group) {
    return doc.optionGroups[group].heading.empty() ? std::string(kOptionsSection)
                                                   : doc.optionGroups[group].heading;
}

[[nodiscard]] inline const DocumentedOption* findOption(const Checked& checked,
                                                        const std::string& name) {
    for (const DocumentedOption& option : checked.options) {
        if (option.name == name) {
            return &option;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const DocumentedTool* findTool(const Checked& checked,
                                                    const std::string& name) {
    for (const DocumentedTool& tool : checked.tools) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const DocumentedArgument* findArgument(const DocumentedTool& tool,
                                                            const std::string& name) {
    for (const DocumentedArgument& argument : tool.arguments) {
        if (argument.name == name) {
            return &argument;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const DocumentedSetting* findSetting(const Checked& checked,
                                                          const std::string& key) {
    for (const DocumentedSetting& setting : checked.settings) {
        if (setting.key == key) {
            return &setting;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::size_t modelOptionCount(const DocSet& doc) {
    std::size_t count = 0;
    for (const OptionGroup& group : doc.optionGroups) {
        count += group.rows.size();
    }
    return count;
}

[[nodiscard]] inline std::size_t modelSettingCount(const DocSet& doc) {
    std::size_t count = 0;
    for (const SettingGroup& group : doc.settingGroups) {
        count += group.rows.size();
    }
    return count;
}

[[nodiscard]] inline bool hasDefect(const std::vector<DocDefect>& defects, DocDefectKind kind,
                                    std::string_view name, std::string_view document,
                                    std::string_view section) {
    return std::any_of(defects.begin(), defects.end(), [&](const DocDefect& defect) {
        return defect.kind == kind && defect.name == name && defect.document == document &&
               defect.section == section;
    });
}

// ===========================================================================
// Generation
// ===========================================================================

/// A uniform index in [0, bound), shrinking towards 0.
///
/// `rc::gen::inRange` SCALES its range with RapidCheck's `size` parameter, which
/// rises across a run — so a bare `inRange` would draw only low indices in the early
/// cases. For a categorical choice (a layout, a group count, a mutation kind) that
/// means whole categories never being exercised: measured over a 100-case run of
/// task 12.4's property, five of twenty mutation kinds were never drawn. Resizing to
/// the nominal size makes every draw uniform over the whole range while keeping
/// `inRange`'s shrinking, so a reported counterexample is still the small one.
[[nodiscard]] inline std::size_t drawIndex(std::size_t bound) {
    return *rc::gen::resize(rc::kNominalSize, rc::gen::inRange<std::size_t>(0, bound));
}

[[nodiscard]] inline bool drawFlag() { return drawIndex(2) == 1; }

inline const std::vector<std::string>& optionSectionPool() {
    static const std::vector<std::string> pool{
        "User-settable options", "Derived, read-only entries", "Hardware codec options",
        "Test and warning options"};
    return pool;
}

inline const std::vector<std::string>& settingSectionPool() {
    static const std::vector<std::string> pool{"Every configuration input", "Remote-access inputs",
                                               "Endpoint and agent inputs"};
    return pool;
}

/// Synthetic result-field names. Deliberately none of `status`, `noOp` or
/// `indication`: those three are what the *(command result)* marker stands for, and
/// a collision would make a documented field's conditionality ambiguous.
inline const std::vector<std::string>& resultFieldPool() {
    static const std::vector<std::string> pool{"alphaField", "betaField",    "gammaField",
                                               "deltaField", "epsilonField", "zetaField"};
    return pool;
}

/// Split `count` items into `groups` contiguous, non-empty runs. Returns the size of
/// each run.
[[nodiscard]] inline std::vector<std::size_t> splitSizes(std::size_t count, std::size_t groups) {
    std::vector<std::size_t> sizes(groups, count / groups);
    for (std::size_t i = 0; i < count % groups; ++i) {
        sizes[i] += 1;
    }
    return sizes;
}

/// The option groups of a document: the live options, in manifest order, split into
/// `groups` `###` sections. The first section's heading may be absent, which is the
/// one case the extractor reports under the region's default section name.
[[nodiscard]] inline std::vector<OptionGroup> buildOptionGroups(std::size_t groups,
                                                                bool firstHeadingAbsent,
                                                                std::size_t headingOffset) {
    const std::vector<LiveOption>& options = live().options;
    const std::vector<std::size_t> sizes = splitSizes(options.size(), groups);

    std::vector<OptionGroup> built;
    std::size_t cursor = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        OptionGroup group;
        if (g != 0 || !firstHeadingAbsent) {
            group.heading = optionSectionPool()[(headingOffset + g) % optionSectionPool().size()];
        }
        for (std::size_t i = 0; i < sizes[g]; ++i, ++cursor) {
            group.rows.push_back(OptionRow{options[cursor].name, options[cursor].type});
        }
        built.push_back(std::move(group));
    }
    return built;
}

[[nodiscard]] inline std::vector<SettingGroup> buildSettingGroups(std::size_t groups,
                                                                  std::size_t headingOffset) {
    const std::vector<LiveSetting>& settings = live().settings;
    const std::vector<std::size_t> sizes = splitSizes(settings.size(), groups);

    std::vector<SettingGroup> built;
    std::size_t cursor = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        SettingGroup group;
        group.heading = settingSectionPool()[(headingOffset + g) % settingSectionPool().size()];
        for (std::size_t i = 0; i < sizes[g]; ++i, ++cursor) {
            group.rows.push_back(SettingRow{
                settings[cursor].key, settings[cursor].environmentVariable, settings[cursor].flag});
        }
        built.push_back(std::move(group));
    }
    return built;
}

/// One tool section's model. `unconditional` and `conditional` are counts, and
/// `observeConditional` decides how many of the conditional fields a payload
/// happened to carry — the rest are documented-but-unobserved, which is legal
/// precisely because they are marked conditional.
[[nodiscard]] inline ToolModel buildTool(const LiveTool& liveTool, ArgumentLayout layout,
                                         bool resultTable, bool commandResult,
                                         std::size_t unconditional, std::size_t conditional,
                                         std::size_t observeConditional, bool boldRequired,
                                         bool uuidFirst) {
    ToolModel tool;
    tool.name = liveTool.name;
    tool.layout = layout;
    tool.resultTable = resultTable;
    tool.commandResult = commandResult;
    tool.declaresNoArguments = liveTool.arguments.empty();
    for (const LiveArgument& argument : liveTool.arguments) {
        tool.arguments.push_back(ArgumentModel{argument.name, argument.type, argument.required,
                                               argument.uuid, boldRequired, uuidFirst});
    }

    const std::vector<std::string>& pool = resultFieldPool();
    const std::size_t total = std::min(pool.size(), unconditional + conditional);
    for (std::size_t i = 0; i < total; ++i) {
        if (i < unconditional) {
            tool.resultFields.push_back(pool[i]);
        } else {
            tool.conditionalResultFields.push_back(pool[i]);
        }
    }
    // Every unconditional field must be observed (that is what unconditional means);
    // a prefix of the conditional ones is.
    tool.observedFields = tool.resultFields;
    for (std::size_t i = 0; i < std::min(observeConditional, tool.conditionalResultFields.size());
         ++i) {
        tool.observedFields.push_back(tool.conditionalResultFields[i]);
    }
    if (commandResult) {
        // The *(command result)* trio is documented and conditional, so a payload
        // may carry `status` without the document naming it explicitly.
        tool.observedFields.push_back("status");
    }
    return tool;
}

/// A generated well-formed documentation set.
///
/// The NAMES are the running system's; the PRESENTATION is drawn: the number of
/// `###` option groups and their titles, whether the first one is titled at all,
/// which column order each table uses, whether option types are backticked, and per
/// tool the argument-table layout, the bold `Required` marker, the side the `*uuid*`
/// marker sits on, the argument order inside the table, the result shape
/// (`| Field |` table or `Result:` paragraph), the *(command result)* marker and how
/// many result fields are unconditional, conditional and observed.
///
/// Four positions are then overwritten to GUARANTEE that every mutation of
/// Property 77 has a target and that every clause of Property 76 is exercised: a
/// tool rendered with the result table, one rendered with the result paragraph, one
/// carrying the *(command result)* marker, and one carrying a conditional field that
/// NO payload observed. Which tools those are depends on the live registry, so the
/// guarantee fixes shapes, not names.
[[nodiscard]] inline DocSet drawDocSet() {
    DocSet doc;
    doc.optionLayout = static_cast<OptionLayout>(drawIndex(3));
    doc.settingLayout = static_cast<SettingLayout>(drawIndex(2));
    doc.optionTypesBackticked = drawFlag();
    doc.optionGroups = buildOptionGroups(1 + drawIndex(3), drawFlag(), drawIndex(4));
    doc.settingGroups = buildSettingGroups(1 + drawIndex(3), drawIndex(3));

    for (const LiveTool& liveTool : live().tools) {
        ToolModel tool = buildTool(liveTool, static_cast<ArgumentLayout>(drawIndex(3)),
                                   /*resultTable=*/drawFlag(), /*commandResult=*/drawFlag(),
                                   /*unconditional=*/1 + drawIndex(3),
                                   /*conditional=*/drawIndex(3),
                                   /*observeConditional=*/drawIndex(3),
                                   /*boldRequired=*/drawFlag(), /*uuidFirst=*/drawFlag());
        // Argument order inside a table is not part of any requirement, so it is
        // generated: a Fisher-Yates shuffle over the drawn tool's arguments.
        for (std::size_t i = tool.arguments.size(); i > 1; --i) {
            std::swap(tool.arguments[i - 1], tool.arguments[drawIndex(i)]);
        }
        doc.tools.push_back(std::move(tool));
    }

    // The shape guarantees, imposed on the first four tools.
    doc.tools[0].resultTable = true;
    doc.tools[1].resultTable = false;
    doc.tools[2].commandResult = true;
    if (std::find(doc.tools[2].observedFields.begin(), doc.tools[2].observedFields.end(),
                  "status") == doc.tools[2].observedFields.end()) {
        doc.tools[2].observedFields.push_back("status");
    }
    // A conditional field no payload carried: legal, and Property 77's target for
    // the "promote it to unconditional" mutation.
    ToolModel& withUnobserved = doc.tools[3];
    if (withUnobserved.conditionalResultFields.empty()) {
        withUnobserved.conditionalResultFields.push_back(resultFieldPool().back());
    }
    const std::string& unobserved = withUnobserved.conditionalResultFields.back();
    withUnobserved.observedFields.erase(
        std::remove(withUnobserved.observedFields.begin(), withUnobserved.observedFields.end(),
                    unobserved),
        withUnobserved.observedFields.end());
    return doc;
}

/// The same shape with nothing drawn, for the exhaustive mutation-coverage test:
/// the layouts and result shapes cycle so all of them appear, and the same four
/// guarantees hold.
[[nodiscard]] inline DocSet fixedDocSet() {
    DocSet doc;
    doc.optionLayout = OptionLayout::OptionTypeDefaultEffect;
    doc.settingLayout = SettingLayout::KeyEnvironmentFlagDefault;
    doc.optionTypesBackticked = true;
    doc.optionGroups = buildOptionGroups(2, /*firstHeadingAbsent=*/false, 0);
    doc.settingGroups = buildSettingGroups(2, 0);

    std::size_t i = 0;
    for (const LiveTool& liveTool : live().tools) {
        doc.tools.push_back(buildTool(liveTool, static_cast<ArgumentLayout>(i % 3),
                                      /*resultTable=*/i % 2 == 0,
                                      /*commandResult=*/i % 3 == 2,
                                      /*unconditional=*/1 + (i % 3),
                                      /*conditional=*/1 + (i % 2),
                                      /*observeConditional=*/i % 2,
                                      /*boldRequired=*/i % 2 == 0,
                                      /*uuidFirst=*/i % 3 == 1));
        ++i;
    }
    doc.tools[0].resultTable = true;
    doc.tools[1].resultTable = false;
    doc.tools[2].commandResult = true;
    ToolModel& withUnobserved = doc.tools[3];
    if (withUnobserved.conditionalResultFields.empty()) {
        withUnobserved.conditionalResultFields.push_back(resultFieldPool().back());
    }
    const std::string& unobserved = withUnobserved.conditionalResultFields.back();
    withUnobserved.observedFields.erase(
        std::remove(withUnobserved.observedFields.begin(), withUnobserved.observedFields.end(),
                    unobserved),
        withUnobserved.observedFields.end());
    return doc;
}

// ===========================================================================
// Mutation — every mismatch class Requirement 16.8 names, over every name class
// ===========================================================================

enum class Mutation {
    // Options (docs/BUILD.md)
    RenameAnOption,
    DeleteAnOption,
    AddAnUnknownOption,
    ChangeAnOptionType,
    RemoveTheOptionMarkers,
    EmptyTheMarkedOptionRegion,
    // Tools and arguments (docs/TOOLS.md)
    RenameATool,
    DeleteATool,
    AddAnUnknownTool,
    SwapTwoToolSections,
    RenameAnArgument,
    DeleteAnArgument,
    AddAnUnknownArgument,
    ChangeAnArgumentType,
    DropAUuidMarker,
    AddASpuriousUuidMarker,
    FlipARequiredMarking,
    ClaimNoArgumentsForAToolThatHasThem,
    OmitTheNoArgumentsClaim,
    DemoteEveryToolHeading,
    // Result fields (docs/TOOLS.md)
    DeleteADocumentedResultField,
    DocumentAResultFieldNoPayloadCarries,
    PromoteAnUnobservedConditionalField,
    // Settings (docs/REMOTE_ACCESS.md)
    RenameASettingsKey,
    DeleteASettingsRow,
    AddAnUnknownSettingsKey,
    WrongEnvironmentVariable,
    WrongCommandLineFlag,
    MakeTheSettingsTableUnrecognisable
};

constexpr std::array<Mutation, 29> kMutations{Mutation::RenameAnOption,
                                              Mutation::DeleteAnOption,
                                              Mutation::AddAnUnknownOption,
                                              Mutation::ChangeAnOptionType,
                                              Mutation::RemoveTheOptionMarkers,
                                              Mutation::EmptyTheMarkedOptionRegion,
                                              Mutation::RenameATool,
                                              Mutation::DeleteATool,
                                              Mutation::AddAnUnknownTool,
                                              Mutation::SwapTwoToolSections,
                                              Mutation::RenameAnArgument,
                                              Mutation::DeleteAnArgument,
                                              Mutation::AddAnUnknownArgument,
                                              Mutation::ChangeAnArgumentType,
                                              Mutation::DropAUuidMarker,
                                              Mutation::AddASpuriousUuidMarker,
                                              Mutation::FlipARequiredMarking,
                                              Mutation::ClaimNoArgumentsForAToolThatHasThem,
                                              Mutation::OmitTheNoArgumentsClaim,
                                              Mutation::DemoteEveryToolHeading,
                                              Mutation::DeleteADocumentedResultField,
                                              Mutation::DocumentAResultFieldNoPayloadCarries,
                                              Mutation::PromoteAnUnobservedConditionalField,
                                              Mutation::RenameASettingsKey,
                                              Mutation::DeleteASettingsRow,
                                              Mutation::AddAnUnknownSettingsKey,
                                              Mutation::WrongEnvironmentVariable,
                                              Mutation::WrongCommandLineFlag,
                                              Mutation::MakeTheSettingsTableUnrecognisable};

[[nodiscard]] inline std::string_view describe(Mutation kind) {
    switch (kind) {
        case Mutation::RenameAnOption:
            return "rename a documented option";
        case Mutation::DeleteAnOption:
            return "delete a documented option row";
        case Mutation::AddAnUnknownOption:
            return "document an option the tree does not define";
        case Mutation::ChangeAnOptionType:
            return "document an option with the wrong type";
        case Mutation::RemoveTheOptionMarkers:
            return "remove the palmier-options extraction markers";
        case Mutation::EmptyTheMarkedOptionRegion:
            return "empty the marked option region";
        case Mutation::RenameATool:
            return "rename a documented tool";
        case Mutation::DeleteATool:
            return "delete a tool section";
        case Mutation::AddAnUnknownTool:
            return "document a tool the registry does not publish";
        case Mutation::SwapTwoToolSections:
            return "swap two adjacent tool sections";
        case Mutation::RenameAnArgument:
            return "rename a documented argument";
        case Mutation::DeleteAnArgument:
            return "delete a documented argument row";
        case Mutation::AddAnUnknownArgument:
            return "document an argument the schema does not declare";
        case Mutation::ChangeAnArgumentType:
            return "document an argument with the wrong JSON type";
        case Mutation::DropAUuidMarker:
            return "drop the *uuid* marker from a uuid argument";
        case Mutation::AddASpuriousUuidMarker:
            return "add a *uuid* marker to a plain argument";
        case Mutation::FlipARequiredMarking:
            return "flip an argument's required/optional marking";
        case Mutation::ClaimNoArgumentsForAToolThatHasThem:
            return "claim \"No arguments.\" for a tool that declares some";
        case Mutation::OmitTheNoArgumentsClaim:
            return "omit \"No arguments.\" from an argument-free tool";
        case Mutation::DemoteEveryToolHeading:
            return "demote every tool heading below `## `";
        case Mutation::DeleteADocumentedResultField:
            return "delete a result field the payload carries";
        case Mutation::DocumentAResultFieldNoPayloadCarries:
            return "document an unconditional result field no payload carries";
        case Mutation::PromoteAnUnobservedConditionalField:
            return "promote an unobserved conditional field to unconditional";
        case Mutation::RenameASettingsKey:
            return "rename a documented configuration key";
        case Mutation::DeleteASettingsRow:
            return "delete a configuration-input row";
        case Mutation::AddAnUnknownSettingsKey:
            return "document a configuration key the key table does not recognise";
        case Mutation::WrongEnvironmentVariable:
            return "document the wrong environment variable for a key";
        case Mutation::WrongCommandLineFlag:
            return "document the wrong command-line flag for a key";
        case Mutation::MakeTheSettingsTableUnrecognisable:
            return "rename the configuration table's first header cell";
    }
    return "unknown mutation";
}

/// One applied mutation: the mutated model, its rendering, and the defect the check
/// must report — its kind, the mismatched NAME, and the DOCUMENT and SECTION it
/// appears in, which together are Requirement 16.8's "documentation section in which
/// it appears".
struct Mutant {
    bool applicable{true};
    DocSet doc;
    Rendered text;
    DocDefectKind expected{DocDefectKind::UndocumentedName};
    std::string name;
    std::string document;
    std::string section;
    /// True for the four faults whose whole point is to make a region unreadable;
    /// the readback guards below are then not applicable to that region.
    bool structural{false};
};

/// A `(group, row)` pair into the option groups, chosen by `choice`.
struct OptionTarget {
    std::size_t group{0};
    std::size_t row{0};
};

[[nodiscard]] inline OptionTarget optionTarget(const DocSet& doc, std::size_t choice) {
    std::vector<OptionTarget> candidates;
    for (std::size_t g = 0; g < doc.optionGroups.size(); ++g) {
        for (std::size_t r = 0; r < doc.optionGroups[g].rows.size(); ++r) {
            candidates.push_back(OptionTarget{g, r});
        }
    }
    return candidates[choice % candidates.size()];
}

}  // namespace testsupport_documentationconsistencypropertytest
}  // namespace palmier

#endif  // PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT2_HPP_
