// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/MediaBrowserViewModel.hpp — the Qt-free presentation model behind the
// Media Browser panel (task 19.5; Requirements 3.1, 3.4, 5.3, 5.4).
//
// The Media Browser is the editor panel where the user imports media, browses
// the project's media library, inspects a clip's retained versions, and sees a
// clip's key-moment markers. Following the project's "C++ for models exposed via
// QAbstractItemModel; QML/QWidget for the view" layering (design.md Presentation
// Layer), all of that panel LOGIC lives here in a pure C++ view model that
// depends only on the domain core and the (Qt-free) key-moment marker service —
// NOT on Qt. The QWidget/QML surface (MediaBrowserPanel, guarded by
// PALMIER_HAVE_QT) is a thin skin over this model, so the panel's behaviour is
// fully unit-testable on any platform without Qt installed.
//
// What this model exposes, mapped to requirements:
//
//   * Import (Req 3.1 / 3.2 / 3.3) — importMedia() runs the injected import
//     validator (which, in the real build, is the FFmpeg-backed
//     media::validateMediaImport that names an unsupported format (3.2) or an
//     unreadable file (3.3)) and, only on success, adds the asset to the project
//     library through the Media Manager, making it available for placement on
//     the timeline (3.1). On any rejection the library is left unchanged and a
//     human-readable error message is retained for the view to display. The
//     validator is injected so this model needs no FFmpeg to be tested.
//
//   * Library display (Req 3.1) — library() returns the imported/generated
//     assets, in import order, as display rows.
//
//   * Clip version history (Req 3.4) — versionsFor()/selectVersion() expose a
//     clip's retained versions (the imported original plus generated
//     replacements, at least the 10 most recent) as SELECTABLE rows, driven
//     through the Media Manager so selecting a version rolls the clip back to
//     that take.
//
//   * Key-moment markers (Req 5.3 / 5.4) — keyMomentsFor() reports, for a clip,
//     either the list of detected marker positions to draw (5.3) or a distinct
//     "no key moments were found" indication with no markers (5.4), reading the
//     completed-detection state recorded in the KeyMomentMarkerModel. A clip
//     with no detection recorded yet is reported as NotAnalyzed (neither markers
//     nor the "no key moments" indication).
//
// The model holds references to a MediaManager and a KeyMomentMarkerModel that
// must outlive it; it never owns project state itself.

#ifndef PALMIER_UI_MEDIABROWSERVIEWMODEL_HPP
#define PALMIER_UI_MEDIABROWSERVIEWMODEL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Clip.hpp"  // ClipId
#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "services/KeyMomentMarkers.hpp"  // KeyMomentMarkerModel, MarkerPresence

namespace palmier {
class MediaManager;  // core/MediaManager.hpp — the project media library + versions.
}  // namespace palmier

namespace palmier::ui {

// ---------------------------------------------------------------------------
// MediaLibraryEntry — one row of the media library list (Requirement 3.1)
// ---------------------------------------------------------------------------

/// A single imported/generated asset as shown in the media library. `displayName`
/// is a short, user-facing label derived from the asset's source path (its file
/// name), falling back to the asset id when no path is available.
struct MediaLibraryEntry {
    Uuid        assetId;
    std::string sourcePath;
    std::string displayName;
};

// ---------------------------------------------------------------------------
// ClipVersionEntry — one selectable version row for a clip (Requirement 3.4)
// ---------------------------------------------------------------------------

/// A single retained version of a clip, as shown in the version list. `index` is
/// the oldest-first position used to select the version; `selected` marks the
/// version currently active for the clip.
struct ClipVersionEntry {
    std::size_t   index = 0;
    MediaAssetRef assetRef;
    Duration      sourceIn;
    Duration      sourceOut;
    bool          generated = false;  ///< True iff a generative replacement.
    bool          selected = false;   ///< True iff currently the active version.

    /// Duration this version occupies from its source: sourceOut - sourceIn.
    [[nodiscard]] Duration duration() const noexcept { return sourceOut - sourceIn; }
};

// ---------------------------------------------------------------------------
// Key-moment marker display (Requirements 5.3, 5.4)
// ---------------------------------------------------------------------------

/// How a clip's key-moment state should be presented in the browser.
enum class KeyMomentDisplayState {
    NotAnalyzed,      ///< No detection recorded for the clip yet: nothing to show.
    NoKeyMoments,     ///< Detection completed with zero moments: show indication (5.4).
    KeyMomentsFound,  ///< Detection found >= 1 moment: draw one marker each (5.3).
};

/// A single marker to draw, as a whole-millisecond offset from the clip start.
struct KeyMomentMarkerView {
    std::int64_t milliseconds = 0;
};

/// The full key-moment display for a clip: the state plus, when
/// KeyMomentsFound, one marker per detected timestamp in ascending order.
struct KeyMomentDisplay {
    KeyMomentDisplayState            state = KeyMomentDisplayState::NotAnalyzed;
    std::vector<KeyMomentMarkerView> markers;

    /// True iff there are markers to draw on the clip (Requirement 5.3).
    [[nodiscard]] bool hasMarkers() const noexcept {
        return state == KeyMomentDisplayState::KeyMomentsFound;
    }
    /// True iff the "no key moments were found" indication should be shown
    /// (Requirement 5.4).
    [[nodiscard]] bool showNoKeyMomentsIndication() const noexcept {
        return state == KeyMomentDisplayState::NoKeyMoments;
    }
    /// Number of markers to draw (0 unless KeyMomentsFound).
    [[nodiscard]] std::size_t markerCount() const noexcept { return markers.size(); }
};

// ---------------------------------------------------------------------------
// MediaBrowserViewModel
// ---------------------------------------------------------------------------

/// Pure C++ presentation model for the Media Browser panel. Wires the import UI
/// to the Media Manager and exposes the library, per-clip version history, and
/// key-moment markers for display. Holds references to the MediaManager and
/// KeyMomentMarkerModel it observes; both must outlive the view model.
class MediaBrowserViewModel {
public:
    /// Validates a candidate import and, on success, yields the project-scoped
    /// MediaAssetRef to catalog. In the real build this wraps the FFmpeg-backed
    /// media::validateMediaImport (which distinguishes unsupported-format (3.2)
    /// from unreadable-file (3.3) rejections); injecting it keeps this model
    /// Qt- and FFmpeg-free and fully unit-testable.
    using ImportValidator = std::function<Result<MediaAssetRef>(const std::filesystem::path&)>;

    /// Construct a view model over `media` (the project media library + version
    /// history) and `markers` (the completed key-moment detection state), using
    /// `validator` to gate imports. `media` and `markers` must outlive this model.
    MediaBrowserViewModel(MediaManager& media, services::KeyMomentMarkerModel& markers,
                          ImportValidator validator);

    // --- Import (Requirements 3.1, 3.2, 3.3) -------------------------------

    /// Validate `path` and, on success, add the resulting asset to the project
    /// library through the Media Manager, making it available for placement
    /// (Requirement 3.1). On any rejection — a validation failure (3.2/3.3), a
    /// missing validator, or a duplicate asset — the library is left unchanged,
    /// a human-readable message is retained (see lastImportError()), and the
    /// error is returned. On success the retained error message is cleared.
    [[nodiscard]] Result<MediaAssetRef> importMedia(const std::filesystem::path& path);

    /// The message describing the most recent failed import, or std::nullopt if
    /// the last import succeeded (or none has been attempted). Intended for the
    /// view to surface the "unsupported format" / "could not be read" message.
    [[nodiscard]] const std::optional<std::string>& lastImportError() const noexcept {
        return lastImportError_;
    }
    /// True iff the most recent import attempt failed and its message is retained.
    [[nodiscard]] bool hasImportError() const noexcept { return lastImportError_.has_value(); }

    // --- Library (Requirement 3.1) -----------------------------------------

    /// The project media library as display rows, in import order.
    [[nodiscard]] std::vector<MediaLibraryEntry> library() const;

    /// Number of assets in the library.
    [[nodiscard]] std::size_t libraryCount() const;

    /// True iff an asset with `assetId` is in the library.
    [[nodiscard]] bool libraryContains(const Uuid& assetId) const;

    // --- Clip selection ----------------------------------------------------

    /// Select the clip whose versions and key-moment markers the panel shows.
    void selectClip(ClipId clipId);

    /// Clear the current clip selection.
    void clearClipSelection() noexcept;

    /// The currently selected clip, or std::nullopt if none is selected.
    [[nodiscard]] std::optional<ClipId> selectedClip() const noexcept { return selectedClip_; }

    // --- Clip version history (Requirement 3.4) ----------------------------

    /// The retained versions of `clipId` as selectable display rows, oldest
    /// first, with the active version marked `selected`. Empty if the clip is
    /// not tracked.
    [[nodiscard]] std::vector<ClipVersionEntry> versionsFor(ClipId clipId) const;

    /// The retained versions of the currently selected clip (empty if none is
    /// selected or the clip is not tracked).
    [[nodiscard]] std::vector<ClipVersionEntry> versionsForSelectedClip() const;

    /// The oldest-first index of the active version for `clipId`, or
    /// std::nullopt if the clip is not tracked.
    [[nodiscard]] std::optional<std::size_t> selectedVersionIndex(ClipId clipId) const;

    /// Make the version at `index` (oldest-first) the active version of `clipId`,
    /// rolling the clip back to that take. Returns the Media Manager's result
    /// (NotFound if the clip is untracked, OutOfRange for a bad index).
    [[nodiscard]] Result<void> selectVersion(ClipId clipId, std::size_t index);

    // --- Key-moment markers (Requirements 5.3, 5.4) ------------------------

    /// The key-moment display for `clipId`: NotAnalyzed when no detection is
    /// recorded, NoKeyMoments (with no markers) when detection found none (5.4),
    /// or KeyMomentsFound with one marker per detected timestamp (5.3).
    [[nodiscard]] KeyMomentDisplay keyMomentsFor(ClipId clipId) const;

    /// The key-moment display for the currently selected clip (NotAnalyzed when
    /// no clip is selected).
    [[nodiscard]] KeyMomentDisplay keyMomentsForSelectedClip() const;

private:
    MediaManager&                    media_;
    services::KeyMomentMarkerModel&  markers_;
    ImportValidator                  validator_;
    std::optional<ClipId>            selectedClip_;
    std::optional<std::string>       lastImportError_;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_MEDIABROWSERVIEWMODEL_HPP
