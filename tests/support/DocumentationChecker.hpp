// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/DocumentationChecker.hpp — the documentation consistency checker
// (task 12.7; Requirements 16.7, 16.8).
//
// Requirement 16.7, in full:
//
//   "WHEN the Verification_Suite runs, THE Verification_Suite SHALL compare the
//    CMake option names stated in the documentation against the options defined
//    by the build system, and the tool names and argument names stated in the
//    documentation against the names the Tool_Surface returns from `tools/list`."
//
// Requirement 16.8 adds that a mismatch must FAIL, must be reported with "the
// documentation section in which it appears", and must leave the documentation
// unmodified.
//
// Shape of the solution, following the design's ReportParser decision ("the
// documentation consistency checker reuses the same parser style"): a
// dependency-free extractor turns each checked-in Markdown document into value
// types, and the checkers are pure functions from (documented, live) to a
// `std::vector<DocDefect>`. Nothing here opens a file for writing, and nothing
// here holds a mutable reference to a document — Requirement 16.8's "leave the
// documentation unmodified" is a property of the type signatures, not a promise.
//
// WHERE "LIVE" COMES FROM. Every expectation is derived from the running system
// or from the source of record, never from a list restated in the test:
//
//   * CMake options   — `palmier_options.txt`, written at configure time by
//                       cmake/PalmierOptionsManifest.cmake from
//                       `get_cmake_property(... CACHE_VARIABLES)` filtered to
//                       `PALMIER_*`. A test binary cannot ask CMake anything at
//                       run time, so configure time is the only moment the set is
//                       knowable.
//   * tools/arguments — `buildDefaultToolRegistry(...)`, i.e. the same
//                       `ToolRegistry::describe()` payload `tools/list` publishes,
//                       plus each tool's `ToolSchema`.
//   * result fields   — the field names the REAL handlers put in their success
//                       payloads, observed by invoking them.
//   * settings keys   — `AppSettings::recognizedKeys()` and its
//                       environment-variable / flag accessors, which read the one
//                       key table in src/app/AppSettings.cpp. (`AppSettings::usage()`
//                       is generated from that table and so cannot drift from it;
//                       the documentation is the artefact that can.)
//
// A duplicated list inside the test would make every property below true of the
// list rather than of the system, so the only names this header knows are the
// names of the DOCUMENTS and of the sections inside them.

#ifndef PALMIER_TESTS_SUPPORT_DOCUMENTATIONCHECKER_HPP
#define PALMIER_TESTS_SUPPORT_DOCUMENTATIONCHECKER_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::testsupport {

// ===========================================================================
// Defects
// ===========================================================================

enum class DocDefectKind {
    UndocumentedName,       ///< The system has the name; the documentation does not.
    UnknownDocumentedName,  ///< The documentation has the name; the system does not.
    TypeMismatch,           ///< Documented and actual type disagree.
    RequiredMismatch,       ///< Documented and actual required/optional marking disagree.
    OrderMismatch,          ///< Documented order differs from the published order.
    SectionMissing,         ///< A section the extraction contract requires is absent.
    InputUnreadable         ///< A document or the manifest could not be read.
};

[[nodiscard]] std::string_view describe(DocDefectKind kind);

/// One mismatch. `document` and `section` together are Requirement 16.8's
/// "documentation section in which it appears"; `name` is the mismatched name.
struct DocDefect {
    DocDefectKind kind{DocDefectKind::UndocumentedName};
    std::string   name;
    std::string   document;
    std::string   section;
    std::string   detail;

    [[nodiscard]] std::string toString() const;
};

/// Every defect on its own line, for a failure message.
[[nodiscard]] std::string toString(const std::vector<DocDefect>& defects);

// ===========================================================================
// The documented surface, as extracted from Markdown
// ===========================================================================

/// A `PALMIER_*` cache option as docs/BUILD.md states it.
struct DocumentedOption {
    std::string name;     ///< e.g. "PALMIER_ENABLE_NVENC"
    std::string type;     ///< the Type column: BOOL, STRING, INTERNAL, ...
    std::string section;  ///< the `###` heading the row sits under
};

/// One argument of one tool as docs/TOOLS.md states it.
struct DocumentedArgument {
    std::string name;
    std::string type;            ///< "string", "integer", "number", "boolean", "array", "object"
    bool        required{false};  ///< the Required column reads "yes"
    bool        uuid{false};      ///< the Type column carries the *uuid* marker
};

/// One tool section of docs/TOOLS.md.
struct DocumentedTool {
    std::string name;
    bool        declaresNoArguments{false};  ///< the section says "No arguments."
    bool        commandResult{false};        ///< the section carries *(command result)*
    std::vector<DocumentedArgument> arguments;
    /// Result fields the section states are always present.
    std::vector<std::string> resultFields;
    /// Result fields the section states are present only under a stated condition.
    std::vector<std::string> conditionalResultFields;

    /// Every documented result field, conditional or not.
    [[nodiscard]] std::vector<std::string> allResultFields() const;
};

/// One configuration input as docs/REMOTE_ACCESS.md states it.
struct DocumentedSetting {
    std::string key;                  ///< config-file key, e.g. "remote.port"
    std::string environmentVariable;  ///< e.g. "PALMIER_REMOTE_PORT"
    std::string flag;                 ///< e.g. "--remote-port"
    std::string section;
};

// ===========================================================================
// The live surface, as read out of the running system
// ===========================================================================

struct LiveOption {
    std::string name;
    std::string type;
};

struct LiveArgument {
    std::string name;
    std::string type;   ///< `services::jsonKindName(kind)`
    bool        required{false};
    bool        uuid{false};
};

struct LiveTool {
    std::string name;
    std::vector<LiveArgument> arguments;
};

struct LiveSetting {
    std::string key;
    std::string environmentVariable;
    std::string flag;
};

/// The field names one real invocation of `toolName` put in its success payload.
/// Several entries may name the same tool: a tool is invoked in more than one
/// scenario precisely so its conditional fields are seen at least once.
struct ObservedResult {
    std::string toolName;
    std::vector<std::string> fields;
};

// ===========================================================================
// Extraction (pure, total: a malformed document yields defects, never an
// exception, so the checker cannot pass by failing to parse)
// ===========================================================================

/// docs/BUILD.md's option tables, taken ONLY from between the
/// `<!-- palmier-options:begin -->` / `:end` markers, as that file's own
/// "Extraction contract" section requires. A missing marker appends a
/// `SectionMissing` defect and yields no options.
[[nodiscard]] std::vector<DocumentedOption> extractDocumentedOptions(
    std::string_view markdown, std::vector<DocDefect>& defects);

/// docs/TOOLS.md's per-tool sections. See the file's "Extraction contract"
/// section for the exact grammar this implements.
[[nodiscard]] std::vector<DocumentedTool> extractDocumentedTools(
    std::string_view markdown, std::vector<DocDefect>& defects);

/// docs/REMOTE_ACCESS.md's configuration-input table (the one whose first header
/// cell is "Config-file key").
[[nodiscard]] std::vector<DocumentedSetting> extractDocumentedSettings(
    std::string_view markdown, std::vector<DocDefect>& defects);

/// `palmier_options.txt`'s `NAME|TYPE` records.
[[nodiscard]] std::vector<LiveOption> parseOptionsManifest(
    std::string_view manifest, std::vector<DocDefect>& defects);

/// The field names a named function passes to `Json::set("...")`, for the two
/// tools whose success payload is rendered by a collaborator the tool surface only
/// holds a hook for. `anchor` is a substring of the function's signature; the body
/// is taken by brace matching from the first `{` after it. `found` is false when
/// the anchor is absent — which the caller must report, so that a renamed function
/// fails loudly instead of yielding an empty, vacuously-consistent set.
struct FunctionFieldScan {
    bool found{false};
    std::vector<std::string> fields;
};
[[nodiscard]] FunctionFieldScan scanJsonSetFields(std::string_view source, std::string_view anchor);

// ===========================================================================
// The checks
// ===========================================================================

/// Two-way on names, plus the Type column (which is also what distinguishes
/// docs/BUILD.md's user-settable table from its derived `INTERNAL` table).
[[nodiscard]] std::vector<DocDefect> checkOptions(const std::vector<DocumentedOption>& documented,
                                                 const std::vector<LiveOption>& live);

/// Two-way on tool names and, per tool, two-way on argument names plus each
/// argument's JSON type and required/optional marking. Also checks that the
/// documented tool order is the order `tools/list` publishes, which docs/TOOLS.md
/// claims in prose.
[[nodiscard]] std::vector<DocDefect> checkTools(const std::vector<DocumentedTool>& documented,
                                               const std::vector<LiveTool>& live);

/// Two-way on config-file keys and, per key, on the environment variable and the
/// command-line flag that name it.
[[nodiscard]] std::vector<DocDefect> checkSettings(const std::vector<DocumentedSetting>& documented,
                                                  const std::vector<LiveSetting>& live);

/// Every observed result field must be documented, and every result field
/// documented as unconditional must be observed at least once. A tool with no
/// observation contributes no defect — the caller is responsible for saying which
/// tools it could not invoke and why.
[[nodiscard]] std::vector<DocDefect> checkResultFields(
    const std::vector<DocumentedTool>& documented,
    const std::vector<ObservedResult>& observed);

/// A bare two-way name-set comparison, for the surfaces whose live side is a set
/// of names rather than a richer value.
[[nodiscard]] std::vector<DocDefect> checkNameSets(const std::vector<std::string>& documented,
                                                  const std::vector<std::string>& live,
                                                  std::string_view document,
                                                  std::string_view section);

// ===========================================================================
// Reading the documents
// ===========================================================================

/// The whole file, or an empty string when it cannot be read (which the callers
/// turn into an `InputUnreadable` defect rather than an empty pass).
[[nodiscard]] std::string readWholeFile(const std::string& path);

}  // namespace palmier::testsupport

#endif  // PALMIER_TESTS_SUPPORT_DOCUMENTATIONCHECKER_HPP
