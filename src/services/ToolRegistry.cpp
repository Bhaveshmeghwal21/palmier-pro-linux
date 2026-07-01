// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolRegistry.cpp — the default editor tool surface and its handlers
// (task 15.1). Each structural-edit tool parses its JSON arguments, constructs
// the same concrete EditCommand the UI uses, and applies it atomically through
// TimelineEngine::apply, so the UI, the MCP server, and the in-app agent all
// drive one undoable/observable editing path (design.md Property P4;
// Requirements 7.4, 7.8).

#include "services/ToolRegistry.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {

namespace {

// ---------------------------------------------------------------------------
// Tool names (MCP-style dotted namespaces, matching design.md examples).
// ---------------------------------------------------------------------------

constexpr const char* kReadTimeline   = "timeline.read";
constexpr const char* kAddClip        = "timeline.add_clip";
constexpr const char* kDeleteClip     = "timeline.delete_clip";
constexpr const char* kMoveClip       = "timeline.move_clip";
constexpr const char* kTrimClip       = "timeline.trim_clip";
constexpr const char* kSplitClip      = "timeline.split_clip";
constexpr const char* kReorderClips   = "timeline.reorder_clips";
constexpr const char* kAddEffect      = "timeline.add_effect";
constexpr const char* kAddTransition  = "timeline.add_transition";
constexpr const char* kGenerate       = "generation.generate";
constexpr const char* kExport         = "timeline.export";

// ---------------------------------------------------------------------------
// Enum <-> string helpers
// ---------------------------------------------------------------------------

std::string_view effectTypeName(EffectType type) {
    switch (type) {
        case EffectType::Brightness:    return "brightness";
        case EffectType::Contrast:      return "contrast";
        case EffectType::Blur:          return "blur";
        case EffectType::CropTransform: return "crop_transform";
        case EffectType::ColorGrade:    return "color_grade";
        case EffectType::Custom:        return "custom";
    }
    return "custom";
}

EffectType parseEffectType(std::string_view s) {
    if (s == "brightness")     return EffectType::Brightness;
    if (s == "contrast")       return EffectType::Contrast;
    if (s == "blur")           return EffectType::Blur;
    if (s == "crop_transform") return EffectType::CropTransform;
    if (s == "color_grade")    return EffectType::ColorGrade;
    return EffectType::Custom;
}

std::string_view transitionKindName(TransitionKind kind) {
    switch (kind) {
        case TransitionKind::Crossfade:  return "crossfade";
        case TransitionKind::DipToColor: return "dip_to_color";
        case TransitionKind::Wipe:       return "wipe";
        case TransitionKind::Slide:      return "slide";
        case TransitionKind::Fade:       return "fade";
    }
    return "crossfade";
}

std::optional<TransitionKind> parseTransitionKind(std::string_view s) {
    if (s == "crossfade")    return TransitionKind::Crossfade;
    if (s == "dip_to_color") return TransitionKind::DipToColor;
    if (s == "wipe")         return TransitionKind::Wipe;
    if (s == "slide")        return TransitionKind::Slide;
    if (s == "fade")         return TransitionKind::Fade;
    return std::nullopt;
}

std::string_view trackKindName(TrackKind kind) {
    return kind == TrackKind::Audio ? "audio" : "video";
}

// ---------------------------------------------------------------------------
// Argument parsing helpers — each yields a descriptive InvalidArgument on failure
// so the executor (task 15.3) can validate inputs before creating a command.
// ---------------------------------------------------------------------------

Result<Uuid> requireUuid(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isString()) {
        return err<Uuid>(invalidArgument(
            std::string("missing or non-string field '") + std::string(key) + "'"));
    }
    std::optional<Uuid> parsed = Uuid::parse(m->asString());
    if (!parsed) {
        return err<Uuid>(invalidArgument(
            std::string("field '") + std::string(key) + "' is not a valid UUID"));
    }
    return *parsed;
}

Result<std::int64_t> requireInt(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isNumber()) {
        return err<std::int64_t>(invalidArgument(
            std::string("missing or non-numeric field '") + std::string(key) + "'"));
    }
    return m->isInt() ? m->asInt() : static_cast<std::int64_t>(m->asDouble());
}

Result<std::string> requireString(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isString()) {
        return err<std::string>(invalidArgument(
            std::string("missing or non-string field '") + std::string(key) + "'"));
    }
    return m->asString();
}

// Locate a mutable clip by id across all tracks (used by the local
// add_transition command). Returns nullptr when not found.
Clip* findClipPtr(Project& project, const ClipId& clipId) {
    for (Track& track : project.tracks) {
        for (Clip& clip : track.clips) {
            if (clip.id == clipId) return &clip;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// add_transition has no dedicated core EditCommand; this local command applies
// the equivalent edit through the SAME engine path (atomic, undoable,
// observable), so `add_transition` honors Property P4 like the other edits.
// It sets a clip's incoming transition and restores the prior one on revert.
// ---------------------------------------------------------------------------

class SetTransitionCommand final : public EditCommand {
public:
    SetTransitionCommand(ClipId clipId, Transition transition)
        : clipId_(clipId), transition_(std::move(transition)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "AddTransition"; }

    [[nodiscard]] Result<void> apply(Project& project) override {
        Clip* clip = findClipPtr(project, clipId_);
        if (clip == nullptr) {
            return err(notFound("AddTransition: clip not found"));
        }
        prior_ = clip->transitionIn;
        captured_ = true;
        clip->transitionIn = transition_;
        return ok();
    }

    [[nodiscard]] Result<void> revert(Project& project) override {
        if (!captured_) {
            return err(failedPrecondition("AddTransition: revert before a successful apply"));
        }
        Clip* clip = findClipPtr(project, clipId_);
        if (clip == nullptr) {
            return err(notFound("AddTransition: clip not found"));
        }
        clip->transitionIn = prior_;
        return ok();
    }

private:
    ClipId                    clipId_;
    Transition                transition_;
    std::optional<Transition> prior_;
    bool                      captured_ = false;
};

// ---------------------------------------------------------------------------
// Timeline -> JSON serialization (the `timeline.read` payload).
// ---------------------------------------------------------------------------

Json serializeEffect(const Effect& effect) {
    Json params = Json::object();
    for (const auto& [key, value] : effect.parameters) {
        params.set(key, Json(value));
    }
    Json out = Json::object();
    out.set("id", effect.id.toString());
    out.set("type", std::string(effectTypeName(effect.type)));
    out.set("parameters", std::move(params));
    return out;
}

Json serializeTransition(const Transition& t) {
    Json out = Json::object();
    out.set("id", t.id.toString());
    out.set("kind", std::string(transitionKindName(t.kind)));
    out.set("durationNs", static_cast<std::int64_t>(t.duration.nanoseconds()));
    return out;
}

Json serializeClip(const Clip& clip) {
    Json out = Json::object();
    out.set("id", clip.id.toString());
    out.set("assetId", clip.assetRef.assetId.toString());
    out.set("sourcePath", clip.assetRef.sourcePath);
    out.set("timelineStartNs", static_cast<std::int64_t>(clip.timelineStart.nanoseconds()));
    out.set("sourceInNs", static_cast<std::int64_t>(clip.sourceIn.nanoseconds()));
    out.set("sourceOutNs", static_cast<std::int64_t>(clip.sourceOut.nanoseconds()));
    out.set("durationNs", static_cast<std::int64_t>(clip.duration().nanoseconds()));
    out.set("opacity", clip.opacity);
    out.set("gain", clip.gain);

    Json effects = Json::array();
    for (const Effect& e : clip.effects) effects.push_back(serializeEffect(e));
    out.set("effects", std::move(effects));

    if (clip.transitionIn.has_value()) {
        out.set("transitionIn", serializeTransition(*clip.transitionIn));
    } else {
        out.set("transitionIn", Json(nullptr));
    }
    return out;
}

Json serializeTrack(const Track& track) {
    Json out = Json::object();
    out.set("id", track.id.toString());
    out.set("kind", std::string(trackKindName(track.kind)));
    out.set("muted", track.muted);
    out.set("locked", track.locked);
    Json clips = Json::array();
    for (const Clip& c : track.clips) clips.push_back(serializeClip(c));
    out.set("clips", std::move(clips));
    return out;
}

Json serializeProject(const Project& project) {
    Json out = Json::object();
    out.set("id", project.id.toString());
    out.set("name", project.name);

    Json fps = Json::object();
    fps.set("numerator", static_cast<std::int64_t>(project.timelineFps.numerator()));
    fps.set("denominator", static_cast<std::int64_t>(project.timelineFps.denominator()));
    out.set("timelineFps", std::move(fps));

    Json canvas = Json::object();
    canvas.set("width", static_cast<std::int64_t>(project.canvas.width));
    canvas.set("height", static_cast<std::int64_t>(project.canvas.height));
    out.set("canvas", std::move(canvas));

    out.set("colorSpace", std::string(toStringView(project.colorSpace)));
    out.set("version", project.version.toString());

    Json tracks = Json::array();
    for (const Track& t : project.tracks) tracks.push_back(serializeTrack(t));
    out.set("tracks", std::move(tracks));

    Json assets = Json::array();
    for (const MediaAssetRef& a : project.assets) {
        Json ref = Json::object();
        ref.set("assetId", a.assetId.toString());
        ref.set("sourcePath", a.sourcePath);
        assets.push_back(std::move(ref));
    }
    out.set("assets", std::move(assets));

    out.set("durationNs", static_cast<std::int64_t>(timelineDuration(project).nanoseconds()));
    return out;
}

// ---------------------------------------------------------------------------
// Apply a command and translate the CommandResult into a Result<Json>.
//   * Failed -> the engine's Error, forwarded verbatim (the executor rolls back).
//   * NoOp   -> success payload annotated with the engine's indication.
//   * Applied-> success payload.
// ---------------------------------------------------------------------------

Result<Json> applyCommand(TimelineEngine& engine, std::unique_ptr<EditCommand> cmd,
                          Json success) {
    CommandResult result = engine.apply(std::move(cmd));
    if (result.isError()) {
        return err<Json>(result.error());
    }
    if (result.isNoOp()) {
        success.set("noOp", true);
        success.set("indication", result.message());
    } else {
        success.set("status", "applied");
    }
    return success;
}

// ---------------------------------------------------------------------------
// JSON-Schema construction helpers.
// ---------------------------------------------------------------------------

Json prop(std::string_view type, std::string_view description) {
    Json p = Json::object();
    p.set("type", std::string(type));
    p.set("description", std::string(description));
    return p;
}

Json objectSchema(std::initializer_list<Json::Member> properties,
                  std::vector<std::string> required) {
    Json props = Json::object();
    for (const auto& [name, schema] : properties) props.set(name, schema);

    Json requiredArr = Json::array();
    for (std::string& r : required) requiredArr.push_back(Json(std::move(r)));

    Json schema = Json::object();
    schema.set("type", "object");
    schema.set("properties", std::move(props));
    schema.set("required", std::move(requiredArr));
    schema.set("additionalProperties", true);
    return schema;
}

// ---------------------------------------------------------------------------
// Tool handler factories (each captures the engine by reference).
// ---------------------------------------------------------------------------

Tool makeReadTool(TimelineEngine& engine) {
    Tool t;
    t.name = kReadTimeline;
    t.description = "Read the current project timeline (tracks, clips, effects, transitions).";
    t.inputSchema = objectSchema({}, {});
    t.handler = [&engine](const Json&) -> Result<Json> {
        return serializeProject(engine.snapshot());
    };
    return t;
}

Tool makeAddClipTool(TimelineEngine& engine) {
    Tool t;
    t.name = kAddClip;
    t.description = "Add a clip referencing an asset onto a track at a timeline position.";
    t.inputSchema = objectSchema(
        {{"trackId", prop("string", "UUID of the target track.")},
         {"assetId", prop("string", "UUID of the media asset the clip references.")},
         {"sourcePath", prop("string", "Optional informational source path/locator.")},
         {"clipId", prop("string", "Optional explicit clip UUID; generated when omitted.")},
         {"timelineStartNs", prop("integer", "Timeline start position in nanoseconds.")},
         {"sourceInNs", prop("integer", "Source in-point in nanoseconds (default 0).")},
         {"sourceOutNs", prop("integer", "Source out-point in nanoseconds (> sourceInNs).")},
         {"opacity", prop("number", "Video opacity in [0,1] (default 1.0).")},
         {"gain", prop("number", "Audio gain >= 0 (default 1.0).")}},
        {"trackId", "assetId", "sourceOutNs"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        Result<Uuid> assetId = requireUuid(in, "assetId");
        if (assetId.isError()) return err<Json>(std::move(assetId).error());
        Result<std::int64_t> sourceOut = requireInt(in, "sourceOutNs");
        if (sourceOut.isError()) return err<Json>(std::move(sourceOut).error());

        const std::int64_t sourceIn = in.intOr("sourceInNs", 0);
        if (sourceOut.value() <= sourceIn) {
            return err<Json>(invalidArgument("sourceOutNs must be greater than sourceInNs"));
        }

        Clip clip;
        if (const Json* cid = in.find("clipId"); cid != nullptr && cid->isString()) {
            std::optional<Uuid> parsed = Uuid::parse(cid->asString());
            if (!parsed) return err<Json>(invalidArgument("field 'clipId' is not a valid UUID"));
            clip.id = *parsed;
        } else {
            clip.id = Uuid::generateV4();
        }
        clip.assetRef = MediaAssetRef(assetId.value(), in.stringOr("sourcePath"));
        clip.timelineStart = Duration::fromNanoseconds(in.intOr("timelineStartNs", 0));
        clip.sourceIn = Duration::fromNanoseconds(sourceIn);
        clip.sourceOut = Duration::fromNanoseconds(sourceOut.value());
        clip.opacity = in.doubleOr("opacity", 1.0);
        clip.gain = in.doubleOr("gain", 1.0);

        Json out = Json::object();
        out.set("clipId", clip.id.toString());
        return applyCommand(engine, std::make_unique<AddClipCommand>(trackId.value(),
                                                                     std::move(clip)),
                            std::move(out));
    };
    return t;
}

Tool makeDeleteClipTool(TimelineEngine& engine) {
    Tool t;
    t.name = kDeleteClip;
    t.description = "Delete a clip by id from whichever track holds it.";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the clip to delete.")}}, {"clipId"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine, std::make_unique<DeleteClipCommand>(clipId.value()),
                            std::move(out));
    };
    return t;
}

Tool makeMoveClipTool(TimelineEngine& engine) {
    Tool t;
    t.name = kMoveClip;
    t.description = "Move a clip to a new timeline start on its track (rejects overlaps).";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the clip to move.")},
         {"timelineStartNs", prop("integer", "New timeline start position in nanoseconds.")}},
        {"clipId", "timelineStartNs"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::int64_t> start = requireInt(in, "timelineStartNs");
        if (start.isError()) return err<Json>(std::move(start).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        out.set("timelineStartNs", start.value());
        return applyCommand(engine,
                            std::make_unique<MoveClipCommand>(
                                clipId.value(), Duration::fromNanoseconds(start.value())),
                            std::move(out));
    };
    return t;
}

Tool makeTrimClipTool(TimelineEngine& engine) {
    Tool t;
    t.name = kTrimClip;
    t.description = "Trim a clip's start or end edge to a new source boundary.";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the clip to trim.")},
         {"edge", prop("string", "Which edge to trim: 'start' or 'end'.")},
         {"boundaryNs", prop("integer", "New source boundary in nanoseconds.")},
         {"sourceDurationNs", prop("integer",
              "Full source media length in nanoseconds (defaults to the clip's out-point).")}},
        {"clipId", "edge", "boundaryNs"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> edgeStr = requireString(in, "edge");
        if (edgeStr.isError()) return err<Json>(std::move(edgeStr).error());
        Result<std::int64_t> boundary = requireInt(in, "boundaryNs");
        if (boundary.isError()) return err<Json>(std::move(boundary).error());

        TrimClipCommand::Edge edge;
        if (edgeStr.value() == "start")     edge = TrimClipCommand::Edge::Start;
        else if (edgeStr.value() == "end")  edge = TrimClipCommand::Edge::End;
        else return err<Json>(invalidArgument("field 'edge' must be 'start' or 'end'"));

        // Defaults are derived from the current project state / frame rate.
        std::optional<Clip> clip = engine.clip(clipId.value());
        if (!clip) {
            return err<Json>(notFound("trim_clip: clip " + clipId.value().toString() +
                                      " not found"));
        }
        const FrameRate fps = engine.snapshot().timelineFps;
        const Duration sourceDuration = in.contains("sourceDurationNs")
            ? Duration::fromNanoseconds(in.intOr("sourceDurationNs", 0))
            : clip->sourceOut;

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine,
                            std::make_unique<TrimClipCommand>(
                                clipId.value(), edge,
                                Duration::fromNanoseconds(boundary.value()), fps,
                                sourceDuration),
                            std::move(out));
    };
    return t;
}

Tool makeSplitClipTool(TimelineEngine& engine) {
    Tool t;
    t.name = kSplitClip;
    t.description = "Split a clip at an interior playhead into two contiguous clips.";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the clip to split.")},
         {"playheadNs", prop("integer", "Playhead position in nanoseconds (inside the clip).")}},
        {"clipId", "playheadNs"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::int64_t> playhead = requireInt(in, "playheadNs");
        if (playhead.isError()) return err<Json>(std::move(playhead).error());

        auto cmd = std::make_unique<SplitClipCommand>(
            clipId.value(), Duration::fromNanoseconds(playhead.value()));
        SplitClipCommand* raw = cmd.get();  // valid after a successful apply (kept by the stack)
        CommandResult result = engine.apply(std::move(cmd));
        if (result.isError()) return err<Json>(result.error());

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        if (std::optional<ClipId> rightId = raw->rightClipId()) {
            out.set("rightClipId", rightId->toString());
        }
        out.set("status", "applied");
        return out;
    };
    return t;
}

Tool makeReorderClipsTool(TimelineEngine& engine) {
    Tool t;
    t.name = kReorderClips;
    t.description = "Reorder a track's clips into a new sequence (preserves clip count).";
    t.inputSchema = objectSchema(
        {{"trackId", prop("string", "UUID of the track to reorder.")},
         {"order", prop("array", "Clip UUIDs, a permutation of the track's current clips.")}},
        {"trackId", "order"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        const Json* order = in.find("order");
        if (order == nullptr || !order->isArray()) {
            return err<Json>(invalidArgument("missing or non-array field 'order'"));
        }
        std::vector<ClipId> newOrder;
        newOrder.reserve(order->asArray().size());
        for (const Json& entry : order->asArray()) {
            if (!entry.isString()) {
                return err<Json>(invalidArgument("field 'order' must contain UUID strings"));
            }
            std::optional<Uuid> parsed = Uuid::parse(entry.asString());
            if (!parsed) {
                return err<Json>(invalidArgument("field 'order' contains an invalid UUID"));
            }
            newOrder.push_back(*parsed);
        }
        Json out = Json::object();
        out.set("trackId", trackId.value().toString());
        return applyCommand(engine,
                            std::make_unique<ReorderClipsCommand>(trackId.value(),
                                                                  std::move(newOrder)),
                            std::move(out));
    };
    return t;
}

Tool makeAddEffectTool(TimelineEngine& engine) {
    Tool t;
    t.name = kAddEffect;
    t.description = "Append an effect to a clip's effect chain.";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the target clip.")},
         {"type", prop("string",
              "Effect type: brightness, contrast, blur, crop_transform, color_grade, custom.")},
         {"parameters", prop("object", "Named numeric effect parameters.")}},
        {"clipId", "type"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> typeStr = requireString(in, "type");
        if (typeStr.isError()) return err<Json>(std::move(typeStr).error());

        Effect effect;
        effect.id = Uuid::generateV4();
        effect.type = parseEffectType(typeStr.value());
        if (const Json* params = in.find("parameters"); params != nullptr && params->isObject()) {
            for (const auto& [key, value] : params->asObject()) {
                if (value.isNumber()) effect.parameters[key] = value.asDouble();
            }
        }
        Json out = Json::object();
        out.set("effectId", effect.id.toString());
        return applyCommand(engine,
                            std::make_unique<AddEffectCommand>(clipId.value(),
                                                               std::move(effect)),
                            std::move(out));
    };
    return t;
}

Tool makeAddTransitionTool(TimelineEngine& engine) {
    Tool t;
    t.name = kAddTransition;
    t.description = "Set a clip's incoming transition (crossfade, wipe, slide, fade, ...).";
    t.inputSchema = objectSchema(
        {{"clipId", prop("string", "UUID of the clip whose incoming transition is set.")},
         {"kind", prop("string",
              "Transition kind: crossfade, dip_to_color, wipe, slide, fade.")},
         {"durationNs", prop("integer", "Transition region length in nanoseconds (>= 0).")}},
        {"clipId", "kind", "durationNs"});
    t.handler = [&engine](const Json& in) -> Result<Json> {
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> kindStr = requireString(in, "kind");
        if (kindStr.isError()) return err<Json>(std::move(kindStr).error());
        Result<std::int64_t> durationNs = requireInt(in, "durationNs");
        if (durationNs.isError()) return err<Json>(std::move(durationNs).error());

        std::optional<TransitionKind> kind = parseTransitionKind(kindStr.value());
        if (!kind) return err<Json>(invalidArgument("field 'kind' is not a known transition"));
        if (durationNs.value() < 0) {
            return err<Json>(invalidArgument("durationNs must be >= 0"));
        }

        Transition transition(Uuid::generateV4(), *kind,
                              Duration::fromNanoseconds(durationNs.value()));
        Json out = Json::object();
        out.set("transitionId", transition.id.toString());
        return applyCommand(engine,
                            std::make_unique<SetTransitionCommand>(clipId.value(),
                                                                   std::move(transition)),
                            std::move(out));
    };
    return t;
}

Tool makeGenerateTool(Tool::Handler hook) {
    Tool t;
    t.name = kGenerate;
    t.description = "Trigger generative media (image/video) from a prompt and place it.";
    t.inputSchema = objectSchema(
        {{"prompt", prop("string", "Generation prompt (1..2000 characters).")},
         {"model", prop("string", "Selected SOTA model id (e.g. 'veo', 'gpt-image').")},
         {"mediaType", prop("string", "Requested media type: 'video' or 'image'.")},
         {"trackId", prop("string", "UUID of the track to place the generated clip on.")},
         {"framePosition", prop("integer", "Placement position in frames from timeline start.")}},
        {"prompt", "model"});
    if (hook) {
        t.handler = std::move(hook);
    } else {
        t.handler = [](const Json&) -> Result<Json> {
            return err<Json>(unsupported(
                "generation.generate is not available: no generative backend is configured"));
        };
    }
    return t;
}

Tool makeExportTool(Tool::Handler hook) {
    Tool t;
    t.name = kExport;
    t.description = "Render the timeline to an output file at a selected format/resolution.";
    t.inputSchema = objectSchema(
        {{"outputPath", prop("string", "Destination file path for the rendered output.")},
         {"format", prop("string", "Output container/codec format.")},
         {"width", prop("integer", "Output width in pixels.")},
         {"height", prop("integer", "Output height in pixels.")}},
        {"outputPath", "format"});
    if (hook) {
        t.handler = std::move(hook);
    } else {
        t.handler = [](const Json&) -> Result<Json> {
            return err<Json>(unsupported(
                "timeline.export is not available: no export engine is configured"));
        };
    }
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::add(Tool tool) {
    for (Tool& existing : tools_) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

bool ToolRegistry::has(std::string_view name) const {
    return find(name) != nullptr;
}

const Tool* ToolRegistry::find(std::string_view name) const {
    for (const Tool& t : tools_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

Json ToolRegistry::describe() const {
    Json out = Json::array();
    for (const Tool& t : tools_) {
        Json entry = Json::object();
        entry.set("name", t.name);
        entry.set("description", t.description);
        entry.set("inputSchema", t.inputSchema);
        out.push_back(std::move(entry));
    }
    return out;
}

Result<Json> ToolRegistry::invoke(std::string_view name, const Json& input) const {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        return err<Json>(notFound(std::string("unknown tool '") + std::string(name) + "'"));
    }
    if (!tool->handler) {
        return err<Json>(makeError(ErrorCode::Internal,
                                   std::string("tool '") + std::string(name) +
                                       "' has no handler"));
    }
    return tool->handler(input);
}

// ---------------------------------------------------------------------------
// Default surface
// ---------------------------------------------------------------------------

ToolRegistry buildDefaultToolRegistry(TimelineEngine& engine, ToolRegistryHooks hooks) {
    ToolRegistry registry;
    registry.add(makeReadTool(engine));
    registry.add(makeAddClipTool(engine));
    registry.add(makeDeleteClipTool(engine));
    registry.add(makeMoveClipTool(engine));
    registry.add(makeTrimClipTool(engine));
    registry.add(makeSplitClipTool(engine));
    registry.add(makeReorderClipsTool(engine));
    registry.add(makeAddEffectTool(engine));
    registry.add(makeAddTransitionTool(engine));
    registry.add(makeGenerateTool(std::move(hooks.generate)));
    registry.add(makeExportTool(std::move(hooks.exportTimeline)));
    return registry;
}

}  // namespace palmier::services
