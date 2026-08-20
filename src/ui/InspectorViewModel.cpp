// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/InspectorViewModel.cpp — implementation of the Qt-free Inspector model
// (task 19.4).
//
// The model holds the current clip selection, projects the selected clip's
// properties and effect chain from the engine's immutable snapshot, and maps each
// user edit onto an EditCommand applied through TimelineEngine::apply so every
// Inspector edit is atomic, undoable, and observable — the same path the timeline
// view, the MCP server, and the in-app agent use.

#include "ui/InspectorViewModel.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "core/EditCommands.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/TimelineEngine.hpp"
#include "services/Json.hpp"
#include "ui/GuiToolGateway.hpp"

namespace palmier {

namespace {

// A located clip: the track that holds it and its index within that track.
struct MutableClipLocation {
    Track*      track = nullptr;
    std::size_t index = 0;
};

// Locate a clip by id across every track (mutating). std::nullopt when absent.
std::optional<MutableClipLocation> locateClip(Project& project, const ClipId& clipId) {
    for (Track& track : project.tracks) {
        for (std::size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id == clipId) {
                return MutableClipLocation{&track, i};
            }
        }
    }
    return std::nullopt;
}

std::string idLabel(const Uuid& id) {
    return id.isNil() ? std::string{"<nil>"} : id.toString();
}

// Translate a gateway tool call's Result<Json> into the CommandResult shape
// InspectorViewModel's mutation methods have always returned, so the caller
// (and the existing tests) see the identical outcome whether the mutation went
// through the gateway or straight through TimelineEngine::apply.
CommandResult toCommandResult(const Result<services::Json>& result) {
    if (result.isOk()) {
        return CommandResult::applied();
    }
    return CommandResult::failed(result.error());
}

} // namespace

namespace ui {

// ===========================================================================
// SetClipPropertyCommand
// ===========================================================================

SetClipPropertyCommand::SetClipPropertyCommand(ClipId clipId, Property property, double value)
    : clipId_(clipId), property_(property), value_(value) {}

Result<void> SetClipPropertyCommand::apply(Project& project) {
    std::optional<MutableClipLocation> loc = locateClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetClipPropertyCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    switch (property_) {
        case Property::Opacity:
            prior_ = clip.opacity;
            captured_ = true;
            clip.opacity = value_;
            break;
        case Property::Gain:
            prior_ = clip.gain;
            captured_ = true;
            clip.gain = value_;
            break;
    }
    return ok();
}

Result<void> SetClipPropertyCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetClipPropertyCommand: revert before a successful apply"));
    }
    std::optional<MutableClipLocation> loc = locateClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetClipPropertyCommand: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    switch (property_) {
        case Property::Opacity: clip.opacity = prior_; break;
        case Property::Gain:    clip.gain = prior_; break;
    }
    return ok();
}

// ===========================================================================
// SetEffectParameterCommand
// ===========================================================================

SetEffectParameterCommand::SetEffectParameterCommand(ClipId clipId, Uuid effectId,
                                                     std::string parameter, double value)
    : clipId_(clipId),
      effectId_(effectId),
      parameter_(std::move(parameter)),
      value_(value) {}

Result<void> SetEffectParameterCommand::apply(Project& project) {
    std::optional<MutableClipLocation> loc = locateClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetEffectParameterCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("SetEffectParameterCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }

    // Capture the prior value (or its absence) so revert() is an exact inverse.
    auto param = it->parameters.find(parameter_);
    hadPrior_ = param != it->parameters.end();
    prior_ = hadPrior_ ? param->second : 0.0;
    captured_ = true;

    it->parameters[parameter_] = value_;
    return ok();
}

Result<void> SetEffectParameterCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetEffectParameterCommand: revert before a successful apply"));
    }
    std::optional<MutableClipLocation> loc = locateClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetEffectParameterCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("SetEffectParameterCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }
    if (hadPrior_) {
        it->parameters[parameter_] = prior_;
    } else {
        it->parameters.erase(parameter_);
    }
    return ok();
}

// ===========================================================================
// InspectorViewModel
// ===========================================================================

InspectorViewModel::InspectorViewModel(TimelineEngine& engine, ui::GuiToolGateway* gateway)
    : engine_(engine), gateway_(gateway) {
    // Refresh the panel when the project changes from any surface. The callback
    // only forwards the notification; the projection is pulled lazily on demand.
    subscription_ = engine_.observe([this](const ChangeSet&) { notifyChanged(); });
}

InspectorViewModel::~InspectorViewModel() = default;

void InspectorViewModel::selectClip(ClipId id) {
    if (selected_ != std::optional<ClipId>(id)) {
        selected_ = id;
        notifyChanged();
    }
}

void InspectorViewModel::clearSelection() {
    if (selected_.has_value()) {
        selected_.reset();
        notifyChanged();
    }
}

std::optional<ClipInspectorView> InspectorViewModel::selectedClip() const {
    if (!selected_) {
        return std::nullopt;
    }
    std::optional<Clip> clip = engine_.clip(*selected_);
    if (!clip) {
        return std::nullopt;
    }

    ClipInspectorView view;
    view.id = clip->id;
    view.timelineStart = clip->timelineStart;
    view.sourceIn = clip->sourceIn;
    view.sourceOut = clip->sourceOut;
    view.duration = clip->duration();
    view.gain = clip->gain;
    view.opacity = clip->opacity;

    view.effects.reserve(clip->effects.size());
    for (const Effect& effect : clip->effects) {
        EffectView ev;
        ev.id = effect.id;
        ev.type = effect.type;
        // std::map iterates in sorted key order, giving a deterministic control list.
        ev.parameters.reserve(effect.parameters.size());
        for (const auto& [name, value] : effect.parameters) {
            ev.parameters.push_back(EffectParameterView{name, value});
        }
        view.effects.push_back(std::move(ev));
    }
    return view;
}

CommandResult InspectorViewModel::addEffect(Effect effect) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    if (gateway_ != nullptr) {
        return toCommandResult(gateway_->addEffect(*selected_, effect));
    }
    return engine_.apply(std::make_unique<AddEffectCommand>(*selected_, std::move(effect)));
}

CommandResult InspectorViewModel::addBrightnessEffect(double amount) {
    return addEffect(Effect::brightness(amount));
}

CommandResult InspectorViewModel::addContrastEffect(double amount) {
    return addEffect(Effect::contrast(amount));
}

CommandResult InspectorViewModel::addBlurEffect(double radius) {
    return addEffect(Effect::blur(radius));
}

CommandResult InspectorViewModel::setEffectParameter(Uuid effectId, std::string parameter,
                                                     double value) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    return engine_.apply(std::make_unique<SetEffectParameterCommand>(
        *selected_, effectId, std::move(parameter), value));
}

CommandResult InspectorViewModel::setOpacity(double opacity) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    return engine_.apply(std::make_unique<SetClipPropertyCommand>(
        *selected_, SetClipPropertyCommand::Property::Opacity, opacity));
}

CommandResult InspectorViewModel::setGain(double gain) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    return engine_.apply(std::make_unique<SetClipPropertyCommand>(
        *selected_, SetClipPropertyCommand::Property::Gain, gain));
}

CommandResult InspectorViewModel::trimStart(Duration newSourceIn, FrameRate fps,
                                            Duration sourceDuration) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    if (gateway_ != nullptr) {
        return toCommandResult(
            gateway_->trimClip(*selected_, TrimClipCommand::Edge::Start, newSourceIn,
                               sourceDuration));
    }
    return engine_.apply(std::make_unique<TrimClipCommand>(
        *selected_, TrimClipCommand::Edge::Start, newSourceIn, fps, sourceDuration));
}

CommandResult InspectorViewModel::trimEnd(Duration newSourceOut, FrameRate fps,
                                          Duration sourceDuration) {
    if (!selected_) {
        return CommandResult::failed(failedPrecondition("Inspector: no clip is selected"));
    }
    if (gateway_ != nullptr) {
        return toCommandResult(
            gateway_->trimClip(*selected_, TrimClipCommand::Edge::End, newSourceOut,
                               sourceDuration));
    }
    return engine_.apply(std::make_unique<TrimClipCommand>(
        *selected_, TrimClipCommand::Edge::End, newSourceOut, fps, sourceDuration));
}

void InspectorViewModel::setOnChanged(std::function<void()> callback) {
    onChanged_ = std::move(callback);
}

void InspectorViewModel::notifyChanged() const {
    if (onChanged_) {
        onChanged_();
    }
}

} // namespace ui
} // namespace palmier
