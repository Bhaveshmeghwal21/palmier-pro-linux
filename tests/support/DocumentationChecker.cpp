// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/DocumentationChecker.cpp — the extraction and the checks declared
// in DocumentationChecker.hpp (task 12.7; Requirements 16.7, 16.8).
//
// Two rules govern everything below.
//
// 1. TOTALITY. Every extractor is total: a document that does not follow its
//    stated contract produces a defect, never an exception and never a silent
//    empty result. An extractor that returned "no names found" for a document it
//    failed to understand would let the checker pass vacuously, which is the one
//    failure mode a consistency check cannot afford.
//
// 2. PURITY. Every function here takes its input by value or by
//    `std::string_view` and returns values. No path is opened for writing
//    anywhere in this translation unit, which is how Requirement 16.8's "SHALL
//    leave the documentation unmodified" is met.

#include "support/DocumentationChecker.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace palmier::testsupport {

namespace {

// ---------------------------------------------------------------------------
// Small text utilities
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()
           && (std::isspace(static_cast<unsigned char>(text[begin])) != 0)) {
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

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// True for a Markdown table row: the trimmed line begins with '|'.
[[nodiscard]] bool isTableRow(std::string_view line) {
    const std::string_view trimmed = trim(line);
    return !trimmed.empty() && trimmed.front() == '|';
}

/// The cells of a table row, trimmed, without the empty edges the leading and
/// trailing pipes produce.
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

/// A `|---|---|` alignment row.
[[nodiscard]] bool isSeparatorRow(const std::vector<std::string>& cells) {
    if (cells.empty()) {
        return false;
    }
    return std::all_of(cells.begin(), cells.end(), [](const std::string& cell) {
        return !cell.empty() && cell.find_first_not_of("-: ") == std::string::npos;
    });
}

/// The content of the first `` `...` `` span in `cell`, or an empty string.
[[nodiscard]] std::string firstBackticked(std::string_view cell) {
    const std::size_t open = cell.find('`');
    if (open == std::string_view::npos) {
        return {};
    }
    const std::size_t close = cell.find('`', open + 1);
    if (close == std::string_view::npos) {
        return {};
    }
    return std::string(cell.substr(open + 1, close - open - 1));
}

/// True when `text` is a plain identifier, which is what every option name, tool
/// argument name and result field name in this project is.
[[nodiscard]] bool isIdentifier(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    if ((std::isalpha(static_cast<unsigned char>(text.front())) == 0) && text.front() != '_') {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](const char c) {
        return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
    });
}

/// Locate the column index whose header cell equals `header`, case-insensitively.
[[nodiscard]] std::size_t columnIndex(const std::vector<std::string>& headerCells,
                                      std::string_view header,
                                      std::size_t fallback) {
    for (std::size_t i = 0; i < headerCells.size(); ++i) {
        if (toLower(headerCells[i]) == toLower(header)) {
            return i;
        }
    }
    return fallback;
}

/// Every `` `name` `` immediately followed by a parenthesised note, paired with
/// that note. This is docs/TOOLS.md's result-field grammar: a documented field is
/// always written as a backticked identifier followed by its type in parentheses.
/// Parentheses nest, so the note is taken by depth matching rather than by the
/// first ')'.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> backtickedFieldsWithNotes(
    std::string_view paragraph) {
    std::vector<std::pair<std::string, std::string>> found;
    std::size_t cursor = 0;
    while (cursor < paragraph.size()) {
        const std::size_t open = paragraph.find('`', cursor);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = paragraph.find('`', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        const std::string_view candidate = paragraph.substr(open + 1, close - open - 1);
        cursor = close + 1;
        if (!isIdentifier(candidate)) {
            continue;
        }
        // The note must open immediately after the closing backtick, allowing only
        // spaces between. Anything else means the name was mentioned in prose
        // rather than declared as a field.
        std::size_t probe = cursor;
        while (probe < paragraph.size() && paragraph[probe] == ' ') {
            ++probe;
        }
        if (probe >= paragraph.size() || paragraph[probe] != '(') {
            continue;
        }
        std::size_t depth = 0;
        std::size_t noteEnd = probe;
        for (; noteEnd < paragraph.size(); ++noteEnd) {
            if (paragraph[noteEnd] == '(') {
                ++depth;
            } else if (paragraph[noteEnd] == ')') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        const std::string note{paragraph.substr(probe, noteEnd < paragraph.size()
                                                           ? noteEnd - probe + 1
                                                           : paragraph.size() - probe)};
        found.emplace_back(std::string(candidate), note);
        cursor = noteEnd < paragraph.size() ? noteEnd + 1 : paragraph.size();
    }
    return found;
}

/// The phrases docs/TOOLS.md uses to say "this field is not always there". A note
/// or a Notes cell carrying one of these marks the field conditional, which
/// exempts it from the "must be observed at least once" half of the result check
/// (it may legitimately be absent from every scenario the suite can construct).
[[nodiscard]] bool statesConditionalPresence(std::string_view text) {
    const std::string lowered = toLower(text);
    return contains(lowered, "present only") || contains(lowered, "present when")
           || contains(lowered, "only when") || contains(lowered, "absent");
}

void addDefect(std::vector<DocDefect>& defects,
               DocDefectKind kind,
               std::string name,
               std::string_view document,
               std::string_view section,
               std::string detail) {
    defects.push_back(DocDefect{kind, std::move(name), std::string(document),
                                std::string(section), std::move(detail)});
}

constexpr std::string_view kBuildDoc = "docs/BUILD.md";
constexpr std::string_view kToolsDoc = "docs/TOOLS.md";
constexpr std::string_view kRemoteDoc = "docs/REMOTE_ACCESS.md";
constexpr std::string_view kManifest = "palmier_options.txt";

constexpr std::string_view kOptionsBeginMarker = "<!-- palmier-options:begin -->";
constexpr std::string_view kOptionsEndMarker = "<!-- palmier-options:end -->";

}  // namespace

// ===========================================================================
// Defect rendering
// ===========================================================================

std::string_view describe(DocDefectKind kind) {
    switch (kind) {
    case DocDefectKind::UndocumentedName:
        return "the system defines this name and the documentation does not state it";
    case DocDefectKind::UnknownDocumentedName:
        return "the documentation states this name and the system does not define it";
    case DocDefectKind::TypeMismatch:
        return "the documented type differs from the actual type";
    case DocDefectKind::RequiredMismatch:
        return "the documented required/optional marking differs from the actual one";
    case DocDefectKind::OrderMismatch:
        return "the documented order differs from the published order";
    case DocDefectKind::SectionMissing:
        return "a section the extraction contract requires is absent";
    case DocDefectKind::InputUnreadable:
        return "an input could not be read";
    }
    return "unknown defect";
}

std::string DocDefect::toString() const {
    std::ostringstream out;
    out << document;
    if (!section.empty()) {
        out << " [" << section << ']';
    }
    out << ": " << name << " \u2014 " << describe(kind);
    if (!detail.empty()) {
        out << " (" << detail << ')';
    }
    return out.str();
}

std::string toString(const std::vector<DocDefect>& defects) {
    std::ostringstream out;
    for (std::size_t i = 0; i < defects.size(); ++i) {
        out << "  " << (i + 1) << ". " << defects[i].toString() << '\n';
    }
    return out.str();
}

std::vector<std::string> DocumentedTool::allResultFields() const {
    std::vector<std::string> all = resultFields;
    all.insert(all.end(), conditionalResultFields.begin(), conditionalResultFields.end());
    return all;
}

// ===========================================================================
// docs/BUILD.md — the CMake option tables
// ===========================================================================

std::vector<DocumentedOption> extractDocumentedOptions(std::string_view markdown,
                                                       std::vector<DocDefect>& defects) {
    const std::size_t begin = markdown.find(kOptionsBeginMarker);
    const std::size_t end = markdown.find(kOptionsEndMarker);
    if (begin == std::string_view::npos || end == std::string_view::npos || end < begin) {
        addDefect(defects, DocDefectKind::SectionMissing, std::string(kOptionsBeginMarker),
                  kBuildDoc, "CMake options",
                  "the option tables must sit between the palmier-options markers");
        return {};
    }

    const std::string_view region =
        markdown.substr(begin + kOptionsBeginMarker.size(), end - begin - kOptionsBeginMarker.size());

    std::vector<DocumentedOption> options;
    std::string section = "CMake options";
    std::vector<std::string> header;

    for (const std::string& line : splitLines(region)) {
        const std::string_view trimmed = trim(line);
        if (trimmed.rfind("###", 0) == 0) {
            section = std::string(trim(trimmed.substr(3)));
            header.clear();
            continue;
        }
        if (!isTableRow(trimmed)) {
            continue;
        }
        std::vector<std::string> cells = tableCells(trimmed);
        if (isSeparatorRow(cells)) {
            continue;
        }
        const std::string firstCell = toLower(cells.front());
        if (firstCell == "option" || firstCell == "entry") {
            header = cells;  // the header row of one of the two tables
            continue;
        }
        const std::string name = firstBackticked(cells.front());
        if (name.rfind("PALMIER_", 0) != 0) {
            continue;  // not an option row
        }
        const std::size_t typeColumn = columnIndex(header, "Type", 1);
        std::string type =
            typeColumn < cells.size() ? std::string(trim(firstBackticked(cells[typeColumn]))) : "";
        if (type.empty() && typeColumn < cells.size()) {
            type = cells[typeColumn];  // the derived table writes the type unquoted
        }
        options.push_back(DocumentedOption{name, type, section});
    }

    if (options.empty()) {
        addDefect(defects, DocDefectKind::SectionMissing, "option rows", kBuildDoc,
                  "CMake options", "the marked region contains no PALMIER_* table rows");
    }
    return options;
}

std::vector<LiveOption> parseOptionsManifest(std::string_view manifest,
                                            std::vector<DocDefect>& defects) {
    std::vector<LiveOption> options;
    for (const std::string& line : splitLines(manifest)) {
        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const std::size_t bar = trimmed.find('|');
        if (bar == std::string_view::npos) {
            addDefect(defects, DocDefectKind::InputUnreadable, std::string(trimmed), kManifest,
                      "", "expected a NAME|TYPE record");
            continue;
        }
        options.push_back(LiveOption{std::string(trim(trimmed.substr(0, bar))),
                                     std::string(trim(trimmed.substr(bar + 1)))});
    }
    if (options.empty()) {
        addDefect(defects, DocDefectKind::InputUnreadable, "manifest", kManifest, "",
                  "no PALMIER_* records; the configure-time emission did not run");
    }
    return options;
}

// ===========================================================================
// docs/TOOLS.md — the tool sections
// ===========================================================================

namespace {

/// Accumulate one tool section's argument table, result table and result prose.
class ToolSectionReader {
public:
    explicit ToolSectionReader(std::string name) { tool_.name = std::move(name); }

    void consume(const std::vector<std::string>& lines, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end;) {
            const std::string_view trimmed = trim(lines[i]);
            if (trimmed == "No arguments.") {
                tool_.declaresNoArguments = true;
                ++i;
                continue;
            }
            if (contains(trimmed, "*(command result)*")) {
                tool_.commandResult = true;
            }
            if (isTableRow(trimmed)) {
                i = consumeTable(lines, i, end);
                continue;
            }
            if (!trimmed.empty()) {
                i = consumeParagraph(lines, i, end);
                continue;
            }
            ++i;
        }
        if (tool_.commandResult) {
            // A command-result tool reports EITHER "status" OR the pair
            // "noOp"/"indication" (docs/TOOLS.md, "Rules that hold for every
            // tool"). All three are documented; none is guaranteed by a single
            // invocation, so all three are conditional.
            for (const char* field : {"status", "noOp", "indication"}) {
                addConditional(field);
            }
        }
    }

    [[nodiscard]] DocumentedTool release() { return std::move(tool_); }

private:
    /// One table. Its first header cell decides what it declares: "Argument" an
    /// argument table, "Field" a result table. Anything else is prose furniture
    /// (docs/TOOLS.md has no other tables inside a tool section).
    std::size_t consumeTable(const std::vector<std::string>& lines,
                             std::size_t start,
                             std::size_t end) {
        const std::vector<std::string> header = tableCells(lines[start]);
        const std::string kind = header.empty() ? std::string{} : toLower(header.front());
        std::size_t i = start + 1;
        for (; i < end && isTableRow(lines[i]); ++i) {
            std::vector<std::string> cells = tableCells(lines[i]);
            if (isSeparatorRow(cells)) {
                continue;
            }
            const std::string name = firstBackticked(cells.front());
            if (!isIdentifier(name)) {
                continue;
            }
            if (kind == "argument") {
                readArgumentRow(header, cells, name);
            } else if (kind == "field") {
                const std::size_t notesColumn =
                    columnIndex(header, "Notes", cells.size() - 1);
                const std::string notes = notesColumn < cells.size() ? cells[notesColumn] : "";
                if (statesConditionalPresence(notes)) {
                    addConditional(name);
                } else {
                    addResultField(name);
                }
            }
        }
        return i;
    }

    void readArgumentRow(const std::vector<std::string>& header,
                         const std::vector<std::string>& cells,
                         const std::string& name) {
        DocumentedArgument argument;
        argument.name = name;

        const std::size_t typeColumn = columnIndex(header, "Type", 1);
        std::string type = typeColumn < cells.size() ? cells[typeColumn] : std::string{};
        const std::size_t uuidMarker = type.find("*uuid*");
        if (uuidMarker != std::string::npos) {
            argument.uuid = true;
            type.erase(uuidMarker, std::string_view{"*uuid*"}.size());
        }
        argument.type = std::string(trim(type));

        const std::size_t requiredColumn = columnIndex(header, "Required", 2);
        std::string required =
            requiredColumn < cells.size() ? cells[requiredColumn] : std::string{};
        required.erase(std::remove(required.begin(), required.end(), '*'), required.end());
        argument.required = toLower(std::string(trim(required))) == "yes";

        tool_.arguments.push_back(std::move(argument));
    }

    /// One blank-line-delimited paragraph. A paragraph that OPENS with "Result"
    /// declares the tool's unconditional result fields; any other paragraph
    /// declares fields only if it also states when they are present, which is what
    /// keeps a tool section's explanatory prose (and, for `timeline.read`, its
    /// descriptions of NESTED entries) from being read as a top-level field list.
    std::size_t consumeParagraph(const std::vector<std::string>& lines,
                                 std::size_t start,
                                 std::size_t end) {
        std::string paragraph;
        std::size_t i = start;
        for (; i < end; ++i) {
            const std::string_view trimmed = trim(lines[i]);
            if (trimmed.empty() || isTableRow(trimmed)) {
                break;
            }
            if (!paragraph.empty()) {
                paragraph.push_back(' ');
            }
            paragraph.append(trimmed);
        }

        const bool isResultParagraph = paragraph.rfind("Result", 0) == 0;
        const bool statesPresence = statesConditionalPresence(paragraph);
        if (!isResultParagraph && !statesPresence) {
            return i;
        }
        for (const auto& [name, note] : backtickedFieldsWithNotes(paragraph)) {
            if (!isResultParagraph || statesConditionalPresence(note)) {
                addConditional(name);
            } else {
                addResultField(name);
            }
        }
        return i;
    }

    void addResultField(const std::string& name) {
        if (std::find(tool_.resultFields.begin(), tool_.resultFields.end(), name)
            == tool_.resultFields.end()) {
            tool_.resultFields.push_back(name);
        }
    }

    void addConditional(const std::string& name) {
        if (std::find(tool_.resultFields.begin(), tool_.resultFields.end(), name)
            != tool_.resultFields.end()) {
            return;
        }
        if (std::find(tool_.conditionalResultFields.begin(), tool_.conditionalResultFields.end(),
                      name)
            == tool_.conditionalResultFields.end()) {
            tool_.conditionalResultFields.push_back(name);
        }
    }

    DocumentedTool tool_;
};

/// The tool name a `## \`name\`` heading carries, or an empty string for any other
/// `##` heading (docs/TOOLS.md's prose sections are unbackticked, so this is what
/// separates a tool section from the rules and the "Next" links).
[[nodiscard]] std::string toolHeadingName(std::string_view line) {
    const std::string_view trimmed = trim(line);
    if (trimmed.rfind("## ", 0) != 0) {
        return {};
    }
    const std::string name = firstBackticked(trimmed.substr(3));
    return name.find('.') == std::string::npos ? std::string{} : name;
}

}  // namespace

std::vector<DocumentedTool> extractDocumentedTools(std::string_view markdown,
                                                   std::vector<DocDefect>& defects) {
    const std::vector<std::string> lines = splitLines(markdown);

    // Section boundaries first, so each reader sees exactly its own section.
    std::vector<std::pair<std::string, std::size_t>> starts;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string name = toolHeadingName(lines[i]);
        if (!name.empty()) {
            starts.emplace_back(std::move(name), i + 1);
        }
    }
    if (starts.empty()) {
        addDefect(defects, DocDefectKind::SectionMissing, "tool sections", kToolsDoc, "",
                  "no `## `tool.name`` headings found");
        return {};
    }

    std::vector<DocumentedTool> tools;
    for (std::size_t s = 0; s < starts.size(); ++s) {
        std::size_t end = lines.size();
        for (std::size_t i = starts[s].second; i < lines.size(); ++i) {
            if (trim(lines[i]).rfind("## ", 0) == 0) {
                end = i;
                break;
            }
        }
        ToolSectionReader reader{starts[s].first};
        reader.consume(lines, starts[s].second, end);
        tools.push_back(reader.release());
    }
    return tools;
}

// ===========================================================================
// docs/REMOTE_ACCESS.md — the configuration-input table
// ===========================================================================

std::vector<DocumentedSetting> extractDocumentedSettings(std::string_view markdown,
                                                         std::vector<DocDefect>& defects) {
    const std::vector<std::string> lines = splitLines(markdown);

    std::vector<DocumentedSetting> settings;
    std::string section;
    bool inSettingsTable = false;
    std::vector<std::string> header;

    for (const std::string& line : lines) {
        const std::string_view trimmed = trim(line);
        if (trimmed.rfind("## ", 0) == 0) {
            section = std::string(trim(trimmed.substr(3)));
            inSettingsTable = false;
            continue;
        }
        if (!isTableRow(trimmed)) {
            inSettingsTable = false;
            continue;
        }
        std::vector<std::string> cells = tableCells(trimmed);
        if (isSeparatorRow(cells)) {
            continue;
        }
        if (!cells.empty() && toLower(cells.front()) == "config-file key") {
            inSettingsTable = true;
            header = cells;
            continue;
        }
        if (!inSettingsTable) {
            continue;
        }
        const std::size_t envColumn = columnIndex(header, "Environment variable", 1);
        const std::size_t flagColumn = columnIndex(header, "Command-line flag", 2);
        DocumentedSetting setting;
        setting.key = firstBackticked(cells.front());
        setting.environmentVariable =
            envColumn < cells.size() ? firstBackticked(cells[envColumn]) : std::string{};
        setting.flag = flagColumn < cells.size() ? firstBackticked(cells[flagColumn])
                                                 : std::string{};
        setting.section = section;
        if (!setting.key.empty()) {
            settings.push_back(std::move(setting));
        }
    }

    if (settings.empty()) {
        addDefect(defects, DocDefectKind::SectionMissing, "configuration inputs", kRemoteDoc, "",
                  "no table whose first header cell is \"Config-file key\"");
    }
    return settings;
}

// ===========================================================================
// Source-derived result fields, for the hook-owned success payloads
// ===========================================================================

FunctionFieldScan scanJsonSetFields(std::string_view source, std::string_view anchor) {
    FunctionFieldScan scan;
    const std::size_t at = source.find(anchor);
    if (at == std::string_view::npos) {
        return scan;
    }
    const std::size_t bodyStart = source.find('{', at);
    if (bodyStart == std::string_view::npos) {
        return scan;
    }
    scan.found = true;

    std::size_t depth = 0;
    std::size_t i = bodyStart;
    for (; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                break;
            }
        }
    }
    const std::string_view body = source.substr(bodyStart, std::min(i, source.size() - 1) + 1 - bodyStart);

    constexpr std::string_view kSetCall = ".set(\"";
    std::size_t cursor = 0;
    while (true) {
        const std::size_t call = body.find(kSetCall, cursor);
        if (call == std::string_view::npos) {
            break;
        }
        const std::size_t nameStart = call + kSetCall.size();
        const std::size_t nameEnd = body.find('"', nameStart);
        if (nameEnd == std::string_view::npos) {
            break;
        }
        const std::string_view name = body.substr(nameStart, nameEnd - nameStart);
        if (isIdentifier(name)
            && std::find(scan.fields.begin(), scan.fields.end(), name) == scan.fields.end()) {
            scan.fields.emplace_back(name);
        }
        cursor = nameEnd + 1;
    }
    return scan;
}

// ===========================================================================
// The checks
// ===========================================================================

std::vector<DocDefect> checkNameSets(const std::vector<std::string>& documented,
                                     const std::vector<std::string>& live,
                                     std::string_view document,
                                     std::string_view section) {
    const std::set<std::string> documentedSet{documented.begin(), documented.end()};
    const std::set<std::string> liveSet{live.begin(), live.end()};

    std::vector<DocDefect> defects;
    for (const std::string& name : liveSet) {
        if (documentedSet.count(name) == 0) {
            addDefect(defects, DocDefectKind::UndocumentedName, name, document, section, {});
        }
    }
    for (const std::string& name : documentedSet) {
        if (liveSet.count(name) == 0) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName, name, document, section, {});
        }
    }
    return defects;
}

std::vector<DocDefect> checkOptions(const std::vector<DocumentedOption>& documented,
                                    const std::vector<LiveOption>& live) {
    std::map<std::string, DocumentedOption> documentedByName;
    for (const DocumentedOption& option : documented) {
        documentedByName.emplace(option.name, option);
    }
    std::map<std::string, LiveOption> liveByName;
    for (const LiveOption& option : live) {
        liveByName.emplace(option.name, option);
    }

    std::vector<DocDefect> defects;
    for (const auto& [name, option] : liveByName) {
        const auto documentedIt = documentedByName.find(name);
        if (documentedIt == documentedByName.end()) {
            addDefect(defects, DocDefectKind::UndocumentedName, name, kBuildDoc, "CMake options",
                      "the build tree defines this " + option.type + " cache entry");
            continue;
        }
        if (documentedIt->second.type != option.type) {
            addDefect(defects, DocDefectKind::TypeMismatch, name, kBuildDoc,
                      documentedIt->second.section,
                      "documented as " + documentedIt->second.type + ", actually " + option.type);
        }
    }
    for (const auto& [name, option] : documentedByName) {
        if (liveByName.count(name) == 0) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName, name, kBuildDoc,
                      option.section, "no such PALMIER_* cache entry in the configured tree");
        }
    }
    return defects;
}

std::vector<DocDefect> checkTools(const std::vector<DocumentedTool>& documented,
                                  const std::vector<LiveTool>& live) {
    std::vector<DocDefect> defects;

    std::vector<std::string> documentedNames;
    documentedNames.reserve(documented.size());
    for (const DocumentedTool& tool : documented) {
        documentedNames.push_back(tool.name);
    }
    std::vector<std::string> liveNames;
    liveNames.reserve(live.size());
    for (const LiveTool& tool : live) {
        liveNames.push_back(tool.name);
    }

    const std::vector<DocDefect> nameDefects =
        checkNameSets(documentedNames, liveNames, kToolsDoc, "tool sections");
    defects.insert(defects.end(), nameDefects.begin(), nameDefects.end());

    // Order is only meaningful once the two name sets agree; reporting it while a
    // name is missing would just repeat the same finding in a less useful form.
    if (nameDefects.empty() && documentedNames != liveNames) {
        for (std::size_t i = 0; i < liveNames.size(); ++i) {
            if (documentedNames[i] != liveNames[i]) {
                addDefect(defects, DocDefectKind::OrderMismatch, liveNames[i], kToolsDoc,
                          "tool sections",
                          "published at position " + std::to_string(i + 1) + ", documented at "
                              + std::to_string(1
                                               + static_cast<std::size_t>(std::distance(
                                                     documentedNames.begin(),
                                                     std::find(documentedNames.begin(),
                                                               documentedNames.end(),
                                                               liveNames[i])))));
                break;  // one report is enough to locate the reordering
            }
        }
    }

    std::map<std::string, const DocumentedTool*> documentedByName;
    for (const DocumentedTool& tool : documented) {
        documentedByName.emplace(tool.name, &tool);
    }

    for (const LiveTool& liveTool : live) {
        const auto it = documentedByName.find(liveTool.name);
        if (it == documentedByName.end()) {
            continue;  // already reported as undocumented
        }
        const DocumentedTool& doc = *it->second;

        std::vector<std::string> documentedArguments;
        for (const DocumentedArgument& argument : doc.arguments) {
            documentedArguments.push_back(argument.name);
        }
        std::vector<std::string> liveArguments;
        for (const LiveArgument& argument : liveTool.arguments) {
            liveArguments.push_back(argument.name);
        }

        const std::vector<DocDefect> argumentDefects =
            checkNameSets(documentedArguments, liveArguments, kToolsDoc, liveTool.name);
        defects.insert(defects.end(), argumentDefects.begin(), argumentDefects.end());

        // "No arguments." and an argument table are mutually exclusive claims, and
        // a tool that takes no arguments must make the claim explicitly — silence
        // would otherwise read as an accurate empty argument set.
        if (liveTool.arguments.empty() && !doc.declaresNoArguments) {
            addDefect(defects, DocDefectKind::UndocumentedName, liveTool.name, kToolsDoc,
                      liveTool.name, "takes no arguments, but the section does not say so");
        }
        if (!liveTool.arguments.empty() && doc.declaresNoArguments) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName, liveTool.name, kToolsDoc,
                      liveTool.name, "documented as taking no arguments, but it declares "
                                        + std::to_string(liveTool.arguments.size()));
        }

        for (const LiveArgument& liveArgument : liveTool.arguments) {
            const auto documentedArgument =
                std::find_if(doc.arguments.begin(), doc.arguments.end(),
                             [&liveArgument](const DocumentedArgument& candidate) {
                                 return candidate.name == liveArgument.name;
                             });
            if (documentedArgument == doc.arguments.end()) {
                continue;  // already reported
            }
            if (documentedArgument->type != liveArgument.type) {
                addDefect(defects, DocDefectKind::TypeMismatch, liveArgument.name, kToolsDoc,
                          liveTool.name,
                          "documented as \"" + documentedArgument->type + "\", schema publishes \""
                              + liveArgument.type + '"');
            }
            if (documentedArgument->uuid != liveArgument.uuid) {
                addDefect(defects, DocDefectKind::TypeMismatch, liveArgument.name, kToolsDoc,
                          liveTool.name,
                          liveArgument.uuid
                              ? "the schema publishes \"format\": \"uuid\"; the *uuid* marker is "
                                "missing"
                              : "marked *uuid*, but the schema publishes no uuid format");
            }
            if (documentedArgument->required != liveArgument.required) {
                addDefect(defects, DocDefectKind::RequiredMismatch, liveArgument.name, kToolsDoc,
                          liveTool.name,
                          liveArgument.required ? "required by the schema, documented as optional"
                                                : "optional in the schema, documented as required");
            }
        }
    }
    return defects;
}

std::vector<DocDefect> checkSettings(const std::vector<DocumentedSetting>& documented,
                                     const std::vector<LiveSetting>& live) {
    std::map<std::string, DocumentedSetting> documentedByKey;
    for (const DocumentedSetting& setting : documented) {
        documentedByKey.emplace(setting.key, setting);
    }

    std::vector<DocDefect> defects;
    for (const LiveSetting& liveSetting : live) {
        const auto it = documentedByKey.find(liveSetting.key);
        if (it == documentedByKey.end()) {
            addDefect(defects, DocDefectKind::UndocumentedName, liveSetting.key, kRemoteDoc,
                      "Every configuration input",
                      "the key table defines it (" + liveSetting.environmentVariable + ", "
                          + liveSetting.flag + ')');
            continue;
        }
        if (it->second.environmentVariable != liveSetting.environmentVariable) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName,
                      it->second.environmentVariable, kRemoteDoc, it->second.section,
                      "the environment variable for " + liveSetting.key + " is "
                          + liveSetting.environmentVariable);
        }
        if (it->second.flag != liveSetting.flag) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName, it->second.flag, kRemoteDoc,
                      it->second.section,
                      "the command-line flag for " + liveSetting.key + " is " + liveSetting.flag);
        }
    }
    for (const auto& [key, setting] : documentedByKey) {
        const bool known = std::any_of(live.begin(), live.end(),
                                       [&key](const LiveSetting& candidate) {
                                           return candidate.key == key;
                                       });
        if (!known) {
            addDefect(defects, DocDefectKind::UnknownDocumentedName, key, kRemoteDoc,
                      setting.section, "the key table recognises no such key");
        }
    }
    return defects;
}

std::vector<DocDefect> checkResultFields(const std::vector<DocumentedTool>& documented,
                                         const std::vector<ObservedResult>& observed) {
    std::map<std::string, const DocumentedTool*> documentedByName;
    for (const DocumentedTool& tool : documented) {
        documentedByName.emplace(tool.name, &tool);
    }

    // The union of every field seen across a tool's scenarios.
    std::map<std::string, std::set<std::string>> seenByTool;
    for (const ObservedResult& result : observed) {
        seenByTool[result.toolName].insert(result.fields.begin(), result.fields.end());
    }

    std::vector<DocDefect> defects;
    for (const auto& [toolName, seen] : seenByTool) {
        const auto it = documentedByName.find(toolName);
        if (it == documentedByName.end()) {
            continue;  // the tool itself is already reported as undocumented
        }
        const DocumentedTool& doc = *it->second;
        const std::vector<std::string> all = doc.allResultFields();
        const std::set<std::string> documentedFields{all.begin(), all.end()};

        for (const std::string& field : seen) {
            if (documentedFields.count(field) == 0) {
                addDefect(defects, DocDefectKind::UndocumentedName, field, kToolsDoc, toolName,
                          "the handler returns this result field");
            }
        }
        for (const std::string& field : doc.resultFields) {
            if (seen.count(field) == 0) {
                addDefect(defects, DocDefectKind::UnknownDocumentedName, field, kToolsDoc,
                          toolName,
                          "documented as an unconditional result field, but no invocation "
                          "returned it");
            }
        }
    }
    return defects;
}

// ===========================================================================
// Reading
// ===========================================================================

std::string readWholeFile(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

}  // namespace palmier::testsupport
