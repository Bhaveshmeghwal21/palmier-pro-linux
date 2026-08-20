// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/OfflineIntentInterpreter.cpp — implementation of the built-in,
// network-free agent interpreter (task 10.1; Requirements 11.2, 11.3, 11.4, 11.8,
// 11.9). See the header for the phrase table and the design rationale.

#include "services/OfflineIntentInterpreter.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Tool names — the Tool_Surface entries the table resolves to. Spelled here
// rather than shared with ToolRegistry.cpp because this component must not depend
// on the registry: the property test is what checks that every name below is
// actually registered (Property 60).
// ---------------------------------------------------------------------------

constexpr std::string_view kSplitClip     = "timeline.split_clip";
constexpr std::string_view kSetTrackMuted = "timeline.set_track_muted";
constexpr std::string_view kAddTrack      = "timeline.add_track";
constexpr std::string_view kDeleteClip    = "timeline.delete_clip";
constexpr std::string_view kMediaImport   = "media.import";
constexpr std::string_view kExport        = "timeline.export";
constexpr std::string_view kProjectSave   = "project.save";
constexpr std::string_view kUndo          = "edit.undo";
constexpr std::string_view kRedo          = "edit.redo";
constexpr std::string_view kReadTimeline  = "timeline.read";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

[[nodiscard]] Error invalidUtterance(std::string message) {
    return makeError(ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] Error missingContext(std::string message) {
    return makeError(ErrorCode::FailedPrecondition, std::move(message));
}

// ---------------------------------------------------------------------------
// Normalization (Requirement 11.3)
// ---------------------------------------------------------------------------

/// The whitespace set trimming and collapsing recognize. Deliberately the ASCII
/// set: the phrase table is fixed English, so no locale-dependent classification
/// is involved and the result is identical on every host.
[[nodiscard]] bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// True when `text` holds nothing but whitespace (so it is "empty after
/// whitespace removal" in Requirement 11.9's words).
[[nodiscard]] bool blank(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char c) { return isSpace(c); });
}

// ---------------------------------------------------------------------------
// Pattern-argument capture
// ---------------------------------------------------------------------------

/// If `normalized` begins with `prefix` followed by a space, the remainder;
/// otherwise nullopt. The remainder is never empty (the normalized form has no
/// trailing whitespace, so a lone prefix has no following space at all).
[[nodiscard]] std::optional<std::string_view> afterPrefix(std::string_view normalized,
                                                          std::string_view prefix) {
    if (normalized.size() <= prefix.size() + 1) return std::nullopt;
    if (normalized.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (normalized[prefix.size()] != ' ') return std::nullopt;
    return normalized.substr(prefix.size() + 1);
}

/// A 1-based ordinal: a run of ASCII digits and nothing else, at least 1, small
/// enough that no project could hold that many tracks either way.
[[nodiscard]] std::optional<std::size_t> parseOrdinal(std::string_view text) {
    if (text.empty() || text.size() > 6) return std::nullopt;
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    if (value == 0) return std::nullopt;
    return value;
}

/// The captured argument of a suffix pattern, taken from the ORIGINAL utterance
/// rather than the normalized one so a path keeps its letter case. The normalized
/// form only decides *where* the argument starts; the bytes come from the input.
///
/// The suffix is located by counting the normalized prefix's words in the original
/// text, which is exact because normalization only removes leading whitespace,
/// collapses interior runs and lowercases — it never reorders or drops words.
[[nodiscard]] std::string originalSuffix(std::string_view utterance, std::size_t wordsToSkip) {
    std::size_t index = 0;
    for (std::size_t word = 0; word < wordsToSkip; ++word) {
        while (index < utterance.size() && isSpace(utterance[index])) ++index;
        while (index < utterance.size() && !isSpace(utterance[index])) ++index;
    }
    while (index < utterance.size() && isSpace(utterance[index])) ++index;

    std::string_view tail = utterance.substr(std::min(index, utterance.size()));
    while (!tail.empty() && isSpace(tail.back())) tail.remove_suffix(1);
    return std::string(tail);
}

/// How many space-separated words `text` holds. `text` is a canonical pattern
/// prefix, so this is just a space count plus one.
[[nodiscard]] std::size_t wordCount(std::string_view text) {
    if (text.empty()) return 0;
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), ' '));
}

// ---------------------------------------------------------------------------
// Argument-object construction per pattern
// ---------------------------------------------------------------------------

/// `timeline.split_clip` needs the selected clip and the playhead.
[[nodiscard]] Result<Json> splitArgs(const EditorContext& context) {
    if (!context.selectedClipId.has_value()) {
        return err<Json>(missingContext(
            "'split the clip at the playhead' needs a selected clip, and no clip is "
            "selected"));
    }
    if (context.playheadNs < 0) {
        return err<Json>(missingContext(
            "'split the clip at the playhead' needs a non-negative playhead position"));
    }
    Json args = Json::object();
    args.set("clipId", context.selectedClipId->toString());
    args.set("playheadNs", context.playheadNs);
    return args;
}

/// `timeline.set_track_muted` needs the Nth track's identifier.
[[nodiscard]] Result<Json> muteArgs(const EditorContext& context, std::size_t ordinal,
                                    bool muted) {
    if (ordinal > context.trackIds.size()) {
        return err<Json>(missingContext(
            "track " + std::to_string(ordinal) + " does not exist: the current project has " +
            std::to_string(context.trackIds.size()) + " track(s)"));
    }
    Json args = Json::object();
    args.set("trackId", context.trackIds[ordinal - 1].toString());
    args.set("muted", muted);
    return args;
}

/// `timeline.delete_clip` needs the selected clip.
[[nodiscard]] Result<Json> deleteArgs(const EditorContext& context) {
    if (!context.selectedClipId.has_value()) {
        return err<Json>(missingContext(
            "'delete the selected clip' needs a selected clip, and no clip is selected"));
    }
    Json args = Json::object();
    args.set("clipId", context.selectedClipId->toString());
    return args;
}

/// `project.save` uses the recorded document path; omitting `path` is what tells
/// the tool to use that path itself, so an unsaved project produces no `path` and
/// the tool reports the missing destination.
[[nodiscard]] Json saveArgs(const EditorContext& context) {
    Json args = Json::object();
    if (context.documentPath.has_value()) {
        args.set("path", context.documentPath->string());
    }
    return args;
}

}  // namespace

// ---------------------------------------------------------------------------
// utteranceRangeText
// ---------------------------------------------------------------------------

std::string utteranceRangeText() {
    return std::to_string(kMinUtteranceChars) + " to " + std::to_string(kMaxUtteranceChars) +
           " characters";
}

// ---------------------------------------------------------------------------
// PhrasePattern
// ---------------------------------------------------------------------------

std::string PhrasePattern::display() const {
    switch (match) {
        case PhraseMatch::Exact:   return std::string(text);
        case PhraseMatch::Ordinal: return std::string(text) + " N";
        case PhraseMatch::Suffix:  return std::string(text) + " <path>";
    }
    return std::string(text);
}

std::string PhrasePattern::canonicalUtterance(std::size_t ordinal,
                                              std::string_view argument) const {
    switch (match) {
        case PhraseMatch::Exact:
            return std::string(text);
        case PhraseMatch::Ordinal:
            return std::string(text) + " " + std::to_string(ordinal == 0 ? 1 : ordinal);
        case PhraseMatch::Suffix:
            return std::string(text) + " " +
                   std::string(argument.empty() ? std::string_view("/tmp/palmier.mp4")
                                                : argument);
    }
    return std::string(text);
}

// ---------------------------------------------------------------------------
// The documented phrase table (design.md D9)
// ---------------------------------------------------------------------------
//
// `export as mp4 to` deliberately precedes nothing else that shares its prefix,
// and the two ordinal patterns are distinct prefixes ("unmute track" is not a
// suffix of "mute track" because matching anchors at the start), so the table is
// unambiguous: at most one pattern can match a given normalized utterance. The
// order below is the documentation order, not a precedence order.

const std::vector<PhrasePattern>& phrasePatterns() {
    static const std::vector<PhrasePattern> table = {
        {"split the clip at the playhead", PhraseMatch::Exact, kSplitClip,
         "Split the selected clip at the current playhead position."},
        {"mute track", PhraseMatch::Ordinal, kSetTrackMuted,
         "Mute the Nth track of the current project (1-based, in timeline order)."},
        {"unmute track", PhraseMatch::Ordinal, kSetTrackMuted,
         "Unmute the Nth track of the current project (1-based, in timeline order)."},
        {"add a video track", PhraseMatch::Exact, kAddTrack, "Append a video track."},
        {"add an audio track", PhraseMatch::Exact, kAddTrack, "Append an audio track."},
        {"delete the selected clip", PhraseMatch::Exact, kDeleteClip,
         "Delete the currently selected clip."},
        {"import", PhraseMatch::Suffix, kMediaImport,
         "Import the media file at the given path into the media library."},
        {"export as mp4 to", PhraseMatch::Suffix, kExport,
         "Export the timeline as an MP4 file to the given path."},
        {"save the project", PhraseMatch::Exact, kProjectSave,
         "Save the project to its recorded document path."},
        {"undo", PhraseMatch::Exact, kUndo, "Revert the most recent edit."},
        {"redo", PhraseMatch::Exact, kRedo, "Re-apply the most recently undone edit."},
        {"show the timeline", PhraseMatch::Exact, kReadTimeline,
         "Read the current timeline (tracks, clips, effects, transitions)."},
    };
    return table;
}

// ---------------------------------------------------------------------------
// makeSessionEditorContextProvider
// ---------------------------------------------------------------------------

EditorContextProvider makeSessionEditorContextProvider(
    ProjectSession& session, std::function<std::optional<Uuid>()> selection,
    std::function<std::int64_t()> playhead) {
    return [&session, selection = std::move(selection),
            playhead = std::move(playhead)]() -> EditorContext {
        EditorContext context;
        // Read through the session, per invocation, for the same reason every tool
        // handler does (design.md D1): a project opened after this provider was
        // built must be the one a phrase resolves against.
        const Project project = session.engine().snapshot();
        context.trackIds.reserve(project.tracks.size());
        for (const Track& track : project.tracks) context.trackIds.push_back(track.id);
        context.documentPath = session.documentPath();
        if (selection) context.selectedClipId = selection();
        if (playhead) context.playheadNs = playhead();
        return context;
    };
}

// ---------------------------------------------------------------------------
// OfflineIntentInterpreter
// ---------------------------------------------------------------------------

OfflineIntentInterpreter::OfflineIntentInterpreter(Options options)
    : options_(std::move(options)) {}

const std::vector<PhrasePattern>& OfflineIntentInterpreter::patterns() {
    return phrasePatterns();
}

EditorContext OfflineIntentInterpreter::context() const {
    return options_.context ? options_.context() : EditorContext{};
}

std::string OfflineIntentInterpreter::normalize(std::string_view utterance) {
    std::string out;
    out.reserve(utterance.size());
    bool pendingSpace = false;
    for (const char c : utterance) {
        if (isSpace(c)) {
            // A run of any whitespace becomes at most one space, and a run at the
            // start or the end becomes nothing at all (the flag is only flushed
            // when a non-space follows, and `out` starts empty).
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(lowerAscii(c));
    }
    return out;
}

Result<AgentIntent> OfflineIntentInterpreter::interpret(std::string_view utterance) const {
    // --- Requirement 11.9: the length bounds, checked before anything else -----
    //
    // Both arms name the permitted range, because the caller's remedy is the same
    // in each case: submit something inside it.
    if (utterance.size() > kMaxUtteranceChars) {
        return err<AgentIntent>(invalidUtterance(
            "the utterance is " + std::to_string(utterance.size()) +
            " characters; an utterance must be " + utteranceRangeText()));
    }
    if (blank(utterance)) {
        return err<AgentIntent>(invalidUtterance(
            "the utterance is empty after whitespace removal; an utterance must be " +
            utteranceRangeText()));
    }

    const std::string normalized = normalize(utterance);

    // --- The table lookup -----------------------------------------------------
    for (const PhrasePattern& pattern : phrasePatterns()) {
        switch (pattern.match) {
            case PhraseMatch::Exact: {
                if (normalized != pattern.text) continue;
                Json args = Json::object();
                if (pattern.toolName == kAddTrack) {
                    args.set("kind", std::string(pattern.text == "add a video track" ? "video"
                                                                                    : "audio"));
                } else if (pattern.toolName == kSplitClip) {
                    Result<Json> built = splitArgs(context());
                    if (built.isError()) return err<AgentIntent>(std::move(built).error());
                    args = std::move(built).value();
                } else if (pattern.toolName == kDeleteClip) {
                    Result<Json> built = deleteArgs(context());
                    if (built.isError()) return err<AgentIntent>(std::move(built).error());
                    args = std::move(built).value();
                } else if (pattern.toolName == kProjectSave) {
                    args = saveArgs(context());
                }
                // `edit.undo`, `edit.redo` and `timeline.read` take no arguments,
                // so the empty object is already correct for them.
                return AgentIntent{std::string(pattern.toolName), std::move(args)};
            }

            case PhraseMatch::Ordinal: {
                const std::optional<std::string_view> tail =
                    afterPrefix(normalized, pattern.text);
                if (!tail.has_value()) continue;
                const std::optional<std::size_t> ordinal = parseOrdinal(*tail);
                if (!ordinal.has_value()) continue;  // "mute track left" is not this pattern
                Result<Json> built =
                    muteArgs(context(), *ordinal, pattern.text == "mute track");
                if (built.isError()) return err<AgentIntent>(std::move(built).error());
                return AgentIntent{std::string(pattern.toolName), std::move(built).value()};
            }

            case PhraseMatch::Suffix: {
                const std::optional<std::string_view> tail =
                    afterPrefix(normalized, pattern.text);
                if (!tail.has_value()) continue;
                // The argument is taken from the ORIGINAL utterance so a path keeps
                // its case; the normalized match only established where it starts.
                const std::string argument =
                    originalSuffix(utterance, wordCount(pattern.text));
                if (argument.empty()) continue;

                Json args = Json::object();
                if (pattern.toolName == kExport) {
                    args.set("outputPath", argument);
                    args.set("format", std::string("mp4"));
                } else {
                    args.set("path", argument);
                }
                return AgentIntent{std::string(pattern.toolName), std::move(args)};
            }
        }
    }

    // --- Requirement 11.4: quote the unrecognised request, invoke no tool ------
    //
    // The utterance is quoted as submitted, not as normalized, so the user sees
    // what they typed. Returning an error is the whole of "invoke no tool": this
    // component never executes anything, so there is nothing to undo and the
    // project cannot have been touched.
    return err<AgentIntent>(invalidUtterance(
        "the request \"" + std::string(utterance) +
        "\" does not match any known offline command phrase; no tool was invoked"));
}

IntentInterpreter OfflineIntentInterpreter::asInterpreter() const {
    return [self = *this](std::string_view message) -> Result<AgentIntent> {
        return self.interpret(message);
    };
}

IntentInterpreter makeOfflineIntentInterpreter(OfflineIntentInterpreter::Options options) {
    return OfflineIntentInterpreter(std::move(options)).asInterpreter();
}

}  // namespace palmier::services
