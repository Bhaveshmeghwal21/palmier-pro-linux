// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EffectEditCommands.cpp — the effect and transition edits, including tone-curve point editing and
// effect resource assignment.
//
// Split out of core/EditCommands.cpp, which held all 24 command implementations in
// one 1583-line translation unit. Every definition below was moved rather than
// rewritten. The commands are declared in the public core/EditCommands.hpp, which
// is unchanged; the helpers they share are in core/EditCommandSupport.hpp.

#include "core/EditCommands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/EditCommandSupport.hpp"
#include "core/Error.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"

namespace palmier {

using namespace palmier::detail;  // NOLINT(google-build-using-namespace)

namespace {

/// Locate one effect, or say which of the two things was missing.
Result<std::vector<Effect>::iterator> findEffect(Project& project, ClipId clipId,
                                                 const Uuid& effectId, const char* who) {
    std::optional<ClipLocation> loc = findClip(project, clipId);
    if (!loc) {
        return err<std::vector<Effect>::iterator>(
            notFound(std::string(who) + ": clip " + idLabel(clipId) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId; });
    if (it == effects.end()) {
        return err<std::vector<Effect>::iterator>(
            notFound(std::string(who) + ": effect " + idLabel(effectId) + " not found on clip " +
                     idLabel(clipId)));
    }
    return ok(it);
}

/// Every parameter of one channel's points, keyed by full parameter name.
///
/// Selected by name prefix rather than by walking indices, because the whole point of
/// the capture is to restore state that may not be contiguous — a set walked with
/// curvePoints() would stop at the first gap and quietly fail to restore anything
/// beyond it.
std::map<std::string, double> channelParameters(const std::map<std::string, double>& all,
                                                CurveChannel channel) {
    const std::string prefix = curveChannelParameterPrefix(channel);
    std::map<std::string, double> mine;
    for (const auto& [key, value] : all) {
        if (key.compare(0, prefix.size(), prefix) == 0) {
            mine.emplace(key, value);
        }
    }
    return mine;
}

}  // namespace

// ===========================================================================
// AddEffectCommand
// ===========================================================================

AddEffectCommand::AddEffectCommand(ClipId clipId, Effect effect)
    : clipId_(clipId), effect_(std::move(effect)) {}

Result<void> AddEffectCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].effects.push_back(effect_);
    return ok();
}

Result<void> AddEffectCommand::revert(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    auto& effects = loc->track->clips[loc->index].effects;
    effects.erase(std::remove_if(effects.begin(), effects.end(),
                                 [&](const Effect& e) { return e.id == effect_.id; }),
                  effects.end());
    return ok();
}

// ===========================================================================
// SetClipPropertiesCommand
// ===========================================================================

SetClipPropertiesCommand::SetClipPropertiesCommand(ClipId clipId, Properties properties)
    : clipId_(clipId), properties_(properties) {}

Result<void> SetClipPropertiesCommand::apply(Project& project) {
    // A call naming no property at all is applied as a no-op rather than refused, for the same
    // reason SetTrackMutedCommand accepts setting a flag to the value it already holds: it is a
    // legal, idempotent edit whose revert is equally a no-op. Refusing it would also put the
    // advertised schema and the handler in permanent disagreement, because "at least one of
    // these optional arguments" is a relation between arguments that the schema cannot state.
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetClipProperties: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];

    // Capture both, not only the fields being changed: revert() then restores the clip's whole
    // property state regardless of which fields this command carried.
    priorOpacity_ = clip.opacity;
    priorGain_ = clip.gain;
    priorTransform_ = clip.transform;
    priorFadeIn_ = clip.fadeIn;
    priorFadeOut_ = clip.fadeOut;
    priorSpeed_ = clip.speed;
    captured_ = true;

    if (properties_.opacity.has_value()) clip.opacity = *properties_.opacity;
    if (properties_.gain.has_value()) clip.gain = *properties_.gain;
    if (properties_.centerX.has_value()) clip.transform.centerX = *properties_.centerX;
    if (properties_.centerY.has_value()) clip.transform.centerY = *properties_.centerY;
    if (properties_.scale.has_value()) clip.transform.scale = *properties_.scale;
    if (properties_.rotationDegrees.has_value()) {
        clip.transform.rotationDegrees = *properties_.rotationDegrees;
    }
    if (properties_.flipHorizontal.has_value()) {
        clip.transform.flipHorizontal = *properties_.flipHorizontal;
    }
    if (properties_.flipVertical.has_value()) {
        clip.transform.flipVertical = *properties_.flipVertical;
    }
    if (properties_.fadeIn.has_value()) clip.fadeIn = *properties_.fadeIn;
    if (properties_.fadeOut.has_value()) clip.fadeOut = *properties_.fadeOut;
    if (properties_.speed.has_value()) clip.speed = *properties_.speed;

    // A slower speed makes the clip LONGER -- duration() is the source span divided by speed -- so
    // it can push the clip past the one after it. Refused rather than applied, for the same reason
    // MoveClipCommand refuses an overlapping drop: a successful tool call must not leave a timeline
    // whose clips overlap outside a transition region. Making room first, or rippling, is the
    // caller's decision and not something to do as a side effect of setting a property.
    //
    // Only speed needs the check. Every other property here leaves the clip's length alone.
    if (properties_.speed.has_value() && !trackOrderedAndNonOverlapping(*loc->track)) {
        clip.opacity = priorOpacity_;
        clip.gain = priorGain_;
        clip.transform = priorTransform_;
        clip.fadeIn = priorFadeIn_;
        clip.fadeOut = priorFadeOut_;
        clip.speed = priorSpeed_;
        captured_ = false;  // the apply did not take, so a later revert must be refused
        return err(failedPrecondition("SetClipProperties: speed " +
                                      std::to_string(*properties_.speed) + " would lengthen clip " +
                                      idLabel(clipId_) +
                                      " past the next clip on the track; make room first"));
    }
    return ok();
}

Result<void> SetClipPropertiesCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetClipProperties: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetClipProperties: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    clip.opacity = priorOpacity_;
    clip.gain = priorGain_;
    clip.transform = priorTransform_;
    clip.fadeIn = priorFadeIn_;
    clip.fadeOut = priorFadeOut_;
    clip.speed = priorSpeed_;
    return ok();
}

// ===========================================================================
// SetKeyframesCommand
// ===========================================================================

SetKeyframesCommand::SetKeyframesCommand(ClipId clipId, AnimatableProperty property,
                                         std::vector<Keyframe> keyframes)
    : clipId_(clipId), property_(property), keyframes_(std::move(keyframes)) {
    sortKeyframes(keyframes_);
}

Result<void> SetKeyframesCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetKeyframes: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];

    // The whole track list is captured, not just this property's track, so revert restores the
    // clip's animation exactly whether this command added a track, replaced one or removed one.
    prior_ = clip.keyframes;
    captured_ = true;

    std::erase_if(clip.keyframes,
                  [this](const KeyframeTrack& track) { return track.property == property_; });
    if (!keyframes_.empty()) {
        KeyframeTrack track;
        track.property = property_;
        track.keyframes = keyframes_;
        clip.keyframes.push_back(std::move(track));
    }
    return ok();
}

Result<void> SetKeyframesCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetKeyframes: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("SetKeyframes: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].keyframes = prior_;
    return ok();
}

// ===========================================================================
// SetClipTransformsCommand
// ===========================================================================

SetClipTransformsCommand::SetClipTransformsCommand(std::vector<Placement> placements)
    : placements_(std::move(placements)) {}

Result<void> SetClipTransformsCommand::apply(Project& project) {
    if (placements_.empty()) {
        return err(invalidArgument("SetClipTransforms: no clip was named"));
    }

    // Resolve every clip before writing anything, so a bad id refuses the whole arrangement
    // rather than leaving some clips moved and others not.
    std::vector<ClipLocation> located;
    located.reserve(placements_.size());
    for (const Placement& placement : placements_) {
        std::optional<ClipLocation> loc = findClip(project, placement.clipId);
        if (!loc) {
            return err(
                notFound("SetClipTransforms: clip " + idLabel(placement.clipId) + " not found"));
        }
        located.push_back(*loc);
    }

    prior_.clear();
    prior_.reserve(placements_.size());
    for (std::size_t i = 0; i < placements_.size(); ++i) {
        Clip& clip = located[i].track->clips[located[i].index];
        prior_.push_back(clip.transform);
        clip.transform = placements_[i].transform;
    }
    captured_ = true;
    return ok();
}

Result<void> SetClipTransformsCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("SetClipTransforms: revert before a successful apply"));
    }
    for (std::size_t i = 0; i < placements_.size(); ++i) {
        std::optional<ClipLocation> loc = findClip(project, placements_[i].clipId);
        if (!loc) {
            return err(notFound("SetClipTransforms: clip " + idLabel(placements_[i].clipId) +
                                " not found"));
        }
        loc->track->clips[loc->index].transform = prior_[i];
    }
    return ok();
}

// ===========================================================================
// SetTransitionCommand
// ===========================================================================

SetTransitionCommand::SetTransitionCommand(ClipId clipId, Transition transition)
    : clipId_(clipId), transition_(std::move(transition)) {}

Result<void> SetTransitionCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddTransition: clip " + idLabel(clipId_) + " not found"));
    }
    Clip& clip = loc->track->clips[loc->index];
    prior_ = clip.transitionIn;
    captured_ = true;
    clip.transitionIn = transition_;
    return ok();
}

Result<void> SetTransitionCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("AddTransition: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("AddTransition: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].transitionIn = prior_;
    return ok();
}

// ===========================================================================
// RemoveEffectCommand
// ===========================================================================

RemoveEffectCommand::RemoveEffectCommand(ClipId clipId, Uuid effectId)
    : clipId_(clipId), effectId_(effectId) {}

Result<void> RemoveEffectCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RemoveEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    auto it = std::find_if(effects.begin(), effects.end(),
                           [&](const Effect& e) { return e.id == effectId_; });
    if (it == effects.end()) {
        return err(notFound("RemoveEffectCommand: effect " + idLabel(effectId_) +
                            " not found on clip " + idLabel(clipId_)));
    }
    index_ = static_cast<std::size_t>(std::distance(effects.begin(), it));
    removed_ = *it;
    effects.erase(it);
    return ok();
}

Result<void> RemoveEffectCommand::revert(Project& project) {
    if (!removed_) {
        return err(failedPrecondition("RemoveEffectCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("RemoveEffectCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;
    const std::size_t index = std::min(index_, effects.size());
    effects.insert(effects.begin() + static_cast<std::ptrdiff_t>(index), *removed_);
    return ok();
}

// ===========================================================================
// ReorderEffectsCommand
// ===========================================================================

ReorderEffectsCommand::ReorderEffectsCommand(ClipId clipId, std::vector<Uuid> newOrder)
    : clipId_(clipId), newOrder_(std::move(newOrder)) {}

Result<void> ReorderEffectsCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("ReorderEffectsCommand: clip " + idLabel(clipId_) + " not found"));
    }
    std::vector<Effect>& effects = loc->track->clips[loc->index].effects;

    // newOrder_ must be a permutation of the clip's current effect ids.
    if (newOrder_.size() != effects.size()) {
        return err(invalidArgument(
            "ReorderEffectsCommand: new order size does not match the clip's effect count"));
    }
    std::unordered_map<Uuid, std::size_t> present;
    present.reserve(effects.size());
    for (const Effect& effect : effects) {
        ++present[effect.id];
    }
    for (const Uuid& id : newOrder_) {
        auto it = present.find(id);
        if (it == present.end() || it->second == 0) {
            return err(invalidArgument(
                "ReorderEffectsCommand: new order is not a permutation of the clip's effects"));
        }
        --it->second;
    }

    priorEffects_ = effects;  // capture for an exact revert
    captured_ = true;

    // A pure permutation: no field of an Effect depends on its position in the
    // chain, so the new order is simply the requested ids looked up in the prior
    // list (Requirement 6.4 — rendering, not this command, is what reads the
    // resulting order and changes output accordingly).
    std::vector<Effect> reordered;
    reordered.reserve(newOrder_.size());
    for (const Uuid& id : newOrder_) {
        auto it = std::find_if(priorEffects_.begin(), priorEffects_.end(),
                               [&](const Effect& e) { return e.id == id; });
        reordered.push_back(*it);
    }
    effects = std::move(reordered);
    return ok();
}

Result<void> ReorderEffectsCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("ReorderEffectsCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
    if (!loc) {
        return err(notFound("ReorderEffectsCommand: clip " + idLabel(clipId_) + " not found"));
    }
    loc->track->clips[loc->index].effects = priorEffects_;
    return ok();
}

// ===========================================================================
// SetEffectParameterCommand
// ===========================================================================

SetEffectParameterCommand::SetEffectParameterCommand(ClipId clipId, Uuid effectId,
                                                     std::string parameter, double value)
    : clipId_(clipId), effectId_(effectId), parameter_(std::move(parameter)), value_(value) {}

Result<void> SetEffectParameterCommand::apply(Project& project) {
    std::optional<ClipLocation> loc = findClip(project, clipId_);
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
        return err(
            failedPrecondition("SetEffectParameterCommand: revert before a successful apply"));
    }
    std::optional<ClipLocation> loc = findClip(project, clipId_);
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
// EditCurvePointCommand
// ===========================================================================

EditCurvePointCommand::EditCurvePointCommand(ClipId clipId, Uuid effectId, CurveChannel channel,
                                             Operation operation, std::size_t index, double x,
                                             double y)
    : clipId_(clipId),
      effectId_(effectId),
      channel_(channel),
      operation_(operation),
      index_(index),
      x_(x),
      y_(y) {}

Result<void> EditCurvePointCommand::apply(Project& project) {
    auto found = findEffect(project, clipId_, effectId_, "EditCurvePointCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();

    // The points as they stand, and the whole channel as it stands. The points drive
    // validation; the parameter set is the inverse.
    std::vector<CurvePoint> points = curvePoints(it->parameters, channel_);
    prior_ = channelParameters(it->parameters, channel_);

    if (operation_ != Operation::Add && index_ >= points.size()) {
        return err(notFound("EditCurvePointCommand: point " + std::to_string(index_) +
                            " does not exist on the " + curveChannelName(channel_) +
                            " curve, which has " + std::to_string(points.size()) + " point(s)"));
    }

    switch (operation_) {
        case Operation::Add:
            points.push_back(CurvePoint{x_, y_});
            break;
        case Operation::Move:
            points[index_] = CurvePoint{x_, y_};
            break;
        case Operation::Remove:
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(index_));
            break;
    }

    // Rewrite the channel from scratch. Erasing first is what keeps the indices
    // contiguous after a removal: writing the shortened list over the old one would
    // leave the final point's coordinates behind as a duplicate at the end.
    for (const auto& [key, value] : prior_) {
        (void)value;
        it->parameters.erase(key);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        it->parameters[curvePointParameterName(channel_, i, /*isY=*/false)] = points[i].x;
        it->parameters[curvePointParameterName(channel_, i, /*isY=*/true)] = points[i].y;
    }

    captured_ = true;
    return ok();
}

Result<void> EditCurvePointCommand::revert(Project& project) {
    if (!captured_) {
        return err(failedPrecondition("EditCurvePointCommand: revert before a successful apply"));
    }
    auto found = findEffect(project, clipId_, effectId_, "EditCurvePointCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();

    // Remove whatever this channel holds now, then put back exactly what it held
    // before. Both halves are needed: without the erase, undoing an Add would leave
    // the added point in place; without the restore, undoing a Move or Remove would
    // leave the channel empty.
    const std::map<std::string, double> current = channelParameters(it->parameters, channel_);
    for (const auto& [key, value] : current) {
        (void)value;
        it->parameters.erase(key);
    }
    for (const auto& [key, value] : prior_) {
        it->parameters[key] = value;
    }
    return ok();
}

// ===========================================================================
// SetEffectResourceCommand
// ===========================================================================

SetEffectResourceCommand::SetEffectResourceCommand(ClipId clipId, Uuid effectId, std::string path)
    : clipId_(clipId), effectId_(effectId), path_(std::move(path)) {}

Result<void> SetEffectResourceCommand::apply(Project& project) {
    auto found = findEffect(project, clipId_, effectId_, "SetEffectResourceCommand");
    if (!found) {
        return err(found.error());
    }
    auto it = found.value();
    prior_ = it->resourcePath;
    captured_ = true;
    it->resourcePath = path_;
    // Deliberately no existence check. Requirement 7.8 requires a missing LUT to leave the
    // effect in the chain and render un-graded, so refusing an unreadable path here would
    // make a project that opens on one machine un-editable on another.
    return ok();
}

Result<void> SetEffectResourceCommand::revert(Project& project) {
    if (!captured_) {
        return err(
            failedPrecondition("SetEffectResourceCommand: revert before a successful apply"));
    }
    auto found = findEffect(project, clipId_, effectId_, "SetEffectResourceCommand");
    if (!found) {
        return err(found.error());
    }
    // Restores an EMPTY prior too, which is the common case: an effect just added has no
    // resource, so skipping the restore when the prior was empty would leave the first LUT
    // applied to an effect un-undoable.
    found.value()->resourcePath = prior_;
    return ok();
}

}  // namespace palmier
