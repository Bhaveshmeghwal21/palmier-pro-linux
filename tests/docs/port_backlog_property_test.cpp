// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/port_backlog_property_test.cpp — the quantified half of the
// Port_Backlog checks: Property 71 (well-formedness) and Property 72 (the check
// detects every malformation) (task 12.5; Requirements 14.1, 14.3, 14.9, 14.10,
// 14.11).
//
// HOW THIS FILE RELATES TO TASK 12.3's `tests/docs/report_parser_test.cpp`
// ---------------------------------------------------------------------------
// That file is the EXAMPLE-BASED half: it checks the checked-in
// `docs/PORT_BACKLOG.md`, proves the parser total, and produces each defect kind by
// a single targeted TEXT EDIT of the real document. This file is the QUANTIFIED
// half over the very same pure functions of `tests/support/ReportParser.hpp`, and
// deliberately does not restate those single-edit cases. Instead it:
//
//   * GENERATES whole backlogs to the grammar the header documents — the ten
//     identifiers Requirement 14.2 names plus 0-30 further entries, in a generated
//     order, with generated summaries (across both boundaries of the 1-200
//     character bound), dispositions, statuses, rationales, notes and acceptance
//     checks — and asserts the check accepts every one of them (Property 71); and
//   * MUTATES a generated backlog at the MODEL level (omit a field, omit the
//     `check:` block, omit one leg of a check, duplicate an identifier, put a check
//     on a `not-applicable` entry, ...), re-renders it, and asserts the check
//     reports the corresponding defect naming the offending entry (Property 72).
//
// Ten is the FLOOR on a well-formed backlog, not one: Requirement 14.2 names ten
// upstream changes and the check reports each absent one, so a generated backlog
// spans 10-40 entries. The degenerate end (no entries at all) is a totality case
// and belongs to task 12.3, which asserts it directly.
//
// WHY THESE PROPERTIES CANNOT PASS VACUOUSLY
// ---------------------------------------------------------------------------
// A generator that emitted something the parser could not read would make
// `check(parse(doc)).empty()` a green tick over nothing. Four guards, on every case:
//
//   1. THE CHECKED-IN DOCUMENT IS A CASE OF EVERY RUN. Both properties assert that
//      `docs/PORT_BACKLOG.md` was found (non-zero bytes), that ten entries were
//      parsed out of it carrying every required field, that nine of them (every
//      `port`/`adapt` one) carry a complete `given`/`when`/`then` check, that its
//      provenance window is Requirement 14.1's, and that it has no defects.
//   2. EVERY GENERATED BACKLOG IS ASSERTED FIELD-FOR-FIELD AGAINST ITS MODEL. Each
//      entry is located by index, its heading, identifier, summary, disposition,
//      Linux component, rationale, status, note and each leg of its acceptance check
//      compared with the model's, and the provenance compared key for key. A parser
//      that found headings and no fields fails here.
//   3. THE MUTANT IS ASSERTED TO STILL BE A READABLE DOCUMENT. Property 72 asserts
//      the mutant's parsed entry count equals the mutated model's, that every
//      entry's heading was read back in order, that the count of parsed identifiers
//      matches the model's, and that the defect list does NOT contain the parser's
//      "the document holds no entry to check" defect — so the asserted defect is
//      attributable to the injected fault.
//   4. THE UNMUTATED BACKLOG OF THE SAME CASE IS ASSERTED CLEAN, so a mutation is
//      only counted as evidence when the thing it was applied to passed.
//
// THE DEFECT KINDS THIS DOCUMENT'S GRAMMAR CAN EXPRESS
// ---------------------------------------------------------------------------
// Six of the nine: `MissingEntry`, `MissingField`, `MissingRationale`,
// `InvalidStatus`, `MissingCheck` and `DuplicateIdentifier` — the last two being
// exactly the two the Parity_Report cannot express (Requirements 14.3 and 14.10 are
// backlog-only rules). `DuplicateEntry`, `InvalidPriority` and `OutOfOrder` are
// Parity_Report kinds and are covered by Property 70 in
// `tests/docs/parity_report_property_test.cpp`; the union of the two files is the
// whole nine-kind vocabulary, which
// `BacklogCheckMutations.EveryMutationKindIsDetectedAndNamesItsEntry` states and
// asserts. That test also drives EVERY mutation kind on a fixed backlog on every
// run, so full kind coverage does not depend on the sampling of the property.
//
// ON REQUIREMENT 14.9
// ---------------------------------------------------------------------------
// Requirement 14.9 is a WHERE clause: it binds only where an upstream change
// targets a macOS-only framework, and then asks for the framework's name and the
// reason no Linux equivalent is in scope. The generator therefore produces
// `not-applicable` entries of both shapes and the property asserts, for the ones
// that cite a framework, that the parsed rationale names that framework AND states
// the no-equivalent reason. The checked-in document's single `not-applicable` entry
// (PR 401, non-English README maintenance) cites a structural reason rather than a
// framework, so the clause does not bind to it — which is why this is asserted over
// the generated space and not as a blanket rule about every `not-applicable` entry.
//
// COST
// ---------------------------------------------------------------------------
// The checked-in document is read and checked ONCE per process (a function-local
// static); each generated case builds a Markdown string of at most 40 entries and
// parses it. The binary stays far inside the 600 s per-test limit.
//
// _Requirements: 14.1, 14.3, 14.9, 14.10, 14.11_

#include "support/ReportParser.hpp"

#include <gtest/gtest.h>

#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace {

using testsupport::BacklogEntry;
using testsupport::Defect;
using testsupport::DefectKind;
using testsupport::PortBacklog;

// ===========================================================================
// Parse and check in one step — exactly what a CI gate does
// ===========================================================================

struct Checked {
    std::size_t         markdownBytes{0};
    PortBacklog         backlog;
    std::vector<Defect> defects;  ///< parse defects followed by check defects
};

[[nodiscard]] Checked checkMarkdown(std::string_view markdown) {
    Checked outcome;
    outcome.markdownBytes = markdown.size();
    outcome.backlog = testsupport::parsePortBacklog(markdown, outcome.defects);
    const std::vector<Defect> checked = testsupport::checkPortBacklog(outcome.backlog);
    outcome.defects.insert(outcome.defects.end(), checked.begin(), checked.end());
    return outcome;
}

/// `docs/PORT_BACKLOG.md`, read and checked once per process. Read from the SOURCE
/// tree via PALMIER_DOCS_DIR, because ctest's working directory is the build tree
/// where the document does not exist — and a check that cannot find its input would
/// pass vacuously, hence the byte-count assertion at every use.
const Checked& checkedInBacklog() {
    static const Checked outcome = checkMarkdown(
        testsupport::readReportDocument(std::string{PALMIER_DOCS_DIR} + "/PORT_BACKLOG.md"));
    return outcome;
}

[[nodiscard]] std::size_t countCompleteChecks(const PortBacklog& backlog) {
    return static_cast<std::size_t>(
        std::count_if(backlog.entries.begin(), backlog.entries.end(),
                      [](const BacklogEntry& entry) { return entry.check.complete(); }));
}

// ===========================================================================
// The document model — a Port_Backlog as values, plus a renderer
// ===========================================================================
//
// The generator builds this model and renders it to Markdown; Property 72's
// mutations edit the model and re-render. The renderer writes the grammar
// `ReportParser.hpp` documents: three provenance bullets, then one `### ` heading
// per entry over flat `key: value` lines, with the acceptance check's legs indented
// under a `check:` line. A field left empty is rendered as NO LINE AT ALL, which is
// how "the entry omits this field" is expressed.

struct Entry {
    std::string heading;
    std::string identifier;
    std::string summary;
    std::string disposition{"port"};
    std::string component{"core::TimelineEngine"};
    std::string rationale;
    std::string status{"not-started"};
    std::string note;
    bool        declareCheck{false};
    /// Render the legs but not the `check:` line that declares them — the fault the
    /// checker must read as "this entry has no acceptance check".
    bool        omitCheckKeyword{false};
    std::string given;
    std::string when;
    std::string then;
    /// Requirement 14.9's WHERE clause: this entry is `not-applicable` BECAUSE of a
    /// macOS-only framework, so its rationale must name the framework and the reason.
    std::string framework;
};

struct Doc {
    std::string        repository{"https://github.com/palmier-io/palmier-pro"};
    std::string        range{"v2026.06.3..v2026.07.4"};
    std::string        window{"2026-06-25..2026-07-25"};
    std::vector<Entry> entries;
};

[[nodiscard]] std::string render(const Doc& doc) {
    std::string text = "# A generated port backlog\n\n## Provenance\n\n";
    const auto bullet = [&text](std::string_view key, const std::string& value) {
        if (!value.empty()) {
            text += "- ";
            text += key;
            text += ": " + value + "\n";
        }
    };
    bullet("upstream-repository", doc.repository);
    bullet("upstream-range", doc.range);
    bullet("window", doc.window);
    text += "\n";

    const auto field = [&text](std::string_view key, const std::string& value) {
        if (!value.empty()) {
            text += key;
            text += ": " + value + "\n";
        }
    };
    for (const Entry& entry : doc.entries) {
        text += "### " + entry.heading + "\n\n";
        field("identifier", entry.identifier);
        field("summary", entry.summary);
        field("disposition", entry.disposition);
        field("linux-component", entry.component);
        field("rationale", entry.rationale);
        if (entry.declareCheck) {
            if (!entry.omitCheckKeyword) {
                text += "check:\n";
            }
            if (!entry.given.empty()) {
                text += "  given: " + entry.given + "\n";
            }
            if (!entry.when.empty()) {
                text += "  when:  " + entry.when + "\n";
            }
            if (!entry.then.empty()) {
                text += "  then:  " + entry.then + "\n";
            }
        }
        field("status", entry.status);
        field("note", entry.note);
        text += "\n";
    }
    return text;
}

// ===========================================================================
// Generation
// ===========================================================================

const std::vector<std::string>& componentPool() {
    static const std::vector<std::string> pool{
        "services::ProjectSession", "media::DecoderTeardownQueue", "gpu::EffectKernels",
        "ui::MainWindow, ui::TimelinePanel", "core::TimelineEngine, services::ToolRegistry"};
    return pool;
}

const std::vector<std::string>& titlePool() {
    static const std::vector<std::string> pool{"a change to the editor shell",
                                               "a change to the export path",
                                               "a change to the generation catalog",
                                               "a change to the dependency pins",
                                               "a change to the timeline model"};
    return pool;
}

const std::vector<std::string>& frameworkPool() {
    static const std::vector<std::string> pool{"AVFoundation", "SwiftUI", "Core Image",
                                               "VideoToolbox"};
    return pool;
}

/// A uniform index in [0, bound), shrinking towards 0.
///
/// `rc::gen::inRange` SCALES its range with RapidCheck's `size` parameter, which
/// rises across a run — so a bare `inRange` would draw only low indices in the early
/// cases, and for a categorical choice (a disposition, a status, a pool entry, a
/// mutation kind) whole categories would go unexercised. Resizing to the nominal size
/// makes every draw uniform over the whole range while keeping `inRange`'s shrinking,
/// so the reported counterexample is still the small one.
[[nodiscard]] std::size_t drawIndex(std::size_t bound) {
    return *rc::gen::resize(rc::kNominalSize, rc::gen::inRange<std::size_t>(0, bound));
}

[[nodiscard]] bool drawFlag() { return drawIndex(2) == 1; }

[[nodiscard]] const std::string& drawFrom(const std::vector<std::string>& pool) {
    return pool[drawIndex(pool.size())];
}

/// ASCII text of exactly `length` characters (so a code-point count and a byte count
/// agree), never ending in a space. Used for the 1-200 character summary bound of
/// Requirement 14.1, whose boundaries the generator draws over.
[[nodiscard]] std::string textOfLength(std::string seed, std::size_t length) {
    static constexpr std::string_view kFiller = " and the affected component is named beside it";
    while (seed.size() < length) {
        seed.append(kFiller);
    }
    seed.resize(length);
    if (!seed.empty() && seed.back() == ' ') {
        seed.back() = '.';
    }
    return seed;
}

/// A rationale of at least one sentence: the checker asks for 20 or more characters
/// and a sentence terminator (Requirement 14.2).
[[nodiscard]] std::string rationaleFor(const Entry& entry) {
    if (!entry.framework.empty()) {
        return "the change targets " + entry.framework
               + ", a macOS-only framework, and no Linux equivalent is in scope for "
                 "this port, so it does not apply.";
    }
    if (entry.component == "none") {
        return "no Linux component carries this behaviour yet, so the change has nothing "
               "to land in until one exists.";
    }
    return "the change lands in " + entry.component
           + ", which is where the Linux port carries this behaviour.";
}

void fillCheck(Entry& entry) {
    entry.declareCheck = true;
    entry.given = "a project with one clip on one video track and one clip on one audio track";
    entry.when = "the documented action is performed exactly once through the Tool_Surface";
    entry.then = "the documented observable outcome holds and nothing else changes";
}

void clearCheck(Entry& entry) {
    entry.declareCheck = false;
    entry.omitCheckKeyword = false;
    entry.given.clear();
    entry.when.clear();
    entry.then.clear();
}

/// Force a disposition, keeping the entry well-formed: `port`/`adapt` carry a
/// complete acceptance check and `not-applicable` carries none (Requirement 14.3).
void setDisposition(Entry& entry, const std::string& disposition, bool citeFramework) {
    entry.disposition = disposition;
    entry.framework.clear();
    if (disposition == "not-applicable") {
        clearCheck(entry);
        if (citeFramework) {
            entry.framework = frameworkPool().front();
        }
    } else {
        fillCheck(entry);
    }
    entry.rationale = rationaleFor(entry);
}

[[nodiscard]] Entry drawEntry(std::string identifier) {
    Entry entry;
    entry.identifier = std::move(identifier);
    entry.heading = entry.identifier + " \u2014 " + drawFrom(titlePool());
    entry.summary = textOfLength("record the upstream change under " + entry.identifier
                                     + " and what the Linux port does about it",
                                 drawIndex(200) + 1);
    entry.component = drawFlag() ? std::string{"none"} : drawFrom(componentPool());
    entry.status = testsupport::backlogStatusValues()[drawIndex(3)];
    if (drawFlag()) {
        entry.note = "a note carrying what the fixed fields cannot";
    }
    setDisposition(entry, testsupport::backlogDispositionValues()[drawIndex(3)], drawFlag());
    return entry;
}

/// A generated well-formed backlog: the ten identifiers Requirement 14.2 names plus
/// 0-30 further entries (an entry outside the required ten is permitted — the check
/// requires the ten to be PRESENT, not that nothing else is), in a generated order.
///
/// Three positions are then overwritten to GUARANTEE that every mutation of
/// Property 72 has a target and that every clause of Property 71 is exercised: a
/// `port` entry with a complete check, an `adapt` entry with a complete check, and a
/// `not-applicable` entry citing a macOS framework (Requirement 14.9). The
/// identifiers at those positions vary with the shuffle, so the guarantee fixes
/// shapes, not entries.
[[nodiscard]] Doc drawDoc() {
    Doc doc;
    for (const std::string& identifier : testsupport::requiredBacklogIdentifiers()) {
        doc.entries.push_back(drawEntry(identifier));
    }
    const std::size_t extras = drawIndex(31);
    for (std::size_t i = 0; i < extras; ++i) {
        doc.entries.push_back(drawEntry("PR " + std::to_string(500 + i)));
    }
    for (std::size_t i = doc.entries.size() - 1; i > 0; --i) {
        std::swap(doc.entries[i], doc.entries[drawIndex(i + 1)]);
    }

    setDisposition(doc.entries[0], "port", false);
    setDisposition(doc.entries[1], "adapt", false);
    setDisposition(doc.entries[2], "not-applicable", true);
    return doc;
}

/// The same shape with nothing drawn, for the exhaustive mutation-coverage test:
/// dispositions cycle port / adapt / not-applicable across the ten required
/// identifiers.
[[nodiscard]] Doc fixedDoc() {
    Doc doc;
    std::size_t i = 0;
    for (const std::string& identifier : testsupport::requiredBacklogIdentifiers()) {
        Entry entry;
        entry.identifier = identifier;
        entry.heading = identifier + " \u2014 " + titlePool()[i % titlePool().size()];
        entry.summary = "record the upstream change under " + identifier
                        + " and what the Linux port does about it";
        entry.component = componentPool()[i % componentPool().size()];
        entry.status = testsupport::backlogStatusValues()[i % 3];
        setDisposition(entry, testsupport::backlogDispositionValues()[i % 3], i % 2 == 0);
        doc.entries.push_back(std::move(entry));
        ++i;
    }
    setDisposition(doc.entries[0], "port", false);
    setDisposition(doc.entries[1], "adapt", false);
    setDisposition(doc.entries[2], "not-applicable", true);
    return doc;
}

// ===========================================================================
// Mutation — every malformation Requirement 14.11 names, and then some
// ===========================================================================

enum class Mutation {
    OmitARequiredEntry,
    RenameAnIdentifierOutsideTheRequiredSet,
    DuplicateAnIdentifier,
    OmitTheIdentifier,
    HeadingThatDoesNotNameTheIdentifier,
    OmitTheSummary,
    OverlongSummary,
    OmitTheLinuxComponent,
    OmitTheRationale,
    RationaleThatIsNotASentence,
    OmitTheDisposition,
    DispositionOutsideItsValueSet,
    OmitTheStatus,
    StatusOutsideItsValueSet,
    OmitTheCheckBlock,
    OmitOneLegOfACheck,
    CheckOnANotApplicableEntry,
    OmitAProvenanceField,
    MalformedWindow
};

constexpr std::array<Mutation, 19> kMutations{
    Mutation::OmitARequiredEntry,
    Mutation::RenameAnIdentifierOutsideTheRequiredSet,
    Mutation::DuplicateAnIdentifier,
    Mutation::OmitTheIdentifier,
    Mutation::HeadingThatDoesNotNameTheIdentifier,
    Mutation::OmitTheSummary,
    Mutation::OverlongSummary,
    Mutation::OmitTheLinuxComponent,
    Mutation::OmitTheRationale,
    Mutation::RationaleThatIsNotASentence,
    Mutation::OmitTheDisposition,
    Mutation::DispositionOutsideItsValueSet,
    Mutation::OmitTheStatus,
    Mutation::StatusOutsideItsValueSet,
    Mutation::OmitTheCheckBlock,
    Mutation::OmitOneLegOfACheck,
    Mutation::CheckOnANotApplicableEntry,
    Mutation::OmitAProvenanceField,
    Mutation::MalformedWindow};

[[nodiscard]] std::string_view describe(Mutation kind) {
    switch (kind) {
    case Mutation::OmitARequiredEntry:
        return "omit an entry Requirement 14.2 names";
    case Mutation::RenameAnIdentifierOutsideTheRequiredSet:
        return "rename an identifier outside the required set";
    case Mutation::DuplicateAnIdentifier:
        return "two entries sharing one identifier";
    case Mutation::OmitTheIdentifier:
        return "omit the upstream identifier";
    case Mutation::HeadingThatDoesNotNameTheIdentifier:
        return "a heading that does not name the identifier";
    case Mutation::OmitTheSummary:
        return "omit the one-line summary";
    case Mutation::OverlongSummary:
        return "a summary over the 200-character bound";
    case Mutation::OmitTheLinuxComponent:
        return "omit the Linux component";
    case Mutation::OmitTheRationale:
        return "omit the rationale";
    case Mutation::RationaleThatIsNotASentence:
        return "a rationale that is not a sentence";
    case Mutation::OmitTheDisposition:
        return "omit the disposition";
    case Mutation::DispositionOutsideItsValueSet:
        return "a disposition outside its value set";
    case Mutation::OmitTheStatus:
        return "omit the status";
    case Mutation::StatusOutsideItsValueSet:
        return "a status outside its value set";
    case Mutation::OmitTheCheckBlock:
        return "omit the acceptance check of a port or adapt entry";
    case Mutation::OmitOneLegOfACheck:
        return "omit one leg of an acceptance check";
    case Mutation::CheckOnANotApplicableEntry:
        return "an acceptance check on a not-applicable entry";
    case Mutation::OmitAProvenanceField:
        return "omit a provenance field";
    case Mutation::MalformedWindow:
        return "a window outside YYYY-MM-DD..YYYY-MM-DD";
    }
    return "unknown mutation";
}

/// One applied mutation: the mutated model, its rendering, and the defect the check
/// must report against which entry (Requirement 14.11's "naming the offending entry
/// and each missing field").
struct Mutant {
    bool        applicable{true};
    Doc         doc;
    std::string markdown;
    DefectKind  expected{DefectKind::MissingField};
    std::string entry;
};

template <typename Predicate>
[[nodiscard]] std::vector<std::size_t> entriesWhere(const Doc& doc, Predicate predicate) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < doc.entries.size(); ++i) {
        if (predicate(doc.entries[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

[[nodiscard]] bool isRequiredIdentifier(const std::string& identifier) {
    const std::vector<std::string>& required = testsupport::requiredBacklogIdentifiers();
    return std::find(required.begin(), required.end(), identifier) != required.end();
}

/// Pure in (base, kind, choice): `choice` selects the target entry and the variant
/// within the kind by modulo, so the property draws it and the coverage test
/// enumerates it.
[[nodiscard]] Mutant mutate(const Doc& base, Mutation kind, std::size_t choice) {
    Mutant mutant;
    mutant.doc = base;
    Doc& doc = mutant.doc;

    const auto target = [&](const std::vector<std::size_t>& candidates) -> std::size_t {
        if (candidates.empty()) {
            mutant.applicable = false;  // a generator guarantee was broken; callers assert it
            return 0;
        }
        return candidates[choice % candidates.size()];
    };
    const auto anyEntry = [](const Entry&) { return true; };
    const auto required = [](const Entry& entry) {
        return isRequiredIdentifier(entry.identifier);
    };
    const auto carriesCheck = [](const Entry& entry) { return entry.declareCheck; };
    const auto notApplicable = [](const Entry& entry) {
        return entry.disposition == "not-applicable";
    };

    switch (kind) {
    case Mutation::OmitARequiredEntry: {
        const std::size_t i = target(entriesWhere(doc, required));
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = doc.entries[i].identifier;
        doc.entries.erase(doc.entries.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    case Mutation::RenameAnIdentifierOutsideTheRequiredSet: {
        const std::size_t i = target(entriesWhere(doc, required));
        Entry& entry = doc.entries[i];
        mutant.expected = DefectKind::MissingEntry;
        mutant.entry = entry.identifier;  // the identifier that is now absent
        const std::string renamed = "PR 9" + std::to_string(10 + (choice % 80));
        entry.heading = renamed + " \u2014 a renamed entry";
        entry.identifier = renamed;
        break;
    }
    case Mutation::DuplicateAnIdentifier: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::DuplicateIdentifier;
        mutant.entry = doc.entries[i].identifier;
        const Entry copy = doc.entries[i];
        doc.entries.push_back(copy);
        break;
    }
    case Mutation::OmitTheIdentifier: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingField;
        // With no identifier the checker reports the entry by its heading, which
        // still names the identifier the entry was written for.
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].identifier.clear();
        break;
    }
    case Mutation::HeadingThatDoesNotNameTheIdentifier: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].heading = "an entry heading naming no upstream identifier";
        break;
    }
    case Mutation::OmitTheSummary: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].summary.clear();
        break;
    }
    case Mutation::OverlongSummary: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].summary =
            textOfLength("this summary is over the bound Requirement 14.1 fixes",
                         201 + (choice % 5));
        break;
    }
    case Mutation::OmitTheLinuxComponent: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingField;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].component.clear();
        break;
    }
    case Mutation::OmitTheRationale: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingRationale;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].rationale.clear();
        break;
    }
    case Mutation::RationaleThatIsNotASentence: {
        static const std::array<std::string_view, 3> kFragments{"too short", "ports", "see above"};
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::MissingRationale;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].rationale = std::string(kFragments[choice % kFragments.size()]);
        break;
    }
    case Mutation::OmitTheDisposition: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::InvalidStatus;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].disposition.clear();
        break;
    }
    case Mutation::DispositionOutsideItsValueSet: {
        static const std::array<std::string_view, 3> kBogus{"skip", "PORT", "deferred"};
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::InvalidStatus;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].disposition = std::string(kBogus[choice % kBogus.size()]);
        break;
    }
    case Mutation::OmitTheStatus: {
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::InvalidStatus;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].status.clear();
        break;
    }
    case Mutation::StatusOutsideItsValueSet: {
        static const std::array<std::string_view, 3> kBogus{"nearly", "COMPLETE", "done"};
        const std::size_t i = target(entriesWhere(doc, anyEntry));
        mutant.expected = DefectKind::InvalidStatus;
        mutant.entry = doc.entries[i].identifier;
        doc.entries[i].status = std::string(kBogus[choice % kBogus.size()]);
        break;
    }
    case Mutation::OmitTheCheckBlock: {
        const std::size_t i = target(entriesWhere(doc, carriesCheck));
        mutant.expected = DefectKind::MissingCheck;
        mutant.entry = doc.entries[i].identifier;
        if (choice % 2 == 0) {
            clearCheck(doc.entries[i]);  // no `check:` and no legs at all
        } else {
            doc.entries[i].omitCheckKeyword = true;  // legs with nothing declaring them
        }
        break;
    }
    case Mutation::OmitOneLegOfACheck: {
        const std::size_t i = target(entriesWhere(doc, carriesCheck));
        mutant.expected = DefectKind::MissingCheck;
        mutant.entry = doc.entries[i].identifier;
        switch (choice % 3) {
        case 0:
            doc.entries[i].given.clear();
            break;
        case 1:
            doc.entries[i].when.clear();
            break;
        default:
            doc.entries[i].then.clear();
            break;
        }
        break;
    }
    case Mutation::CheckOnANotApplicableEntry: {
        const std::size_t i = target(entriesWhere(doc, notApplicable));
        mutant.expected = DefectKind::MissingCheck;
        mutant.entry = doc.entries[i].identifier;
        fillCheck(doc.entries[i]);  // Requirement 14.3 gives such an entry no check
        break;
    }
    case Mutation::OmitAProvenanceField: {
        static const std::array<std::string_view, 3> kKeys{"upstream-repository", "upstream-range",
                                                           "window"};
        const std::size_t which = choice % kKeys.size();
        mutant.expected = DefectKind::MissingField;
        mutant.entry = std::string(kKeys[which]);
        switch (which) {
        case 0:
            doc.repository.clear();
            break;
        case 1:
            doc.range.clear();
            break;
        default:
            doc.window.clear();
            break;
        }
        break;
    }
    case Mutation::MalformedWindow: {
        static const std::array<std::string_view, 3> kWindows{
            "June to July 2026", "2026-06-25..2026-7-25", "2026-06-25 - 2026-07-25"};
        mutant.expected = DefectKind::MissingField;
        mutant.entry = "window";
        doc.window = std::string(kWindows[choice % kWindows.size()]);
        break;
    }
    }

    mutant.markdown = render(doc);
    return mutant;
}

// ===========================================================================
// The checked-in document, asserted before anything generated
// ===========================================================================

/// Shared by both properties: the checked-in backlog is a case of every run, and
/// what was PARSED out of it is asserted alongside "it has no defects".
void assertCheckedInBacklogIsWellFormed() {
    const Checked& real = checkedInBacklog();
    RC_ASSERT(real.markdownBytes > 0);  // PALMIER_DOCS_DIR/PORT_BACKLOG.md was found
    RC_ASSERT(real.backlog.entries.size() == testsupport::requiredBacklogIdentifiers().size());

    std::set<std::string> identifiers;
    std::size_t           requiringChecks = 0;
    for (const BacklogEntry& entry : real.backlog.entries) {
        RC_ASSERT(!entry.identifier.empty());
        RC_ASSERT(!entry.summary.empty());
        RC_ASSERT(!entry.disposition.empty());
        RC_ASSERT(!entry.rationale.empty());
        RC_ASSERT(!entry.status.empty());
        identifiers.insert(entry.identifier);
        if (entry.requiresCheck()) {
            RC_ASSERT(entry.check.complete());
            ++requiringChecks;
        }
    }
    // Requirement 14.10's uniqueness, and the nine port/adapt entries of the
    // checked-in document (PR 401 is `not-applicable` and carries no check).
    RC_ASSERT(identifiers.size() == real.backlog.entries.size());
    RC_ASSERT(requiringChecks == 9);
    RC_ASSERT(countCompleteChecks(real.backlog) == 9);
    RC_ASSERT(!real.backlog.provenance.upstreamRepository.empty());
    RC_ASSERT(!real.backlog.provenance.upstreamRange.empty());
    RC_ASSERT(real.backlog.provenance.window == "2026-06-25..2026-07-25");
    RC_ASSERT(real.defects.empty());
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 71: Port_Backlog
// well-formedness — for all entries in the Port_Backlog, the entry carries a
// non-empty upstream identifier, a summary of 1-200 characters, exactly one
// disposition from `port`/`adapt`/`not-applicable`, and a non-empty rationale; no
// two entries share an upstream identifier; every `port` or `adapt` entry carries
// at least one acceptance check stating a starting state, an action and an
// observable expected outcome; and every `not-applicable` entry citing a macOS
// framework names that framework and a reason no Linux equivalent is in scope.
//
// The backlog set quantified over is {the checked-in `docs/PORT_BACKLOG.md`} union
// {generated backlogs of 10-40 entries}: the checked-in document is asserted on
// every case, so this property is a statement about the real document as well as
// about the checker, and the generated half is what stops a pass being an accident
// of one document — identifiers, entry order, summaries (including both boundaries
// of the 1-200 bound), dispositions, statuses, components, notes and the
// framework-citing shape of Requirement 14.9 are all drawn.
//
// **Validates: Requirements 14.1, 14.3, 14.9, 14.10**
// ===========================================================================
RC_GTEST_PROP(PortBacklogProperties, EveryWellFormedBacklogPassesTheBacklogCheck, ()) {
    assertCheckedInBacklogIsWellFormed();

    const Doc     doc = drawDoc();
    const Checked generated = checkMarkdown(render(doc));

    // --- non-vacuity: the generated backlog was read back field for field -----
    RC_ASSERT(generated.backlog.entries.size() == doc.entries.size());
    RC_ASSERT(generated.backlog.entries.size() >= testsupport::requiredBacklogIdentifiers().size());
    RC_ASSERT(generated.backlog.provenance.upstreamRepository == doc.repository);
    RC_ASSERT(generated.backlog.provenance.upstreamRange == doc.range);
    RC_ASSERT(generated.backlog.provenance.window == doc.window);

    std::set<std::string> identifiers;
    std::size_t           withChecks = 0;
    std::size_t           citingFramework = 0;
    for (std::size_t i = 0; i < doc.entries.size(); ++i) {
        const Entry&        model = doc.entries[i];
        const BacklogEntry& parsed = generated.backlog.entries[i];
        RC_ASSERT(parsed.heading == model.heading);
        RC_ASSERT(parsed.identifier == model.identifier);
        RC_ASSERT(parsed.summary == model.summary);
        RC_ASSERT(parsed.disposition == model.disposition);
        RC_ASSERT(parsed.linuxComponent == model.component);
        RC_ASSERT(parsed.rationale == model.rationale);
        RC_ASSERT(parsed.status == model.status);
        RC_ASSERT(parsed.note == model.note);
        RC_ASSERT(parsed.check.declared == model.declareCheck);

        // The clauses of Property 71, restated over what the checker accepted.
        RC_ASSERT(!parsed.identifier.empty());
        RC_ASSERT(!parsed.summary.empty());
        RC_ASSERT(parsed.summary.size() <= 200);
        RC_ASSERT(!parsed.rationale.empty());
        RC_ASSERT(!parsed.status.empty());
        RC_ASSERT(std::find(testsupport::backlogDispositionValues().begin(),
                            testsupport::backlogDispositionValues().end(), parsed.disposition)
                  != testsupport::backlogDispositionValues().end());
        RC_ASSERT(identifiers.insert(parsed.identifier).second);  // Requirement 14.10
        if (parsed.requiresCheck()) {
            RC_ASSERT(parsed.check.complete());
            RC_ASSERT(!parsed.check.given.empty());
            RC_ASSERT(!parsed.check.when.empty());
            RC_ASSERT(!parsed.check.then.empty());
            ++withChecks;
        } else {
            RC_ASSERT(!parsed.check.declared);  // Requirement 14.3 gives it no check
        }
        if (!model.framework.empty()) {
            // Requirement 14.9's WHERE clause, for the entries it binds to.
            RC_ASSERT(parsed.disposition == "not-applicable");
            RC_ASSERT(parsed.rationale.find(model.framework) != std::string::npos);
            RC_ASSERT(parsed.rationale.find("no Linux equivalent is in scope")
                      != std::string::npos);
            ++citingFramework;
        }
    }
    // Every required identifier is present, and the generator's guarantees hold, so
    // the clauses above were exercised rather than skipped.
    for (const std::string& identifier : testsupport::requiredBacklogIdentifiers()) {
        RC_ASSERT(identifiers.count(identifier) == 1);
    }
    RC_ASSERT(withChecks >= 2);       // the guaranteed port and adapt entries
    RC_ASSERT(citingFramework >= 1);  // the guaranteed framework-citing entry

    // --- and only then: the check accepts it ----------------------------------
    RC_ASSERT(generated.defects.empty());
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 72: The backlog check detects
// every malformation — for any backlog entry with an omitted identifier, summary,
// disposition or rationale, or an omitted acceptance check while dispositioned
// `port` or `adapt`, the check reports the entry invalid, names every missing
// field, and excludes that entry from the set presented as ported.
//
// Quantified over (generated well-formed backlog) x (19 mutation kinds) x (target
// entry, and the variant within the kind). The mutation is applied to the MODEL and
// the backlog re-rendered, so a mutation is a value rather than a text edit and the
// same 19 kinds apply to every generated backlog. "Not presented as ported" is
// asserted as the checker expresses it: a defective entry is reported, by name, as
// invalid — and for the omitted-check kinds the reported defect is `MissingCheck`,
// which is the checker's statement that the entry's `port`/`adapt` disposition is
// unsupported.
//
// Non-vacuity: the unmutated rendering of the same case is asserted clean, the
// mutant is asserted to have been parsed in full (entry count, every heading read
// back in order, the identifier count, and no "the document holds no entry to
// check" defect), and only then is the expected defect kind asserted AGAINST THE
// OFFENDING ENTRY.
//
// **Validates: Requirements 14.11**
// ===========================================================================
RC_GTEST_PROP(PortBacklogProperties, TheBacklogCheckDetectsEveryMalformation, ()) {
    assertCheckedInBacklogIsWellFormed();

    const Doc base = drawDoc();
    RC_ASSERT(checkMarkdown(render(base)).defects.empty());  // the baseline passes

    const Mutation    kind = kMutations[drawIndex(kMutations.size())];
    const std::size_t choice = drawIndex(64);
    RC_TAG(std::string(describe(kind)));

    const Mutant mutant = mutate(base, kind, choice);
    RC_ASSERT(mutant.applicable);  // every kind has a target in a generated backlog
    RC_ASSERT(!mutant.entry.empty());

    const Checked mutated = checkMarkdown(mutant.markdown);

    // --- non-vacuity: the mutant is still the document it was ------------------
    RC_ASSERT(mutated.backlog.entries.size() == mutant.doc.entries.size());
    std::size_t modelIdentifiers = 0;
    for (std::size_t i = 0; i < mutant.doc.entries.size(); ++i) {
        RC_ASSERT(mutated.backlog.entries[i].heading == mutant.doc.entries[i].heading);
        if (!mutant.doc.entries[i].identifier.empty()) {
            ++modelIdentifiers;
            RC_ASSERT(mutated.backlog.entries[i].identifier == mutant.doc.entries[i].identifier);
        }
    }
    RC_ASSERT(modelIdentifiers + 1 >= mutant.doc.entries.size());  // at most one was omitted
    RC_ASSERT(!testsupport::hasDefect(mutated.defects, DefectKind::MissingEntry, "port backlog"));

    // --- the defect, named against the offending entry ------------------------
    RC_ASSERT(!mutated.defects.empty());
    RC_ASSERT(testsupport::hasDefect(mutated.defects, mutant.expected, mutant.entry));
}

// ===========================================================================
// Full mutation-kind coverage on every run, independent of sampling
// ===========================================================================

TEST(BacklogCheckMutations, EveryMutationKindIsDetectedAndNamesItsEntry) {
    const Doc     base = fixedDoc();
    const Checked baseline = checkMarkdown(render(base));
    ASSERT_TRUE(baseline.defects.empty()) << "the fixed well-formed backlog is not clean:\n"
                                         << testsupport::toString(baseline.defects);
    ASSERT_EQ(baseline.backlog.entries.size(), testsupport::requiredBacklogIdentifiers().size());
    ASSERT_GT(countCompleteChecks(baseline.backlog), 0U);

    std::set<DefectKind> produced;
    for (const Mutation kind : kMutations) {
        for (const std::size_t choice : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                                         std::size_t{7}, std::size_t{13}}) {
            const Mutant mutant = mutate(base, kind, choice);
            ASSERT_TRUE(mutant.applicable) << describe(kind) << " found no target";
            const Checked mutated = checkMarkdown(mutant.markdown);
            EXPECT_TRUE(testsupport::hasDefect(mutated.defects, mutant.expected, mutant.entry))
                << describe(kind) << " (choice " << choice << ", entry `" << mutant.entry
                << "`) was not reported:\n"
                << testsupport::toString(mutated.defects);
            // Still a readable document, so the defect is the injected one.
            EXPECT_EQ(mutated.backlog.entries.size(), mutant.doc.entries.size()) << describe(kind);
            produced.insert(mutant.expected);
        }
    }

    const std::set<DefectKind> expected{DefectKind::MissingEntry, DefectKind::MissingField,
                                       DefectKind::MissingRationale, DefectKind::InvalidStatus,
                                       DefectKind::MissingCheck,
                                       DefectKind::DuplicateIdentifier};
    EXPECT_EQ(produced, expected) << "the mutation set no longer covers this grammar's defect kinds";

    // The three kinds a Port_Backlog cannot express are the Parity_Report's, and
    // Property 70 covers them in tests/docs/parity_report_property_test.cpp. Their
    // union is the whole closed vocabulary of ReportParser.hpp.
    std::set<DefectKind> whole = produced;
    whole.insert(DefectKind::DuplicateEntry);
    whole.insert(DefectKind::InvalidPriority);
    whole.insert(DefectKind::OutOfOrder);
    EXPECT_EQ(whole.size(), 9U);
}

}  // namespace
}  // namespace palmier
