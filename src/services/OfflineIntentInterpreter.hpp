// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/OfflineIntentInterpreter.hpp — the built-in, network-free agent
// interpreter (task 10.1; design.md D9; Requirements 11.2, 11.3, 11.4, 11.8,
// 11.9).
//
// Why this exists
// ---------------
// `makeUnconfiguredInterpreter()` used to be the composition root's default
// `IntentInterpreter`: it returned `FailedPrecondition` for every utterance, so a
// build with no model backend had an inert agent. Requirement 11.3 asks for the
// opposite default — a *real*, useful interpreter that works with no hosted
// account, no BYOK credentials and no network at all:
//
//   "WHERE no hosted account and no BYOK credentials are present, THE
//    Agent_Interpreter SHALL use a built-in offline interpreter that maps a
//    documented set of at least 10 command phrases ... to tool invocations in
//    Offline_Mode, matching phrases without regard to letter case or leading and
//    trailing whitespace, returning a mapping within 1 second, and issuing no
//    network request."
//
// So this is the default the registry installs, and the only interpreter that is
// always available.
//
// The documented phrase table (design.md D9 fixes this set)
// ---------------------------------------------------------
// Twelve patterns, each resolving to exactly one tool in the Tool_Surface:
//
//   | # | phrase                          | tool                       | arguments |
//   |---|---------------------------------|----------------------------|-----------|
//   | 1 | split the clip at the playhead  | `timeline.split_clip`      | `clipId` = the selected clip, `playheadNs` = the playhead |
//   | 2 | mute track N                    | `timeline.set_track_muted` | `trackId` = the Nth track, `muted` = true |
//   | 3 | unmute track N                  | `timeline.set_track_muted` | `trackId` = the Nth track, `muted` = false |
//   | 4 | add a video track               | `timeline.add_track`       | `kind` = "video" |
//   | 5 | add an audio track              | `timeline.add_track`       | `kind` = "audio" |
//   | 6 | delete the selected clip        | `timeline.delete_clip`     | `clipId` = the selected clip |
//   | 7 | import <path>                   | `media.import`             | `path` = the captured path |
//   | 8 | export as mp4 to <path>         | `timeline.export`          | `outputPath` = the captured path, `format` = "mp4" |
//   | 9 | save the project                | `project.save`             | `path` = the project's document path, omitted when it has none |
//   |10 | undo                            | `edit.undo`                | none |
//   |11 | redo                            | `edit.redo`                | none |
//   |12 | show the timeline               | `timeline.read`            | none |
//
// `patterns()` publishes the table so the phrase set is documented in code as
// well as in prose, and so a test can quantify over it rather than restate it.
//
// Matching (Requirement 11.3)
// ---------------------------
// The utterance is whitespace-trimmed and lowercased, and its interior runs of
// whitespace — spaces, tabs, newlines, carriage returns, vertical tabs and form
// feeds — are collapsed to single spaces before a pattern is tried. That is what
// makes "  SPLIT   The\tClip At The\nPlayhead  " and
// "split the clip at the playhead" the same request. The comparison is over
// ASCII case only: this is a fixed English phrase table, not a locale-aware one.
//
// Context (why an interpreter needs a seam onto the editor)
// --------------------------------------------------------
// Half the table cannot be resolved from the utterance alone: "split the clip at
// the playhead" names neither a clip nor a position, "delete the selected clip"
// names no clip, "mute track 2" names an ordinal rather than an identifier, and
// "save the project" has no destination. Those facts live in the editor, so they
// arrive through an injected `EditorContextProvider` which is read once per
// interpretation. When the context cannot answer — nothing is selected, the
// project has fewer than N tracks — the interpreter returns a
// `FailedPrecondition` naming what was missing and invokes no tool, which is the
// same shape Requirement 11.4 asks for on an unmappable utterance.
//
// No network (Requirement 11.3), and how that is *proven*
// ------------------------------------------------------
// This translation is a table lookup over a string: nothing here opens a socket,
// resolves a name or reads a credential. To make that a checked property rather
// than a claim, `Options::network` is the single declared route to the network
// for any interpreter built on this component — a seam an implementation would
// have to call to reach the outside world. The offline interpreter never calls
// it, and the property test installs a gate that fails the test if it ever is
// called. The test additionally interposes the C library's socket entry points
// for the duration of each interpretation, so a network call made *around* the
// seam is caught too.
//
// Length bounds (Requirement 11.9)
// --------------------------------
// An utterance that is empty after whitespace removal, or longer than
// `kMaxUtteranceChars` (2000) characters, is rejected with an `InvalidArgument`
// error that states the permitted 1-2000 character range. The length is measured
// on the utterance as submitted, which is what "exceeds 2000 characters" says;
// emptiness is measured after whitespace removal, which is what "empty after
// whitespace removal" says.

#ifndef PALMIER_SERVICES_OFFLINEINTENTINTERPRETER_HPP
#define PALMIER_SERVICES_OFFLINEINTENTINTERPRETER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"  // AgentIntent, IntentInterpreter

namespace palmier::services {

class ProjectSession;  // services/ProjectSession.hpp

// ---------------------------------------------------------------------------
// Utterance bounds (Requirement 11.9)
// ---------------------------------------------------------------------------

/// The shortest accepted utterance, measured after whitespace removal.
inline constexpr std::size_t kMinUtteranceChars = 1;

/// The longest accepted utterance, measured as submitted.
inline constexpr std::size_t kMaxUtteranceChars = 2000;

/// The permitted range, rendered exactly as the rejection message states it
/// ("1 to 2000 characters"). Shared so the message and the constants agree.
[[nodiscard]] std::string utteranceRangeText();

// ---------------------------------------------------------------------------
// EditorContext — the editor facts a phrase may refer to
// ---------------------------------------------------------------------------

/// What the interpreter is allowed to know about the editor. Read once per
/// interpretation, so a phrase always resolves against the state at submission.
///
/// Every field is optional in the sense that a default-constructed context is
/// legal: it simply makes the phrases that need a fact unresolvable, and each of
/// those reports the missing fact by name.
struct EditorContext {
    /// The clip the user has selected, if any. Needed by "split the clip at the
    /// playhead" and "delete the selected clip".
    std::optional<Uuid> selectedClipId;

    /// The playhead position in nanoseconds. Needed by "split the clip at the
    /// playhead". Never negative.
    std::int64_t playheadNs = 0;

    /// The current project's track identifiers **in project order**. "mute track
    /// N" addresses the Nth entry, 1-based, exactly as the timeline shows them.
    std::vector<Uuid> trackIds;

    /// Where the project is stored, when it has been saved before. "save the
    /// project" omits `path` when this is absent, which is precisely what
    /// `project.save` treats as "use the recorded document path".
    std::optional<std::filesystem::path> documentPath;
};

/// Supplies the editor context for one interpretation. Left empty it means "an
/// empty context", i.e. no selection, no tracks, playhead at zero and no document
/// path.
using EditorContextProvider = std::function<EditorContext()>;

/// Build a provider reading the live context from `session`: its project's tracks
/// in order and its document path. Selection and the playhead are not session
/// facts — they belong to the shell — so they are passed in and are constant for
/// the provider's lifetime unless the caller supplies its own provider. `session`
/// must outlive the returned provider.
[[nodiscard]] EditorContextProvider makeSessionEditorContextProvider(
    ProjectSession& session, std::function<std::optional<Uuid>()> selection = {},
    std::function<std::int64_t()> playhead = {});

// ---------------------------------------------------------------------------
// The network seam
// ---------------------------------------------------------------------------

/// The single declared route to the network for an interpreter. An implementation
/// that needs to contact a service calls this with the endpoint it wants; the
/// offline interpreter never does. Returning an error means "not permitted".
///
/// Left empty it means "no network is available", which is the Offline_Mode
/// default: any caller would be refused.
using InterpreterNetworkGate = std::function<Result<void>(std::string_view endpoint)>;

// ---------------------------------------------------------------------------
// The phrase table
// ---------------------------------------------------------------------------

/// How a pattern's text relates to the utterance.
enum class PhraseMatch {
    Exact,   ///< the normalized utterance equals the pattern text
    Ordinal, ///< the pattern text is a prefix followed by a 1-based ordinal
    Suffix,  ///< the pattern text is a prefix followed by a free-form argument
};

/// One documented phrase pattern. `text` is the pattern in its canonical
/// (lowercase, single-spaced) form, which is also the form `PhraseMatch::Exact`
/// compares against and the form a test perturbs.
struct PhrasePattern {
    std::string_view text;      ///< e.g. "mute track" or "undo"
    PhraseMatch      match = PhraseMatch::Exact;
    std::string_view toolName;  ///< the Tool_Surface tool it resolves to
    std::string_view summary;   ///< one line, for documentation and diagnostics

    /// The pattern as a user would type it, with the argument placeholder shown:
    /// "mute track N", "import <path>", "undo".
    [[nodiscard]] std::string display() const;

    /// A representative utterance in canonical form: the pattern text with a
    /// concrete argument substituted (`ordinal` for an ordinal pattern,
    /// `argument` for a suffix pattern). This is what a case- and
    /// whitespace-perturbation test starts from.
    [[nodiscard]] std::string canonicalUtterance(std::size_t ordinal = 1,
                                                 std::string_view argument = "") const;
};

/// The documented phrase table, in the order design.md D9 lists it. Exactly the
/// 12 patterns Requirement 11.3's "documented set of at least 10 command phrases"
/// refers to.
[[nodiscard]] const std::vector<PhrasePattern>& phrasePatterns();

// ---------------------------------------------------------------------------
// OfflineIntentInterpreter
// ---------------------------------------------------------------------------

/// The built-in offline interpreter: a phrase table over a normalized utterance,
/// resolving to a tool invocation with captured arguments. Pure translation — it
/// executes nothing, touches no project and opens no socket, so a rejected or
/// unmappable utterance cannot have changed anything by construction
/// (Requirement 11.4).
class OfflineIntentInterpreter {
public:
    struct Options {
        /// Where the editor facts come from. Empty means an empty context.
        EditorContextProvider context;

        /// The only sanctioned route to the network (see the file comment). The
        /// offline interpreter never calls it; it is declared so that "issues no
        /// network request" is a checkable property of this component.
        InterpreterNetworkGate network;
    };

    OfflineIntentInterpreter() = default;
    explicit OfflineIntentInterpreter(Options options);

    /// Translate `utterance` into exactly one tool invocation.
    ///
    ///   * empty after whitespace removal, or longer than 2000 characters ->
    ///     `InvalidArgument` stating the permitted 1-2000 character range
    ///     (Requirement 11.9);
    ///   * matches no pattern -> `InvalidArgument` **quoting the utterance**
    ///     (Requirement 11.4);
    ///   * matches a pattern whose argument the context cannot supply ->
    ///     `FailedPrecondition` naming the missing fact;
    ///   * otherwise -> the tool name and its argument object.
    ///
    /// Never blocks: no I/O, no waiting, no allocation beyond the strings it
    /// builds, so the 1-second bound of Requirement 11.3 is met by construction.
    [[nodiscard]] Result<AgentIntent> interpret(std::string_view utterance) const;

    /// The same translation as an `IntentInterpreter`, which is what the
    /// orchestrator and the composition root consume.
    [[nodiscard]] IntentInterpreter asInterpreter() const;

    /// The documented phrase table (see `phrasePatterns()`).
    [[nodiscard]] static const std::vector<PhrasePattern>& patterns();

    /// Normalize `utterance` the way matching does: trim, lowercase (ASCII) and
    /// collapse interior whitespace runs to single spaces. Exposed because it is
    /// the definition of "without regard to letter case or leading and trailing
    /// whitespace" the property test quantifies over.
    [[nodiscard]] static std::string normalize(std::string_view utterance);

private:
    /// The editor context for this interpretation: the provider's answer, or an
    /// empty context when no provider was installed.
    [[nodiscard]] EditorContext context() const;

    Options options_;
};

/// The offline interpreter as a ready-to-install `IntentInterpreter`.
[[nodiscard]] IntentInterpreter makeOfflineIntentInterpreter(
    OfflineIntentInterpreter::Options options = {});

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_OFFLINEINTENTINTERPRETER_HPP
