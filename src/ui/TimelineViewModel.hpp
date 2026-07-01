// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineViewModel.hpp — the Qt-free presentation adapter for the timeline
// (task 19.2).
//
// The Qt/QML Timeline view is expected to expose the project through a
// QAbstractItemModel and translate user gestures (drag-move, trim, split,
// reorder) into editing commands (Requirement 2). This class is the UI-agnostic
// half of that view: it maps the engine's Project state into a stable
// row/column model shape (tracks x clips) that a QAbstractItemModel can surface
// verbatim, and it translates each gesture into the SAME concrete EditCommand
// the MCP server and the in-app agent use, driving it through
// TimelineEngine::apply so undo/redo and observers behave identically no matter
// who issues the edit (design.md Component 1: "All mutations flow through a
// Command object").
//
// Keeping this logic Qt-free serves two ends:
//   * It is unit-testable without a Qt runtime (the sandbox has no Qt 6), so the
//     gesture -> command mapping and the model-shape projection are verified
//     directly; the thin QAbstractItemModel subclass (TimelineModel, guarded by
//     PALMIER_HAVE_QT) only forwards to this adapter.
//   * It mirrors the project's layered style: the engine, commands, and services
//     are all Qt-free; only the outermost shell depends on Qt.
//
// Change reflection: the adapter subscribes to the engine's ChangeSet stream and
// keeps a cached snapshot in sync, so readers always observe the current state.
// A view listener (set by the Qt model) is invoked with each ChangeSet so the
// QAbstractItemModel can emit the matching begin/end row notifications.
//
// Indications (Requirement 2.3 / 2.6 / 2.10): every gesture returns a
// GestureResult classifying the outcome — Applied, an InvalidDrop (an
// overlapping/negative move rejected with the clip retained), NothingToSplit (a
// split whose playhead misses the clip), a NoOp (undo/redo with empty history),
// or a generic Rejected failure — plus the human-readable message the engine or
// command produced. The most recent indication is also retained so a status area
// can display it.

#ifndef PALMIER_UI_TIMELINEVIEWMODEL_HPP
#define PALMIER_UI_TIMELINEVIEWMODEL_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/ChangeSet.hpp"
#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Subscription.hpp"
#include "core/Track.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"

namespace palmier::ui {

/// How a gesture resolved, from the view's perspective. Derived from the
/// CommandResult the engine returned together with the gesture kind, so the
/// timeline can surface the exact indication Requirement 2 calls for.
enum class GestureIndication {
    None,            ///< No indication to show (cleared).
    Applied,         ///< The edit changed the project.
    InvalidDrop,     ///< A move/drop was rejected (overlap or negative position);
                     ///< the clip is retained at its original position (Req 2.3).
    NothingToSplit,  ///< A split was requested where no clip lies under the
                     ///< playhead; nothing changed (Req 2.6).
    NoOp,            ///< Undo/redo requested with empty history (Req 2.10).
    Rejected,        ///< A generic command failure (e.g. unknown id).
};

/// A short, stable label for a GestureIndication (useful for logs/tests/QML).
[[nodiscard]] std::string_view toStringView(GestureIndication indication) noexcept;

/// The outcome of a single gesture routed through the adapter.
struct GestureResult {
    CommandOutcome    outcome = CommandOutcome::Failed;
    GestureIndication indication = GestureIndication::None;
    std::string       message;
    /// Ids of clips newly created by the gesture (e.g. a split's right half),
    /// taken from the emitted ChangeSet; empty unless the gesture applied.
    std::vector<ClipId> addedClips;

    /// True when the gesture actually mutated the project.
    [[nodiscard]] bool changed() const noexcept { return outcome == CommandOutcome::Applied; }
    /// True unless the gesture failed (Applied or NoOp).
    [[nodiscard]] bool ok() const noexcept { return outcome != CommandOutcome::Failed; }
};

/// One track as the view sees it: identity, kind, flags, and its clip count.
struct TrackRow {
    Uuid        id;
    TrackKind   kind = TrackKind::Video;
    bool        muted = false;
    bool        locked = false;
    std::size_t clipCount = 0;
};

/// One clip as the view sees it: identity plus the geometry and display
/// attributes a timeline cell needs. `duration` is sourceOut - sourceIn.
struct ClipView {
    ClipId        id;
    MediaAssetRef assetRef;
    Duration      timelineStart;
    Duration      duration;
    Duration      sourceIn;
    Duration      sourceOut;
    double        opacity = 1.0;
    double        gain = 1.0;
    bool          hasTransitionIn = false;
    std::size_t   effectCount = 0;

    [[nodiscard]] Duration timelineEnd() const noexcept { return timelineStart + duration; }
};

/// The Qt-free presentation adapter over a TimelineEngine.
///
/// Rows are tracks (in project order); within each track, columns are clips (in
/// timelineStart order, as the engine maintains them). The adapter never mutates
/// the project except through the engine, so every edit is undoable and observed.
class TimelineViewModel {
public:
    /// The multi-track timeline supports between 1 and 50 tracks (Requirement 2.1).
    static constexpr std::size_t kMinTracks = 1;
    static constexpr std::size_t kMaxTracks = 50;

    /// Binds to `engine` (which must outlive this adapter) and takes an initial
    /// snapshot. Subscribes to the engine's ChangeSet stream to stay in sync.
    explicit TimelineViewModel(TimelineEngine& engine);

    ~TimelineViewModel();

    TimelineViewModel(const TimelineViewModel&) = delete;
    TimelineViewModel& operator=(const TimelineViewModel&) = delete;
    TimelineViewModel(TimelineViewModel&&) = delete;
    TimelineViewModel& operator=(TimelineViewModel&&) = delete;

    // --- Model shape (what a QAbstractItemModel surfaces) ------------------

    /// Number of tracks (top-level rows).
    [[nodiscard]] std::size_t trackCount() const noexcept { return cached_.tracks.size(); }

    /// Number of clips on the track at `trackRow` (0 if out of range).
    [[nodiscard]] std::size_t clipCount(std::size_t trackRow) const noexcept;

    /// The track at `trackRow`, or std::nullopt if out of range.
    [[nodiscard]] std::optional<TrackRow> trackAt(std::size_t trackRow) const;

    /// The clip at (`trackRow`, `clipColumn`), or std::nullopt if out of range.
    [[nodiscard]] std::optional<ClipView> clipAt(std::size_t trackRow,
                                                 std::size_t clipColumn) const;

    /// Locate a clip by id, returning (trackRow, clipColumn), or std::nullopt.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    locate(ClipId id) const;

    /// Total timeline length (latest clip end across all tracks).
    [[nodiscard]] Duration timelineDuration() const;

    /// Whether the current track count lies within the supported 1-50 range
    /// (Requirement 2.1). An empty project (0 tracks) is not yet within range.
    [[nodiscard]] bool trackCountSupported() const noexcept {
        const std::size_t n = trackCount();
        return n >= kMinTracks && n <= kMaxTracks;
    }

    /// Whether another track could be added without exceeding the 50-track cap.
    [[nodiscard]] bool canAddTrack() const noexcept { return trackCount() < kMaxTracks; }

    /// An immutable copy of the current project (for readers needing full state).
    [[nodiscard]] const Project& project() const noexcept { return cached_; }

    // --- Undo/redo enablement ---------------------------------------------

    [[nodiscard]] bool canUndo() const noexcept { return engine_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return engine_.canRedo(); }

    // --- Gestures (mapped to the shared EditCommands) ----------------------

    /// Drag-move: reposition a clip to `newStart` on its own track (Req 2.2). An
    /// overlapping or negative destination is rejected with an InvalidDrop
    /// indication and the clip stays where it was (Req 2.3).
    GestureResult moveClip(ClipId id, Duration newStart);

    /// Trim a clip's leading or trailing edge to `newBoundary` in source time,
    /// constrained to [1 frame, sourceDuration] (Req 2.4).
    GestureResult trimClip(ClipId id, TrimClipCommand::Edge edge, Duration newBoundary,
                           FrameRate fps, Duration sourceDuration);

    /// Convenience: trim the start (in-point) edge.
    GestureResult trimClipStart(ClipId id, Duration newSourceIn, FrameRate fps,
                                Duration sourceDuration) {
        return trimClip(id, TrimClipCommand::Edge::Start, newSourceIn, fps, sourceDuration);
    }
    /// Convenience: trim the end (out-point) edge.
    GestureResult trimClipEnd(ClipId id, Duration newSourceOut, FrameRate fps,
                              Duration sourceDuration) {
        return trimClip(id, TrimClipCommand::Edge::End, newSourceOut, fps, sourceDuration);
    }

    /// Split a clip at an interior `playhead` (Req 2.5). If the playhead is not
    /// inside the clip, nothing changes and a NothingToSplit indication is
    /// returned (Req 2.6). On success, the new right-half id is in addedClips.
    GestureResult splitClip(ClipId id, Duration playhead);

    /// Reorder a track's clips into `newOrder` (a permutation of its clip ids),
    /// preserving the clip count (Req 2.7).
    GestureResult reorderClips(Uuid trackId, std::vector<ClipId> newOrder);

    /// Place a clip on a track at its timelineStart (Req 2.1 placement).
    GestureResult addClip(Uuid trackId, Clip clip);

    /// Remove a clip by id.
    GestureResult removeClip(ClipId id);

    /// Undo / redo the most recent edit. Empty history is a NoOp (Req 2.10).
    GestureResult undo();
    GestureResult redo();

    // --- Indication surface -----------------------------------------------

    /// The indication kind of the most recent gesture.
    [[nodiscard]] GestureIndication lastIndication() const noexcept { return lastIndication_; }
    /// The human-readable message of the most recent gesture.
    [[nodiscard]] const std::string& lastMessage() const noexcept { return lastMessage_; }

    // --- Change notification for the Qt binding ---------------------------

    /// Register a listener invoked (after the cache is refreshed) with every
    /// ChangeSet the engine emits. The Qt model uses this to translate a change
    /// into begin/end row notifications. Passing nullptr clears the listener.
    void setChangeListener(std::function<void(const ChangeSet&)> listener);

    /// The most recent ChangeSet observed, or std::nullopt if none yet.
    [[nodiscard]] const std::optional<ChangeSet>& lastChange() const noexcept {
        return lastChange_;
    }

    /// Force a resynchronization of the cached snapshot from the engine.
    void refresh();

private:
    // Route a freshly-built command through the engine and classify the result.
    GestureResult run(std::unique_ptr<EditCommand> cmd, GestureIndication onFailure);

    // Turn a CommandResult (+ gesture-specific failure kind) into a GestureResult
    // and update the retained last-indication state.
    GestureResult classify(const CommandResult& result, GestureIndication onFailure);

    // Called for every ChangeSet the engine emits: refresh cache, forward.
    void onChange(const ChangeSet& change);

    TimelineEngine&                          engine_;
    Project                                  cached_;
    Subscription                             subscription_;
    std::optional<ChangeSet>                 lastChange_;
    std::function<void(const ChangeSet&)>    changeListener_;
    GestureIndication                        lastIndication_ = GestureIndication::None;
    std::string                              lastMessage_;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_TIMELINEVIEWMODEL_HPP
