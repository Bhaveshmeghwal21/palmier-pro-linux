// SPDX-License-Identifier: GPL-3.0-or-later
//
// Continuation of the documentation_consistency_property_test fixtures; see documentation_consistency_property_test_support1.hpp.

#include "documentation_consistency_property_test_support2.hpp"

// Part 3 of 3 of the shared fixtures for documentation_consistency_property_test, split because the
// fixtures alone exceed the 1000-line ceiling this repository enforces. Cut only at
// points where the brace depth is zero, so no entity is divided.

#ifndef PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT3_HPP_
#define PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT3_HPP_

namespace palmier {
namespace testsupport_documentationconsistencypropertytest {

struct SettingTarget {
    std::size_t group{0};
    std::size_t row{0};
};

[[nodiscard]] inline SettingTarget settingTarget(const DocSet& doc, std::size_t choice) {
    std::vector<SettingTarget> candidates;
    for (std::size_t g = 0; g < doc.settingGroups.size(); ++g) {
        for (std::size_t r = 0; r < doc.settingGroups[g].rows.size(); ++r) {
            candidates.push_back(SettingTarget{g, r});
        }
    }
    return candidates[choice % candidates.size()];
}

template <typename Predicate>
[[nodiscard]] inline std::vector<std::size_t> toolsWhere(const DocSet& doc, Predicate predicate) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < doc.tools.size(); ++i) {
        if (predicate(doc.tools[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

/// A JSON type that is not `type`, so a type mutation always changes something.
[[nodiscard]] inline std::string otherJsonType(const std::string& type) {
    static const std::array<std::string_view, 6> kTypes{"string",  "integer", "number",
                                                        "boolean", "array",   "object"};
    for (const std::string_view candidate : kTypes) {
        if (candidate != type) {
            return std::string(candidate);
        }
    }
    return "string";
}

[[nodiscard]] inline std::string otherCacheType(const std::string& type) {
    static const std::array<std::string_view, 3> kTypes{"BOOL", "STRING", "INTERNAL"};
    for (const std::string_view candidate : kTypes) {
        if (candidate != type) {
            return std::string(candidate);
        }
    }
    return "BOOL";
}

/// Pure in (base, kind, choice): `choice` selects the target row, tool, argument or
/// variant by modulo, so the property draws it and the coverage test enumerates it.
[[nodiscard]] inline Mutant mutate(const DocSet& base, Mutation kind, std::size_t choice) {
    Mutant mutant;
    mutant.doc = base;
    DocSet& doc = mutant.doc;

    /// Pick one tool index out of `candidates`, marking the mutant inapplicable when
    /// a generator guarantee was broken (the callers assert applicability).
    const auto pickTool = [&](const std::vector<std::size_t>& candidates) -> std::size_t {
        if (candidates.empty()) {
            mutant.applicable = false;
            return 0;
        }
        return candidates[choice % candidates.size()];
    };
    const auto hasArguments = [](const ToolModel& tool) { return !tool.arguments.empty(); };

    switch (kind) {
        case Mutation::RenameAnOption: {
            const OptionTarget target = optionTarget(doc, choice);
            OptionRow& row = doc.optionGroups[target.group].rows[target.row];
            row.name += "_RENAMED";
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = row.name;
            mutant.document = std::string(kBuildDoc);
            mutant.section = sectionOf(doc, target.group);
            break;
        }
        case Mutation::DeleteAnOption: {
            const OptionTarget target = optionTarget(doc, choice);
            std::vector<OptionRow>& rows = doc.optionGroups[target.group].rows;
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = rows[target.row].name;
            mutant.document = std::string(kBuildDoc);
            mutant.section = std::string(kOptionsSection);  // the system's side has no section
            rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(target.row));
            break;
        }
        case Mutation::AddAnUnknownOption: {
            const std::size_t group = choice % doc.optionGroups.size();
            static const std::array<std::string_view, 2> kNames{"PALMIER_ENABLE_TELEPATHY",
                                                                "PALMIER_BUILD_HOLOGRAMS"};
            const std::string name{kNames[choice % kNames.size()]};
            doc.optionGroups[group].rows.push_back(OptionRow{name, "BOOL"});
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = name;
            mutant.document = std::string(kBuildDoc);
            mutant.section = sectionOf(doc, group);
            break;
        }
        case Mutation::ChangeAnOptionType: {
            const OptionTarget target = optionTarget(doc, choice);
            OptionRow& row = doc.optionGroups[target.group].rows[target.row];
            row.type = otherCacheType(row.type);
            mutant.expected = DocDefectKind::TypeMismatch;
            mutant.name = row.name;
            mutant.document = std::string(kBuildDoc);
            mutant.section = sectionOf(doc, target.group);
            break;
        }
        case Mutation::RemoveTheOptionMarkers: {
            doc.optionMarkersPresent = false;
            mutant.structural = true;
            mutant.expected = DocDefectKind::SectionMissing;
            mutant.name = "<!-- palmier-options:begin -->";
            mutant.document = std::string(kBuildDoc);
            mutant.section = std::string(kOptionsSection);
            break;
        }
        case Mutation::EmptyTheMarkedOptionRegion: {
            for (OptionGroup& group : doc.optionGroups) {
                group.rows.clear();
            }
            mutant.structural = true;
            mutant.expected = DocDefectKind::SectionMissing;
            mutant.name = "option rows";
            mutant.document = std::string(kBuildDoc);
            mutant.section = std::string(kOptionsSection);
            break;
        }
        case Mutation::RenameATool: {
            const std::size_t i = choice % doc.tools.size();
            doc.tools[i].name += "_renamed";  // still a dotted name, so still a section
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = doc.tools[i].name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = std::string(kToolSectionsSection);
            break;
        }
        case Mutation::DeleteATool: {
            const std::size_t i = choice % doc.tools.size();
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = doc.tools[i].name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = std::string(kToolSectionsSection);
            doc.tools.erase(doc.tools.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        case Mutation::AddAnUnknownTool: {
            static const std::array<std::string_view, 2> kNames{"timeline.telepathy",
                                                                "project.holograms"};
            ToolModel invented;
            invented.name = std::string(kNames[choice % kNames.size()]);
            invented.declaresNoArguments = true;
            invented.resultFields = {resultFieldPool().front()};
            invented.observedFields = invented.resultFields;
            doc.tools.insert(
                doc.tools.begin() + static_cast<std::ptrdiff_t>(choice % (doc.tools.size() + 1)),
                std::move(invented));
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = std::string(kNames[choice % kNames.size()]);
            mutant.document = std::string(kToolsDoc);
            mutant.section = std::string(kToolSectionsSection);
            break;
        }
        case Mutation::SwapTwoToolSections: {
            if (doc.tools.size() < 2) {
                mutant.applicable = false;
                break;
            }
            const std::size_t i = choice % (doc.tools.size() - 1);
            std::swap(doc.tools[i], doc.tools[i + 1]);
            // The check reports the first position at which the documented order differs
            // from the published one, naming the tool `tools/list` publishes there.
            mutant.expected = DocDefectKind::OrderMismatch;
            mutant.name = doc.tools[i + 1].name;  // the one that was published at i
            mutant.document = std::string(kToolsDoc);
            mutant.section = std::string(kToolSectionsSection);
            break;
        }
        case Mutation::RenameAnArgument: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            ArgumentModel& argument =
                doc.tools[i].arguments[choice % doc.tools[i].arguments.size()];
            argument.name += "Renamed";
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = argument.name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::DeleteAnArgument: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            std::vector<ArgumentModel>& arguments = doc.tools[i].arguments;
            const std::size_t a = choice % arguments.size();
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = arguments[a].name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(a));
            // A tool whose last documented argument is gone must not then claim it takes
            // none: that would inject a second, different fault.
            break;
        }
        case Mutation::AddAnUnknownArgument: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            static const std::array<std::string_view, 2> kNames{"phaseOfTheMoon", "extraArgument"};
            const std::string name{kNames[choice % kNames.size()]};
            doc.tools[i].arguments.push_back(
                ArgumentModel{name, "string", false, false, true, false});
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::ChangeAnArgumentType: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            ArgumentModel& argument =
                doc.tools[i].arguments[choice % doc.tools[i].arguments.size()];
            argument.type = otherJsonType(argument.type);
            mutant.expected = DocDefectKind::TypeMismatch;
            mutant.name = argument.name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::DropAUuidMarker: {
            const std::size_t i = pickTool(toolsWhere(doc, [](const ToolModel& tool) {
                return std::any_of(tool.arguments.begin(), tool.arguments.end(),
                                   [](const ArgumentModel& argument) { return argument.uuid; });
            }));
            if (!mutant.applicable) {
                break;
            }
            std::vector<std::size_t> uuidArguments;
            for (std::size_t a = 0; a < doc.tools[i].arguments.size(); ++a) {
                if (doc.tools[i].arguments[a].uuid) {
                    uuidArguments.push_back(a);
                }
            }
            ArgumentModel& argument =
                doc.tools[i].arguments[uuidArguments[choice % uuidArguments.size()]];
            argument.uuid = false;
            mutant.expected = DocDefectKind::TypeMismatch;
            mutant.name = argument.name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::AddASpuriousUuidMarker: {
            const std::size_t i = pickTool(toolsWhere(doc, [](const ToolModel& tool) {
                return std::any_of(tool.arguments.begin(), tool.arguments.end(),
                                   [](const ArgumentModel& argument) {
                                       return !argument.uuid && argument.type == "string";
                                   });
            }));
            if (!mutant.applicable) {
                break;
            }
            std::vector<std::size_t> plainArguments;
            for (std::size_t a = 0; a < doc.tools[i].arguments.size(); ++a) {
                const ArgumentModel& candidate = doc.tools[i].arguments[a];
                if (!candidate.uuid && candidate.type == "string") {
                    plainArguments.push_back(a);
                }
            }
            ArgumentModel& argument =
                doc.tools[i].arguments[plainArguments[choice % plainArguments.size()]];
            argument.uuid = true;
            mutant.expected = DocDefectKind::TypeMismatch;
            mutant.name = argument.name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::FlipARequiredMarking: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            ArgumentModel& argument =
                doc.tools[i].arguments[choice % doc.tools[i].arguments.size()];
            argument.required = !argument.required;
            mutant.expected = DocDefectKind::RequiredMismatch;
            mutant.name = argument.name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::ClaimNoArgumentsForAToolThatHasThem: {
            const std::size_t i = pickTool(toolsWhere(doc, hasArguments));
            if (!mutant.applicable) {
                break;
            }
            doc.tools[i].declaresNoArguments = true;  // the table stays: two claims, one wrong
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = doc.tools[i].name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::OmitTheNoArgumentsClaim: {
            const std::size_t i = pickTool(
                toolsWhere(doc, [](const ToolModel& tool) { return tool.arguments.empty(); }));
            if (!mutant.applicable) {
                break;
            }
            doc.tools[i].declaresNoArguments = false;
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = doc.tools[i].name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::DemoteEveryToolHeading: {
            doc.toolHeadingsAreSections = false;
            mutant.structural = true;
            mutant.expected = DocDefectKind::SectionMissing;
            mutant.name = "tool sections";
            mutant.document = std::string(kToolsDoc);
            mutant.section = "";
            break;
        }
        case Mutation::DeleteADocumentedResultField: {
            const std::size_t i = pickTool(
                toolsWhere(doc, [](const ToolModel& tool) { return !tool.resultFields.empty(); }));
            if (!mutant.applicable) {
                break;
            }
            std::vector<std::string>& fields = doc.tools[i].resultFields;
            const std::size_t f = choice % fields.size();
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = fields[f];
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            // The payload still carries it — that is the mismatch.
            fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(f));
            break;
        }
        case Mutation::DocumentAResultFieldNoPayloadCarries: {
            const std::size_t i = choice % doc.tools.size();
            static const std::array<std::string_view, 2> kNames{"phaseOfTheMoon", "inventedField"};
            const std::string name{kNames[choice % kNames.size()]};
            doc.tools[i].resultFields.push_back(name);
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = name;
            mutant.document = std::string(kToolsDoc);
            mutant.section = doc.tools[i].name;
            break;
        }
        case Mutation::PromoteAnUnobservedConditionalField: {
            const std::size_t i = pickTool(toolsWhere(doc, [](const ToolModel& tool) {
                return std::any_of(
                    tool.conditionalResultFields.begin(), tool.conditionalResultFields.end(),
                    [&tool](const std::string& field) {
                        return std::find(tool.observedFields.begin(), tool.observedFields.end(),
                                         field) == tool.observedFields.end();
                    });
            }));
            if (!mutant.applicable) {
                break;
            }
            ToolModel& tool = doc.tools[i];
            std::vector<std::size_t> unobserved;
            for (std::size_t f = 0; f < tool.conditionalResultFields.size(); ++f) {
                if (std::find(tool.observedFields.begin(), tool.observedFields.end(),
                              tool.conditionalResultFields[f]) == tool.observedFields.end()) {
                    unobserved.push_back(f);
                }
            }
            const std::size_t f = unobserved[choice % unobserved.size()];
            const std::string field = tool.conditionalResultFields[f];
            tool.conditionalResultFields.erase(tool.conditionalResultFields.begin() +
                                               static_cast<std::ptrdiff_t>(f));
            tool.resultFields.push_back(field);
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = field;
            mutant.document = std::string(kToolsDoc);
            mutant.section = tool.name;
            break;
        }
        case Mutation::RenameASettingsKey: {
            const SettingTarget target = settingTarget(doc, choice);
            SettingRow& row = doc.settingGroups[target.group].rows[target.row];
            row.key += "_renamed";
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = row.key;
            mutant.document = std::string(kRemoteDoc);
            mutant.section = doc.settingGroups[target.group].heading;
            break;
        }
        case Mutation::DeleteASettingsRow: {
            const SettingTarget target = settingTarget(doc, choice);
            std::vector<SettingRow>& rows = doc.settingGroups[target.group].rows;
            mutant.expected = DocDefectKind::UndocumentedName;
            mutant.name = rows[target.row].key;
            mutant.document = std::string(kRemoteDoc);
            mutant.section = std::string(kSettingsSection);  // the system's side has no section
            rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(target.row));
            break;
        }
        case Mutation::AddAnUnknownSettingsKey: {
            const std::size_t group = choice % doc.settingGroups.size();
            static const std::array<std::string_view, 2> kKeys{"remote.telepathy",
                                                               "agent.holograms"};
            const std::string key{kKeys[choice % kKeys.size()]};
            doc.settingGroups[group].rows.push_back(
                SettingRow{key, "PALMIER_REMOTE_TELEPATHY", "--remote-telepathy"});
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = key;
            mutant.document = std::string(kRemoteDoc);
            mutant.section = doc.settingGroups[group].heading;
            break;
        }
        case Mutation::WrongEnvironmentVariable: {
            const SettingTarget target = settingTarget(doc, choice);
            SettingRow& row = doc.settingGroups[target.group].rows[target.row];
            row.environmentVariable += "S";
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = row.environmentVariable;
            mutant.document = std::string(kRemoteDoc);
            mutant.section = doc.settingGroups[target.group].heading;
            break;
        }
        case Mutation::WrongCommandLineFlag: {
            const SettingTarget target = settingTarget(doc, choice);
            SettingRow& row = doc.settingGroups[target.group].rows[target.row];
            row.flag += "s";
            mutant.expected = DocDefectKind::UnknownDocumentedName;
            mutant.name = row.flag;
            mutant.document = std::string(kRemoteDoc);
            mutant.section = doc.settingGroups[target.group].heading;
            break;
        }
        case Mutation::MakeTheSettingsTableUnrecognisable: {
            doc.settingsHeaderRecognisable = false;
            mutant.structural = true;
            mutant.expected = DocDefectKind::SectionMissing;
            mutant.name = "configuration inputs";
            mutant.document = std::string(kRemoteDoc);
            mutant.section = "";
            break;
        }
    }

    mutant.text = render(mutant.doc);
    return mutant;
}

// ===========================================================================
// Requirement 16.8's third clause: the check modifies nothing
// ===========================================================================

struct DocumentFingerprint {
    std::string path;
    std::size_t bytes{0};
    std::uint64_t hash{0};

    [[nodiscard]] bool operator==(const DocumentFingerprint& other) const {
        return path == other.path && bytes == other.bytes && hash == other.hash;
    }
};

/// FNV-1a over the file's bytes. A hash plus the byte count, so neither a length
/// change nor an in-place edit of the same length can go unnoticed.
[[nodiscard]] inline DocumentFingerprint fingerprintOf(const std::filesystem::path& path) {
    const std::string content = testsupport::readWholeFile(path.string());
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char byte : content) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= 0x100000001b3ULL;
    }
    return DocumentFingerprint{path.filename().string(), content.size(), hash};
}

/// EVERY Markdown document in `docs/`, not merely the three the checker reads: the
/// obligation is that the check leaves the documentation alone, and enumerating the
/// directory rather than a list means a document added later is covered with no edit
/// here.
[[nodiscard]] inline std::vector<DocumentFingerprint> fingerprintTheDocumentation() {
    std::vector<std::filesystem::path> paths;
    std::error_code ignored;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(PALMIER_DOCS_DIR, ignored)) {
        if (entry.is_regular_file(ignored) && entry.path().extension() == ".md") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    std::vector<DocumentFingerprint> fingerprints;
    fingerprints.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        fingerprints.push_back(fingerprintOf(path));
    }
    return fingerprints;
}

}  // namespace testsupport_documentationconsistencypropertytest
}  // namespace palmier

#endif  // PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT3_HPP_
