// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/MediaManager.hpp — the project media library and per-clip version history.
//
// The Media_Manager "imports, catalogs, stores, and versions media assets within
// a project" (design.md Glossary / Architecture). This component owns two pieces
// of state that the rest of the editor builds on:
//
//   1. The media library — the catalog of imported and generated media assets
//      (MediaAssetRef) available for placement on the timeline (Requirement 3.1).
//      Importing a media asset adds it to the library and makes it available; a
//      clip may only be backed by an asset that resolves in the library, mirroring
//      the project validation rule that "every Clip.assetRef resolves to an entry
//      in assets".
//
//   2. Per-clip version history — when a generated clip replaces an existing clip,
//      the prior version is retained as a SELECTABLE version of that clip, and at
//      least the 10 most recent versions are preserved (Requirement 3.4). This lets
//      the user roll a clip back to a previous take (imported original or an earlier
//      generation) without re-importing or re-generating it.
//
// This type is pure domain data: it depends only on the core value types (no
// FFmpeg, no GPU, no Qt), so it can be driven headlessly by the UI, the MCP
// server, and the in-app agent alike. The concrete media decoding/generation that
// produces the assets catalogued here lives in the Media Engine / Generative
// client (separate tasks); MediaManager records and versions their references.
//
// Design property P12 (generated-clip version retention, Requirement 3.4) gets a
// dedicated property test in a later task; this header provides the mechanism.

#ifndef PALMIER_CORE_MEDIAMANAGER_HPP
#define PALMIER_CORE_MEDIAMANAGER_HPP

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"

namespace palmier {

/// A single retained version of a clip's content.
///
/// A version captures everything needed to restore what a clip presented at a
/// point in its history: the backing media asset and the source in/out range
/// within that media. `generated` records whether this version came from a
/// generative replacement (true) or from the clip's imported original / a
/// user-placed asset (false).
struct ClipVersion {
    MediaAssetRef assetRef;         ///< The media backing this version (resolves in the library).
    Duration      sourceIn;         ///< In-point within the source media.
    Duration      sourceOut;        ///< Out-point within the source media (> sourceIn).
    bool          generated = false;///< True iff produced by a generative replacement.

    /// Duration this version occupies from its source: sourceOut - sourceIn.
    [[nodiscard]] Duration duration() const noexcept { return sourceOut - sourceIn; }

    [[nodiscard]] friend bool operator==(const ClipVersion& a, const ClipVersion& b) noexcept {
        return a.assetRef == b.assetRef && a.sourceIn == b.sourceIn &&
               a.sourceOut == b.sourceOut && a.generated == b.generated;
    }
    [[nodiscard]] friend bool operator!=(const ClipVersion& a, const ClipVersion& b) noexcept {
        return !(a == b);
    }
};

/// The media library and per-clip generated-clip version history for a project.
///
/// Instances are cheap value types with no external dependencies; a project owns
/// one. Mutating operations return Result<void> and never partially apply: on
/// failure the manager is left unchanged.
class MediaManager {
public:
    /// The minimum number of most-recent versions the manager guarantees to retain
    /// for a clip (Requirement 3.4: "at least the 10 most recent versions"). The
    /// retention capacity can never be configured below this floor.
    static constexpr std::size_t kMinRetainedVersions = 10;

    /// Constructs a manager whose per-clip history retains up to `retentionCapacity`
    /// most-recent versions. Any value below kMinRetainedVersions is raised to the
    /// floor, so the "at least 10 most recent" guarantee always holds.
    explicit MediaManager(std::size_t retentionCapacity = kMinRetainedVersions);

    // --- Media library (Requirement 3.1) -----------------------------------

    /// Import a media asset into the library, making it available for placement on
    /// the timeline.
    ///
    /// Returns:
    ///   * ErrorCode::InvalidArgument if the reference names the nil asset id;
    ///   * ErrorCode::AlreadyExists   if an asset with the same id is already in
    ///                                the library;
    ///   * ok() on success.
    [[nodiscard]] Result<void> importAsset(const MediaAssetRef& asset);

    /// True iff an asset with `assetId` is present in the library.
    [[nodiscard]] bool hasAsset(const Uuid& assetId) const;

    /// The library entry for `assetId`, or std::nullopt if it is not present.
    [[nodiscard]] std::optional<MediaAssetRef> asset(const Uuid& assetId) const;

    /// The full library, in import order.
    [[nodiscard]] const std::vector<MediaAssetRef>& library() const noexcept { return library_; }

    /// Number of assets in the library.
    [[nodiscard]] std::size_t assetCount() const noexcept { return library_.size(); }

    /// Remove an asset that is no longer referenced by any tracked clip history.
    /// This is used by undoable generated-placement commands to reverse an asset
    /// import that the same command performed.
    [[nodiscard]] Result<void> removeAsset(const Uuid& assetId);

    // --- Per-clip version history (Requirement 3.4) -------------------------

    /// Begin tracking `clipId`, establishing its initial (base) version from a
    /// library asset and source range. This is the clip's imported original / the
    /// asset it was first placed with; it becomes the selected version.
    ///
    /// Returns:
    ///   * ErrorCode::InvalidArgument if `sourceOut <= sourceIn`;
    ///   * ErrorCode::NotFound        if `assetRef` does not resolve in the library;
    ///   * ErrorCode::AlreadyExists   if `clipId` is already tracked;
    ///   * ok() on success.
    [[nodiscard]] Result<void> registerClip(ClipId clipId, const MediaAssetRef& assetRef,
                                            Duration sourceIn, Duration sourceOut,
                                            bool generated = false);

    /// Replace the tracked clip's content with a generated clip, retaining the
    /// prior version as a selectable version and making the new generated version
    /// the selected one. At least the kMinRetainedVersions most recent versions are
    /// preserved; older versions beyond the retention capacity are dropped oldest-first.
    ///
    /// Returns:
    ///   * ErrorCode::InvalidArgument    if `sourceOut <= sourceIn`;
    ///   * ErrorCode::NotFound           if the generated `assetRef` is not in the
    ///                                   library;
    ///   * ErrorCode::FailedPrecondition if `clipId` is not yet tracked (there is no
    ///                                   existing clip to replace);
    ///   * ok() on success.
    [[nodiscard]] Result<void> replaceWithGeneratedClip(ClipId clipId,
                                                        const MediaAssetRef& assetRef,
                                                        Duration sourceIn, Duration sourceOut);

    /// True iff `clipId` has a tracked version history.
    [[nodiscard]] bool tracksClip(ClipId clipId) const;

    /// Number of retained versions for `clipId` (0 if the clip is not tracked).
    [[nodiscard]] std::size_t versionCount(ClipId clipId) const;

    /// All retained versions for `clipId`, oldest first (empty if not tracked).
    [[nodiscard]] std::vector<ClipVersion> versions(ClipId clipId) const;

    /// The version at `index` (oldest-first) for `clipId`, or std::nullopt if the
    /// clip is not tracked or the index is out of range.
    [[nodiscard]] std::optional<ClipVersion> versionAt(ClipId clipId, std::size_t index) const;

    /// The index (oldest-first) of the currently selected version for `clipId`, or
    /// std::nullopt if the clip is not tracked.
    [[nodiscard]] std::optional<std::size_t> selectedVersionIndex(ClipId clipId) const;

    /// The currently selected version for `clipId`, or std::nullopt if not tracked.
    [[nodiscard]] std::optional<ClipVersion> selectedVersion(ClipId clipId) const;

    /// Select a previously retained version of a clip as the active one.
    ///
    /// Returns:
    ///   * ErrorCode::NotFound   if `clipId` is not tracked;
    ///   * ErrorCode::OutOfRange if `index` is not a valid version index;
    ///   * ok() on success.
    [[nodiscard]] Result<void> selectVersion(ClipId clipId, std::size_t index);

    /// The effective retention capacity (>= kMinRetainedVersions).
    [[nodiscard]] std::size_t retentionCapacity() const noexcept { return retentionCapacity_; }

private:
    struct ClipHistory {
        std::vector<ClipVersion> versions; ///< Chronological, oldest first.
        std::size_t              selected = 0;
    };

    std::size_t                            retentionCapacity_;
    std::vector<MediaAssetRef>             library_;
    std::unordered_map<Uuid, std::size_t>  assetIndex_; ///< assetId -> index into library_.
    std::unordered_map<ClipId, ClipHistory> histories_;
};

} // namespace palmier

#endif // PALMIER_CORE_MEDIAMANAGER_HPP
