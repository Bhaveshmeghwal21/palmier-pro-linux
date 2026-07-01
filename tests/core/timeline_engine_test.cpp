// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the TimelineEngine (task 3.2) — the authoritative timeline /
// project model that wraps a Project and the UndoRedoStack.
//
// Coverage:
//   * Queries: snapshot(), clip(id), duration().
//   * apply(): success path (records + emits), null-command rejection, atomic
//     rollback when the command fails, and atomic rollback + rejection when the
//     result would violate a timeline invariant (Requirement 6.6 — no partial
//     mutation).
//   * undo()/redo(): round-trip restore/reproduce (Requirement 2.9) and empty-
//     history no-op (Requirement 2.10), including the emitted ChangeSet origin.
//   * observe(): granular ChangeSet delivery (added/modified/removed clips) and
//     RAII unsubscription.
//
// The concrete editing commands are implemented in task 3.3, so these tests use
// small in-file EditCommand doubles that mutate tracks/clips directly.
//
// _Requirements: 2.9, 2.10, 6.6_

#include "core/TimelineEngine.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

// --- Fixtures --------------------------------------------------------------

Clip makeClip(ClipId id, Duration timelineStart, Duration sourceIn, Duration sourceOut) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = timelineStart;
    clip.sourceIn = sourceIn;
    clip.sourceOut = sourceOut;
    return clip;
}

// A project with a single (empty) video track.
Project makeProjectWithOneTrack() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    project.tracks.push_back(std::move(track));
    return project;
}

// Appends a clip to a track (by index). revert() removes it again by id.
class AddClipCommand : public EditCommand {
public:
    AddClipCommand(std::size_t trackIndex, Clip clip)
        : trackIndex_(trackIndex), clip_(std::move(clip)) {}

    std::string_view name() const noexcept override { return "AddClip"; }

    Result<void> apply(Project& project) override {
        if (trackIndex_ >= project.tracks.size()) {
            return err(notFound("track index out of range"));
        }
        project.tracks[trackIndex_].clips.push_back(clip_);
        return ok();
    }

    Result<void> revert(Project& project) override {
        auto& clips = project.tracks[trackIndex_].clips;
        clips.erase(std::remove_if(clips.begin(), clips.end(),
                                   [&](const Clip& c) { return c.id == clip_.id; }),
                    clips.end());
        return ok();
    }

private:
    std::size_t trackIndex_;
    Clip        clip_;
};

// Sets the opacity of an existing clip (by id); revert restores the prior value.
class SetOpacityCommand : public EditCommand {
public:
    SetOpacityCommand(ClipId id, double opacity) : id_(id), opacity_(opacity) {}

    std::string_view name() const noexcept override { return "SetOpacity"; }

    Result<void> apply(Project& project) override {
        for (auto& track : project.tracks) {
            for (auto& clip : track.clips) {
                if (clip.id == id_) {
                    previous_ = clip.opacity;
                    clip.opacity = opacity_;
                    return ok();
                }
            }
        }
        return err(notFound("clip not found"));
    }

    Result<void> revert(Project& project) override {
        for (auto& track : project.tracks) {
            for (auto& clip : track.clips) {
                if (clip.id == id_) {
                    clip.opacity = previous_;
                    return ok();
                }
            }
        }
        return err(notFound("clip not found"));
    }

private:
    ClipId id_;
    double opacity_;
    double previous_ = 1.0;
};

// A command that mutates the project and THEN reports failure, used to prove the
// engine rolls back partial mutations (Requirement 6.6).
class PartialThenFailCommand : public EditCommand {
public:
    explicit PartialThenFailCommand(Clip clip) : clip_(std::move(clip)) {}

    std::string_view name() const noexcept override { return "PartialThenFail"; }

    Result<void> apply(Project& project) override {
        project.tracks[0].clips.push_back(clip_);  // partial mutation ...
        return err(failedPrecondition("intentional failure"));  // ... then fail
    }

    Result<void> revert(Project&) override { return ok(); }

private:
    Clip clip_;
};

// Convenience: apply a freshly-constructed AddClipCommand.
CommandResult addClip(TimelineEngine& engine, std::size_t trackIndex, const Clip& clip) {
    return engine.apply(std::make_unique<AddClipCommand>(trackIndex, clip));
}

// --- Queries ---------------------------------------------------------------

TEST(TimelineEngine, SnapshotReflectsAppliedEdits) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const Clip clip = makeClip(Uuid::generateV4(), Duration::zero(),
                               Duration::zero(), Duration::fromMilliseconds(1000));

    ASSERT_TRUE(addClip(engine, 0, clip).changed());

    const Project snap = engine.snapshot();
    ASSERT_EQ(snap.tracks.size(), 1u);
    ASSERT_EQ(snap.tracks[0].clips.size(), 1u);
    EXPECT_EQ(snap.tracks[0].clips[0].id, clip.id);
}

TEST(TimelineEngine, ClipLookupFindsAndMisses) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(id, Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(500)))
                    .changed());

    const auto found = engine.clip(id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, id);

    EXPECT_FALSE(engine.clip(Uuid::generateV4()).has_value());
}

TEST(TimelineEngine, DurationIsLatestClipEnd) {
    TimelineEngine engine(makeProjectWithOneTrack());
    EXPECT_TRUE(engine.duration().isZero());

    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(Uuid::generateV4(), Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(Uuid::generateV4(), Duration::fromMilliseconds(2000),
                                 Duration::zero(), Duration::fromMilliseconds(500)))
                    .changed());

    // Latest end = 2000ms start + 500ms duration = 2500ms.
    EXPECT_EQ(engine.duration(), Duration::fromMilliseconds(2500));
}

// --- apply(): success, null, and atomic rollback ---------------------------

TEST(TimelineEngine, ApplySucceedsRecordsAndEnablesUndo) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const auto result = addClip(engine, 0,
                                makeClip(Uuid::generateV4(), Duration::zero(),
                                         Duration::zero(), Duration::fromMilliseconds(1000)));
    EXPECT_TRUE(result.changed());
    EXPECT_EQ(result.message(), "AddClip");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());
}

TEST(TimelineEngine, ApplyNullCommandFails) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const auto result = engine.apply(nullptr);
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(engine.canUndo());
}

TEST(TimelineEngine, ApplyRollsBackWhenCommandFails) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const Clip orphan = makeClip(Uuid::generateV4(), Duration::zero(),
                                 Duration::zero(), Duration::fromMilliseconds(1000));

    const auto result =
        engine.apply(std::make_unique<PartialThenFailCommand>(orphan));

    // Failure is reported, the partial mutation is undone, nothing is recorded.
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_TRUE(engine.snapshot().tracks[0].clips.empty());
    EXPECT_FALSE(engine.canUndo());
}

TEST(TimelineEngine, ApplyRejectsAndRollsBackInvariantViolation) {
    TimelineEngine engine(makeProjectWithOneTrack());

    // First clip occupies [0ms, 1000ms).
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(Uuid::generateV4(), Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());

    // Second clip starts at 500ms and overlaps the first with no transition.
    const auto result =
        addClip(engine, 0,
                makeClip(Uuid::generateV4(), Duration::fromMilliseconds(500),
                         Duration::zero(), Duration::fromMilliseconds(1000)));

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    // The overlapping clip was not committed: only the first clip remains.
    EXPECT_EQ(engine.snapshot().tracks[0].clips.size(), 1u);
    // The rejected command is not on the undo history.
    EXPECT_TRUE(engine.canUndo());  // only the first (valid) add is undoable
    ASSERT_TRUE(engine.undo().changed());
    EXPECT_FALSE(engine.canUndo());
}

TEST(TimelineEngine, ApplyRejectsClipWithNonPositiveDuration) {
    TimelineEngine engine(makeProjectWithOneTrack());
    // sourceOut <= sourceIn violates the positive-duration invariant.
    const auto result =
        addClip(engine, 0,
                makeClip(Uuid::generateV4(), Duration::zero(),
                         Duration::fromMilliseconds(500), Duration::fromMilliseconds(500)));
    EXPECT_TRUE(result.isError());
    EXPECT_TRUE(engine.snapshot().tracks[0].clips.empty());
}

// --- undo()/redo() ---------------------------------------------------------

TEST(TimelineEngine, UndoRestoresAndRedoReproduces) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(id, Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());

    const auto undo = engine.undo();
    EXPECT_TRUE(undo.changed());
    EXPECT_TRUE(engine.snapshot().tracks[0].clips.empty());
    EXPECT_FALSE(engine.clip(id).has_value());

    const auto redo = engine.redo();
    EXPECT_TRUE(redo.changed());
    EXPECT_TRUE(engine.clip(id).has_value());
}

TEST(TimelineEngine, UndoOnEmptyHistoryIsNoOp) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const auto result = engine.undo();
    EXPECT_TRUE(result.isNoOp());
    EXPECT_FALSE(result.message().empty());
    EXPECT_FALSE(engine.canUndo());
}

// --- observe(): granular ChangeSet delivery + RAII unsubscribe -------------

TEST(TimelineEngine, ObserverReceivesAddedClipChangeSet) {
    TimelineEngine engine(makeProjectWithOneTrack());

    std::vector<ChangeSet> received;
    Subscription sub = engine.observe([&](const ChangeSet& cs) { received.push_back(cs); });

    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(id, Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].origin, ChangeOrigin::Apply);
    EXPECT_EQ(received[0].description, "AddClip");
    ASSERT_EQ(received[0].addedClips.size(), 1u);
    EXPECT_EQ(received[0].addedClips[0], id);
    EXPECT_TRUE(received[0].modifiedClips.empty());
    EXPECT_TRUE(received[0].removedClips.empty());
    EXPECT_EQ(received[0].currentDuration, Duration::fromMilliseconds(1000));
}

TEST(TimelineEngine, ObserverReceivesModifiedAndRemovedChangeSets) {
    TimelineEngine engine(makeProjectWithOneTrack());
    const ClipId id = Uuid::generateV4();
    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(id, Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());

    std::vector<ChangeSet> received;
    Subscription sub = engine.observe([&](const ChangeSet& cs) { received.push_back(cs); });

    // Modify the clip's opacity.
    ASSERT_TRUE(engine.apply(std::make_unique<SetOpacityCommand>(id, 0.5)).changed());
    ASSERT_EQ(received.size(), 1u);
    ASSERT_EQ(received[0].modifiedClips.size(), 1u);
    EXPECT_EQ(received[0].modifiedClips[0], id);

    // Undo removes the modification (still a "modified" clip, opacity back to 1).
    received.clear();
    ASSERT_TRUE(engine.undo().changed());
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].origin, ChangeOrigin::Undo);
    ASSERT_EQ(received[0].modifiedClips.size(), 1u);
    EXPECT_EQ(received[0].modifiedClips[0], id);
}

TEST(TimelineEngine, ResetSubscriptionStopsNotifications) {
    TimelineEngine engine(makeProjectWithOneTrack());

    int calls = 0;
    Subscription sub = engine.observe([&](const ChangeSet&) { ++calls; });

    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(Uuid::generateV4(), Duration::zero(), Duration::zero(),
                                 Duration::fromMilliseconds(1000)))
                    .changed());
    EXPECT_EQ(calls, 1);

    sub.reset();
    EXPECT_FALSE(sub.active());

    ASSERT_TRUE(addClip(engine, 0,
                        makeClip(Uuid::generateV4(), Duration::fromMilliseconds(2000),
                                 Duration::zero(), Duration::fromMilliseconds(500)))
                    .changed());
    EXPECT_EQ(calls, 1);  // no further notifications after reset
}

TEST(TimelineEngine, SubscriptionSafeAfterEngineDestroyed) {
    Subscription sub;
    {
        TimelineEngine engine(makeProjectWithOneTrack());
        sub = engine.observe([](const ChangeSet&) {});
        EXPECT_TRUE(sub.active());
    }
    // Engine (and its registry) is gone; resetting the outliving subscription
    // must not crash.
    EXPECT_NO_THROW(sub.reset());
}

}  // namespace
}  // namespace palmier
