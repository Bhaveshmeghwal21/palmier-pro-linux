// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/TimelineViewModel.cpp — implementation of the Qt-free timeline adapter.
//
// See TimelineViewModel.hpp for the contract. The adapter is a thin, well-defined
// bridge: it projects the engine's Project into a tracks x clips model shape and
// funnels every gesture into the corresponding concrete EditCommand, driven
// through TimelineEngine::apply / undo / redo so the edit is atomic, undoable,
// and observed exactly like an edit issued by the MCP server or the agent.

#include "ui/TimelineViewModel.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "core/Error.hpp"
#include "services/Json.hpp"
#include "ui/GuiToolGateway.hpp"

namespace palmier::ui {

std::string_view toStringView(GestureIndication indication) noexcept {
    switch (indication) {
        case GestureIndication::None:           return "None";
        case GestureIndication::Applied:        return "Applied";
        case GestureIndication::InvalidDrop:    return "InvalidDrop";
        case GestureIndication::NothingToSplit: return "NothingToSplit";
        case GestureIndication::NoOp:           return "NoOp";
        case GestureIndication::Rejected:       return "Rejected";
    }
    return "None";
}

TimelineViewModel::TimelineViewModel(TimelineEngine& engine, GuiToolGateway* gateway)
    : engine_(engine), gateway_(gateway), cached_(engine.snapshot()) {
    // Stay in sync with every state-changing engine operation, whoever issued it
    // (UI gesture, MCP tool call, or agent), so readers never observe stale rows.
    subscription_ = engine_.observe([this](const ChangeSet& change) { onChange(change); });
}

TimelineViewModel::~TimelineViewModel() = default;

// --- Model shape -----------------------------------------------------------

std::size_t TimelineViewModel::clipCount(std::size_t trackRow) const noexcept {
    if (trackRow >= cached_.tracks.size()) {
        return 0;
    }
    return cached_.tracks[trackRow].clips.size();
}

std::optional<TrackRow> TimelineViewModel::trackAt(std::size_t trackRow) const {
    if (trackRow >= cached_.tracks.size()) {
        return std::nullopt;
    }
    const Track& track = cached_.tracks[trackRow];
    return TrackRow{track.id, track.kind, track.muted, track.locked, track.clips.size()};
}

std::optional<ClipView> TimelineViewModel::clipAt(std::size_t trackRow,
                                                  std::size_t clipColumn) const {
    if (trackRow >= cached_.tracks.size()) {
        return std::nullopt;
    }
    const Track& track = cached_.tracks[trackRow];
    if (clipColumn >= track.clips.size()) {
        return std::nullopt;
    }
    const Clip& clip = track.clips[clipColumn];
    return ClipView{clip.id,
                    clip.assetRef,
                    clip.timelineStart,
                    clip.duration(),
                    clip.sourceIn,
                    clip.sourceOut,
                    clip.opacity,
                    clip.gain,
                    clip.transitionIn.has_value(),
                    clip.effects.size()};
}

std::optional<std::pair<std::size_t, std::size_t>>
TimelineViewModel::locate(ClipId id) const {
    for (std::size_t t = 0; t < cached_.tracks.size(); ++t) {
        const std::vector<Clip>& clips = cached_.tracks[t].clips;
        for (std::size_t c = 0; c < clips.size(); ++c) {
            if (clips[c].id == id) {
                return std::make_pair(t, c);
            }
        }
    }
    return std::nullopt;
}

Duration TimelineViewModel::timelineDuration() const { return palmier::timelineDuration(cached_); }

// --- Gestures --------------------------------------------------------------

GestureResult TimelineViewModel::moveClip(ClipId id, Duration newStart) {
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->moveClip(id, newStart), GestureIndication::InvalidDrop);
    }
    return run(std::make_unique<MoveClipCommand>(id, newStart), GestureIndication::InvalidDrop);
}

GestureResult TimelineViewModel::trimClip(ClipId id, TrimClipCommand::Edge edge,
                                          Duration newBoundary, FrameRate fps,
                                          Duration sourceDuration) {
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->trimClip(id, edge, newBoundary, sourceDuration),
                                  GestureIndication::Rejected);
    }
    return run(std::make_unique<TrimClipCommand>(id, edge, newBoundary, fps, sourceDuration),
               GestureIndication::Rejected);
}

GestureResult TimelineViewModel::splitClip(ClipId id, Duration playhead) {
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->splitClip(id, playhead),
                                  GestureIndication::NothingToSplit);
    }
    return run(std::make_unique<SplitClipCommand>(id, playhead),
               GestureIndication::NothingToSplit);
}

GestureResult TimelineViewModel::reorderClips(Uuid trackId, std::vector<ClipId> newOrder) {
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->reorderClips(trackId, std::move(newOrder)),
                                  GestureIndication::Rejected);
    }
    return run(std::make_unique<ReorderClipsCommand>(trackId, std::move(newOrder)),
               GestureIndication::Rejected);
}

GestureResult TimelineViewModel::addClip(Uuid trackId, Clip clip) {
    // An add that overlaps an existing clip is rejected by the engine invariant
    // check; surface that as an invalid drop so the placement gesture matches
    // the move gesture's indication (Requirement 2.3).
    if (gateway_ != nullptr) {
        return classifyToolResult(
            gateway_->addClip(trackId, clip.assetRef.assetId, clip.assetRef.sourcePath,
                              clip.id, clip.timelineStart, clip.sourceIn, clip.sourceOut,
                              clip.opacity, clip.gain),
            GestureIndication::InvalidDrop);
    }
    return run(std::make_unique<AddClipCommand>(trackId, std::move(clip)),
               GestureIndication::InvalidDrop);
}

GestureResult TimelineViewModel::removeClip(ClipId id) {
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->deleteClip(id), GestureIndication::Rejected);
    }
    return run(std::make_unique<DeleteClipCommand>(id), GestureIndication::Rejected);
}

GestureResult TimelineViewModel::addTrack(TrackKind kind) {
    // Enforce the 50-track UI ceiling here: AddTrackCommand itself only rejects
    // at the (much higher) 64-tracks-per-kind cap of Requirement 3.8, so without
    // this check a 51st track could still be added by construction even though
    // canAddTrack() (and the menu action MainWindow disables from it) says no.
    if (!canAddTrack()) {
        GestureResult out;
        out.outcome = CommandOutcome::Failed;
        out.indication = GestureIndication::Rejected;
        out.message = "the timeline already holds the maximum of " +
                      std::to_string(kMaxTracks) + " tracks";
        lastIndication_ = out.indication;
        lastMessage_ = out.message;
        return out;
    }
    if (gateway_ != nullptr) {
        return classifyToolResult(gateway_->addTrack(kind), GestureIndication::Rejected);
    }
    return run(std::make_unique<AddTrackCommand>(kind), GestureIndication::Rejected);
}

GestureResult TimelineViewModel::undo() {
    return classify(engine_.undo(), GestureIndication::Rejected);
}

GestureResult TimelineViewModel::redo() {
    return classify(engine_.redo(), GestureIndication::Rejected);
}

// --- Internals -------------------------------------------------------------

GestureResult TimelineViewModel::run(std::unique_ptr<EditCommand> cmd,
                                     GestureIndication onFailure) {
    return classify(engine_.apply(std::move(cmd)), onFailure);
}

GestureResult TimelineViewModel::classify(const CommandResult& result,
                                          GestureIndication onFailure) {
    GestureResult out;
    out.outcome = result.outcome();

    switch (result.outcome()) {
        case CommandOutcome::Applied:
            out.indication = GestureIndication::Applied;
            out.message = result.message();
            // Surface any clips the change created (e.g. a split's right half).
            if (lastChange_.has_value()) {
                out.addedClips = lastChange_->addedClips;
            }
            break;

        case CommandOutcome::NoOp:
            out.indication = GestureIndication::NoOp;
            out.message = result.message();
            break;

        case CommandOutcome::Failed:
            // A missing target (unknown clip/track) is a generic rejection, not
            // the gesture-specific invalid-drop / nothing-to-split indication.
            out.indication = (result.error().code() == ErrorCode::NotFound)
                                 ? GestureIndication::Rejected
                                 : onFailure;
            out.message = result.error().message();
            break;
    }

    lastIndication_ = out.indication;
    lastMessage_ = out.message;
    return out;
}

GestureResult TimelineViewModel::classifyToolResult(const Result<services::Json>& result,
                                                    GestureIndication onFailure) {
    GestureResult out;

    if (result.isOk()) {
        // The gateway's tool call applies its EditCommand to the SAME engine this
        // adapter observes, so the subscription above has already refreshed
        // cached_ and lastChange_ synchronously by the time control returns here
        // (Requirements 1.7, 9.4, 11.5 — GUI/MCP/agent equivalence).
        out.outcome = CommandOutcome::Applied;
        out.indication = GestureIndication::Applied;
        if (lastChange_.has_value()) {
            out.addedClips = lastChange_->addedClips;
        }
        // A split's right half is also named in the tool's own JSON result;
        // fold it in too in case the ChangeSet path did not carry it (defensive
        // — the ChangeSet is expected to be the authoritative source).
        if (const services::Json& payload = result.value(); payload.isObject()) {
            if (const services::Json* rightId = payload.find("rightClipId");
                rightId != nullptr && rightId->isString()) {
                if (std::optional<Uuid> parsed = Uuid::parse(rightId->asString())) {
                    if (std::find(out.addedClips.begin(), out.addedClips.end(), *parsed) ==
                        out.addedClips.end()) {
                        out.addedClips.push_back(*parsed);
                    }
                }
            }
        }
        lastIndication_ = out.indication;
        lastMessage_ = out.message;
        return out;
    }

    const Error& error = result.error();
    out.outcome = CommandOutcome::Failed;
    out.indication =
        (error.code() == ErrorCode::NotFound) ? GestureIndication::Rejected : onFailure;
    out.message = error.message();

    lastIndication_ = out.indication;
    lastMessage_ = out.message;
    return out;
}

void TimelineViewModel::onChange(const ChangeSet& change) {
    cached_ = engine_.snapshot();
    lastChange_ = change;
    if (changeListener_) {
        changeListener_(change);
    }
}

void TimelineViewModel::setChangeListener(std::function<void(const ChangeSet&)> listener) {
    changeListener_ = std::move(listener);
}

void TimelineViewModel::refresh() { cached_ = engine_.snapshot(); }

}  // namespace palmier::ui
