// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/InspectorViewModel.hpp — the Qt-free presentation model behind the Inspector
// and Effects panel (task 19.4).
//
// The Inspector/Effects panel lets the user inspect the currently selected clip
// and adjust its properties and its effect chain (Requirement 2.4 — trimming a
// clip's boundaries; plus per-clip opacity/gain and per-effect parameters). Like
// every other editing surface in Palmier (the timeline view, the MCP server, and
// the in-app agent), the panel must drive all mutations through EditCommands
// applied on the TimelineEngine so they are atomic, undoable, and observable
// (design.md "All mutations flow through a Command object").
//
// This class is deliberately split out from any Qt type so the whole
// selection -> read -> edit -> command mapping is unit-testable without Qt (which
// is not present in every build/CI environment). The thin QWidget/QML surface
// (InspectorPanel, guarded by PALMIER_HAVE_QT) simply renders the view structs
// this model exposes and forwards user gestures to the mutation methods below.
//
// Reads are pulled on demand from the engine's immutable snapshot()/clip() query
// surface, so the model never caches stale project state. The model also observes
// the engine, so an edit issued from any other surface (undo/redo, MCP, agent)
// refreshes the panel through the same onChanged callback.
//
// Gateway routing (task 11.4; Requirements 1.7, 9.4, 11.5): addEffect() (and its
// addBrightnessEffect/addContrastEffect/addBlurEffect convenience wrappers),
// trimStart()/trimEnd(), removeEffect(), reorderEffects() and
// setEffectParameter() (task 9) are routed through an optional
// `ui::GuiToolGateway` when one is installed, because each has a published tool
// (`timeline.add_effect`, `timeline.trim_clip`, `timeline.remove_effect`,
// `timeline.reorder_effects`, `timeline.set_effect_parameter`) that the MCP
// endpoint and the in-app agent already call through the identical EditCommand
// path. setOpacity() and setGain() have NO tool-surface equivalent —
// SetClipPropertyCommand is Inspector-only, not exposed as an MCP tool — so it
// always calls TimelineEngine::apply directly; routing it through a nonexistent
// tool is not an option this task adds. With no gateway installed (the default),
// every mutation calls the engine directly, exactly as before.

#ifndef PALMIER_UI_INSPECTORVIEWMODEL_HPP
#define PALMIER_UI_INSPECTORVIEWMODEL_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/Result.hpp"
#include "core/Subscription.hpp"
#include "core/TextStyle.hpp"
#include "core/Uuid.hpp"

namespace palmier {

class TimelineEngine;

namespace ui { class GuiToolGateway; }  // ui/GuiToolGateway.hpp

namespace ui {

// ---------------------------------------------------------------------------
// View data — the flattened, read-only projection the panel renders.
// ---------------------------------------------------------------------------

/// One named scalar parameter of an effect, ready to bind to a slider/spinbox.
struct EffectParameterView {
    std::string name;
    double      value = 0.0;
};

/// One effect in the selected clip's chain, with its parameters in a stable
/// (name-sorted) order so the rendered control list is deterministic.
struct EffectView {
    Uuid                             id;
    EffectType                       type = EffectType::Custom;
    std::vector<EffectParameterView> parameters;
};

/// The read-only projection of a text clip's styling (usable-editor task 12;
/// Requirement 9), present on ClipInspectorView iff the selected clip is a
/// text clip (Clip::isTextClip()).
struct TextStyleView {
    std::string   content;
    std::string   fontFamily;
    double        pointSize = 24.0;
    double        colorR = 1.0;
    double        colorG = 1.0;
    double        colorB = 1.0;
    double        colorA = 1.0;
    TextAlignment alignment = TextAlignment::Center;
    double        x = 0.5;
    double        y = 0.5;
};

/// The full read-only projection of the selected clip that the Inspector shows:
/// its timeline/source geometry, its adjustable properties, and its effect chain.
struct ClipInspectorView {
    ClipId                       id;
    Duration                     timelineStart;
    Duration                     sourceIn;
    Duration                     sourceOut;
    Duration                     duration;   ///< sourceOut - sourceIn (timeline extent).
    double                       gain = 1.0;
    double                       opacity = 1.0;
    std::vector<EffectView>      effects;
    std::optional<TextStyleView> textStyle;  ///< Present iff this is a text clip.
    std::optional<std::string>  captionText; ///< Present iff this is a caption cue.
};

// ---------------------------------------------------------------------------
// EditCommands issued by the Inspector for edits the core does not already model.
//
// Adding an effect (AddEffectCommand) and trimming a clip edge (TrimClipCommand)
// already have concrete core commands; the Inspector reuses those. Adjusting a
// clip's opacity/gain and editing an existing effect's parameter do not, so the
// two small, self-contained commands below provide them on the same undoable
// EditCommand path. Both capture the exact prior state so revert() is an exact
// inverse, and both leave the project unchanged on their own failure paths.
// ---------------------------------------------------------------------------

/// Set a scalar property (opacity or gain) of a clip. The engine's invariant
/// check enforces the valid ranges (opacity in [0,1], gain >= 0) and rolls back
/// an out-of-range value, so the Inspector surfaces the rejection without needing
/// to duplicate the bounds here.
class SetClipPropertyCommand final : public EditCommand {
public:
    enum class Property { Opacity, Gain };

    SetClipPropertyCommand(ClipId clipId, Property property, double value);

    [[nodiscard]] std::string_view name() const noexcept override { return "SetClipProperty"; }
    [[nodiscard]] Result<void> apply(Project& project) override;
    [[nodiscard]] Result<void> revert(Project& project) override;

private:
    ClipId   clipId_;
    Property property_;
    double   value_;
    double   prior_ = 0.0;
    bool     captured_ = false;
};

// SetEffectParameterCommand moved to core::EditCommands (usable-editor task 9;
// Requirement 6.1), since a parameter change is now a Tool_Surface operation
// (`timeline.set_effect_parameter`) and not Inspector-only. setEffectParameter()
// below constructs the core:: type.

// ---------------------------------------------------------------------------
// InspectorViewModel — selection state + read projection + edit -> command map.
// ---------------------------------------------------------------------------

class InspectorViewModel {
public:
    /// Binds the model to an engine. The engine must outlive the model. The model
    /// observes the engine so external edits refresh the panel via onChanged.
    /// When `gateway` is non-null (it must then outlive this model), addEffect()
    /// and trimStart()/trimEnd() route through it instead of calling
    /// `TimelineEngine::apply` directly (task 11.4).
    explicit InspectorViewModel(TimelineEngine& engine, ui::GuiToolGateway* gateway = nullptr);

    ~InspectorViewModel();

    InspectorViewModel(const InspectorViewModel&) = delete;
    InspectorViewModel& operator=(const InspectorViewModel&) = delete;

    // --- Selection ---------------------------------------------------------

    /// Select the clip the Inspector should show. Selecting an id that is not in
    /// the project is allowed (the projection will simply be empty until such a
    /// clip appears); this mirrors selecting a clip that a later edit removes.
    void selectClip(ClipId id);

    /// Clear the current selection (the Inspector shows nothing).
    void clearSelection();

    [[nodiscard]] bool hasSelection() const noexcept { return selected_.has_value(); }
    [[nodiscard]] std::optional<ClipId> selectedClipId() const noexcept { return selected_; }

    // --- Read projection ---------------------------------------------------

    /// The current projection of the selected clip, read from the engine's live
    /// snapshot. std::nullopt when nothing is selected or the selected clip is not
    /// (currently) present in the project.
    [[nodiscard]] std::optional<ClipInspectorView> selectedClip() const;

    // --- Mutations (each maps to an EditCommand via TimelineEngine::apply) --

    /// Append an effect to the selected clip's chain (AddEffectCommand).
    [[nodiscard]] CommandResult addEffect(Effect effect);

    /// Convenience wrappers building the named effect factories, then addEffect.
    [[nodiscard]] CommandResult addBrightnessEffect(double amount);
    [[nodiscard]] CommandResult addContrastEffect(double amount);
    [[nodiscard]] CommandResult addBlurEffect(double radius);

    /// Set a parameter on an existing effect in the selected clip
    /// (SetEffectParameterCommand).
    [[nodiscard]] CommandResult setEffectParameter(Uuid effectId, std::string parameter,
                                                   double value);

    /// Add, move or remove one tone-curve control point on an effect in the selected
    /// clip (EditCurvePointCommand; monitoring-and-grading Requirement 5.7).
    ///
    /// `index` is ignored for Add and `point` for Remove, matching the command and the
    /// tool: the caller does not have to invent values the operation will not use.
    [[nodiscard]] CommandResult editCurvePoint(Uuid effectId, CurveChannel channel,
                                               EditCurvePointCommand::Operation operation,
                                               std::size_t index, CurvePoint point);

    /// Remove an effect from the selected clip's chain by id (RemoveEffectCommand;
    /// Requirement 6.1, 6.2).
    [[nodiscard]] CommandResult removeEffect(Uuid effectId);

    /// Reorder the selected clip's effect chain (ReorderEffectsCommand;
    /// Requirement 6.1, 6.4). `newOrder` must name every effect currently on the
    /// clip exactly once.
    [[nodiscard]] CommandResult reorderEffects(std::vector<Uuid> newOrder);

    /// Adjust the selected clip's opacity / gain (SetClipPropertyCommand). An
    /// out-of-range value is rejected by the engine and the project left unchanged.
    [[nodiscard]] CommandResult setOpacity(double opacity);
    [[nodiscard]] CommandResult setGain(double gain);

    /// Trim the selected clip's start / end edge to a new source boundary
    /// (TrimClipCommand), constrained to [1 frame, sourceDuration] (Requirement 2.4).
    [[nodiscard]] CommandResult trimStart(Duration newSourceIn, FrameRate fps,
                                          Duration sourceDuration);
    [[nodiscard]] CommandResult trimEnd(Duration newSourceOut, FrameRate fps,
                                        Duration sourceDuration);

    /// Change the selected text clip's displayed string (SetTextContentCommand,
    /// via `timeline.set_text_content` when a gateway is installed; usable-editor
    /// task 12, Requirement 9.2, 9.4). Refused when the selection is not a text
    /// clip.
    [[nodiscard]] CommandResult setTextContent(std::string content);

    /// Change the selected text clip's font, size, colour, alignment and/or
    /// position (SetTextStyleCommand, via `timeline.set_text_style`). Every
    /// parameter left as std::nullopt leaves that field exactly as it was.
    /// Refused when the selection is not a text clip.
    [[nodiscard]] CommandResult setTextStyle(std::optional<std::string> fontFamily,
                                             std::optional<double> pointSize,
                                             std::optional<double> colorR,
                                             std::optional<double> colorG,
                                             std::optional<double> colorB,
                                             std::optional<double> colorA,
                                             std::optional<TextAlignment> alignment,
                                             std::optional<double> x, std::optional<double> y);

    /// Change the selected caption cue's displayed text (SetCaptionTextCommand,
    /// via `timeline.set_caption_text` when a gateway is installed;
    /// usable-editor task 13, Requirement 10.2). Refused when the selection is
    /// not a caption cue.
    [[nodiscard]] CommandResult setCaptionText(std::string text);

    /// Change the selected caption cue's start position and/or duration
    /// (RetimeCaptionCueCommand, via `timeline.retime_caption_cue`;
    /// Requirement 10.2). Either left as std::nullopt leaves that field exactly
    /// as it was; at least one must be given. Refused when the selection is not
    /// a caption cue.
    [[nodiscard]] CommandResult retimeCaptionCue(std::optional<Duration> newTimelineStart,
                                                 std::optional<Duration> newDuration);

    // --- Change notification (for the thin Qt view) ------------------------

    /// Register a callback invoked whenever the projection may have changed:
    /// after a selection change and after any engine change (this model's own
    /// edits or edits from undo/redo, MCP, or the agent).
    void setOnChanged(std::function<void()> callback);

    /// Install (or clear, with nullptr) the gateway addEffect()/trimStart()/
    /// trimEnd()/removeEffect()/reorderEffects()/setEffectParameter() route
    /// through.
    void setGateway(ui::GuiToolGateway* gateway) noexcept { gateway_ = gateway; }

    /// The currently installed gateway, or nullptr when those mutations call
    /// the engine directly.
    [[nodiscard]] ui::GuiToolGateway* gateway() const noexcept { return gateway_; }

private:
    void notifyChanged() const;

    TimelineEngine&        engine_;
    ui::GuiToolGateway*     gateway_ = nullptr;
    std::optional<ClipId>  selected_;
    std::function<void()>  onChanged_;
    Subscription           subscription_;
};

} // namespace ui
} // namespace palmier

#endif // PALMIER_UI_INSPECTORVIEWMODEL_HPP
