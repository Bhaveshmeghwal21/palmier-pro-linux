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

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_OPTIONS_MANIFEST
#error "PALMIER_OPTIONS_MANIFEST must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace {

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

[[nodiscard]] std::string docsPath(std::string_view name) {
    return std::string{PALMIER_DOCS_DIR} + "/" + std::string{name};
}

// ===========================================================================
// The live surface, read out of the running system
// ===========================================================================

/// Exactly what an MCP client sees: `describe()` IS the `tools/list` result and each
/// entry's `inputSchema` is the rendered `ToolSchema`. Reading the live side from the
/// published payload is what makes this Requirement 16.7's comparison.
[[nodiscard]] std::vector<LiveTool> liveToolsFrom(const services::Json& described) {
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
[[nodiscard]] std::vector<ObservedResult> observeRealResults() {
    services::ProjectSession session;
    services::ToolRegistry   registry = services::buildDefaultToolRegistry(session);

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
    std::vector<LiveOption>     options;
    std::vector<LiveTool>       tools;
    std::vector<LiveSetting>    settings;
    std::vector<ObservedResult> observed;
    std::vector<DocDefect>      manifestDefects;
    std::size_t                 manifestBytes{0};
};

/// Built once per process: the manifest is a file read, `describe()` renders 22
/// schemas and the observation applies real commands.
const LiveSurface& live() {
    static const LiveSurface surface = [] {
        LiveSurface out;
        const std::string manifest = testsupport::readWholeFile(PALMIER_OPTIONS_MANIFEST);
        out.manifestBytes = manifest.size();
        out.options = testsupport::parseOptionsManifest(manifest, out.manifestDefects);

        services::ProjectSession* noSession = nullptr;
        const services::ToolRegistry advertised = services::buildDefaultToolRegistry(noSession);
        out.tools = liveToolsFrom(advertised.describe());

        for (const std::string_view key : app::AppSettings::recognizedKeys()) {
            out.settings.push_back(
                LiveSetting{std::string(key),
                            std::string(app::AppSettings::environmentVariableFor(key)),
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

    std::vector<DocumentedOption>  options;
    std::vector<DocumentedTool>    tools;
    std::vector<DocumentedSetting> settings;

    std::vector<DocDefect> extraction;  ///< defects raised while extracting
    std::vector<DocDefect> defects;     ///< extraction defects followed by check defects
};

void append(std::vector<DocDefect>& into, const std::vector<DocDefect>& more) {
    into.insert(into.end(), more.begin(), more.end());
}

/// Extract the three documents and compare each name class against the live
/// surface. `observed` is the success-payload side of Requirement 16.4's
/// result-field clause.
[[nodiscard]] Checked checkDocuments(std::string_view build,
                                     std::string_view tools,
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
[[nodiscard]] Checked checkCheckedInDocuments() {
    return checkDocuments(testsupport::readWholeFile(docsPath("BUILD.md")),
                          testsupport::readWholeFile(docsPath("TOOLS.md")),
                          testsupport::readWholeFile(docsPath("REMOTE_ACCESS.md")),
                          live().observed);
}

const Checked& checkedInDocuments() {
    static const Checked outcome = checkCheckedInDocuments();
    return outcome;
}

[[nodiscard]] std::size_t liveArgumentCount() {
    std::size_t count = 0;
    for (const LiveTool& tool : live().tools) {
        count += tool.arguments.size();
    }
    return count;
}

/// Shared by both properties: the checked-in revision is a case of every run, and
/// what was EXTRACTED from it is asserted to have the real counts before "it has no
/// defects" is asserted.
void assertCheckedInDocumentationAgreesWithTheRunningSystem() {
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
    KeyFlagEnvironment          ///< `| Config-file key | Command-line flag | Environment variable |`
};

struct OptionRow {
    std::string name;
    std::string type;
};

struct OptionGroup {
    std::string            heading;  ///< the `###` heading; empty only for the first group
    std::vector<OptionRow> rows;
};

struct ArgumentModel {
    std::string name;
    std::string type;
    bool        required{false};
    bool        uuid{false};
    bool        boldRequiredMarker{true};  ///< `**yes**` versus `yes`
    bool        uuidMarkerFirst{false};    ///< `*uuid* string` versus `string *uuid*`
};

struct ToolModel {
    std::string                name;
    std::vector<ArgumentModel> arguments;
    bool                       declaresNoArguments{false};
    std::vector<std::string>   resultFields;
    std::vector<std::string>   conditionalResultFields;
    std::vector<std::string>   observedFields;  ///< the payload a real invocation "returned"
    bool                       resultTable{false};   ///< `| Field |` table versus a `Result:` paragraph
    bool                       commandResult{false}; ///< carries the *(command result)* marker
    ArgumentLayout             layout{ArgumentLayout::NameTypeRequired};
};

struct SettingRow {
    std::string key;
    std::string environmentVariable;
    std::string flag;
};

struct SettingGroup {
    std::string             heading;  ///< the `##` heading the rows sit under
    std::vector<SettingRow> rows;
};

struct DocSet {
    std::vector<OptionGroup>  optionGroups;
    std::vector<ToolModel>    tools;
    std::vector<SettingGroup> settingGroups;

    OptionLayout  optionLayout{OptionLayout::OptionTypeDefaultEffect};
    SettingLayout settingLayout{SettingLayout::KeyEnvironmentFlagDefault};
    bool          optionTypesBackticked{false};

    // The three structural faults, expressed as model values so that they too are
    // mutations of a value rather than text edits.
    bool optionMarkersPresent{true};
    bool settingsHeaderRecognisable{true};
    bool toolHeadingsAreSections{true};
};

// --- rendering --------------------------------------------------------------

[[nodiscard]] std::string backticked(std::string_view text) {
    return "`" + std::string(text) + "`";
}

[[nodiscard]] std::string renderOptionTable(const DocSet& doc, const OptionGroup& group) {
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
            text += "| " + backticked(row.name) + " | " + type + " | `ON` | what it turns on |\n";
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
[[nodiscard]] std::string renderBuildDoc(const DocSet& doc) {
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

[[nodiscard]] std::string renderArgumentTable(const ToolModel& tool) {
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
            text += "| " + backticked(argument.name) + " | " + type + " | " + required
                    + " | the values the schema declares |\n";
            break;
        case ArgumentLayout::NameRequiredTypeNotes:
            text += "| " + backticked(argument.name) + " | " + required + " | " + type
                    + " | a note |\n";
            break;
        }
    }
    return text;
}

/// The result half of a tool section, in one of the document's two shapes.
[[nodiscard]] std::string renderResults(const ToolModel& tool) {
    std::string text;
    if (tool.resultTable) {
        text += "Result — the success payload:\n\n| Field | Type | Notes |\n|---|---|---|\n";
        for (const std::string& field : tool.resultFields) {
            text += "| " + backticked(field) + " | string | always |\n";
        }
        for (const std::string& field : tool.conditionalResultFields) {
            text += "| " + backticked(field) + " | string | present only when the edit changed "
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
        text += "\n" + backticked(field) + " (string) is present only when the operation produced "
                                       "one.\n";
    }
    return text;
}

/// A `docs/TOOLS.md` to the contract: one `## \`tool.name\`` section per tool in the
/// order `tools/list` publishes, preceded by a prose `##` section that carries no
/// backticked dotted name and must therefore not be read as a tool.
[[nodiscard]] std::string renderToolsDoc(const DocSet& doc) {
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
[[nodiscard]] std::string renderRemoteDoc(const DocSet& doc) {
    std::string text = "# Remote access for a generated tree\n\nBind address, token, "
                       "acknowledgement, TLS, origins, sessions and idle timeout.\n";
    const std::string firstCell = doc.settingsHeaderRecognisable ? "Config-file key" : "Key";
    for (const SettingGroup& group : doc.settingGroups) {
        text += "\n## " + group.heading + "\n\nPrecedence, lowest first: defaults, file, "
                                         "environment, command line.\n\n";
        switch (doc.settingLayout) {
        case SettingLayout::KeyEnvironmentFlagDefault:
            text += "| " + firstCell
                    + " | Environment variable | Command-line flag | Default |\n|---|---|---|---|\n";
            break;
        case SettingLayout::KeyFlagEnvironment:
            text += "| " + firstCell
                    + " | Command-line flag | Environment variable |\n|---|---|---|\n";
            break;
        }
        for (const SettingRow& row : group.rows) {
            switch (doc.settingLayout) {
            case SettingLayout::KeyEnvironmentFlagDefault:
                text += "| " + backticked(row.key) + " | " + backticked(row.environmentVariable) + " | "
                        + backticked(row.flag) + " | *(empty)* |\n";
                break;
            case SettingLayout::KeyFlagEnvironment:
                text += "| " + backticked(row.key) + " | " + backticked(row.flag) + " | "
                        + backticked(row.environmentVariable) + " |\n";
                break;
            }
        }
    }
    text += "\nRemote access without TLS transmits traffic unencrypted.\n";
    return text;
}

struct Rendered {
    std::string build;
    std::string tools;
    std::string remote;
};

[[nodiscard]] Rendered render(const DocSet& doc) {
    return Rendered{renderBuildDoc(doc), renderToolsDoc(doc), renderRemoteDoc(doc)};
}

/// The observation set a generated document's result fields are checked against:
/// per tool, whatever that tool's model says a real payload carried.
[[nodiscard]] std::vector<ObservedResult> observationsOf(const DocSet& doc) {
    std::vector<ObservedResult> observed;
    for (const ToolModel& tool : doc.tools) {
        observed.push_back(ObservedResult{tool.name, tool.observedFields});
    }
    return observed;
}

[[nodiscard]] Checked check(const DocSet& doc) {
    const Rendered text = render(doc);
    return checkDocuments(text.build, text.tools, text.remote, observationsOf(doc));
}

// --- model queries used by the readback and by the mutations -----------------

/// The `###` section the extractor will report a row under: its group's heading, or
/// the region's default when the first group carries none.
[[nodiscard]] std::string sectionOf(const DocSet& doc, std::size_t group) {
    return doc.optionGroups[group].heading.empty() ? std::string(kOptionsSection)
                                                   : doc.optionGroups[group].heading;
}

[[nodiscard]] const DocumentedOption* findOption(const Checked& checked, const std::string& name) {
    for (const DocumentedOption& option : checked.options) {
        if (option.name == name) {
            return &option;
        }
    }
    return nullptr;
}

[[nodiscard]] const DocumentedTool* findTool(const Checked& checked, const std::string& name) {
    for (const DocumentedTool& tool : checked.tools) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

[[nodiscard]] const DocumentedArgument* findArgument(const DocumentedTool& tool,
                                                     const std::string& name) {
    for (const DocumentedArgument& argument : tool.arguments) {
        if (argument.name == name) {
            return &argument;
        }
    }
    return nullptr;
}

[[nodiscard]] const DocumentedSetting* findSetting(const Checked& checked, const std::string& key) {
    for (const DocumentedSetting& setting : checked.settings) {
        if (setting.key == key) {
            return &setting;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t modelOptionCount(const DocSet& doc) {
    std::size_t count = 0;
    for (const OptionGroup& group : doc.optionGroups) {
        count += group.rows.size();
    }
    return count;
}

[[nodiscard]] std::size_t modelSettingCount(const DocSet& doc) {
    std::size_t count = 0;
    for (const SettingGroup& group : doc.settingGroups) {
        count += group.rows.size();
    }
    return count;
}

[[nodiscard]] bool hasDefect(const std::vector<DocDefect>& defects,
                             DocDefectKind kind,
                             std::string_view name,
                             std::string_view document,
                             std::string_view section) {
    return std::any_of(defects.begin(), defects.end(), [&](const DocDefect& defect) {
        return defect.kind == kind && defect.name == name && defect.document == document
               && defect.section == section;
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
[[nodiscard]] std::size_t drawIndex(std::size_t bound) {
    return *rc::gen::resize(rc::kNominalSize, rc::gen::inRange<std::size_t>(0, bound));
}

[[nodiscard]] bool drawFlag() { return drawIndex(2) == 1; }

const std::vector<std::string>& optionSectionPool() {
    static const std::vector<std::string> pool{"User-settable options",
                                               "Derived, read-only entries",
                                               "Hardware codec options",
                                               "Test and warning options"};
    return pool;
}

const std::vector<std::string>& settingSectionPool() {
    static const std::vector<std::string> pool{"Every configuration input",
                                               "Remote-access inputs",
                                               "Endpoint and agent inputs"};
    return pool;
}

/// Synthetic result-field names. Deliberately none of `status`, `noOp` or
/// `indication`: those three are what the *(command result)* marker stands for, and
/// a collision would make a documented field's conditionality ambiguous.
const std::vector<std::string>& resultFieldPool() {
    static const std::vector<std::string> pool{"alphaField", "betaField",  "gammaField",
                                               "deltaField", "epsilonField", "zetaField"};
    return pool;
}

/// Split `count` items into `groups` contiguous, non-empty runs. Returns the size of
/// each run.
[[nodiscard]] std::vector<std::size_t> splitSizes(std::size_t count, std::size_t groups) {
    std::vector<std::size_t> sizes(groups, count / groups);
    for (std::size_t i = 0; i < count % groups; ++i) {
        sizes[i] += 1;
    }
    return sizes;
}

/// The option groups of a document: the live options, in manifest order, split into
/// `groups` `###` sections. The first section's heading may be absent, which is the
/// one case the extractor reports under the region's default section name.
[[nodiscard]] std::vector<OptionGroup> buildOptionGroups(std::size_t groups,
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

[[nodiscard]] std::vector<SettingGroup> buildSettingGroups(std::size_t groups,
                                                           std::size_t headingOffset) {
    const std::vector<LiveSetting>& settings = live().settings;
    const std::vector<std::size_t> sizes = splitSizes(settings.size(), groups);

    std::vector<SettingGroup> built;
    std::size_t cursor = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        SettingGroup group;
        group.heading = settingSectionPool()[(headingOffset + g) % settingSectionPool().size()];
        for (std::size_t i = 0; i < sizes[g]; ++i, ++cursor) {
            group.rows.push_back(SettingRow{settings[cursor].key,
                                            settings[cursor].environmentVariable,
                                            settings[cursor].flag});
        }
        built.push_back(std::move(group));
    }
    return built;
}

/// One tool section's model. `unconditional` and `conditional` are counts, and
/// `observeConditional` decides how many of the conditional fields a payload
/// happened to carry — the rest are documented-but-unobserved, which is legal
/// precisely because they are marked conditional.
[[nodiscard]] ToolModel buildTool(const LiveTool& liveTool,
                                  ArgumentLayout layout,
                                  bool resultTable,
                                  bool commandResult,
                                  std::size_t unconditional,
                                  std::size_t conditional,
                                  std::size_t observeConditional,
                                  bool boldRequired,
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
[[nodiscard]] DocSet drawDocSet() {
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
    if (std::find(doc.tools[2].observedFields.begin(), doc.tools[2].observedFields.end(), "status")
        == doc.tools[2].observedFields.end()) {
        doc.tools[2].observedFields.push_back("status");
    }
    // A conditional field no payload carried: legal, and Property 77's target for
    // the "promote it to unconditional" mutation.
    ToolModel& withUnobserved = doc.tools[3];
    if (withUnobserved.conditionalResultFields.empty()) {
        withUnobserved.conditionalResultFields.push_back(resultFieldPool().back());
    }
    const std::string& unobserved = withUnobserved.conditionalResultFields.back();
    withUnobserved.observedFields.erase(std::remove(withUnobserved.observedFields.begin(),
                                                    withUnobserved.observedFields.end(),
                                                    unobserved),
                                        withUnobserved.observedFields.end());
    return doc;
}

/// The same shape with nothing drawn, for the exhaustive mutation-coverage test:
/// the layouts and result shapes cycle so all of them appear, and the same four
/// guarantees hold.
[[nodiscard]] DocSet fixedDocSet() {
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
    withUnobserved.observedFields.erase(std::remove(withUnobserved.observedFields.begin(),
                                                    withUnobserved.observedFields.end(),
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

constexpr std::array<Mutation, 29> kMutations{
    Mutation::RenameAnOption,
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

[[nodiscard]] std::string_view describe(Mutation kind) {
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
    bool        applicable{true};
    DocSet      doc;
    Rendered    text;
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

[[nodiscard]] OptionTarget optionTarget(const DocSet& doc, std::size_t choice) {
    std::vector<OptionTarget> candidates;
    for (std::size_t g = 0; g < doc.optionGroups.size(); ++g) {
        for (std::size_t r = 0; r < doc.optionGroups[g].rows.size(); ++r) {
            candidates.push_back(OptionTarget{g, r});
        }
    }
    return candidates[choice % candidates.size()];
}

struct SettingTarget {
    std::size_t group{0};
    std::size_t row{0};
};

[[nodiscard]] SettingTarget settingTarget(const DocSet& doc, std::size_t choice) {
    std::vector<SettingTarget> candidates;
    for (std::size_t g = 0; g < doc.settingGroups.size(); ++g) {
        for (std::size_t r = 0; r < doc.settingGroups[g].rows.size(); ++r) {
            candidates.push_back(SettingTarget{g, r});
        }
    }
    return candidates[choice % candidates.size()];
}

template <typename Predicate>
[[nodiscard]] std::vector<std::size_t> toolsWhere(const DocSet& doc, Predicate predicate) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < doc.tools.size(); ++i) {
        if (predicate(doc.tools[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

/// A JSON type that is not `type`, so a type mutation always changes something.
[[nodiscard]] std::string otherJsonType(const std::string& type) {
    static const std::array<std::string_view, 6> kTypes{"string",  "integer", "number",
                                                        "boolean", "array",   "object"};
    for (const std::string_view candidate : kTypes) {
        if (candidate != type) {
            return std::string(candidate);
        }
    }
    return "string";
}

[[nodiscard]] std::string otherCacheType(const std::string& type) {
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
[[nodiscard]] Mutant mutate(const DocSet& base, Mutation kind, std::size_t choice) {
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
        doc.tools.insert(doc.tools.begin()
                             + static_cast<std::ptrdiff_t>(choice % (doc.tools.size() + 1)),
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
        doc.tools[i].arguments.push_back(ArgumentModel{name, "string", false, false, true, false});
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
        const std::size_t i = pickTool(toolsWhere(
            doc, [](const ToolModel& tool) { return tool.arguments.empty(); }));
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
        const std::size_t i = pickTool(toolsWhere(
            doc, [](const ToolModel& tool) { return !tool.resultFields.empty(); }));
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
            return std::any_of(tool.conditionalResultFields.begin(),
                               tool.conditionalResultFields.end(),
                               [&tool](const std::string& field) {
                                   return std::find(tool.observedFields.begin(),
                                                    tool.observedFields.end(), field)
                                          == tool.observedFields.end();
                               });
        }));
        if (!mutant.applicable) {
            break;
        }
        ToolModel& tool = doc.tools[i];
        std::vector<std::size_t> unobserved;
        for (std::size_t f = 0; f < tool.conditionalResultFields.size(); ++f) {
            if (std::find(tool.observedFields.begin(), tool.observedFields.end(),
                          tool.conditionalResultFields[f])
                == tool.observedFields.end()) {
                unobserved.push_back(f);
            }
        }
        const std::size_t f = unobserved[choice % unobserved.size()];
        const std::string field = tool.conditionalResultFields[f];
        tool.conditionalResultFields.erase(tool.conditionalResultFields.begin()
                                           + static_cast<std::ptrdiff_t>(f));
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
        static const std::array<std::string_view, 2> kKeys{"remote.telepathy", "agent.holograms"};
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
    std::string   path;
    std::size_t   bytes{0};
    std::uint64_t hash{0};

    [[nodiscard]] bool operator==(const DocumentFingerprint& other) const {
        return path == other.path && bytes == other.bytes && hash == other.hash;
    }
};

/// FNV-1a over the file's bytes. A hash plus the byte count, so neither a length
/// change nor an in-place edit of the same length can go unnoticed.
[[nodiscard]] DocumentFingerprint fingerprintOf(const std::filesystem::path& path) {
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
[[nodiscard]] std::vector<DocumentFingerprint> fingerprintTheDocumentation() {
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

// ===========================================================================
// Full mutation-kind coverage on every run, independent of sampling
// ===========================================================================

TEST(DocumentationCheckMutations, EveryMutationKindIsDetectedAndNamesItsSection) {
    const DocSet  base = fixedDocSet();
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
            const Checked mutated =
                checkDocuments(mutant.text.build, mutant.text.tools, mutant.text.remote,
                               observationsOf(mutant.doc));
            EXPECT_TRUE(hasDefect(mutated.defects, mutant.expected, mutant.name, mutant.document,
                                  mutant.section))
                << describe(kind) << " (choice " << choice << ", name `" << mutant.name
                << "`, section `" << mutant.section << "`) was not reported:\n"
                << testsupport::toString(mutated.defects);
            if (!mutant.structural) {
                // Still a readable document set, so the defect is the injected one.
                EXPECT_EQ(mutated.options.size(), modelOptionCount(mutant.doc)) << describe(kind);
                EXPECT_EQ(mutated.tools.size(), mutant.doc.tools.size()) << describe(kind);
                EXPECT_EQ(mutated.settings.size(), modelSettingCount(mutant.doc))
                    << describe(kind);
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
