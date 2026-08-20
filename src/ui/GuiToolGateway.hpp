// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/GuiToolGateway.hpp — routes every GUI mutating gesture through the shared
// tool surface (task 11.1).
//
// design.md's Property 4 / Property 2 ("UI / MCP / agent edit equivalence")
// requires that issuing the same edit through the UI, an MCP tool call, or the
// in-app agent produces the same resulting project state (Requirements 1.7, 9.4,
// 11.5). Before this task the Qt-free view-models (TimelineViewModel,
// InspectorViewModel, MediaBrowserViewModel) called `TimelineEngine::apply(...)`
// directly, which bypasses the tool surface's schema validation, invocation
// logging and rollback policy — a real, if narrow, divergence between the GUI
// and the other two surfaces. GuiToolGateway closes that gap: it is a thin,
// Qt-free adapter with one method per gesture, each building the exact JSON
// arguments the corresponding tool (see services::ToolRegistry) declares and
// calling `services::McpToolExecutor::executeTool(name, args,
// InvocationSource::Gui)` — the SAME execution path the MCP endpoint and the
// in-app agent use. `InvocationSource::Gui` is a logging tag only; the
// validation, atomicity and rollback policy are identical for all three sources.
//
// GuiToolGateway carries no Qt dependency, so it is unit-testable without a Qt
// runtime and builds in both the headless and the UI trees. It is a pure
// routing layer: it performs no validation of its own beyond what is needed to
// serialize its typed arguments into Json, and it returns the tool's raw
// Result<Json> — the caller (a view-model) is responsible for translating that
// into its own GestureResult / CommandResult shape.

#ifndef PALMIER_UI_GUITOOLGATEWAY_HPP
#define PALMIER_UI_GUITOOLGATEWAY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"

namespace palmier::ui {

using services::Json;

/// Routes every GUI mutating gesture through the shared tool surface
/// (`services::McpToolExecutor`), tagging each call `InvocationSource::Gui`
/// (Requirement 1.7). One method per gesture; each returns the tool's raw
/// `Result<Json>` so a view-model can classify the outcome the same way it
/// already classifies a `CommandResult`.
///
/// Binds to an `McpToolExecutor&`, which must outlive the gateway. The executor
/// itself resolves the current `ProjectSession` at call time (task 3.4's D1), so
/// the gateway needs no session reference of its own and continues to route
/// correctly across a `project.open`.
class GuiToolGateway {
public:
    explicit GuiToolGateway(services::McpToolExecutor& executor) : executor_(executor) {}

    // --- Timeline gestures (mirrors TimelineViewModel's prior direct calls) --

    /// timeline.move_clip
    [[nodiscard]] Result<Json> moveClip(ClipId id, Duration newStart);

    /// timeline.trim_clip
    [[nodiscard]] Result<Json> trimClip(ClipId id, TrimClipCommand::Edge edge,
                                        Duration newBoundary, Duration sourceDuration);

    /// timeline.split_clip
    [[nodiscard]] Result<Json> splitClip(ClipId id, Duration playhead);

    /// timeline.reorder_clips
    [[nodiscard]] Result<Json> reorderClips(Uuid trackId, std::vector<ClipId> newOrder);

    /// timeline.add_clip
    [[nodiscard]] Result<Json> addClip(Uuid trackId, Uuid assetId, std::string sourcePath,
                                       std::optional<ClipId> clipId, Duration timelineStart,
                                       Duration sourceIn, Duration sourceOut, double opacity,
                                       double gain);

    /// timeline.delete_clip
    [[nodiscard]] Result<Json> deleteClip(ClipId id);

    /// timeline.ripple_delete (task 8.3; Requirement 5.1)
    [[nodiscard]] Result<Json> rippleDelete(ClipId id);

    /// timeline.ripple_trim (task 8.3; Requirement 5.2)
    [[nodiscard]] Result<Json> rippleTrim(ClipId id, RippleTrimCommand::Edge edge,
                                          Duration newBoundary, Duration sourceDuration);

    /// timeline.close_gap (task 8.3; Requirement 5.5)
    [[nodiscard]] Result<Json> closeGap(ClipId id);

    /// timeline.add_track
    [[nodiscard]] Result<Json> addTrack(TrackKind kind);

    /// timeline.remove_track
    [[nodiscard]] Result<Json> removeTrack(Uuid trackId);

    // --- Inspector / effect gestures ---------------------------------------

    /// timeline.add_effect
    [[nodiscard]] Result<Json> addEffect(ClipId clipId, const Effect& effect);

    /// timeline.add_transition
    [[nodiscard]] Result<Json> addTransition(ClipId clipId, TransitionKind kind,
                                             Duration duration);

    // --- Media / project gestures ------------------------------------------

    /// media.import
    [[nodiscard]] Result<Json> importMedia(const std::string& path);

    /// project.create
    [[nodiscard]] Result<Json> createProject(const std::string& name, double fps,
                                             std::uint32_t width, std::uint32_t height,
                                             const std::string& colorSpace = {});

    /// project.open
    [[nodiscard]] Result<Json> openProject(const std::string& path);

    /// project.save (path empty -> the project's recorded document path)
    [[nodiscard]] Result<Json> saveProject(const std::string& path = {});

    // --- Export --------------------------------------------------------------

    /// Everything an export dialog can ask for. Every field beyond `outputPath`
    /// and `format` is optional in the underlying tool and left unset here maps
    /// to that tool's own project-derived default.
    struct ExportRequest {
        std::string outputPath;
        std::string format;
        std::optional<std::uint32_t> width;
        std::optional<std::uint32_t> height;
        std::optional<std::string>   codec;
        std::optional<double>        fps;
        std::optional<std::int64_t>  bitrateKbps;
        std::optional<bool>          includeAudio;
        std::optional<bool>          preferHardware;
        bool                         overwrite = false;
    };

    /// timeline.export
    [[nodiscard]] Result<Json> exportTimeline(const ExportRequest& request);

private:
    services::McpToolExecutor& executor_;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_GUITOOLGATEWAY_HPP
