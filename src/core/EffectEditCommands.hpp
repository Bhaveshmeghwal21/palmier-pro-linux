// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/EffectEditCommands.hpp - effects, per-clip properties, transforms and transitions.
//
// One of the six command-family headers behind core/EditCommands.hpp. The grouping mirrors
// EffectEditCommands.cpp exactly, so a command's declaration and its
// implementation are always in the matching pair of files.
//
// The apply()/revert() contract these all share, and the invariants the engine enforces
// around them, are documented once in core/EditCommands.hpp.

#ifndef PALMIER_CORE_EFFECTEDITCOMMANDS_HPP
#define PALMIER_CORE_EFFECTEDITCOMMANDS_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Clip.hpp"
#include "core/ClipGeometry.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/Effect.hpp"
#include "core/Keyframes.hpp"
#include "core/Result.hpp"
#include "core/ToneCurve.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier {

struct Project;  // core/Project.hpp - the mutation target (forward-declared).

// ---------------------------------------------------------------------------
// AddEffectCommand â€” append an effect to a clip's effect chain.
// ---------------------------------------------------------------------------
//
// Appends `effect` to the end of the target clip's effects list. revert() removes
// the effect by its id.
class AddEffectCommand final : public EditCommand {
public:
    AddEffectCommand(ClipId clipId, Effect effect);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddEffect"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Effect effect_;
};

// ===========================================================================
// Effect lifecycle management (usable-editor task 9; Requirement 6)
//
// AddEffectCommand already appends an effect; the three commands below cover the
// rest of a clip's effect chain lifecycle: removing one, reordering the chain (the
// rendered result depends on effect order, Requirement 6.4), and changing an
// existing effect's parameter. All three name an effect by its stable Uuid and
// refuse â€” leaving the project unchanged â€” when the clip or the named effect is
// not found (Requirement 6.5).
// ===========================================================================

// ---------------------------------------------------------------------------
// RemoveEffectCommand â€” remove one effect from a clip's chain by id.
// ---------------------------------------------------------------------------
class RemoveEffectCommand final : public EditCommand {
public:
    RemoveEffectCommand(ClipId clipId, Uuid effectId);

    [[nodiscard]] std::string_view name() const noexcept override { return "RemoveEffect"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Uuid effectId_;
    std::size_t index_ = 0;
    std::optional<Effect> removed_;  // captured on apply for an exact revert
};

// ---------------------------------------------------------------------------
// ReorderEffectsCommand â€” reorder a clip's effect chain (preserves effect count).
// ---------------------------------------------------------------------------
//
// A pure permutation of the chain: unlike ReorderClipsCommand, no positional
// field needs recomputing, since an effect carries no timeline geometry of its
// own. `newOrder` must name every effect currently on the clip exactly once;
// anything else â€” a wrong count, an unknown id, a repeat â€” is refused.
class ReorderEffectsCommand final : public EditCommand {
public:
    ReorderEffectsCommand(ClipId clipId, std::vector<Uuid> newOrder);

    [[nodiscard]] std::string_view name() const noexcept override { return "ReorderEffects"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    std::vector<Uuid> newOrder_;
    std::vector<Effect> priorEffects_;  // captured on apply for an exact revert
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// SetEffectParameterCommand â€” set (or insert) a named parameter on one effect.
// ---------------------------------------------------------------------------
//
// revert() restores the parameter's prior value, or removes the key entirely if
// it did not previously exist, so a parameter that was absent before apply() is
// absent again after revert() rather than left at 0.
class SetEffectParameterCommand final : public EditCommand {
public:
    SetEffectParameterCommand(ClipId clipId, Uuid effectId, std::string parameter, double value);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetEffectParameter"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Uuid effectId_;
    std::string parameter_;
    double value_;
    bool hadPrior_ = false;
    double prior_ = 0.0;
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// EditCurvePointCommand â€” add, move or remove one tone-curve control point.
// ---------------------------------------------------------------------------
//
// Requirement 5.7 (monitoring-and-grading): adding, moving and removing a control
// point are each ONE undoable edit. SetEffectParameterCommand cannot serve, because a
// point is a pair: adding one sets two parameters and would therefore be two history
// entries, so a single Undo would leave a half-written point behind â€” an X with no Y,
// which core::curvePoints deliberately refuses to read as a point. The user would see
// their point vanish and the curve change shape in a way no single action explains.
//
// The three operations are one command rather than three because they share their
// target (clip, effect, channel) and differ only in which coordinates matter. What
// makes them one *undoable* thing is the inverse below, which is identical for all
// three.
//
// revert() restores THE WHOLE CHANNEL'S prior parameter set rather than undoing the
// specific mutation. That is not laziness: removing a middle point renumbers every
// point after it, so the inverse of "remove p1" is not "add a point at p1" but
// "restore the numbering that existed before". Capturing the channel wholesale makes
// the inverse exact for every operation, including that one, without a special case â€”
// and the captured set is at most a few dozen doubles.
class EditCurvePointCommand final : public EditCommand {
public:
    enum class Operation { Add, Move, Remove };

    /// `index` is ignored for Add (the point is appended); `x`/`y` are ignored for
    /// Remove. Add appends rather than inserting at `index` because rendering sorts by
    /// x regardless, so an insertion position would be a distinction without a
    /// difference â€” while a point's index IS its identity for a later move or remove.
    EditCurvePointCommand(ClipId clipId, Uuid effectId, CurveChannel channel, Operation operation,
                          std::size_t index, double x, double y);

    [[nodiscard]] std::string_view name() const noexcept override { return "EditCurvePoint"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Uuid effectId_;
    CurveChannel channel_;
    Operation operation_;
    std::size_t index_;
    double x_;
    double y_;
    /// Every parameter of this channel as it stood before apply(), and nothing else:
    /// an edit to the master curve must not restore the red curve.
    std::map<std::string, double> prior_;
    bool captured_ = false;
};

// ---------------------------------------------------------------------------
// SetEffectResourceCommand â€” point one effect at an external resource, or clear it.
// ---------------------------------------------------------------------------
//
// Requirement 7.9 (monitoring-and-grading): applying a LUT by path is ONE undoable edit.
// SetEffectParameterCommand cannot serve, because a path is a string and that map holds
// doubles â€” which is the whole reason schema 1.5 added the field.
//
// revert() restores the prior path, INCLUDING when that path was empty. An empty prior is
// the common case (an effect just added has no LUT yet), so treating "no prior" as "nothing
// to restore" would make the first LUT applied to an effect un-undoable â€” the failure mode
// SetEffectParameterCommand's hadPrior_ exists to avoid, in a case where the absent value
// is representable and so needs no flag.
class SetEffectResourceCommand final : public EditCommand {
public:
    /// An empty `path` clears the resource, which is how a LUT is removed without removing
    /// the effect (Requirement 7.8 keeps the effect in the chain either way).
    SetEffectResourceCommand(ClipId clipId, Uuid effectId, std::string path);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetEffectResource"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId clipId_;
    Uuid effectId_;
    std::string path_;
    std::string prior_;
    bool captured_ = false;
};

/// The operation's name on the tool surface and in the UI, e.g. "move".
///
/// Beside the enum rather than inside the tool registry, so the Inspector and the tool
/// surface name the same operation the same way without either owning the vocabulary.
[[nodiscard]] const char* curvePointOperationToolName(
    EditCurvePointCommand::Operation operation) noexcept;

/// Parse a tool-surface operation name; nullopt when it names no operation.
[[nodiscard]] std::optional<EditCurvePointCommand::Operation> parseCurvePointOperationToolName(
    std::string_view name);

/// The three tool-surface operation names, in add/move/remove order.
[[nodiscard]] const std::vector<std::string>& curvePointOperationToolNames();

// ---------------------------------------------------------------------------
// SetClipPropertiesCommand â€” change a clip's per-clip properties after creation.
// ---------------------------------------------------------------------------
//
// `opacity` and `gain` could previously only be supplied as arguments to
// `timeline.add_clip`, so they were effectively write-once: adjusting a composite's
// blend or a bed's level meant deleting the clip and adding it again, losing its id
// and any effects, transition or text style attached to it. Upstream's
// `set_clip_properties` changes them at any time, and an agent iterating on a look
// needs to.
//
// Every field is optional and an absent field is left alone, so one call can change
// a single property without having to restate the others (which the caller may not
// know). A call carrying no field at all is applied as a no-op, matching
// SetTrackMutedCommand's treatment of a flag set to the value it already holds: a
// legal, idempotent edit whose revert is equally a no-op.
//
// Both prior values are captured on apply() so revert() restores exactly what was
// there, including for a field this command did not touch.
class SetClipPropertiesCommand final : public EditCommand {
public:
    /// The properties to change. An absent field leaves the clip's value alone.
    struct Properties {
        std::optional<double> opacity;
        std::optional<double> gain;
        std::optional<double> centerX;
        std::optional<double> centerY;
        std::optional<double> scale;
        std::optional<double> rotationDegrees;
        std::optional<bool> flipHorizontal;
        std::optional<bool> flipVertical;
        std::optional<Duration> fadeIn;
        std::optional<Duration> fadeOut;
        std::optional<double> speed;

        /// True when at least one field is present.
        [[nodiscard]] bool any() const noexcept {
            return opacity.has_value() || gain.has_value() || centerX.has_value() ||
                   centerY.has_value() || scale.has_value() || rotationDegrees.has_value() ||
                   flipHorizontal.has_value() || flipVertical.has_value() || fadeIn.has_value() ||
                   fadeOut.has_value() || speed.has_value();
        }
    };

    SetClipPropertiesCommand(ClipId clipId, Properties properties);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetClipProperties"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The target clip.
    [[nodiscard]] ClipId clipId() const noexcept { return clipId_; }

    /// The properties this command installs.
    [[nodiscard]] const Properties& properties() const noexcept { return properties_; }

private:
    ClipId clipId_;
    Properties properties_;
    double priorOpacity_{1.0};
    double priorGain_{1.0};
    Transform priorTransform_{};
    Duration priorFadeIn_{Duration::zero()};
    Duration priorFadeOut_{Duration::zero()};
    double priorSpeed_{kUnitSpeed};
    bool captured_{false};  // set on apply so revert can refuse without one
};

// ---------------------------------------------------------------------------
// SetKeyframesCommand â€” replace one property's animation on one clip.
// ---------------------------------------------------------------------------
//
// Replaces the whole keyframe list for a single property, rather than adding or moving individual
// keyframes. That makes the operation idempotent â€” sending the same list twice leaves the same
// animation â€” and it means a caller never has to reason about the identity of a keyframe it did not
// create. An empty list removes the property's animation, returning it to the clip's static value,
// which is the only way back and so deserves a spelling.
//
// The list is sorted on the way in and keyframes sharing an instant collapse to the last given,
// because a property cannot hold two values at one moment and refusing the edit would be a worse
// answer than honouring the most recent one.
class SetKeyframesCommand final : public EditCommand {
public:
    SetKeyframesCommand(ClipId clipId, AnimatableProperty property,
                        std::vector<Keyframe> keyframes);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetKeyframes"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The target clip.
    [[nodiscard]] ClipId clipId() const noexcept { return clipId_; }

    /// The property whose animation this command installs.
    [[nodiscard]] AnimatableProperty property() const noexcept { return property_; }

    /// The keyframes this command installs, sorted and deduplicated.
    [[nodiscard]] const std::vector<Keyframe>& keyframes() const noexcept { return keyframes_; }

private:
    ClipId clipId_;
    AnimatableProperty property_;
    std::vector<Keyframe> keyframes_;
    std::vector<KeyframeTrack> prior_;  // the clip's whole track list, for an exact revert
    bool captured_{false};
};

// ---------------------------------------------------------------------------
// SetClipTransformsCommand â€” place several clips at once.
// ---------------------------------------------------------------------------
//
// One edit covering many clips, which is what an arrangement is: undoing a split screen has to
// restore every participant's placement together, and a half-applied layout is not a state
// anybody asked for. Applying N separate SetClipPropertiesCommands would put N entries on the
// undo stack and leave the timeline in an arrangement nobody chose if one of them failed.
//
// Refused, with the project unchanged, if ANY named clip is missing â€” checked before the first
// assignment, so a bad clip id cannot leave a partial arrangement behind.
class SetClipTransformsCommand final : public EditCommand {
public:
    /// A clip and where it should sit.
    struct Placement {
        ClipId clipId;
        Transform transform;
    };

    explicit SetClipTransformsCommand(std::vector<Placement> placements);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetClipTransforms"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The placements this command installs.
    [[nodiscard]] const std::vector<Placement>& placements() const noexcept { return placements_; }

private:
    std::vector<Placement> placements_;
    std::vector<Transform> prior_;  // parallel to placements_, captured on apply
    bool captured_{false};
};

// ---------------------------------------------------------------------------
// SetTransitionCommand â€” set a clip's incoming transition.
// ---------------------------------------------------------------------------
//
// Replaces the target clip's `transitionIn` with `transition`, capturing the
// prior value (which may be absent) so revert() restores it exactly. This is the
// command behind the `timeline.add_transition` tool; it lives in the core so that
// transitions are applied through the same command path â€” atomic, undoable,
// observable, invariant-checked â€” as every other edit.
class SetTransitionCommand final : public EditCommand {
public:
    SetTransitionCommand(ClipId clipId, Transition transition);

    [[nodiscard]] std::string_view name() const noexcept override { return "AddTransition"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

    /// The identifier of the transition this command installs.
    [[nodiscard]] Uuid transitionId() const noexcept { return transition_.id; }

private:
    ClipId clipId_;
    Transition transition_;
    std::optional<Transition> prior_;  // captured on apply for an exact revert
    bool captured_ = false;
};

}  // namespace palmier

#endif  // PALMIER_CORE_EFFECTEDITCOMMANDS_HPP
