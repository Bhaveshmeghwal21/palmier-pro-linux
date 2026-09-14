// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/documentation_consistency_property_test.cpp — the quantified half of
// the documentation consistency check: Property 76 (documentation and the running
// system agree on every name) and Property 77 (the check reports every mismatch and
// modifies nothing) (task 12.8; Requirements 16.4, 16.7, 16.8).
//
// HOW THIS FILE RELATES TO TASK 12.7's `tests/docs/documentation_consistency_test.cpp`
// ---------------------------------------------------------------------------
// That file is the EXAMPLE-BASED half. It checks the three CHECKED-IN documents
// against the running system, drives a 20-tool scenario so every registry-rendered
// success payload is observed at least once, and produces each defect kind by a
// single hand-written synthetic document. This file is the QUANTIFIED half over the
// very same pure functions of `tests/support/DocumentationChecker.hpp`, so it does
// not restate those single-edit cases. Instead it:
//
//   * GENERATES whole documentation sets — a `docs/BUILD.md`, a `docs/TOOLS.md` and
//     a `docs/REMOTE_ACCESS.md` — whose option, tool, argument and settings NAMES
//     ARE THE RUNNING SYSTEM'S, and whose PRESENTATION is drawn: how many `###`
//     option groups and what they are called, which column order each of the three
//     tables uses, whether an option's type is backticked, whether a tool's result
//     fields are declared by a `| Field |` table or by a `Result:` paragraph,
//     whether the `Required` marking is bolded, which side of the type the `*uuid*`
//     marker sits on, the order of the arguments inside a table, and which fields a
//     success payload happens to carry. Every one of those documents must be
//     accepted (Property 76); and
//   * MUTATES a generated set at the MODEL level — rename an option, drop a tool,
//     invent an argument, flip a required marking, break the extraction markers,
//     give a settings key the wrong environment variable, ... — re-renders it, and
//     asserts the check reports the corresponding defect AGAINST THE OFFENDING NAME
//     AND THE SECTION IT APPEARS IN (Property 77, Requirement 16.8's report
//     obligation).
//
// Mutating the model and re-rendering, rather than editing text, is what makes the
// mutation space quantifiable: a mutation is a pure function of (generated document
// set, mutation kind, choice), so the property draws over all three and the
// coverage test below enumerates them.
//
// WHERE THE "RUNNING SYSTEM" COMES FROM
// ---------------------------------------------------------------------------
// Nothing in this file restates a name the system owns. As in task 12.7:
//
//   * options   — `palmier_options.txt`, written at configure time by
//                 cmake/PalmierOptionsManifest.cmake from this tree's `PALMIER_*`
//                 cache entries.
//   * tools and arguments — the `ToolRegistry::describe()` payload, i.e. the bytes
//                 `tools/list` returns, including each rendered `ToolSchema`.
//   * settings  — `AppSettings::recognizedKeys()` and its environment-variable and
//                 flag accessors.
//   * result fields — the field names REAL handler invocations put in their success
//                 payloads.
//
// So a tool, argument, option or settings key added tomorrow changes what these
// properties generate tomorrow, with no edit here.
//
// WHY THESE PROPERTIES CANNOT PASS VACUOUSLY
// ---------------------------------------------------------------------------
// The failure mode a document check cannot afford is a generated document the
// extractor cannot read: `check(extract(unreadable)) == {}` would be a green tick
// over nothing. Five guards, all asserted on every case:
//
//   1. THE CHECKED-IN DOCUMENTS ARE A CASE OF EVERY RUN. Both properties first
//      assert that `docs/BUILD.md`, `docs/TOOLS.md`, `docs/REMOTE_ACCESS.md` and
//      the options manifest were FOUND (non-zero bytes), that what was extracted
//      from them has the real counts — as many options as the manifest lists, as
//      many tools as `tools/list` publishes, as many settings rows as the key table
//      recognises, all non-zero — and only then that they have no defects.
//   2. EVERY GENERATED DOCUMENT IS READ BACK FIELD FOR FIELD AGAINST ITS MODEL.
//      Not "no defects" alone: every option's name, type and `###` section, every
//      tool's name, position, no-arguments claim, argument names, types, uuid
//      markers and required markings, every result field and its conditionality,
//      and every settings row's key, environment variable, flag and `##` section
//      are compared with the model that produced them. A renderer that emitted
//      something the extractor skipped fails here rather than passing quietly.
//   3. THE GENERATOR'S OWN GUARANTEES ARE ASSERTED, NOT ASSUMED. A document set
//      with no uuid argument, no optional argument, no argument-free tool, no
//      conditional result field or only one option group would exercise fewer
//      clauses than Property 76 claims, so each of those is asserted per case.
//   4. THE MUTANT IS ASSERTED TO STILL BE A READABLE DOCUMENT SET. Property 77
//      asserts the mutant's extracted option, tool, argument and settings counts
//      equal the mutated model's and that every name in it was read back, so the
//      defect being asserted is attributable to the injected fault and not to a
//      garbled document. The four structural mutations, whose whole point is to
//      make a region unreadable, declare that and are asserted to produce exactly
//      the `SectionMissing` the extraction contract promises.
//   5. THE UNMUTATED RENDERING OF THE SAME CASE IS ASSERTED CLEAN. A mutation is
//      only evidence if the thing it was applied to passed.
//
// Requirement 16.8's third clause — "SHALL leave the documentation unmodified" — is
// asserted by Property 77 on every case by fingerprinting EVERY `.md` file in
// `docs/` (not merely the three the checker reads) before and after a full check
// run over the checked-in documents.
//
// COST
// ---------------------------------------------------------------------------
// The live surface and the real result observation are built ONCE per process
// (function-local statics); each generated case renders three Markdown strings of a
// few kilobytes and extracts them, which is microseconds, plus one ~117 KB
// fingerprint pass over `docs/` for Property 77. The binary stays far inside the
// 600 s per-test limit.
//
// _Requirements: 16.4, 16.7, 16.8_

#include "support/DocumentationChecker.hpp"

#include <gtest/gtest.h>

#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/AppSettings.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

// Part 1 of 3 of the shared fixtures for documentation_consistency_property_test, split because the
// fixtures alone exceed the 1000-line ceiling this repository enforces. Cut only at
// points where the brace depth is zero, so no entity is divided.

#ifndef PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT1_HPP_
#define PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT1_HPP_

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_OPTIONS_MANIFEST
#error "PALMIER_OPTIONS_MANIFEST must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace testsupport_documentationconsistencypropertytest {

using testsupport::DocDefect;
using testsupport::DocDefectKind;
using testsupport::DocumentedArgument;
using testsupport::DocumentedOption;
using testsupport::DocumentedSetting;
using testsupport::DocumentedTool;
using testsupport::LiveArgument;
using testsupport::LiveOption;
using testsupport::LiveSetting;
using testsupport::LiveTool;
using testsupport::ObservedResult;

constexpr std::string_view kBuildDoc = "docs/BUILD.md";
constexpr std::string_view kToolsDoc = "docs/TOOLS.md";
constexpr std::string_view kRemoteDoc = "docs/REMOTE_ACCESS.md";

/// The section string `checkOptions` reports for a name the documentation omits,
/// and the default `###` section of the marked option region.
constexpr std::string_view kOptionsSection = "CMake options";
constexpr std::string_view kToolSectionsSection = "tool sections";
constexpr std::string_view kSettingsSection = "Every configuration input";

[[nodiscard]] inline std::string docsPath(std::string_view name) {
    return std::string{PALMIER_DOCS_DIR} + "/" + std::string{name};
}

// ===========================================================================
// The live surface, read out of the running system
// ===========================================================================

/// Exactly what an MCP client sees: `describe()` IS the `tools/list` result and each
/// entry's `inputSchema` is the rendered `ToolSchema`. Reading the live side from the
/// published payload is what makes this Requirement 16.7's comparison.
[[nodiscard]] inline std::vector<LiveTool> liveToolsFrom(const services::Json& described) {
    std::vector<LiveTool> tools;
    for (const services::Json& entry : described.asArray()) {
        LiveTool tool;
        tool.name = entry.stringOr("name");
        const services::Json* schema = entry.find("inputSchema");
        if (schema == nullptr) {
            tools.push_back(std::move(tool));
            continue;
        }
        std::vector<std::string> required;
        if (const services::Json* names = schema->find("required"); names != nullptr) {
            for (const services::Json& name : names->asArray()) {
                required.push_back(name.asString());
            }
        }
        if (const services::Json* properties = schema->find("properties"); properties != nullptr) {
            for (const auto& [name, property] : properties->asObject()) {
                LiveArgument argument;
                argument.name = name;
                argument.type = property.stringOr("type");
                argument.uuid = property.stringOr("format") == "uuid";
                argument.required =
                    std::find(required.begin(), required.end(), name) != required.end();
                tool.arguments.push_back(std::move(argument));
            }
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

/// The four tools whose success payload one real, argument-free (or fully
/// determined) invocation produces, observed for real.
///
/// This is deliberately NOT task 12.7's twenty-tool scenario: that scenario, and
/// the assertion that every registry-rendered payload is observed at least once,
/// belongs to `documentation_consistency_test.cpp` and is not restated here. What
/// this compact observation buys is that the checked-in `docs/TOOLS.md` result-field
/// claims are anchored to REAL payloads on every run of this file too, while the
/// generated cases below quantify over result-field shapes with generated
/// observations.
[[nodiscard]] inline std::vector<ObservedResult> observeRealResults() {
    services::ProjectSession session;
    services::ToolRegistry registry = services::buildDefaultToolRegistry(session);

    std::vector<ObservedResult> observed;
    const auto run = [&](std::string_view tool, const services::Json& args) {
        Result<services::Json> result = registry.invoke(tool, args);
        if (result.isError()) {
            return;  // the callers assert the observation is non-empty and complete
        }
        ObservedResult record;
        record.toolName = std::string(tool);
        for (const auto& [name, value] : result.value().asObject()) {
            record.fields.push_back(name);
        }
        observed.push_back(std::move(record));
    };

    services::Json create = services::Json::object();
    create.set("name", std::string{"Documentation consistency property check"});
    create.set("fps", 30.0);
    create.set("width", static_cast<std::int64_t>(1920));
    create.set("height", static_cast<std::int64_t>(1080));
    create.set("colorSpace", std::string{"Rec.709"});
    run("project.create", create);
    run("project.info", services::Json::object());
    run("media.list", services::Json::object());
    run("timeline.read", services::Json::object());
    return observed;
}

struct LiveSurface {
    std::vector<LiveOption> options;
    std::vector<LiveTool> tools;
    std::vector<LiveSetting> settings;
    std::vector<ObservedResult> observed;
    std::vector<DocDefect> manifestDefects;
    std::size_t manifestBytes{0};
};

/// Built once per process: the manifest is a file read, `describe()` renders 22
/// schemas and the observation applies real commands.
inline const LiveSurface& live() {
    static const LiveSurface surface = [] {
        LiveSurface out;
        const std::string manifest = testsupport::readWholeFile(PALMIER_OPTIONS_MANIFEST);
        out.manifestBytes = manifest.size();
        out.options = testsupport::parseOptionsManifest(manifest, out.manifestDefects);

        services::ProjectSession* noSession = nullptr;
        const services::ToolRegistry advertised = services::buildDefaultToolRegistry(noSession);
        out.tools = liveToolsFrom(advertised.describe());

        for (const std::string_view key : app::AppSettings::recognizedKeys()) {
            out.settings.push_back(LiveSetting{
                std::string(key), std::string(app::AppSettings::environmentVariableFor(key)),
                std::string(app::AppSettings::commandLineFlagFor(key))});
        }
        out.observed = observeRealResults();
        return out;
    }();
    return surface;
}

// ===========================================================================
// One check run over a documentation set — exactly what the CI gate does
// ===========================================================================

struct Checked {
    std::size_t buildBytes{0};
    std::size_t toolsBytes{0};
    std::size_t remoteBytes{0};

    std::vector<DocumentedOption> options;
    std::vector<DocumentedTool> tools;
    std::vector<DocumentedSetting> settings;

    std::vector<DocDefect> extraction;  ///< defects raised while extracting
    std::vector<DocDefect> defects;     ///< extraction defects followed by check defects
};

inline void append(std::vector<DocDefect>& into, const std::vector<DocDefect>& more) {
    into.insert(into.end(), more.begin(), more.end());
}

/// Extract the three documents and compare each name class against the live
/// surface. `observed` is the success-payload side of Requirement 16.4's
/// result-field clause.
[[nodiscard]] inline Checked checkDocuments(std::string_view build, std::string_view tools,
                                            std::string_view remote,
                                            const std::vector<ObservedResult>& observed) {
    Checked out;
    out.buildBytes = build.size();
    out.toolsBytes = tools.size();
    out.remoteBytes = remote.size();

    out.options = testsupport::extractDocumentedOptions(build, out.extraction);
    out.tools = testsupport::extractDocumentedTools(tools, out.extraction);
    out.settings = testsupport::extractDocumentedSettings(remote, out.extraction);

    out.defects = out.extraction;
    append(out.defects, testsupport::checkOptions(out.options, live().options));
    append(out.defects, testsupport::checkTools(out.tools, live().tools));
    append(out.defects, testsupport::checkSettings(out.settings, live().settings));
    append(out.defects, testsupport::checkResultFields(out.tools, observed));
    return out;
}

/// The checked-in documentation set, read from the SOURCE tree through
/// PALMIER_DOCS_DIR (ctest's working directory is the build tree, where the
/// documents do not exist — hence the byte-count assertions at every use).
[[nodiscard]] inline Checked checkCheckedInDocuments() {
    return checkDocuments(testsupport::readWholeFile(docsPath("BUILD.md")),
                          testsupport::readWholeFile(docsPath("TOOLS.md")),
                          testsupport::readWholeFile(docsPath("REMOTE_ACCESS.md")),
                          live().observed);
}

inline const Checked& checkedInDocuments() {
    static const Checked outcome = checkCheckedInDocuments();
    return outcome;
}

[[nodiscard]] inline std::size_t liveArgumentCount() {
    std::size_t count = 0;
    for (const LiveTool& tool : live().tools) {
        count += tool.arguments.size();
    }
    return count;
}

/// Shared by both properties: the checked-in revision is a case of every run, and
/// what was EXTRACTED from it is asserted to have the real counts before "it has no
/// defects" is asserted.
inline void assertCheckedInDocumentationAgreesWithTheRunningSystem() {
    const LiveSurface& system = live();
    RC_ASSERT(system.manifestBytes > 0);        // palmier_options.txt was found
    RC_ASSERT(system.manifestDefects.empty());  // and held NAME|TYPE records
    RC_ASSERT(!system.options.empty());
    RC_ASSERT(!system.tools.empty());
    RC_ASSERT(!system.settings.empty());
    RC_ASSERT(liveArgumentCount() > 0);

    // The compact real observation really ran: four tools, each with a payload.
    RC_ASSERT(system.observed.size() == 4);
    for (const ObservedResult& result : system.observed) {
        RC_ASSERT(result.fields.size() >= 2);
    }

    const Checked& real = checkedInDocuments();
    RC_ASSERT(real.buildBytes > 0);
    RC_ASSERT(real.toolsBytes > 0);
    RC_ASSERT(real.remoteBytes > 0);
    RC_ASSERT(real.extraction.empty());
    RC_ASSERT(real.options.size() == system.options.size());
    RC_ASSERT(real.tools.size() == system.tools.size());
    RC_ASSERT(real.settings.size() == system.settings.size());
    RC_ASSERT(real.defects.empty());
}

// ===========================================================================
// The document model — a documentation set as values, plus three renderers
// ===========================================================================
//
// The generator builds this model from the live surface and renders it to Markdown;
// the mutations of Property 77 edit the model and re-render. The renderers write the
// grammar `docs/BUILD.md`'s and `docs/TOOLS.md`'s own "Extraction contract" sections
// state, which `DocumentationChecker.cpp` implements.

/// Which column order an option table uses. Every layout carries a `Type` column;
/// where it sits is what `columnIndex` has to find.
enum class OptionLayout {
    OptionTypeDefaultEffect,  ///< `| Option | Type | Default | Effect |`
    EntryTypeNotes,           ///< `| Entry | Type | ON when |` — the derived table's shape
    OptionDefaultType         ///< `| Option | Default | Type |` — Type last
};

enum class ArgumentLayout {
    NameTypeRequired,        ///< `| Argument | Type | Required |`
    NameTypeRequiredValues,  ///< `| Argument | Type | Required | Accepted values |`
    NameRequiredTypeNotes    ///< `| Argument | Required | Type | Notes |` — swapped
};

enum class SettingLayout {
    KeyEnvironmentFlagDefault,  ///< `| Config-file key | Environment variable | Command-line flag | Default |`
    KeyFlagEnvironment  ///< `| Config-file key | Command-line flag | Environment variable |`
};

struct OptionRow {
    std::string name;
    std::string type;
};

struct OptionGroup {
    std::string heading;  ///< the `###` heading; empty only for the first group
    std::vector<OptionRow> rows;
};

struct ArgumentModel {
    std::string name;
    std::string type;
    bool required{false};
    bool uuid{false};
    bool boldRequiredMarker{true};  ///< `**yes**` versus `yes`
    bool uuidMarkerFirst{false};    ///< `*uuid* string` versus `string *uuid*`
};

struct ToolModel {
    std::string name;
    std::vector<ArgumentModel> arguments;
    bool declaresNoArguments{false};
    std::vector<std::string> resultFields;
    std::vector<std::string> conditionalResultFields;
    std::vector<std::string> observedFields;  ///< the payload a real invocation "returned"
    bool resultTable{false};                  ///< `| Field |` table versus a `Result:` paragraph
    bool commandResult{false};                ///< carries the *(command result)* marker
    ArgumentLayout layout{ArgumentLayout::NameTypeRequired};
};

struct SettingRow {
    std::string key;
    std::string environmentVariable;
    std::string flag;
};

struct SettingGroup {
    std::string heading;  ///< the `##` heading the rows sit under
    std::vector<SettingRow> rows;
};

struct DocSet {
    std::vector<OptionGroup> optionGroups;
    std::vector<ToolModel> tools;
    std::vector<SettingGroup> settingGroups;

    OptionLayout optionLayout{OptionLayout::OptionTypeDefaultEffect};
    SettingLayout settingLayout{SettingLayout::KeyEnvironmentFlagDefault};
    bool optionTypesBackticked{false};

    // The three structural faults, expressed as model values so that they too are
    // mutations of a value rather than text edits.
    bool optionMarkersPresent{true};
    bool settingsHeaderRecognisable{true};
    bool toolHeadingsAreSections{true};
};

// --- rendering --------------------------------------------------------------

[[nodiscard]] inline std::string backticked(std::string_view text) {
    return "`" + std::string(text) + "`";
}

[[nodiscard]] inline std::string renderOptionTable(const DocSet& doc, const OptionGroup& group) {
    std::string header;
    switch (doc.optionLayout) {
        case OptionLayout::OptionTypeDefaultEffect:
            header = "| Option | Type | Default | Effect |\n|---|---|---|---|\n";
            break;
        case OptionLayout::EntryTypeNotes:
            header = "| Entry | Type | `ON` when |\n|---|---|---|\n";
            break;
        case OptionLayout::OptionDefaultType:
            header = "| Option | Default | Type |\n|---|---|---|\n";
            break;
    }

    std::string text = header;
    for (const OptionRow& row : group.rows) {
        const std::string type = doc.optionTypesBackticked ? backticked(row.type) : row.type;
        switch (doc.optionLayout) {
            case OptionLayout::OptionTypeDefaultEffect:
                text +=
                    "| " + backticked(row.name) + " | " + type + " | `ON` | what it turns on |\n";
                break;
            case OptionLayout::EntryTypeNotes:
                text += "| " + backticked(row.name) + " | " + type + " | detection succeeded |\n";
                break;
            case OptionLayout::OptionDefaultType:
                text += "| " + backticked(row.name) + " | `ON` | " + type + " |\n";
                break;
        }
    }
    return text;
}

/// A `docs/BUILD.md` to the contract: the option tables live between the
/// `palmier-options` markers, grouped under `###` headings. A decoy row naming a
/// non-`PALMIER_` cache entry sits inside the region, and a whole second option
/// table sits OUTSIDE it — both of which the extractor must ignore, and both of
/// which the readback below asserts were ignored.
[[nodiscard]] inline std::string renderBuildDoc(const DocSet& doc) {
    std::string text = "# Building a generated tree\n\nThe configure, build, test and launch "
                       "sequence, then the options.\n\n## CMake options\n\n";
    if (doc.optionMarkersPresent) {
        text += "<!-- palmier-options:begin -->\n";
    }
    for (const OptionGroup& group : doc.optionGroups) {
        if (!group.heading.empty()) {
            text += "\n### " + group.heading + "\n";
        }
        text += "\n" + renderOptionTable(doc, group);
        text += "| `CMAKE_BUILD_TYPE` | STRING | `Release` | not a PALMIER_ entry |\n";
    }
    if (doc.optionMarkersPresent) {
        text += "\n<!-- palmier-options:end -->\n";
    }
    text += "\n## Minimum host specification\n\nProse, and a table the extraction region does "
            "not cover:\n\n| Option | Type | Default | Effect |\n|---|---|---|---|\n"
            "| `PALMIER_OUTSIDE_THE_MARKED_REGION` | BOOL | `ON` | must never be extracted |\n";
    return text;
}

[[nodiscard]] inline std::string renderArgumentTable(const ToolModel& tool) {
    std::string header;
    switch (tool.layout) {
        case ArgumentLayout::NameTypeRequired:
            header = "| Argument | Type | Required |\n|---|---|---|\n";
            break;
        case ArgumentLayout::NameTypeRequiredValues:
            header = "| Argument | Type | Required | Accepted values |\n|---|---|---|---|\n";
            break;
        case ArgumentLayout::NameRequiredTypeNotes:
            header = "| Argument | Required | Type | Notes |\n|---|---|---|---|\n";
            break;
    }

    std::string text = header;
    for (const ArgumentModel& argument : tool.arguments) {
        std::string type = argument.type;
        if (argument.uuid) {
            type = argument.uuidMarkerFirst ? "*uuid* " + type : type + " *uuid*";
        }
        std::string required = argument.required ? "yes" : "no";
        if (argument.boldRequiredMarker) {
            required = "**" + required + "**";
        }
        switch (tool.layout) {
            case ArgumentLayout::NameTypeRequired:
                text += "| " + backticked(argument.name) + " | " + type + " | " + required + " |\n";
                break;
            case ArgumentLayout::NameTypeRequiredValues:
                text += "| " + backticked(argument.name) + " | " + type + " | " + required +
                        " | the values the schema declares |\n";
                break;
            case ArgumentLayout::NameRequiredTypeNotes:
                text += "| " + backticked(argument.name) + " | " + required + " | " + type +
                        " | a note |\n";
                break;
        }
    }
    return text;
}

/// The result half of a tool section, in one of the document's two shapes.
[[nodiscard]] inline std::string renderResults(const ToolModel& tool) {
    std::string text;
    if (tool.resultTable) {
        text += "Result — the success payload:\n\n| Field | Type | Notes |\n|---|---|---|\n";
        for (const std::string& field : tool.resultFields) {
            text += "| " + backticked(field) + " | string | always |\n";
        }
        for (const std::string& field : tool.conditionalResultFields) {
            text += "| " + backticked(field) +
                    " | string | present only when the edit changed "
                    "something |\n";
        }
        if (tool.commandResult) {
            text += "\nThis tool applies an edit, so its payload is *(command result)*.\n";
        }
        return text;
    }

    text += "Result:";
    for (std::size_t i = 0; i < tool.resultFields.size(); ++i) {
        text += (i == 0 ? " " : ", ") + backticked(tool.resultFields[i]) + " (string)";
    }
    if (tool.commandResult) {
        text += ", *(command result)*";
    }
    text += ".\n";
    for (const std::string& field : tool.conditionalResultFields) {
        text += "\n" + backticked(field) +
                " (string) is present only when the operation produced "
                "one.\n";
    }
    return text;
}

/// A `docs/TOOLS.md` to the contract: one `## \`tool.name\`` section per tool in the
/// order `tools/list` publishes, preceded by a prose `##` section that carries no
/// backticked dotted name and must therefore not be read as a tool.
[[nodiscard]] inline std::string renderToolsDoc(const DocSet& doc) {
    std::string text = "# Tool reference for a generated tree\n\n"
                       "## Rules that hold for every tool\n\n"
                       "The published schema is the validator, and the order below is the order "
                       "`tools/list` publishes.\n";
    const std::string_view heading = doc.toolHeadingsAreSections ? "## " : "### ";
    for (const ToolModel& tool : doc.tools) {
        text += "\n" + std::string(heading) + backticked(tool.name) + "\n\n";
        text += "What this tool does, in one sentence of ordinary prose.\n\n";
        if (tool.declaresNoArguments) {
            text += "No arguments.\n\n";
        }
        if (!tool.arguments.empty()) {
            text += renderArgumentTable(tool) + "\n";
        }
        text += renderResults(tool);
    }
    text += "\n## Next\n\nLinks into the rest of the documentation set.\n";
    return text;
}

/// A `docs/REMOTE_ACCESS.md` to the contract: one or more `##` sections, each
/// holding a table whose first header cell is `Config-file key`.
[[nodiscard]] inline std::string renderRemoteDoc(const DocSet& doc) {
    std::string text = "# Remote access for a generated tree\n\nBind address, token, "
                       "acknowledgement, TLS, origins, sessions and idle timeout.\n";
    const std::string firstCell = doc.settingsHeaderRecognisable ? "Config-file key" : "Key";
    for (const SettingGroup& group : doc.settingGroups) {
        text += "\n## " + group.heading +
                "\n\nPrecedence, lowest first: defaults, file, "
                "environment, command line.\n\n";
        switch (doc.settingLayout) {
            case SettingLayout::KeyEnvironmentFlagDefault:
                text +=
                    "| " + firstCell +
                    " | Environment variable | Command-line flag | Default |\n|---|---|---|---|\n";
                break;
            case SettingLayout::KeyFlagEnvironment:
                text += "| " + firstCell +
                        " | Command-line flag | Environment variable |\n|---|---|---|\n";
                break;
        }
        for (const SettingRow& row : group.rows) {
            switch (doc.settingLayout) {
                case SettingLayout::KeyEnvironmentFlagDefault:
                    text += "| " + backticked(row.key) + " | " +
                            backticked(row.environmentVariable) + " | " + backticked(row.flag) +
                            " | *(empty)* |\n";
                    break;
                case SettingLayout::KeyFlagEnvironment:
                    text += "| " + backticked(row.key) + " | " + backticked(row.flag) + " | " +
                            backticked(row.environmentVariable) + " |\n";
                    break;
            }
        }
    }
    text += "\nRemote access without TLS transmits traffic unencrypted.\n";
    return text;
}

}  // namespace testsupport_documentationconsistencypropertytest
}  // namespace palmier

#endif  // PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_PROPERTY_TEST_SUPPORT1_HPP_
