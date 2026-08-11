// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/ReportParser.hpp — the Parity_Report and Port_Backlog parser and
// the two well-formedness checkers (task 12.3; Requirements 13.8, 14.11).
//
// Requirement 13.8, in full:
//
//   "IF the Parity_Report omits a required entry, duplicates an entry, carries a
//    status or priority value outside the defined value sets, or carries a missing
//    or empty rationale where criterion 3 requires one, THEN THE
//    Verification_Suite parity check SHALL fail and SHALL report each offending
//    entry by category or capability-area name and the specific defect."
//
// Requirement 14.11, in full:
//
//   "IF an entry omits its upstream identifier, one-line summary, disposition or
//    rationale, or omits an acceptance check while dispositioned `port` or
//    `adapt`, THEN THE Port_Backlog SHALL be reported as invalid, naming the
//    offending entry and each missing field, and SHALL not present that entry as
//    ported."
//
// Both are Verification_Suite obligations rather than CI scripts, so the checks
// live here, in the suite, as pure functions — the same code backs the CI gate and
// the property tests of tasks 12.4 and 12.5, so a document cannot pass in one
// place and fail in the other (design.md, "How the well-formedness checks are
// implemented").
//
// Shape of the solution, following `tests/support/DocumentationChecker.hpp` (task
// 12.7), which design.md names as the same parser style:
//
//   * DEPENDENCY-FREE. Standard library only. No YAML, no JSON, no regex engine
//     and nothing from `Palmier::`; the grammar is a fixed 7-column Markdown table
//     plus flat `key: value` blocks, and hand-rolling it is the same decision the
//     tree already made for `services::Json`.
//
//   * TOTAL. A malformed document yields DEFECTS, never an exception. Every
//     `parse*` function takes the defect vector by reference and appends to it, so
//     a document the parser cannot understand is reported rather than thrown over
//     — and, critically, is never reported as "nothing to check". Both parsers
//     append a defect when they find no entries at all, and both checkers append a
//     defect when handed an empty report, because a parser that silently returned
//     nothing would let `checkX(parse(doc)).empty()` pass vacuously. That is the
//     one failure mode a document check cannot afford, and
//     `tests/docs/report_parser_test.cpp` asserts it directly.
//
//   * PURE. Every function takes `std::string_view` or const references and
//     returns values. Nothing in this translation unit opens a path for writing,
//     so "the checker never edits the document" is a property of the signatures
//     rather than a promise.
//
// WHERE THE EXPECTED NAMES COME FROM. The 22 tool categories, the 12 capability
// areas and the ten upstream identifiers are stated by Requirements 13.1, 13.2 and
// 14.2 — the requirements document is their source of record, and a checker cannot
// derive them from the document it is checking without checking nothing. They are
// therefore transcribed once, into `requiredToolCategories()`,
// `requiredCapabilityAreas()` and `requiredBacklogIdentifiers()`, and
// `tests/docs/report_parser_test.cpp` cross-checks all three against the
// parenthesised lists in `requirements.md` itself, so a transcription slip fails
// the suite instead of silently narrowing the check.
//
// THE GRAMMAR, stated so tasks 12.4 and 12.5 can generate documents to it.
//
// Parity_Report (docs/UPSTREAM_PARITY.md):
//
//   - a provenance bullet per key:  `- upstream-repository: <text>`
//     `- upstream-ref:`, `- linux-ref:`, `- comparison-date: YYYY-MM-DD`
//     (a bullet's value may wrap onto following indented lines)
//   - two 7-column tables, recognised by their FIRST HEADER CELL — `category` for
//     the tool-category table, `area` for the capability-area table — with columns
//     in the order: name, status, linux-components, priority, rationale,
//     macos-framework, linux-replacement. `-` means "no value".
//   - a heading whose text begins `Build order`, under which every `N. ` line is a
//     build-order item written `<name> (<tool category|capability area>) — <priority>`.
//
// Port_Backlog (docs/PORT_BACKLOG.md):
//
//   - a provenance bullet per key: `- upstream-repository:`, `- upstream-range:`,
//     `- window: YYYY-MM-DD..YYYY-MM-DD`
//   - one entry per `### ` heading, holding flat `key: value` lines drawn from
//     `identifier`, `summary`, `disposition`, `linux-component`, `rationale`,
//     `check`, `status`, `note`; a `check:` line is followed by indented `given:`,
//     `when:` and `then:` lines. A value may wrap onto following indented lines.
//     Table rows are ignored, so the document's own field-rules table is not an
//     entry.
//
// Any line outside those shapes is ignored, which is what lets both documents
// carry the prose a human reader needs.
//
// _Requirements: 13.8, 14.11_

#ifndef PALMIER_TESTS_SUPPORT_REPORTPARSER_HPP
#define PALMIER_TESTS_SUPPORT_REPORTPARSER_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::testsupport {

// ===========================================================================
// Defects
// ===========================================================================

/// The defect vocabulary of task 12.3. It is deliberately closed: a new kind of
/// malformation is expressed as an existing kind plus a `detail`, so the set the
/// property tests of 12.4 and 12.5 assert over cannot drift.
enum class DefectKind {
    /// The entry set does not match the set the requirements fix: a required
    /// entry is absent, or an entry that is not in the required set is present.
    /// Also used for the build-order projection's membership (Requirement 13.9).
    MissingEntry,
    /// The same entry appears more than once where exactly one is required.
    DuplicateEntry,
    /// A `status` or a `disposition` value outside its defined value set.
    InvalidStatus,
    /// A `priority` outside its value set, present where none is permitted,
    /// absent where one is required, or disagreeing with the build-order list.
    InvalidPriority,
    /// A rationale missing, empty, out of its length bounds, not a sentence, or
    /// present where the document's field rules permit none.
    MissingRationale,
    /// An acceptance check missing a leg or missing entirely while the entry is
    /// dispositioned `port`/`adapt` — or present on a `not-applicable` entry,
    /// which Requirement 14.3 gives no check.
    MissingCheck,
    /// A required field is absent, empty, or malformed in a way that is not one
    /// of the more specific kinds above (a provenance key, a summary over its
    /// length bound, a column count, a date format).
    MissingField,
    /// Two Port_Backlog entries share an upstream identifier (Requirement 14.10).
    DuplicateIdentifier,
    /// The build-order list is not sorted `must` before `should` before `later`
    /// (Requirement 13.9).
    OutOfOrder
};

[[nodiscard]] std::string_view describe(DefectKind kind);

/// One defect. `entry` is Requirement 13.8's "offending entry by category or
/// capability-area name" and Requirement 14.11's "offending entry"; `detail` is
/// "the specific defect" / "each missing field".
struct Defect {
    DefectKind  kind{DefectKind::MissingField};
    std::string entry;
    std::string document;
    std::string section;
    std::string detail;
    std::size_t line{0};  ///< 1-based source line, or 0 when not tied to a line.

    [[nodiscard]] std::string toString() const;
};

/// Every defect on its own line, for a failure message.
[[nodiscard]] std::string toString(const std::vector<Defect>& defects);

/// True when some defect has `kind` and an `entry` containing `entrySubstring`
/// (an empty substring matches any entry). For assertions and for 12.4/12.5.
[[nodiscard]] bool hasDefect(const std::vector<Defect>& defects,
                             DefectKind kind,
                             std::string_view entrySubstring = {});

[[nodiscard]] std::size_t countDefects(const std::vector<Defect>& defects, DefectKind kind);

// ===========================================================================
// The Parity_Report as values
// ===========================================================================

/// Requirement 13.1's table and Requirement 13.2's table. An entry is identified
/// by the PAIR (table, name): `multicam` is both a tool category and a capability
/// area, and both rows are required.
enum class ParityTable { ToolCategory, CapabilityArea };

[[nodiscard]] std::string_view describe(ParityTable table);

/// One row of either table, with every cell kept as written (minus surrounding
/// whitespace and backticks) so the checker judges the document rather than a
/// normalisation of it. `-` and the empty cell both mean "no value" and are
/// stored as the empty string.
struct ParityEntry {
    ParityTable table{ParityTable::ToolCategory};
    std::string name;
    std::string status;            ///< `present` | `partial` | `absent`
    std::string linuxComponents;   ///< component names, or `none`
    std::string priority;          ///< `must` | `should` | `later`, or empty
    std::string rationale;         ///< 1-200 characters, or empty
    std::string macosFramework;    ///< framework name, or empty
    std::string linuxReplacement;  ///< technology name | `out-of-scope: <reason>` | empty
    std::size_t line{0};

    /// Requirement 13.3/13.6: a priority and a rationale are required if and only
    /// if the status is `absent` or `partial`.
    [[nodiscard]] bool requiresPriority() const;
};

/// One `N. ` line of the build-order projection (Requirement 13.9).
struct BuildOrderItem {
    std::string name;
    ParityTable table{ParityTable::ToolCategory};
    bool        tableRecognised{false};  ///< the `(...)` label named a known table
    std::string priority;
    std::size_t line{0};
};

/// Requirement 13.4's provenance block.
struct ParityProvenance {
    std::string upstreamRepository;
    std::string upstreamRef;
    std::string linuxRef;
    std::string comparisonDate;
};

struct ParityReport {
    ParityProvenance            provenance;
    std::vector<ParityEntry>    entries;
    std::vector<BuildOrderItem> buildOrder;
    bool categoryTableFound{false};
    bool areaTableFound{false};
    bool buildOrderSectionFound{false};

    /// Every entry of one table, in document order.
    [[nodiscard]] std::vector<const ParityEntry*> entriesIn(ParityTable table) const;
};

// ===========================================================================
// The Port_Backlog as values
// ===========================================================================

/// Requirement 14.3's acceptance check: one observable starting state, one
/// action, one observable outcome.
struct AcceptanceCheck {
    bool        declared{false};  ///< a `check:` line was seen
    std::string given;
    std::string when;
    std::string then;

    [[nodiscard]] bool complete() const;
};

struct BacklogEntry {
    std::string     heading;  ///< the `### ` line, for reporting
    std::string     identifier;
    std::string     summary;
    std::string     disposition;    ///< `port` | `adapt` | `not-applicable`
    std::string     linuxComponent;
    std::string     rationale;
    std::string     status;  ///< `not-started` | `in-progress` | `complete`
    std::string     note;
    AcceptanceCheck check;
    std::size_t     line{0};

    /// Requirement 14.3: a check is required if and only if the disposition is
    /// `port` or `adapt`.
    [[nodiscard]] bool requiresCheck() const;

    /// The name to report this entry by: its identifier, or its heading when the
    /// identifier is the very field that is missing.
    [[nodiscard]] std::string reportingName() const;
};

/// Requirement 14.1's provenance.
struct BacklogProvenance {
    std::string upstreamRepository;
    std::string upstreamRange;
    std::string window;  ///< `YYYY-MM-DD..YYYY-MM-DD`
};

struct PortBacklog {
    BacklogProvenance         provenance;
    std::vector<BacklogEntry> entries;
};

// ===========================================================================
// Parsing (total: a malformed document yields defects, never an exception)
// ===========================================================================

[[nodiscard]] ParityReport parseParityReport(std::string_view markdown,
                                            std::vector<Defect>& defects);

[[nodiscard]] PortBacklog parsePortBacklog(std::string_view markdown,
                                           std::vector<Defect>& defects);

// ===========================================================================
// The checks (pure functions over the parsed values)
// ===========================================================================

/// Requirements 13.1-13.6 and 13.9: entry membership and uniqueness per table,
/// the status value set, the priority value set and its present-if-and-only-if
/// rule, the rationale and its 1-200 character bound, the macOS-framework /
/// Linux-replacement pairing of Requirement 13.5, the provenance block of
/// Requirement 13.4, and the build-order projection of Requirement 13.9
/// (membership, per-item priority agreement, and `must` < `should` < `later`).
[[nodiscard]] std::vector<Defect> checkParityReport(const ParityReport& report);

/// Requirements 14.1-14.3 and 14.9-14.11: the provenance block and its window,
/// the presence of every identifier Requirement 14.2 names, per entry the
/// required fields and their bounds, the disposition and status value sets,
/// identifier uniqueness, and the acceptance check required by a `port`/`adapt`
/// disposition (and forbidden on a `not-applicable` one).
[[nodiscard]] std::vector<Defect> checkPortBacklog(const PortBacklog& backlog);

// ===========================================================================
// The fixed name sets (Requirements 13.1, 13.2, 14.2)
// ===========================================================================

[[nodiscard]] const std::vector<std::string>& requiredToolCategories();
[[nodiscard]] const std::vector<std::string>& requiredCapabilityAreas();
[[nodiscard]] const std::vector<std::string>& requiredBacklogIdentifiers();

[[nodiscard]] const std::vector<std::string>& parityStatusValues();
[[nodiscard]] const std::vector<std::string>& parityPriorityValues();
[[nodiscard]] const std::vector<std::string>& backlogDispositionValues();
[[nodiscard]] const std::vector<std::string>& backlogStatusValues();

/// The document names the defects are reported against.
[[nodiscard]] std::string_view parityDocumentName();
[[nodiscard]] std::string_view backlogDocumentName();

// ===========================================================================
// Reading a document
// ===========================================================================

/// The whole file, or an empty string when it cannot be read. An empty string is
/// itself a malformed document, so it produces defects rather than a pass.
[[nodiscard]] std::string readReportDocument(const std::string& path);

}  // namespace palmier::testsupport

#endif  // PALMIER_TESTS_SUPPORT_REPORTPARSER_HPP
