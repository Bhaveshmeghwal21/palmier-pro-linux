// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/ReportParser.cpp — the parsing and the checks declared in
// ReportParser.hpp (task 12.3; Requirements 13.8, 14.11).
//
// Three rules govern everything below; the header states why.
//
// 1. TOTALITY. No function throws on a malformed document and no function returns
//    an empty result for a document it failed to understand. Every "I could not
//    find this" is a `Defect`. A missing table, a wrong column count, a document
//    with no entries at all, an empty file — each produces a defect, so
//    `checkX(parse(doc)).empty()` can never be true by accident.
//
// 2. PURITY. Inputs by `std::string_view` or const reference, outputs by value.
//    No path is opened for writing in this translation unit.
//
// 3. NO DEPENDENCIES. `<string>`, `<vector>`, `<algorithm>`, `<fstream>` and
//    friends only — no YAML, no regex engine, nothing from `Palmier::`.

#include "support/ReportParser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace palmier::testsupport {

namespace {

// ---------------------------------------------------------------------------
// Small text utilities (deliberately the same shapes DocumentationChecker.cpp
// uses; the two files are separate translation units in one target, and each is
// meant to be readable on its own)
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (std::isspace(static_cast<unsigned char>(text[begin])) != 0)) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)) {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] std::vector<std::string> splitLines(std::string_view content) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        std::string_view piece = end == std::string_view::npos ? content.substr(start)
                                                              : content.substr(start, end - start);
        if (!piece.empty() && piece.back() == '\r') {
            piece.remove_suffix(1);
        }
        lines.emplace_back(piece);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] std::string toLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

/// A cell or field value as the checker judges it: trimmed, with the Markdown
/// emphasis a human author may have added stripped, and with `-` — the documents'
/// spelling of "no value" — collapsed to the empty string.
[[nodiscard]] std::string normalizeValue(std::string_view raw) {
    std::string_view text = trim(raw);
    while (text.size() >= 2 && text.front() == '`' && text.back() == '`') {
        text.remove_prefix(1);
        text.remove_suffix(1);
        text = trim(text);
    }
    while (text.size() >= 4 && text.substr(0, 2) == "**" && text.substr(text.size() - 2) == "**") {
        text.remove_prefix(2);
        text.remove_suffix(2);
        text = trim(text);
    }
    if (text == "-" || text == "\u2014") {
        return {};
    }
    return std::string(text);
}

[[nodiscard]] bool isTableRow(std::string_view line) {
    const std::string_view trimmed = trim(line);
    return !trimmed.empty() && trimmed.front() == '|';
}

[[nodiscard]] std::vector<std::string> tableCells(std::string_view line) {
    std::string_view trimmed = trim(line);
    if (!trimmed.empty() && trimmed.front() == '|') {
        trimmed.remove_prefix(1);
    }
    if (!trimmed.empty() && trimmed.back() == '|') {
        trimmed.remove_suffix(1);
    }
    std::vector<std::string> cells;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t pipe = trimmed.find('|', start);
        const std::string_view piece = pipe == std::string_view::npos
                                           ? trimmed.substr(start)
                                           : trimmed.substr(start, pipe - start);
        cells.emplace_back(trim(piece));
        if (pipe == std::string_view::npos) {
            break;
        }
        start = pipe + 1;
    }
    return cells;
}

[[nodiscard]] bool isSeparatorRow(const std::vector<std::string>& cells) {
    if (cells.empty()) {
        return false;
    }
    return std::all_of(cells.begin(), cells.end(), [](const std::string& cell) {
        return !cell.empty() && cell.find_first_not_of("-: ") == std::string::npos;
    });
}

/// The heading text of a `#`-prefixed line, or an empty optional-by-flag.
[[nodiscard]] bool headingText(std::string_view line, std::string& text, std::size_t& level) {
    const std::string_view trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() != '#') {
        return false;
    }
    std::size_t hashes = 0;
    while (hashes < trimmed.size() && trimmed[hashes] == '#') {
        ++hashes;
    }
    level = hashes;
    text = std::string(trim(trimmed.substr(hashes)));
    return true;
}

/// A `- key: value` bullet. `key` must be a lowercase dashed word, which is what
/// separates a provenance bullet from a prose bullet such as
/// `- **Completeness of the window is unverified.** ...`.
[[nodiscard]] bool bulletKeyValue(std::string_view line, std::string& key, std::string& value) {
    std::string_view trimmed = trim(line);
    if (trimmed.size() < 2 || (trimmed.front() != '-' && trimmed.front() != '*')
        || trimmed[1] != ' ') {
        return false;
    }
    trimmed.remove_prefix(2);
    const std::size_t colon = trimmed.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const std::string_view candidate = trimmed.substr(0, colon);
    const bool dashedWord = std::all_of(candidate.begin(), candidate.end(), [](const char c) {
        return (std::islower(static_cast<unsigned char>(c)) != 0) || c == '-';
    });
    if (!dashedWord) {
        return false;
    }
    key = std::string(candidate);
    value = std::string(trim(trimmed.substr(colon + 1)));
    return true;
}

/// A `key: value` line at the start of a line (no bullet), with `key` drawn from
/// `permitted`. Used for the Port_Backlog's flat entry blocks, so that a `key:`
/// the grammar does not define is treated as prose rather than as a field.
[[nodiscard]] bool fieldKeyValue(std::string_view line,
                                 const std::vector<std::string_view>& permitted,
                                 std::string& key,
                                 std::string& value) {
    const std::string_view trimmed = trim(line);
    const std::size_t colon = trimmed.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const std::string_view candidate = trimmed.substr(0, colon);
    if (std::find(permitted.begin(), permitted.end(), candidate) == permitted.end()) {
        return false;
    }
    key = std::string(candidate);
    value = std::string(trim(trimmed.substr(colon + 1)));
    return true;
}

/// `N. rest` — the build-order item shape. Returns false for anything else.
[[nodiscard]] bool numberedItem(std::string_view line, std::string& rest) {
    const std::string_view trimmed = trim(line);
    std::size_t digits = 0;
    while (digits < trimmed.size() && (std::isdigit(static_cast<unsigned char>(trimmed[digits])) != 0)) {
        ++digits;
    }
    if (digits == 0 || digits + 1 >= trimmed.size() || trimmed[digits] != '.'
        || trimmed[digits + 1] != ' ') {
        return false;
    }
    rest = std::string(trim(trimmed.substr(digits + 2)));
    return true;
}

[[nodiscard]] bool isDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](const char c) {
               return std::isdigit(static_cast<unsigned char>(c)) != 0;
           });
}

/// `YYYY-MM-DD`, checked as a shape rather than as a calendar date: the checker's
/// job is Requirement 13.4's "in `YYYY-MM-DD` form".
[[nodiscard]] bool isIsoDate(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    return isDigits(text.substr(0, 4)) && isDigits(text.substr(5, 2)) && isDigits(text.substr(8, 2));
}

/// `YYYY-MM-DD..YYYY-MM-DD`, Requirement 14.1's window.
[[nodiscard]] bool isIsoDateRange(std::string_view text) {
    const std::size_t dots = text.find("..");
    if (dots == std::string_view::npos) {
        return false;
    }
    return isIsoDate(text.substr(0, dots)) && isIsoDate(text.substr(dots + 2));
}

/// The number of Unicode code points in a UTF-8 string, which is what "1 to 200
/// characters" means for text carrying em dashes and accented names. Continuation
/// bytes (10xxxxxx) are not counted.
[[nodiscard]] std::size_t characterCount(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] bool isOneOf(const std::string& value, const std::vector<std::string>& permitted) {
    return std::find(permitted.begin(), permitted.end(), value) != permitted.end();
}

/// At least one sentence: some prose and a sentence terminator in it
/// (Requirement 14.2's "rationale of at least one sentence").
[[nodiscard]] bool isSentence(std::string_view text) {
    if (characterCount(text) < 20) {
        return false;
    }
    return text.find('.') != std::string_view::npos || text.find('!') != std::string_view::npos
           || text.find('?') != std::string_view::npos;
}

void add(std::vector<Defect>& defects,
         DefectKind kind,
         std::string entry,
         std::string_view document,
         std::string_view section,
         std::string detail,
         std::size_t line = 0) {
    defects.push_back(Defect{kind, std::move(entry), std::string(document), std::string(section),
                             std::move(detail), line});
}

constexpr std::string_view kParityDoc = "docs/UPSTREAM_PARITY.md";
constexpr std::string_view kBacklogDoc = "docs/PORT_BACKLOG.md";
constexpr std::string_view kProvenance = "provenance";
constexpr std::string_view kBuildOrder = "build order";
constexpr std::size_t kMaxRationale = 200;
constexpr std::size_t kMaxSummary = 200;

/// The em dash the two documents separate a build-order item's priority with.
constexpr std::string_view kEmDash = "\u2014";

}  // namespace

// ===========================================================================
// Defect rendering
// ===========================================================================

std::string_view describe(DefectKind kind) {
    switch (kind) {
    case DefectKind::MissingEntry:
        return "the entry set does not match the set the requirements fix";
    case DefectKind::DuplicateEntry:
        return "this entry appears more than once where exactly one is required";
    case DefectKind::InvalidStatus:
        return "the status or disposition is outside its defined value set";
    case DefectKind::InvalidPriority:
        return "the priority is invalid, or present or absent against its rule";
    case DefectKind::MissingRationale:
        return "the rationale is missing, empty, out of bounds, or not permitted here";
    case DefectKind::MissingCheck:
        return "the acceptance check is missing, incomplete, or not permitted here";
    case DefectKind::MissingField:
        return "a required field is absent, empty or malformed";
    case DefectKind::DuplicateIdentifier:
        return "two entries share one upstream identifier";
    case DefectKind::OutOfOrder:
        return "the build-order list is not sorted must before should before later";
    }
    return "unknown defect";
}

std::string Defect::toString() const {
    std::ostringstream out;
    out << document;
    if (!section.empty()) {
        out << " [" << section << ']';
    }
    if (line != 0) {
        out << " (line " << line << ')';
    }
    out << ": " << (entry.empty() ? std::string{"<unnamed>"} : entry) << " \u2014 " << describe(kind);
    if (!detail.empty()) {
        out << " (" << detail << ')';
    }
    return out.str();
}

std::string toString(const std::vector<Defect>& defects) {
    std::ostringstream out;
    for (std::size_t i = 0; i < defects.size(); ++i) {
        out << "  " << (i + 1) << ". " << defects[i].toString() << '\n';
    }
    return out.str();
}

bool hasDefect(const std::vector<Defect>& defects,
               DefectKind kind,
               std::string_view entrySubstring) {
    return std::any_of(defects.begin(), defects.end(), [&](const Defect& defect) {
        return defect.kind == kind
               && (entrySubstring.empty()
                   || defect.entry.find(entrySubstring) != std::string::npos);
    });
}

std::size_t countDefects(const std::vector<Defect>& defects, DefectKind kind) {
    return static_cast<std::size_t>(
        std::count_if(defects.begin(), defects.end(),
                      [kind](const Defect& defect) { return defect.kind == kind; }));
}

std::string_view describe(ParityTable table) {
    return table == ParityTable::ToolCategory ? "tool category" : "capability area";
}

bool ParityEntry::requiresPriority() const {
    return status == "absent" || status == "partial";
}

std::vector<const ParityEntry*> ParityReport::entriesIn(ParityTable table) const {
    std::vector<const ParityEntry*> selected;
    for (const ParityEntry& entry : entries) {
        if (entry.table == table) {
            selected.push_back(&entry);
        }
    }
    return selected;
}

bool AcceptanceCheck::complete() const {
    return declared && !given.empty() && !when.empty() && !then.empty();
}

bool BacklogEntry::requiresCheck() const {
    return disposition == "port" || disposition == "adapt";
}

std::string BacklogEntry::reportingName() const {
    if (!identifier.empty()) {
        return identifier;
    }
    return heading.empty() ? std::string{"<unnamed entry>"} : heading;
}

// ===========================================================================
// The fixed name sets
// ===========================================================================
//
// Transcribed from Requirements 13.1, 13.2 and 14.2, which are their source of
// record. `tests/docs/report_parser_test.cpp` cross-checks all three against the
// parenthesised lists in requirements.md, so a slip here fails the suite rather
// than narrowing the check.

const std::vector<std::string>& requiredToolCategories() {
    static const std::vector<std::string> names{
        "clips",   "timeline", "texts",    "captions", "transcription",    "color",
        "effects", "denoise",  "multicam", "organize", "layout",           "media",
        "import",  "export",   "generate", "projects", "project settings", "search",
        "sync",    "beats",    "capture frame",        "words"};
    return names;
}

const std::vector<std::string>& requiredCapabilityAreas() {
    static const std::vector<std::string> names{"timeline editing",
                                                "multicam",
                                                "transcription and captions",
                                                "text and graphics",
                                                "color and effects",
                                                "audio scrub and metering",
                                                "generation and upscaling",
                                                "project browser and search",
                                                "MCP and agent chat",
                                                "settings",
                                                "telemetry",
                                                "auto-update"};
    return names;
}

const std::vector<std::string>& requiredBacklogIdentifiers() {
    static const std::vector<std::string> identifiers{"PR 395", "PR 396", "PR 397", "PR 399",
                                                      "PR 401", "PR 403", "PR 404", "PR 405",
                                                      "PR 406", "PR 408"};
    return identifiers;
}

const std::vector<std::string>& parityStatusValues() {
    static const std::vector<std::string> values{"present", "partial", "absent"};
    return values;
}

const std::vector<std::string>& parityPriorityValues() {
    static const std::vector<std::string> values{"must", "should", "later"};
    return values;
}

const std::vector<std::string>& backlogDispositionValues() {
    static const std::vector<std::string> values{"port", "adapt", "not-applicable"};
    return values;
}

const std::vector<std::string>& backlogStatusValues() {
    static const std::vector<std::string> values{"not-started", "in-progress", "complete"};
    return values;
}

std::string_view parityDocumentName() { return kParityDoc; }
std::string_view backlogDocumentName() { return kBacklogDoc; }

// ===========================================================================
// Parsing the Parity_Report
// ===========================================================================

namespace {

/// One build-order line: `<name> (<table label>) — <priority>`.
[[nodiscard]] BuildOrderItem parseBuildOrderItem(const std::string& rest,
                                                 std::size_t lineNumber,
                                                 std::vector<Defect>& defects) {
    BuildOrderItem item;
    item.line = lineNumber;

    std::string_view head{rest};
    std::size_t dash = rest.rfind(kEmDash);
    if (dash == std::string::npos) {
        dash = rest.rfind(" - ");
        if (dash != std::string::npos) {
            item.priority = normalizeValue(std::string_view{rest}.substr(dash + 3));
            head = std::string_view{rest}.substr(0, dash);
        } else {
            add(defects, DefectKind::MissingField, normalizeValue(rest), kParityDoc, kBuildOrder,
                "a build-order item must read `<name> (<table>) \u2014 <priority>`", lineNumber);
        }
    } else {
        item.priority = normalizeValue(std::string_view{rest}.substr(dash + kEmDash.size()));
        head = std::string_view{rest}.substr(0, dash);
    }

    head = trim(head);
    const std::size_t open = head.rfind('(');
    const std::size_t close = head.rfind(')');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
        const std::string label = toLower(normalizeValue(head.substr(open + 1, close - open - 1)));
        if (label == "tool category" || label == "category") {
            item.table = ParityTable::ToolCategory;
            item.tableRecognised = true;
        } else if (label == "capability area" || label == "area") {
            item.table = ParityTable::CapabilityArea;
            item.tableRecognised = true;
        }
        item.name = normalizeValue(head.substr(0, open));
        if (!item.tableRecognised) {
            add(defects, DefectKind::MissingField, item.name, kParityDoc, kBuildOrder,
                "the table label `" + label
                    + "` is neither `tool category` nor `capability area`",
                lineNumber);
        }
    } else {
        item.name = normalizeValue(head);
        add(defects, DefectKind::MissingField, item.name, kParityDoc, kBuildOrder,
            "the item names no table; write `<name> (tool category|capability area)`", lineNumber);
    }
    return item;
}

}  // namespace

ParityReport parseParityReport(std::string_view markdown, std::vector<Defect>& defects) {
    ParityReport report;
    const std::vector<std::string> lines = splitLines(markdown);

    std::string section;
    std::string sectionLower;
    std::string* continuation = nullptr;  // the provenance value a wrapped line extends

    for (std::size_t index = 0; index < lines.size();) {
        const std::string& line = lines[index];
        const std::size_t lineNumber = index + 1;

        std::string heading;
        std::size_t level = 0;
        if (headingText(line, heading, level)) {
            section = heading;
            sectionLower = toLower(heading);
            continuation = nullptr;
            ++index;
            continue;
        }

        // --- a 7-column entry table, recognised by its first header cell -----
        if (isTableRow(line)) {
            continuation = nullptr;
            std::vector<std::string> header = tableCells(line);
            const std::string first = header.empty() ? std::string{} : toLower(normalizeValue(header[0]));
            if (first != "category" && first != "area") {
                ++index;  // some other table: the field-rules table, or prose
                continue;
            }
            const ParityTable table =
                first == "category" ? ParityTable::ToolCategory : ParityTable::CapabilityArea;
            if (table == ParityTable::ToolCategory) {
                report.categoryTableFound = true;
            } else {
                report.areaTableFound = true;
            }
            if (header.size() != 7) {
                add(defects, DefectKind::MissingField, std::string(describe(table)), kParityDoc,
                    section,
                    "the entry table must have 7 columns (name, status, linux-components, "
                    "priority, rationale, macos-framework, linux-replacement), found "
                        + std::to_string(header.size()),
                    lineNumber);
            }
            ++index;
            if (index < lines.size() && isTableRow(lines[index])
                && isSeparatorRow(tableCells(lines[index]))) {
                ++index;
            }
            while (index < lines.size() && isTableRow(lines[index])) {
                std::vector<std::string> cells = tableCells(lines[index]);
                if (isSeparatorRow(cells)) {
                    ++index;
                    continue;
                }
                ParityEntry entry;
                entry.table = table;
                entry.line = index + 1;
                if (cells.size() != 7) {
                    add(defects, DefectKind::MissingField,
                        cells.empty() ? std::string{} : normalizeValue(cells[0]), kParityDoc,
                        section,
                        "the row has " + std::to_string(cells.size())
                            + " columns; 7 are required",
                        entry.line);
                }
                cells.resize(7);
                entry.name = normalizeValue(cells[0]);
                entry.status = normalizeValue(cells[1]);
                entry.linuxComponents = normalizeValue(cells[2]);
                entry.priority = normalizeValue(cells[3]);
                entry.rationale = normalizeValue(cells[4]);
                entry.macosFramework = normalizeValue(cells[5]);
                entry.linuxReplacement = normalizeValue(cells[6]);
                report.entries.push_back(std::move(entry));
                ++index;
            }
            continue;
        }

        // --- the provenance bullets -----------------------------------------
        std::string key;
        std::string value;
        if (bulletKeyValue(line, key, value)) {
            continuation = nullptr;
            std::string* target = nullptr;
            if (key == "upstream-repository") {
                target = &report.provenance.upstreamRepository;
            } else if (key == "upstream-ref") {
                target = &report.provenance.upstreamRef;
            } else if (key == "linux-ref") {
                target = &report.provenance.linuxRef;
            } else if (key == "comparison-date") {
                target = &report.provenance.comparisonDate;
            }
            if (target != nullptr && target->empty()) {
                *target = value;
                continuation = target;
            }
            ++index;
            continue;
        }

        // --- a wrapped provenance value --------------------------------------
        if (continuation != nullptr && !line.empty()
            && (std::isspace(static_cast<unsigned char>(line.front())) != 0)
            && !trim(line).empty()) {
            continuation->append(" ").append(trim(line));
            ++index;
            continue;
        }
        continuation = nullptr;

        // --- the build-order projection ---------------------------------------
        std::string rest;
        if (sectionLower.rfind(kBuildOrder, 0) == 0) {
            report.buildOrderSectionFound = true;
            if (numberedItem(line, rest)) {
                report.buildOrder.push_back(parseBuildOrderItem(rest, lineNumber, defects));
            }
        }
        ++index;
    }

    // --- totality: never report "nothing to check" --------------------------
    if (!report.categoryTableFound) {
        add(defects, DefectKind::MissingEntry, std::string(describe(ParityTable::ToolCategory)),
            kParityDoc, "tables",
            "no entry table whose first header cell is `category` was found");
    }
    if (!report.areaTableFound) {
        add(defects, DefectKind::MissingEntry, std::string(describe(ParityTable::CapabilityArea)),
            kParityDoc, "tables", "no entry table whose first header cell is `area` was found");
    }
    if (!report.buildOrderSectionFound) {
        add(defects, DefectKind::MissingField, "build order", kParityDoc, kBuildOrder,
            "no heading beginning `Build order` was found, so Requirement 13.9's list is absent");
    }
    return report;
}

// ===========================================================================
// Parsing the Port_Backlog
// ===========================================================================

PortBacklog parsePortBacklog(std::string_view markdown, std::vector<Defect>& defects) {
    PortBacklog backlog;
    const std::vector<std::string> lines = splitLines(markdown);

    static const std::vector<std::string_view> kEntryKeys{
        "identifier", "summary", "disposition", "linux-component", "rationale",
        "check",      "status",  "note",        "given",           "when",
        "then"};

    bool inEntry = false;
    std::string* continuation = nullptr;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& line = lines[index];
        const std::size_t lineNumber = index + 1;

        std::string heading;
        std::size_t level = 0;
        if (headingText(line, heading, level)) {
            continuation = nullptr;
            if (level == 3) {
                BacklogEntry entry;
                entry.heading = heading;
                entry.line = lineNumber;
                backlog.entries.push_back(std::move(entry));
                inEntry = true;
            } else {
                inEntry = false;
            }
            continue;
        }

        if (isTableRow(line)) {  // the field-rules table is not an entry
            continuation = nullptr;
            continue;
        }

        // --- the provenance bullets -----------------------------------------
        std::string key;
        std::string value;
        if (!inEntry && bulletKeyValue(line, key, value)) {
            std::string* target = nullptr;
            if (key == "upstream-repository") {
                target = &backlog.provenance.upstreamRepository;
            } else if (key == "upstream-range") {
                target = &backlog.provenance.upstreamRange;
            } else if (key == "window") {
                target = &backlog.provenance.window;
            }
            continuation = nullptr;
            if (target != nullptr && target->empty()) {
                *target = value;
                continuation = target;
            }
            continue;
        }

        // --- an entry field ---------------------------------------------------
        if (inEntry && fieldKeyValue(line, kEntryKeys, key, value)) {
            BacklogEntry& entry = backlog.entries.back();
            continuation = nullptr;
            if (key == "identifier") {
                entry.identifier = normalizeValue(value);
                continuation = &entry.identifier;
            } else if (key == "summary") {
                entry.summary = value;
                continuation = &entry.summary;
            } else if (key == "disposition") {
                entry.disposition = normalizeValue(value);
            } else if (key == "linux-component") {
                entry.linuxComponent = value;
                continuation = &entry.linuxComponent;
            } else if (key == "rationale") {
                entry.rationale = value;
                continuation = &entry.rationale;
            } else if (key == "check") {
                entry.check.declared = true;
                if (!value.empty()) {
                    add(defects, DefectKind::MissingCheck, entry.reportingName(), kBacklogDoc,
                        entry.heading,
                        "a `check:` line carries no value; its legs are the indented `given:`, "
                        "`when:` and `then:` lines below it",
                        lineNumber);
                }
            } else if (key == "given") {
                entry.check.given = value;
                continuation = &entry.check.given;
            } else if (key == "when") {
                entry.check.when = value;
                continuation = &entry.check.when;
            } else if (key == "then") {
                entry.check.then = value;
                continuation = &entry.check.then;
            } else if (key == "status") {
                entry.status = normalizeValue(value);
            } else if (key == "note") {
                entry.note = value;
                continuation = &entry.note;
            }
            continue;
        }

        // --- a wrapped value --------------------------------------------------
        if (continuation != nullptr && !line.empty()
            && (std::isspace(static_cast<unsigned char>(line.front())) != 0)
            && !trim(line).empty()) {
            continuation->append(" ").append(trim(line));
            continue;
        }
        if (trim(line).empty()) {
            continue;  // a blank line does not end a field, only unindented prose does
        }
        continuation = nullptr;
    }

    if (backlog.entries.empty()) {
        add(defects, DefectKind::MissingEntry, "port backlog", kBacklogDoc, "entries",
            "no `### ` entry heading was found, so the document holds no entry to check");
    }
    return backlog;
}

// ===========================================================================
// The Parity_Report check (Requirements 13.1-13.6, 13.9; reported per 13.8)
// ===========================================================================

namespace {

using ParityKey = std::pair<int, std::string>;

[[nodiscard]] ParityKey keyOf(ParityTable table, const std::string& name) {
    return {static_cast<int>(table), name};
}

void checkParityProvenance(const ParityReport& report, std::vector<Defect>& defects) {
    const auto require = [&](const std::string& value, std::string_view name) {
        if (trim(value).empty()) {
            add(defects, DefectKind::MissingField, std::string(name), kParityDoc, kProvenance,
                "Requirement 13.4 requires this provenance field");
        }
    };
    require(report.provenance.upstreamRepository, "upstream-repository");
    require(report.provenance.upstreamRef, "upstream-ref");
    require(report.provenance.linuxRef, "linux-ref");
    require(report.provenance.comparisonDate, "comparison-date");

    const std::string date{trim(report.provenance.comparisonDate)};
    if (!date.empty() && !isIsoDate(date)) {
        add(defects, DefectKind::MissingField, "comparison-date", kParityDoc, kProvenance,
            "Requirement 13.4 requires `YYYY-MM-DD` form, found `" + date + '`');
    }
}

void checkParityMembership(const ParityReport& report, std::vector<Defect>& defects) {
    const auto oneTable = [&](ParityTable table, const std::vector<std::string>& required) {
        std::map<std::string, std::size_t> seen;
        for (const ParityEntry* entry : report.entriesIn(table)) {
            ++seen[entry->name];
        }
        for (const std::string& name : required) {
            const auto found = seen.find(name);
            if (found == seen.end()) {
                add(defects, DefectKind::MissingEntry, name, kParityDoc, describe(table),
                    "no entry for this required " + std::string(describe(table)));
            } else if (found->second > 1) {
                add(defects, DefectKind::DuplicateEntry, name, kParityDoc, describe(table),
                    std::to_string(found->second) + " entries where exactly one is required");
            }
        }
        for (const auto& [name, count] : seen) {
            if (!isOneOf(name, required)) {
                add(defects, DefectKind::MissingEntry, name, kParityDoc, describe(table),
                    "this name is not one of the " + std::to_string(required.size())
                        + " the requirements fix for this table");
            }
        }
    };
    oneTable(ParityTable::ToolCategory, requiredToolCategories());
    oneTable(ParityTable::CapabilityArea, requiredCapabilityAreas());
}

void checkParityEntry(const ParityEntry& entry, std::vector<Defect>& defects) {
    const std::string_view section = describe(entry.table);
    const std::string name = entry.name.empty() ? std::string{"<unnamed row>"} : entry.name;

    if (entry.name.empty()) {
        add(defects, DefectKind::MissingField, name, kParityDoc, section,
            "the row carries no category or capability-area name", entry.line);
    }

    if (!isOneOf(entry.status, parityStatusValues())) {
        add(defects, DefectKind::InvalidStatus, name, kParityDoc, section,
            entry.status.empty() ? std::string{"no status; one of present, partial, absent is "
                                               "required"}
                                 : "`" + entry.status + "` is not one of present, partial, absent",
            entry.line);
        return;  // priority and rationale rules are conditioned on the status
    }

    if (entry.linuxComponents.empty()) {
        add(defects, DefectKind::MissingField, name, kParityDoc, section,
            "Requirement 13.1 requires the Linux component names, or `none`", entry.line);
    } else if (entry.linuxComponents == "none" && entry.status != "absent") {
        add(defects, DefectKind::InvalidStatus, name, kParityDoc, section,
            "`linux-components: none` means no Linux component exists, which implies "
            "`status: absent`, but the status is `"
                + entry.status + '`',
            entry.line);
    }

    if (entry.requiresPriority()) {
        if (!isOneOf(entry.priority, parityPriorityValues())) {
            add(defects, DefectKind::InvalidPriority, name, kParityDoc, section,
                entry.priority.empty()
                    ? "Requirement 13.3 requires a priority for an `" + entry.status + "` entry"
                    : "`" + entry.priority + "` is not one of must, should, later",
                entry.line);
        }
        const std::size_t length = characterCount(entry.rationale);
        if (length == 0) {
            add(defects, DefectKind::MissingRationale, name, kParityDoc, section,
                "Requirement 13.3 requires a 1-200 character rationale for an `" + entry.status
                    + "` entry",
                entry.line);
        } else if (length > kMaxRationale) {
            add(defects, DefectKind::MissingRationale, name, kParityDoc, section,
                "the rationale is " + std::to_string(length) + " characters; the bound is 200",
                entry.line);
        }
    } else {
        if (!entry.priority.empty()) {
            add(defects, DefectKind::InvalidPriority, name, kParityDoc, section,
                "Requirement 13.6 permits no priority on a `present` entry, found `"
                    + entry.priority + '`',
                entry.line);
        }
        if (!entry.rationale.empty()) {
            add(defects, DefectKind::MissingRationale, name, kParityDoc, section,
                "a rationale accompanies a priority; a `present` entry has neither", entry.line);
        }
    }

    // Requirement 13.5: a named macOS framework carries a Linux replacement.
    if (!entry.macosFramework.empty()) {
        if (entry.linuxReplacement.empty()) {
            add(defects, DefectKind::MissingField, name, kParityDoc, section,
                "Requirement 13.5 requires a `linux-replacement` beside the macOS framework `"
                    + entry.macosFramework + '`',
                entry.line);
        } else if (entry.linuxReplacement.rfind("out-of-scope", 0) == 0) {
            const std::size_t colon = entry.linuxReplacement.find(':');
            const std::string reason =
                colon == std::string::npos
                    ? std::string{}
                    : std::string(trim(std::string_view{entry.linuxReplacement}.substr(colon + 1)));
            const std::size_t length = characterCount(reason);
            if (length == 0 || length > kMaxRationale) {
                add(defects, DefectKind::MissingField, name, kParityDoc, section,
                    "Requirement 13.5 requires `out-of-scope: <1-200 character reason>`, found "
                    "a reason of "
                        + std::to_string(length) + " characters",
                    entry.line);
            }
        }
    } else if (!entry.linuxReplacement.empty()) {
        add(defects, DefectKind::MissingField, name, kParityDoc, section,
            "a `linux-replacement` is recorded with no macOS framework to replace", entry.line);
    }
}

void checkBuildOrder(const ParityReport& report, std::vector<Defect>& defects) {
    // The projection's membership: exactly the `absent` and `partial` entries.
    std::map<ParityKey, std::size_t> listed;
    for (const BuildOrderItem& item : report.buildOrder) {
        if (item.tableRecognised) {
            ++listed[keyOf(item.table, item.name)];
        }
    }
    std::map<ParityKey, const ParityEntry*> known;
    for (const ParityEntry& entry : report.entries) {
        known.emplace(keyOf(entry.table, entry.name), &entry);
    }

    for (const ParityEntry& entry : report.entries) {
        if (!entry.requiresPriority()) {
            continue;
        }
        const auto found = listed.find(keyOf(entry.table, entry.name));
        if (found == listed.end()) {
            add(defects, DefectKind::MissingEntry, entry.name, kParityDoc, kBuildOrder,
                "Requirement 13.9's list must contain this `" + entry.status + "` "
                    + std::string(describe(entry.table)) + " entry",
                entry.line);
        } else if (found->second > 1) {
            add(defects, DefectKind::DuplicateEntry, entry.name, kParityDoc, kBuildOrder,
                "listed " + std::to_string(found->second) + " times", entry.line);
        }
    }

    for (const BuildOrderItem& item : report.buildOrder) {
        if (!item.tableRecognised) {
            continue;  // already reported by the parser
        }
        const auto entry = known.find(keyOf(item.table, item.name));
        if (entry == known.end()) {
            add(defects, DefectKind::MissingEntry, item.name, kParityDoc, kBuildOrder,
                "the list names no entry of the " + std::string(describe(item.table)) + " table",
                item.line);
            continue;
        }
        if (!entry->second->requiresPriority()) {
            add(defects, DefectKind::MissingEntry, item.name, kParityDoc, kBuildOrder,
                "Requirement 13.9's list holds only `absent` and `partial` entries; this one is `"
                    + entry->second->status + '`',
                item.line);
            continue;
        }
        if (item.priority != entry->second->priority) {
            add(defects, DefectKind::InvalidPriority, item.name, kParityDoc, kBuildOrder,
                "the list records `" + item.priority + "` and the table records `"
                    + entry->second->priority + '`',
                item.line);
        }
    }

    // The order itself: must, then should, then later.
    const auto rank = [](const std::string& priority) -> int {
        if (priority == "must") {
            return 0;
        }
        if (priority == "should") {
            return 1;
        }
        if (priority == "later") {
            return 2;
        }
        return -1;
    };
    int highest = 0;
    std::string previous;
    for (const BuildOrderItem& item : report.buildOrder) {
        const int current = rank(item.priority);
        if (current < 0) {
            add(defects, DefectKind::InvalidPriority, item.name, kParityDoc, kBuildOrder,
                item.priority.empty() ? std::string{"the item records no priority"}
                                      : "`" + item.priority + "` is not one of must, should, later",
                item.line);
            continue;
        }
        if (current < highest) {
            add(defects, DefectKind::OutOfOrder, item.name, kParityDoc, kBuildOrder,
                "`" + item.priority + "` follows `" + previous
                    + "`; Requirement 13.9 orders must, then should, then later",
                item.line);
        } else {
            highest = current;
            previous = item.priority;
        }
    }
}

}  // namespace

std::vector<Defect> checkParityReport(const ParityReport& report) {
    std::vector<Defect> defects;

    // Non-vacuity first: an empty report is a defective report, never a clean one.
    if (report.entries.empty()) {
        add(defects, DefectKind::MissingEntry, "parity report", kParityDoc, "tables",
            "the report holds no entries at all; "
            + std::to_string(requiredToolCategories().size() + requiredCapabilityAreas().size())
            + " are required");
    }

    checkParityProvenance(report, defects);
    checkParityMembership(report, defects);
    for (const ParityEntry& entry : report.entries) {
        checkParityEntry(entry, defects);
    }
    checkBuildOrder(report, defects);
    return defects;
}

// ===========================================================================
// The Port_Backlog check (Requirements 14.1-14.3, 14.9, 14.10; reported per 14.11)
// ===========================================================================

namespace {

void checkBacklogProvenance(const PortBacklog& backlog, std::vector<Defect>& defects) {
    const auto require = [&](const std::string& value, std::string_view name) {
        if (trim(value).empty()) {
            add(defects, DefectKind::MissingField, std::string(name), kBacklogDoc, kProvenance,
                "Requirement 14.1 requires this provenance field");
        }
    };
    require(backlog.provenance.upstreamRepository, "upstream-repository");
    require(backlog.provenance.upstreamRange, "upstream-range");
    require(backlog.provenance.window, "window");

    const std::string window{trim(backlog.provenance.window)};
    if (!window.empty() && !isIsoDateRange(window)) {
        add(defects, DefectKind::MissingField, "window", kBacklogDoc, kProvenance,
            "Requirement 14.1's window reads `YYYY-MM-DD..YYYY-MM-DD`, found `" + window + '`');
    }
}

void checkBacklogEntry(const BacklogEntry& entry, std::vector<Defect>& defects) {
    const std::string name = entry.reportingName();
    const std::string_view section = entry.heading;

    if (entry.identifier.empty()) {
        add(defects, DefectKind::MissingField, name, kBacklogDoc, section,
            "Requirement 14.11: the entry omits its upstream identifier", entry.line);
    } else if (!entry.heading.empty() && entry.heading.find(entry.identifier) == std::string::npos) {
        add(defects, DefectKind::MissingField, name, kBacklogDoc, section,
            "the heading does not name the identifier the entry records", entry.line);
    }

    const std::size_t summaryLength = characterCount(trim(entry.summary));
    if (summaryLength == 0) {
        add(defects, DefectKind::MissingField, name, kBacklogDoc, section,
            "Requirement 14.11: the entry omits its one-line summary", entry.line);
    } else if (summaryLength > kMaxSummary) {
        add(defects, DefectKind::MissingField, name, kBacklogDoc, section,
            "the summary is " + std::to_string(summaryLength) + " characters; Requirement 14.1's "
            "bound is 200",
            entry.line);
    }

    if (!isOneOf(entry.disposition, backlogDispositionValues())) {
        add(defects, DefectKind::InvalidStatus, name, kBacklogDoc, section,
            entry.disposition.empty()
                ? std::string{"Requirement 14.11: the entry omits its disposition"}
                : "`" + entry.disposition + "` is not one of port, adapt, not-applicable",
            entry.line);
    }

    if (trim(entry.linuxComponent).empty()) {
        add(defects, DefectKind::MissingField, name, kBacklogDoc, section,
            "the entry names no Linux component (or `none`)", entry.line);
    }

    if (trim(entry.rationale).empty()) {
        add(defects, DefectKind::MissingRationale, name, kBacklogDoc, section,
            "Requirement 14.11: the entry omits its rationale", entry.line);
    } else if (!isSentence(entry.rationale)) {
        add(defects, DefectKind::MissingRationale, name, kBacklogDoc, section,
            "Requirement 14.2 requires a rationale of at least one sentence, found `"
                + entry.rationale + '`',
            entry.line);
    }

    if (!isOneOf(entry.status, backlogStatusValues())) {
        add(defects, DefectKind::InvalidStatus, name, kBacklogDoc, section,
            entry.status.empty()
                ? std::string{"the entry omits its status"}
                : "`" + entry.status + "` is not one of not-started, in-progress, complete",
            entry.line);
    }

    if (entry.requiresCheck()) {
        if (!entry.check.declared) {
            add(defects, DefectKind::MissingCheck, name, kBacklogDoc, section,
                "Requirement 14.11: a `" + entry.disposition
                    + "` entry omits its acceptance check, so it is not presented as ported",
                entry.line);
        } else if (!entry.check.complete()) {
            std::string missing;
            const auto note = [&missing](bool absent, std::string_view leg) {
                if (absent) {
                    if (!missing.empty()) {
                        missing += ", ";
                    }
                    missing += leg;
                }
            };
            note(entry.check.given.empty(), "given");
            note(entry.check.when.empty(), "when");
            note(entry.check.then.empty(), "then");
            add(defects, DefectKind::MissingCheck, name, kBacklogDoc, section,
                "Requirement 14.3's acceptance check omits: " + missing, entry.line);
        }
    } else if (entry.disposition == "not-applicable" && entry.check.declared) {
        add(defects, DefectKind::MissingCheck, name, kBacklogDoc, section,
            "Requirement 14.3 records no acceptance check for a `not-applicable` entry",
            entry.line);
    }
}

}  // namespace

std::vector<Defect> checkPortBacklog(const PortBacklog& backlog) {
    std::vector<Defect> defects;

    if (backlog.entries.empty()) {
        add(defects, DefectKind::MissingEntry, "port backlog", kBacklogDoc, "entries",
            "the backlog holds no entries at all; the "
                + std::to_string(requiredBacklogIdentifiers().size())
                + " Requirement 14.2 names are required");
    }

    checkBacklogProvenance(backlog, defects);

    std::map<std::string, std::size_t> seen;
    for (const BacklogEntry& entry : backlog.entries) {
        if (!entry.identifier.empty()) {
            ++seen[entry.identifier];
        }
        checkBacklogEntry(entry, defects);
    }

    for (const auto& [identifier, count] : seen) {
        if (count > 1) {
            add(defects, DefectKind::DuplicateIdentifier, identifier, kBacklogDoc, "entries",
                "Requirement 14.10: " + std::to_string(count)
                    + " entries share this upstream identifier");
        }
    }

    for (const std::string& identifier : requiredBacklogIdentifiers()) {
        if (seen.find(identifier) == seen.end()) {
            add(defects, DefectKind::MissingEntry, identifier, kBacklogDoc, "entries",
                "Requirement 14.2 names this upstream change; the backlog has no entry for it");
        }
    }
    return defects;
}

// ===========================================================================
// Reading a document
// ===========================================================================

std::string readReportDocument(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace palmier::testsupport
