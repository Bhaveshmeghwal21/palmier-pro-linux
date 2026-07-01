// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/MentionResolver.hpp — @-mention resolution for the in-app agent chat
// (task 16.2; design.md "Component 6: In-App Agent Orchestrator").
//
// Requirement 8 gives @-mentions their behaviour:
//
//   * 8.2  WHEN a user references a media item with an @ mention, THE Agent_Chat
//          SHALL resolve the mention to the referenced media item that exists in
//          the current project.
//   * 8.3  IF an @ mention matches NO media item in the current project, THEN the
//          reference is rejected with an error indicating the item was not found,
//          WITHOUT submitting the message for processing.
//   * 8.4  IF an @ mention matches MORE THAN ONE media item, THEN the user is
//          prompted to select a single item from the matching candidates BEFORE
//          the message is submitted for processing.
//
// This component is the concrete transform that plugs into the orchestrator's
// `MessagePreprocessor` seam (AgentOrchestrator.hpp): the preprocessor runs
// BEFORE interpretation, and returning an error from it rejects the message so it
// is never submitted — exactly the "without submitting the message" contract of
// 8.3 and 8.4.
//
// A "media item" is an entry in the project's media library (MediaManager /
// Project.assets — a MediaAssetRef). Since a MediaAssetRef carries a stable
// `assetId` and an informational `sourcePath`, a mention is matched (case-
// insensitively) against the item's human-facing names derived from that path —
// its file name (`clip.mp4`), its stem (`clip`), and the full path — as well as
// its canonical asset-id string. On a unique match, the mention token is
// rewritten in place to a canonical `@<assetId>` reference the downstream intent
// interpreter can consume unambiguously (8.2).
//
// The type is deliberately UI-agnostic and dependency-light (core value types +
// the service-layer Json/AgentOrchestrator seam only): the composition root
// (task 21.1) wires a preprocessor built from the live MediaManager into the
// orchestrator, and the UI renders the ambiguous-candidate prompt from the
// structured result.

#ifndef PALMIER_SERVICES_MENTIONRESOLVER_HPP
#define PALMIER_SERVICES_MENTIONRESOLVER_HPP

#include <string>
#include <vector>

#include "core/MediaAssetRef.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"  // MessagePreprocessor seam.

namespace palmier {
class MediaManager;  // core/MediaManager.hpp
struct Project;      // core/Project.hpp
}  // namespace palmier

namespace palmier::services {

// ---------------------------------------------------------------------------
// Result model
// ---------------------------------------------------------------------------

/// One @ mention that resolved to exactly one media item (Req 8.2).
struct ResolvedMention {
    std::string mention;     ///< The name as written by the user (without '@').
    Uuid        assetId;     ///< The resolved media item's stable identity.
    std::string sourcePath;  ///< The resolved item's source path (informational).
};

/// A media item that a mention could refer to — surfaced for the selection
/// prompt when a mention is ambiguous (Req 8.4).
struct MentionCandidate {
    Uuid        assetId;      ///< The candidate item's stable identity.
    std::string sourcePath;   ///< The candidate item's source path.
    std::string displayName;  ///< The item's file name (basename) for display.
};

/// Outcome kind of resolving all mentions in a message.
enum class MentionStatus {
    Resolved,   ///< Every mention (if any) resolved to exactly one item.
    Unmatched,  ///< A mention matched no media item (Req 8.3 — reject).
    Ambiguous,  ///< A mention matched more than one item (Req 8.4 — prompt).
};

/// The structured outcome of resolving a message's @ mentions.
///
/// On MentionStatus::Resolved, `rewrittenMessage` is the message with every
/// mention rewritten to a canonical `@<assetId>` reference and `resolved` lists
/// each resolution. On Unmatched, `problemMention` names the offending mention.
/// On Ambiguous, `problemMention` names the ambiguous mention and `candidates`
/// lists the media items to choose between.
struct MentionResolution {
    MentionStatus                 status = MentionStatus::Resolved;
    std::string                   rewrittenMessage;  ///< Valid on Resolved.
    std::vector<ResolvedMention>  resolved;          ///< Valid on Resolved.
    std::string                   problemMention;    ///< Valid on Unmatched/Ambiguous.
    std::vector<MentionCandidate> candidates;        ///< Valid on Ambiguous.

    [[nodiscard]] bool isResolved() const noexcept { return status == MentionStatus::Resolved; }
};

// ---------------------------------------------------------------------------
// MentionResolver
// ---------------------------------------------------------------------------

/// Resolves @ mentions in an agent chat message against a snapshot of the
/// project's media library.
///
/// The snapshot is taken at construction, so a resolver is a cheap, deterministic
/// value (ideal for tests and for a single send). The composition root builds a
/// fresh resolver per message via makeMentionPreprocessor(), so mentions always
/// resolve against the CURRENT library.
class MentionResolver {
public:
    /// Build a resolver over an explicit set of media items.
    explicit MentionResolver(std::vector<MediaAssetRef> mediaItems);

    /// Snapshot the media library of a MediaManager.
    [[nodiscard]] static MentionResolver fromMediaManager(const MediaManager& manager);

    /// Snapshot the media items referenced by a Project (Project.assets).
    [[nodiscard]] static MentionResolver fromProject(const Project& project);

    /// Parse and resolve every @ mention in `message`.
    ///
    /// Mentions are processed left to right. A message with no mentions resolves
    /// trivially (rewrittenMessage == message). The FIRST mention that matches no
    /// item yields Unmatched (8.3); the first that matches multiple items yields
    /// Ambiguous with its candidates (8.4). Otherwise every mention is rewritten
    /// to its canonical `@<assetId>` form and the result is Resolved (8.2).
    [[nodiscard]] MentionResolution resolve(const std::string& message) const;

    /// The media items the resolver matches against.
    [[nodiscard]] const std::vector<MediaAssetRef>& mediaItems() const noexcept {
        return mediaItems_;
    }

    // --- Parsing (exposed for testing) -------------------------------------

    /// A single @ mention located in a message.
    struct Mention {
        std::string name;   ///< The referenced name (without '@'/brackets/quotes).
        std::size_t begin;  ///< Byte offset of the leading '@'.
        std::size_t end;    ///< Byte offset one past the mention token.
    };

    /// Extract every @ mention token from `message`, in order of appearance.
    ///
    /// A mention begins with '@' at the start of the message or immediately after
    /// whitespace or an opening bracket/quote. The name is either a bracketed /
    /// quoted run (`@"my clip.mp4"`, `@[my clip.mp4]`, `@{my clip.mp4}`) allowing
    /// spaces, or a bare run of name characters (letters, digits, `. _ - / :`).
    [[nodiscard]] static std::vector<Mention> parseMentions(const std::string& message);

private:
    std::vector<MediaAssetRef> mediaItems_;
};

// ---------------------------------------------------------------------------
// MessagePreprocessor adapters (the orchestrator seam)
// ---------------------------------------------------------------------------

/// Map a MentionResolution onto the preprocessor contract:
///   * Resolved  -> ok(rewrittenMessage)
///   * Unmatched -> NotFound error (Req 8.3): the media item was not found; the
///                  message is not submitted.
///   * Ambiguous -> FailedPrecondition error (Req 8.4): a selection is required;
///                  the message lists the candidates and is not submitted.
[[nodiscard]] Result<std::string> toPreprocessorResult(const MentionResolution& resolution);

/// A MessagePreprocessor that resolves mentions against an explicit media-item
/// snapshot. Suitable when the library is fixed for the lifetime of the send.
[[nodiscard]] MessagePreprocessor makeMentionPreprocessor(std::vector<MediaAssetRef> mediaItems);

/// A MessagePreprocessor that resolves mentions against the LIVE MediaManager.
///
/// The returned function captures `manager` by reference and takes a fresh
/// library snapshot on every message, so mentions always resolve against the
/// current project state. `manager` must outlive the orchestrator that installs
/// the preprocessor (the composition root, task 21.1, owns both).
[[nodiscard]] MessagePreprocessor makeMentionPreprocessor(const MediaManager& manager);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MENTIONRESOLVER_HPP
