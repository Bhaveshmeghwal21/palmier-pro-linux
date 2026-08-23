// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/GuiToolGateway.cpp — implementation of the GUI-to-tool-surface adapter.
//
// See GuiToolGateway.hpp for the contract. Every method here builds the exact
// JSON argument object the corresponding tool in services::ToolRegistry.cpp
// declares (same field names, same units — nanoseconds for every duration, as
// Duration::nanoseconds() reports) and calls
// McpToolExecutor::executeTool(name, args, InvocationSource::Gui). No command is
// constructed here and no engine is touched directly: the executor resolves the
// current ProjectSession and applies the SAME EditCommand path the MCP endpoint
// and the in-app agent use, so a GUI gesture is schema-validated, logged and
// rolled back on failure exactly like every other invocation source.

#include "ui/GuiToolGateway.hpp"

namespace palmier::ui {

namespace {

/// Effect type -> the string services::ToolRegistry's `add_effect` tool
/// declares (its effectTypeValues() enum). Kept local and duplicated
/// deliberately: this is a serialization detail of the gateway's own request
/// building, not a shared core facility, and ProjectStore.cpp keeps its own
/// copy of the identical mapping for the same reason.
std::string_view effectTypeName(EffectType type) noexcept {
    switch (type) {
        case EffectType::Brightness:    return "brightness";
        case EffectType::Contrast:      return "contrast";
        case EffectType::Blur:          return "blur";
        case EffectType::CropTransform: return "crop_transform";
        case EffectType::ColorGrade:    return "color_grade";
        case EffectType::InvertColors:  return "invert_colors";
        case EffectType::Custom:        return "custom";
    }
    return "custom";
}

/// TransitionKind -> the string the `add_transition` tool declares
/// (transitionKindValues()).
std::string_view transitionKindName(TransitionKind kind) noexcept {
    switch (kind) {
        case TransitionKind::Crossfade:  return "crossfade";
        case TransitionKind::DipToColor: return "dip_to_color";
        case TransitionKind::Wipe:       return "wipe";
        case TransitionKind::Slide:      return "slide";
        case TransitionKind::Fade:       return "fade";
    }
    return "crossfade";
}

/// TrackKind -> the string the `add_track` tool declares (trackKindValues()).
std::string_view trackKindName(TrackKind kind) noexcept {
    switch (kind) {
        case TrackKind::Audio: return "audio";
        case TrackKind::Text:  return "text";
        case TrackKind::Video: return "video";
    }
    return "video";
}

std::string_view edgeName(TrimClipCommand::Edge edge) noexcept {
    return edge == TrimClipCommand::Edge::Start ? "start" : "end";
}

std::string_view edgeName(RippleTrimCommand::Edge edge) noexcept {
    return edge == RippleTrimCommand::Edge::Start ? "start" : "end";
}

}  // namespace

Result<Json> GuiToolGateway::moveClip(ClipId id, Duration newStart) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    args.set("timelineStartNs", newStart.nanoseconds());
    return executor_.executeTool("timeline.move_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::trimClip(ClipId id, TrimClipCommand::Edge edge,
                                      Duration newBoundary, Duration sourceDuration) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    args.set("edge", std::string(edgeName(edge)));
    args.set("boundaryNs", newBoundary.nanoseconds());
    args.set("sourceDurationNs", sourceDuration.nanoseconds());
    return executor_.executeTool("timeline.trim_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::splitClip(ClipId id, Duration playhead) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    args.set("playheadNs", playhead.nanoseconds());
    return executor_.executeTool("timeline.split_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::reorderClips(Uuid trackId, std::vector<ClipId> newOrder) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    Json order = Json::array();
    for (const ClipId& id : newOrder) {
        order.push_back(Json(id.toString()));
    }
    args.set("order", std::move(order));
    return executor_.executeTool("timeline.reorder_clips", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::addClip(Uuid trackId, Uuid assetId, std::string sourcePath,
                                     std::optional<ClipId> clipId, Duration timelineStart,
                                     Duration sourceIn, Duration sourceOut, double opacity,
                                     double gain) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("assetId", assetId.toString());
    if (!sourcePath.empty()) {
        args.set("sourcePath", std::move(sourcePath));
    }
    if (clipId.has_value()) {
        args.set("clipId", clipId->toString());
    }
    args.set("timelineStartNs", timelineStart.nanoseconds());
    args.set("sourceInNs", sourceIn.nanoseconds());
    args.set("sourceOutNs", sourceOut.nanoseconds());
    args.set("opacity", opacity);
    args.set("gain", gain);
    return executor_.executeTool("timeline.add_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::deleteClip(ClipId id) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    return executor_.executeTool("timeline.delete_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::rippleDelete(ClipId id) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    return executor_.executeTool("timeline.ripple_delete", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::rippleTrim(ClipId id, RippleTrimCommand::Edge edge,
                                       Duration newBoundary, Duration sourceDuration) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    args.set("edge", std::string(edgeName(edge)));
    args.set("boundaryNs", newBoundary.nanoseconds());
    args.set("sourceDurationNs", sourceDuration.nanoseconds());
    return executor_.executeTool("timeline.ripple_trim", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::closeGap(ClipId id) {
    Json args = Json::object();
    args.set("clipId", id.toString());
    return executor_.executeTool("timeline.close_gap", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::addTrack(TrackKind kind) {
    Json args = Json::object();
    args.set("kind", std::string(trackKindName(kind)));
    return executor_.executeTool("timeline.add_track", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::removeTrack(Uuid trackId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    return executor_.executeTool("timeline.remove_track", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::addEffect(ClipId clipId, const Effect& effect) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    args.set("type", std::string(effectTypeName(effect.type)));
    if (!effect.parameters.empty()) {
        Json params = Json::object();
        for (const auto& [name, value] : effect.parameters) {
            params.set(name, value);
        }
        args.set("parameters", std::move(params));
    }
    return executor_.executeTool("timeline.add_effect", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::removeEffect(ClipId clipId, Uuid effectId) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    args.set("effectId", effectId.toString());
    return executor_.executeTool("timeline.remove_effect", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::reorderEffects(ClipId clipId, std::vector<Uuid> newOrder) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    Json order = Json::array();
    for (const Uuid& id : newOrder) {
        order.push_back(Json(id.toString()));
    }
    args.set("order", std::move(order));
    return executor_.executeTool("timeline.reorder_effects", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::setEffectParameter(ClipId clipId, Uuid effectId,
                                                const std::string& parameter, double value) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    args.set("effectId", effectId.toString());
    args.set("parameter", parameter);
    args.set("value", value);
    return executor_.executeTool("timeline.set_effect_parameter", args,
                                 services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::addTransition(ClipId clipId, TransitionKind kind,
                                           Duration duration) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    args.set("kind", std::string(transitionKindName(kind)));
    args.set("durationNs", duration.nanoseconds());
    return executor_.executeTool("timeline.add_transition", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::addTextClip(Uuid trackId, Duration timelineStart, Duration duration,
                                         const std::string& content, const std::string& fontFamily,
                                         double pointSize, double colorR, double colorG,
                                         double colorB, double colorA, TextAlignment alignment,
                                         double x, double y) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("timelineStartNs", timelineStart.nanoseconds());
    args.set("durationNs", duration.nanoseconds());
    args.set("content", content);
    args.set("fontFamily", fontFamily);
    args.set("pointSize", pointSize);
    args.set("colorR", colorR);
    args.set("colorG", colorG);
    args.set("colorB", colorB);
    args.set("colorA", colorA);
    args.set("alignment", std::string(toStringView(alignment)));
    args.set("x", x);
    args.set("y", y);
    return executor_.executeTool("timeline.add_text_clip", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::setTextContent(ClipId clipId, const std::string& content) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    args.set("content", content);
    return executor_.executeTool("timeline.set_text_content", args,
                                 services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::setTextStyle(ClipId clipId, std::optional<std::string> fontFamily,
                                          std::optional<double> pointSize,
                                          std::optional<double> colorR,
                                          std::optional<double> colorG,
                                          std::optional<double> colorB,
                                          std::optional<double> colorA,
                                          std::optional<TextAlignment> alignment,
                                          std::optional<double> x, std::optional<double> y) {
    Json args = Json::object();
    args.set("clipId", clipId.toString());
    if (fontFamily) args.set("fontFamily", *fontFamily);
    if (pointSize) args.set("pointSize", *pointSize);
    if (colorR) args.set("colorR", *colorR);
    if (colorG) args.set("colorG", *colorG);
    if (colorB) args.set("colorB", *colorB);
    if (colorA) args.set("colorA", *colorA);
    if (alignment) args.set("alignment", std::string(toStringView(*alignment)));
    if (x) args.set("x", *x);
    if (y) args.set("y", *y);
    return executor_.executeTool("timeline.set_text_style", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::importMedia(const std::string& path) {
    Json args = Json::object();
    args.set("path", path);
    return executor_.executeTool("media.import", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::createProject(const std::string& name, double fps,
                                           std::uint32_t width, std::uint32_t height,
                                           const std::string& colorSpace) {
    Json args = Json::object();
    args.set("name", name);
    args.set("fps", fps);
    args.set("width", static_cast<std::int64_t>(width));
    args.set("height", static_cast<std::int64_t>(height));
    if (!colorSpace.empty()) {
        args.set("colorSpace", colorSpace);
    }
    return executor_.executeTool("project.create", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::setProjectSettings(std::optional<double> fps,
                                                std::optional<std::uint32_t> width,
                                                std::optional<std::uint32_t> height,
                                                std::optional<std::string> colorSpace) {
    Json args = Json::object();
    if (fps) args.set("fps", *fps);
    if (width) args.set("width", static_cast<std::int64_t>(*width));
    if (height) args.set("height", static_cast<std::int64_t>(*height));
    if (colorSpace) args.set("colorSpace", *colorSpace);
    return executor_.executeTool("project.set_settings", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::openProject(const std::string& path) {
    Json args = Json::object();
    args.set("path", path);
    return executor_.executeTool("project.open", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::saveProject(const std::string& path) {
    Json args = Json::object();
    if (!path.empty()) {
        args.set("path", path);
    }
    return executor_.executeTool("project.save", args, services::InvocationSource::Gui);
}

Result<Json> GuiToolGateway::exportTimeline(const ExportRequest& request) {
    Json args = Json::object();
    args.set("outputPath", request.outputPath);
    args.set("format", request.format);
    if (request.width.has_value()) {
        args.set("width", static_cast<std::int64_t>(*request.width));
    }
    if (request.height.has_value()) {
        args.set("height", static_cast<std::int64_t>(*request.height));
    }
    if (request.codec.has_value()) {
        args.set("codec", *request.codec);
    }
    if (request.fps.has_value()) {
        args.set("fps", *request.fps);
    }
    if (request.bitrateKbps.has_value()) {
        args.set("bitrateKbps", *request.bitrateKbps);
    }
    if (request.includeAudio.has_value()) {
        args.set("includeAudio", *request.includeAudio);
    }
    if (request.preferHardware.has_value()) {
        args.set("preferHardware", *request.preferHardware);
    }
    args.set("overwrite", request.overwrite);
    return executor_.executeTool("timeline.export", args, services::InvocationSource::Gui);
}

}  // namespace palmier::ui
