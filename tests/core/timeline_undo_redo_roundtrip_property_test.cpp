// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for the timeline undo/redo round-trip (task 3.4).
//
// Design property P1 (design.md "Correctness Properties"):
//
//     For any command `c`, `apply(c)` followed by `undo()` restores the exact
//     prior project state, and `redo()` reproduces the post-`apply` state.
//
// This is the undo/redo guarantee of Requirement 2.9 ("support undoing and
// redoing ... at least the last 20 sequential edits"). The TimelineEngine
// (apply/undo/redo over a bounded UndoRedoStack) is implemented in
// core/TimelineEngine.cpp (task 3.2) and the concrete commands in
// core/EditCommands.cpp (task 3.3); this file adds the dedicated RapidCheck
// property that exercises the round-trip across arbitrary valid command
// sequences.
//
// Strategy: build a small valid Project (a few tracks each seeded with a clip)
// inside a TimelineEngine, then apply an arbitrary-length sequence of arbitrary
// concrete EditCommands (AddClip / DeleteClip / MoveClip / TrimClip / SplitClip /
// ReorderClips / AddEffect), each constructed against the engine's live snapshot
// so it targets real clips/tracks. For EVERY command that the engine actually
// applies (CommandResult::changed()):
//   * snapshot the project immediately before (`before`) and immediately after
//     (`after`) the apply;
//   * undo() and assert the snapshot is byte-for-byte equal to `before` (the
//     "restores the exact prior state" clause); and
//   * redo() and assert the snapshot is byte-for-byte equal to `after` (the
//     "reproduces the post-apply state" clause, which requires each command to
//     reproduce any freshly generated ids — e.g. a split's right-half id —
//     deterministically).
// A command the engine rejects or treats as a no-op (overlap, out-of-range, a
// degenerate split, etc.) records nothing on the undo stack, so for those the
// property only asserts the project is unchanged and the sequence continues from
// the post-apply state — building an ever-deeper undo history as it goes.
//
// _Requirements: 2.9_

#include "core/TimelineEngine.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "ui/GuiToolGateway.hpp"

namespace palmier {
namespace {

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// --- Structural equality ---------------------------------------------------
//
// The domain types carry no operator==, so the property compares snapshots
// field-by-field. "Exact prior state" means every clip attribute the commands
// can touch (position, source range, gain/opacity, effects, transition) plus the
// track structure must match, so the comparison is deliberately exhaustive.

bool effectsEqual(const Effect& a, const Effect& b) {
    return a.id == b.id && a.type == b.type && a.parameters == b.parameters;
}

bool transitionsEqual(const std::optional<Transition>& a,
                      const std::optional<Transition>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->id == b->id && a->kind == b->kind && a->duration == b->duration;
}

bool clipsEqual(const Clip& a, const Clip& b) {
    if (a.id != b.id) return false;
    if (a.assetRef.assetId != b.assetRef.assetId) return false;
    if (a.timelineStart != b.timelineStart) return false;
    if (a.sourceIn != b.sourceIn) return false;
    if (a.sourceOut != b.sourceOut) return false;
    if (a.gain != b.gain) return false;
    if (a.opacity != b.opacity) return false;
    if (a.effects.size() != b.effects.size()) return false;
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        if (!effectsEqual(a.effects[i], b.effects[i])) return false;
    }
    return transitionsEqual(a.transitionIn, b.transitionIn);
}

bool tracksEqual(const Track& a, const Track& b) {
    if (a.id != b.id || a.kind != b.kind || a.muted != b.muted || a.locked != b.locked) {
        return false;
    }
    if (a.clips.size() != b.clips.size()) return false;
    for (std::size_t i = 0; i < a.clips.size(); ++i) {
        if (!clipsEqual(a.clips[i], b.clips[i])) return false;
    }
    return true;
}

bool projectsEqual(const Project& a, const Project& b) {
    if (a.id != b.id || a.name != b.name) return false;
    if (a.timelineFps != b.timelineFps) return false;
    if (!(a.canvas == b.canvas)) return false;
    if (a.colorSpace != b.colorSpace) return false;
    if (a.version != b.version) return false;
    if (a.tracks.size() != b.tracks.size()) return false;
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!tracksEqual(a.tracks[i], b.tracks[i])) return false;
    }
    return true;
}

// --- Snapshot helpers ------------------------------------------------------

struct ClipLocator {
    Uuid   trackId;
    ClipId clipId;
    Clip   clip;
};

// Flatten every clip in the snapshot together with its owning track id.
std::vector<ClipLocator> allClips(const Project& p) {
    std::vector<ClipLocator> out;
    for (const Track& t : p.tracks) {
        for (const Clip& c : t.clips) {
            out.push_back(ClipLocator{t.id, c.id, c});
        }
    }
    return out;
}

// The latest clip end on the track with `trackId`, or zero if it has no clips.
Duration trackEnd(const Project& p, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& t : p.tracks) {
        if (t.id != trackId) continue;
        for (const Clip& c : t.clips) {
            if (c.timelineEnd() > end) end = c.timelineEnd();
        }
    }
    return end;
}

// Build a clip whose source range is a positive whole number of frames.
Clip makeClip(ClipId id, Duration start, Duration in, Duration out) {
    Clip clip;
    clip.id = id;
    clip.assetRef = MediaAssetRef(Uuid::generateV4(), "mem://asset");
    clip.timelineStart = start;
    clip.sourceIn = in;
    clip.sourceOut = out;
    return clip;
}

// A valid starting project: `videoTracks` video lanes + one audio lane, each
// seeded with a single non-overlapping clip so commands have targets from step 0.
Project makeSeedProject(int videoTracks) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "roundtrip";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const int total = videoTracks + 1;  // + one audio track
    for (int i = 0; i < total; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = (i < videoTracks) ? TrackKind::Video : TrackKind::Audio;
        track.clips.push_back(
            makeClip(Uuid::generateV4(), ms(0), ms(0), ms(1000)));
        project.tracks.push_back(std::move(track));
    }
    return project;
}

// --- Property --------------------------------------------------------------

// Feature: palmier-pro-linux, Property 1: Undo/redo round-trip — apply(c) then
// undo() restores the exact prior state; redo() reproduces the post-apply state.
// Validates: Requirements 2.9
RC_GTEST_PROP(TimelineEngineUndoRedoProperties,
              ApplyUndoRestoresRedoReproduces,
              ()) {
    const int videoTracks = *rc::gen::inRange(1, 4);   // 1..3 video lanes
    const int numCommands = *rc::gen::inRange(1, 25);  // arbitrary edit sequence

    TimelineEngine engine(makeSeedProject(videoTracks));
    const FrameRate fps = FrameRate::fps30();
    const Duration sourceDuration = ms(600000);  // 10-minute virtual source

    for (int step = 0; step < numCommands; ++step) {
        const Project before = engine.snapshot();
        const std::vector<ClipLocator> clips = allClips(before);

        // Pick a command kind. When there are no clips yet only AddClip applies.
        const int kind = clips.empty() ? 0 : *rc::gen::inRange(0, 7);

        std::unique_ptr<EditCommand> cmd;
        switch (kind) {
            case 0: {  // AddClip — appended past the track end so it never overlaps.
                const std::size_t ti =
                    static_cast<std::size_t>(*rc::gen::inRange(
                        0, static_cast<int>(before.tracks.size())));
                const Uuid trackId = before.tracks[ti].id;
                const Duration gap = ms(*rc::gen::inRange(0, 500));
                const Duration start = trackEnd(before, trackId) + gap;
                const std::int64_t frames = *rc::gen::inRange(1, 60);
                const Duration len = fps.durationForFrames(frames);
                cmd = std::make_unique<AddClipCommand>(
                    trackId, makeClip(Uuid::generateV4(), start, ms(0), len));
                break;
            }
            case 1: {  // DeleteClip — remove an arbitrary existing clip.
                const ClipLocator& loc =
                    clips[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                cmd = std::make_unique<DeleteClipCommand>(loc.clipId);
                break;
            }
            case 2: {  // MoveClip — to a position past its track's end (valid drop).
                const ClipLocator& loc =
                    clips[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const Duration gap = ms(*rc::gen::inRange(0, 500));
                const Duration newStart = trackEnd(before, loc.trackId) + gap;
                cmd = std::make_unique<MoveClipCommand>(loc.clipId, newStart);
                break;
            }
            case 3: {  // TrimClip End — shrink to an interior boundary (never overlaps).
                const ClipLocator& loc =
                    clips[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const Clip& c = loc.clip;
                const std::int64_t curFrames = fps.framesForDuration(c.duration());
                // Choose a smaller frame count in [1, curFrames]; a one-frame clip
                // simply keeps its length (the engine leaves it unchanged then).
                const std::int64_t hi = curFrames < 1 ? 1 : curFrames;
                const std::int64_t frames = *rc::gen::inRange(
                    static_cast<int>(1), static_cast<int>(hi) + 1);
                const Duration newOut = c.sourceIn + fps.durationForFrames(frames);
                cmd = std::make_unique<TrimClipCommand>(
                    loc.clipId, TrimClipCommand::Edge::End, newOut, fps, sourceDuration);
                break;
            }
            case 4: {  // SplitClip — at an interior playhead.
                const ClipLocator& loc =
                    clips[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const Clip& c = loc.clip;
                const std::int64_t durFrames = fps.framesForDuration(c.duration());
                // Interior playhead only exists when the clip spans >= 2 frames.
                if (durFrames >= 2) {
                    const std::int64_t at =
                        *rc::gen::inRange(static_cast<int>(1),
                                          static_cast<int>(durFrames));
                    const Duration playhead =
                        c.timelineStart + fps.durationForFrames(at);
                    cmd = std::make_unique<SplitClipCommand>(loc.clipId, playhead);
                } else {
                    // Fall back to a guaranteed-applicable AddEffect.
                    cmd = std::make_unique<AddEffectCommand>(
                        loc.clipId, Effect::brightness(0.1));
                }
                break;
            }
            case 5: {  // ReorderClips — reverse a track's clip order.
                const std::size_t ti =
                    static_cast<std::size_t>(*rc::gen::inRange(
                        0, static_cast<int>(before.tracks.size())));
                const Track& track = before.tracks[ti];
                std::vector<ClipId> order;
                for (const Clip& c : track.clips) order.push_back(c.id);
                std::reverse(order.begin(), order.end());
                cmd = std::make_unique<ReorderClipsCommand>(track.id, std::move(order));
                break;
            }
            default: {  // AddEffect — append an effect to an arbitrary clip.
                const ClipLocator& loc =
                    clips[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const double amount =
                    static_cast<double>(*rc::gen::inRange(0, 100)) / 100.0;
                cmd = std::make_unique<AddEffectCommand>(
                    loc.clipId, Effect::brightness(amount));
                break;
            }
        }

        RC_ASSERT(cmd != nullptr);
        const CommandResult applied = engine.apply(std::move(cmd));

        if (applied.changed()) {
            const Project after = engine.snapshot();

            // undo() restores the EXACT prior state (P1, clause 1).
            const CommandResult undone = engine.undo();
            RC_ASSERT(undone.changed());
            RC_ASSERT(projectsEqual(engine.snapshot(), before));

            // redo() reproduces the EXACT post-apply state (P1, clause 2).
            const CommandResult redone = engine.redo();
            RC_ASSERT(redone.changed());
            RC_ASSERT(projectsEqual(engine.snapshot(), after));
            // Engine is now back in `after`; the sequence continues from there,
            // deepening the undo history for subsequent commands.
        } else {
            // A rejected / no-op command records nothing and must leave the
            // project exactly as it was.
            RC_ASSERT(projectsEqual(engine.snapshot(), before));
        }
    }
}

// ---------------------------------------------------------------------------
// Task 11.10 — extend the undo round-trip property over the GUI gateway path.
// ---------------------------------------------------------------------------
//
// Feature: end-to-end-editor-integration, Property 3: Undo restores the
// immediately prior state — for any edit issued through the real GUI gateway
// path (ui::GuiToolGateway, task 11.1/11.4), undoing it restores the exact
// project state that preceded it.
// Validates: Requirements 1.8
//
// This exercises the SAME session + engine a ui::TimelineViewModel bound to a
// gateway would observe: a services::ProjectSession's TimelineEngine, edited
// through services::McpToolExecutor::executeTool(..., InvocationSource::Gui) —
// exactly what every ui::GuiToolGateway method does — and undone through the
// engine's own undo() (which is what TimelineViewModel::undo() always calls,
// gateway or not: there is no tool-call form of undo/redo for the GUI's own
// Edit-menu action, only for the offline agent's `edit.undo`/`edit.redo`
// phrases). It is deliberately narrower than the engine-level property above
// (fewer command kinds, using the tool surface's argument shapes rather than
// constructing EditCommands directly) because its purpose is to confirm the
// GUI's call path reaches the identical guarantee, not to re-prove P1 itself.
using palmier::services::Json;
using palmier::services::McpToolExecutor;
using palmier::services::ProjectSession;
using palmier::services::ToolRegistry;
using palmier::services::buildDefaultToolRegistry;
using palmier::services::InvocationSource;

RC_GTEST_PROP(TimelineEngineUndoRedoProperties,
              GuiGatewayUndoRestoresImmediatelyPriorState,
              ()) {
    const int videoTracks = *rc::gen::inRange(1, 4);
    const int numCommands = *rc::gen::inRange(1, 15);
    const FrameRate fps = FrameRate::fps30();

    ProjectSession session;
    (void)session.engine().reset(makeSeedProject(videoTracks));
    TimelineEngine& engine = session.engine();

    ToolRegistry registry = buildDefaultToolRegistry(session);
    McpToolExecutor executor(registry, &session);
    palmier::ui::GuiToolGateway gateway(executor);

    for (int step = 0; step < numCommands; ++step) {
        const Project before = engine.snapshot();
        const std::vector<ClipLocator> clips = allClips(before);

        // A small subset of gestures, each routed through the real gateway
        // method a ui::TimelineViewModel with a gateway installed would call.
        const int kind = clips.empty() ? 0 : *rc::gen::inRange(0, 3);
        Result<Json> outcome = err<Json>(Error{});

        switch (kind) {
            case 0: {  // addClip, past the track end so it never overlaps.
                const std::size_t ti = static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(before.tracks.size())));
                const Uuid trackId = before.tracks[ti].id;
                const Duration gap = ms(*rc::gen::inRange(0, 500));
                const Duration start = trackEnd(before, trackId) + gap;
                const Duration len = fps.durationForFrames(*rc::gen::inRange(1, 60));
                const Uuid assetId = Uuid::generateV4();
                outcome = gateway.addClip(trackId, assetId, "mem://asset", std::nullopt,
                                          start, ms(0), len, 1.0, 1.0);
                break;
            }
            case 1: {  // moveClip, past the track end so it never overlaps.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const Duration gap = ms(*rc::gen::inRange(0, 500));
                const Duration newStart = trackEnd(before, loc.trackId) + gap;
                outcome = gateway.moveClip(loc.clipId, newStart);
                break;
            }
            default: {  // addEffect on an arbitrary clip.
                const ClipLocator& loc = clips[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(clips.size())))];
                const double amount =
                    static_cast<double>(*rc::gen::inRange(0, 100)) / 100.0;
                outcome = gateway.addEffect(loc.clipId, Effect::brightness(amount));
                break;
            }
        }

        if (outcome.isOk()) {
            const Project after = engine.snapshot();

            // The GUI's Undo action always calls the engine's own undo()
            // directly (there is no tool-call form of undo/redo); Property 3
            // is exactly that this restores the immediately prior state.
            const CommandResult undone = engine.undo();
            RC_ASSERT(undone.changed());
            RC_ASSERT(projectsEqual(engine.snapshot(), before));

            const CommandResult redone = engine.redo();
            RC_ASSERT(redone.changed());
            RC_ASSERT(projectsEqual(engine.snapshot(), after));
        } else {
            RC_ASSERT(projectsEqual(engine.snapshot(), before));
        }
    }
}

}  // namespace
}  // namespace palmier
