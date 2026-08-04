// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolRegistry.cpp — the default editor tool surface and its handlers
// (task 15.1). Each structural-edit tool parses its JSON arguments, constructs
// the same concrete EditCommand the UI uses, and applies it atomically through
// TimelineEngine::apply, so the UI, the MCP server, and the in-app agent all
// drive one undoable/observable editing path (design.md Property P4;
// Requirements 7.4, 7.8).
//
// Task 3.2: each tool's accepted arguments are declared exactly once, as a
// `ToolSchema` (design.md D3, "Schema/handler agreement"). The published
// `inputSchema` is rendered from that declaration and the pre-execution validator
// enforces the same constraint set, so an acceptance rule can no longer live only
// inside a handler. Each declaration below records which handler/engine rules it
// lifted and, where the `ArgSpec` vocabulary cannot express one — a relation
// between two arguments, an array's item shape, or anything that depends on
// project state — says so explicitly next to the rule that remains handler-side
// (Requirements 9.3, 9.9, 9.12).

#include "services/ToolRegistry.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"

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

// Session, media-library and track tools (tasks 4.3-4.5; Requirement 3.1).
constexpr const char* kProjectCreate  = "project.create";
constexpr const char* kProjectOpen    = "project.open";
constexpr const char* kProjectSave    = "project.save";
constexpr const char* kProjectInfo    = "project.info";
constexpr const char* kMediaImport    = "media.import";
constexpr const char* kMediaList      = "media.list";
constexpr const char* kAddTrack       = "timeline.add_track";
constexpr const char* kRemoveTrack    = "timeline.remove_track";
constexpr const char* kSetTrackMuted  = "timeline.set_track_muted";
constexpr const char* kUndo           = "edit.undo";
constexpr const char* kRedo           = "edit.redo";

// The accepted length of a filesystem path argument (`project.open`,
// `project.save`, `media.import`), matching the design's tool table.
constexpr std::size_t kMinPathLength = 1;
constexpr std::size_t kMaxPathLength = 4096;

// ---------------------------------------------------------------------------
// "No project is open" (Requirement 3.5)
// ---------------------------------------------------------------------------

/// The one rendering of Requirement 3.5's refusal. Every tool other than
/// `project.create` and `project.open` answers with this — before it parses an
/// argument, applies a command or touches the media library — when no project is
/// current, so the refusal is uniform and no state can have changed by the time it
/// is returned. The wording matches McpToolExecutor's and MediaImportService's, so
/// a caller sees one sentence for one condition regardless of which layer noticed
/// it first.
Error noProjectOpen(std::string_view tool) {
    return failedPrecondition("no project is open: cannot execute tool '" +
                              std::string(tool) + "'");
}

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
        case EffectType::InvertColors:  return "invert_colors";
        case EffectType::Custom:        return "custom";
    }
    return "custom";
}

/// The accepted `type` values of `timeline.add_effect`, declared once: this list
/// is both the `ArgSpec::enumValues` the schema publishes and the exact set
/// `parseEffectType` accepts, so the advertised enum and the handler agree
/// (Requirement 9.12). `invert_colors` is the ported upstream PR 408 effect
/// (Requirement 14.4).
const std::vector<std::string>& effectTypeValues() {
    static const std::vector<std::string> values = {
        "brightness", "contrast", "blur", "crop_transform", "color_grade",
        "invert_colors", "custom"};
    return values;
}

std::optional<EffectType> parseEffectType(std::string_view s) {
    if (s == "brightness")     return EffectType::Brightness;
    if (s == "contrast")       return EffectType::Contrast;
    if (s == "blur")           return EffectType::Blur;
    if (s == "crop_transform") return EffectType::CropTransform;
    if (s == "color_grade")    return EffectType::ColorGrade;
    if (s == "invert_colors")  return EffectType::InvertColors;
    if (s == "custom")         return EffectType::Custom;
    return std::nullopt;
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

/// The accepted `kind` values of `timeline.add_transition`, declared once for the
/// same reason as `effectTypeValues()`: the published enum and
/// `parseTransitionKind`'s accepted set are one list.
const std::vector<std::string>& transitionKindValues() {
    static const std::vector<std::string> values = {"crossfade", "dip_to_color", "wipe",
                                                    "slide", "fade"};
    return values;
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

/// The accepted `kind` values of the track tools, declared once so the published
/// enum and `parseTrackKind`'s accepted set are one list (Requirements 3.3, 3.8).
const std::vector<std::string>& trackKindValues() {
    static const std::vector<std::string> values = {"video", "audio"};
    return values;
}

std::optional<TrackKind> parseTrackKind(std::string_view s) {
    if (s == "video") return TrackKind::Video;
    if (s == "audio") return TrackKind::Audio;
    return std::nullopt;
}

/// The colour spaces `project.create` accepts: exactly the set the domain core
/// exposes (Requirement 3.2), spelled with the core's own stable display names
/// (`ColorSpace::toStringView`) so the published enum, the validator and the
/// reported result all use one vocabulary. `Unknown` is deliberately absent: it is
/// not a selectable working space, and ProjectSession rejects it.
const std::vector<std::string>& colorSpaceValues() {
    static const std::vector<std::string> values = {
        std::string(toStringView(ColorSpace::Srgb)),
        std::string(toStringView(ColorSpace::Rec709)),
        std::string(toStringView(ColorSpace::Rec2020)),
        std::string(toStringView(ColorSpace::Rec2100Pq)),
        std::string(toStringView(ColorSpace::Rec2100Hlg)),
        std::string(toStringView(ColorSpace::DisplayP3)),
        std::string(toStringView(ColorSpace::LinearSrgb))};
    return values;
}

std::optional<ColorSpace> parseColorSpace(std::string_view s) {
    for (const ColorSpace candidate :
         {ColorSpace::Srgb, ColorSpace::Rec709, ColorSpace::Rec2020, ColorSpace::Rec2100Pq,
          ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3, ColorSpace::LinearSrgb}) {
        if (toStringView(candidate) == s) return candidate;
    }
    return std::nullopt;
}

/// The exact FrameRate a `fps` number argument denotes.
///
/// `FrameRate` is an exact rational, while the tool argument is a JSON number, so
/// the conversion has to pick a denominator. Three cases, in order:
///
///   * a whole number becomes `n/1` (30 -> 30/1);
///   * one of the three broadcast pull-down rates, recognised within half a
///     thousandth, becomes its exact NTSC ratio (29.97 -> 30000/1001), because
///     `29970/1000` is a *different* rate that would drift against real media;
///   * anything else is taken to a thousandth of a frame per second (`n/1000`),
///     which is closed over the accepted 1..240 range: rounding to a thousandth
///     never leaves the interval, so a value the schema accepted always yields a
///     rate ProjectSession accepts.
FrameRate frameRateFromFps(double fps) {
    const double rounded = std::round(fps);
    if (std::abs(fps - rounded) < 1e-9 && rounded >= 1.0) {
        return FrameRate{static_cast<std::int64_t>(rounded), 1};
    }
    struct PullDown {
        double    value;
        FrameRate rate;
    };
    const PullDown pullDowns[] = {{24000.0 / 1001.0, FrameRate::fps23_976()},
                                  {30000.0 / 1001.0, FrameRate::fps29_97()},
                                  {60000.0 / 1001.0, FrameRate::fps59_94()}};
    for (const PullDown& candidate : pullDowns) {
        if (std::abs(fps - candidate.value) < 0.0005) return candidate.rate;
    }
    return FrameRate{static_cast<std::int64_t>(std::llround(fps * 1000.0)), 1000};
}

/// Total clip count across every track — the `clipCount` the session tools report
/// (Requirements 3.4, 3.7, 3.10).
std::size_t countClips(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

/// `{numerator, denominator}`, the shape every tool result reports a frame rate in.
Json serializeFrameRate(FrameRate fps) {
    Json out = Json::object();
    out.set("numerator", static_cast<std::int64_t>(fps.numerator()));
    out.set("denominator", static_cast<std::int64_t>(fps.denominator()));
    return out;
}

/// `{width, height}`, the shape every tool result reports a canvas in.
Json serializeCanvas(Resolution canvas) {
    Json out = Json::object();
    out.set("width", static_cast<std::int64_t>(canvas.width));
    out.set("height", static_cast<std::int64_t>(canvas.height));
    return out;
}

/// A document path result field: the path when one is recorded, JSON null when the
/// project has never been written or loaded (the design's `documentPath:null`).
Json serializeDocumentPath(const std::optional<std::filesystem::path>& path) {
    return path.has_value() ? Json(path->string()) : Json(nullptr);
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

Result<double> requireNumber(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isNumber()) {
        return err<double>(invalidArgument(
            std::string("missing or non-numeric field '") + std::string(key) + "'"));
    }
    return m->isInt() ? static_cast<double>(m->asInt()) : m->asDouble();
}

Result<std::string> requireString(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isString()) {
        return err<std::string>(invalidArgument(
            std::string("missing or non-string field '") + std::string(key) + "'"));
    }
    return m->asString();
}

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
// Argument declaration helpers (task 3.2).
//
// Every tool declares its arguments once, as a `ToolSchema`. `Tool::inputSchema()`
// renders the draft-07 schema `tools/list` publishes and `Tool::schema.validate()`
// enforces the identical constraint set before a command is created, so the two
// cannot drift (design.md D3; Requirements 9.3, 9.9, 9.12).
//
// The helpers below cover the shapes that recur across the surface; anything with
// bounds or a closed value set is written out as a full `ArgSpec` at its use site.
// ---------------------------------------------------------------------------

/// A canonical-UUID string argument (publishes `"format":"uuid"`, rejects a
/// non-parsable id — the rule `requireUuid` enforces in every handler).
ArgSpec uuidArg(std::string name, bool required, std::string description) {
    return ArgSpec{.name = std::move(name),
                   .kind = JsonKind::String,
                   .required = required,
                   .description = std::move(description),
                   .uuid = true};
}

/// An exact-integer argument, optionally with a lower bound. Most are nanosecond
/// tick counts (`*Ns` / `*Ticks`), for which a fractional payload is meaningless.
ArgSpec intArg(std::string name, bool required, std::string description,
               std::optional<std::int64_t> minimum = std::nullopt) {
    return ArgSpec{.name = std::move(name),
                   .kind = JsonKind::Integer,
                   .required = required,
                   .description = std::move(description),
                   .minInt = minimum};
}

/// A plain string argument with no closed value set and no length bound.
ArgSpec stringArg(std::string name, bool required, std::string description) {
    return ArgSpec{.name = std::move(name),
                   .kind = JsonKind::String,
                   .required = required,
                   .description = std::move(description)};
}

// ---------------------------------------------------------------------------
// Tool handler factories (each captures the SESSION and resolves its engine at
// invocation time).
//
// Task 3.4 / design.md D1: a handler must not capture a `TimelineEngine&` taken
// when the registry was built. The session owns one stable engine and swaps the
// project value inside it (TimelineEngine::reset), so resolving the engine per
// call is what makes a `project.open` that happens AFTER registration observable
// to these handlers.
//
// Task 4.3 / Requirement 3.5: the session is held as a POINTER, and a null
// pointer is the "no project is current" state — the same representation
// McpToolExecutor and MediaImportService already use. Every handler other than
// `project.create`'s and `project.open`'s therefore opens with
//
//     if (session == nullptr) return err<Json>(noProjectOpen(<tool>));
//     TimelineEngine& engine = session->engine();
//
// so the refusal happens before any argument is parsed, any command is
// constructed or any library is touched: nothing can have changed by the time the
// error is returned.
// ---------------------------------------------------------------------------

Tool makeReadTool(ProjectSession* session) {
    Tool t;
    t.name = kReadTimeline;
    t.description = "Read the current project timeline (tracks, clips, effects, transitions).";
    // No arguments: the rendered schema is an empty object schema, and (unlike the
    // hand-written `additionalProperties: true` form it replaces) an unknown key is
    // now rejected rather than silently ignored (Requirement 9.9).
    t.handler = [session](const Json&) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kReadTimeline));
        return serializeProject(session->engine().snapshot());
    };
    return t;
}

Tool makeAddClipTool(ProjectSession* session) {
    Tool t;
    t.name = kAddClip;
    t.description = "Add a clip referencing an asset onto a track at a timeline position.";
    // Declared rules lifted out of the handler and the engine's invariants:
    // trackId/assetId/clipId are canonical UUIDs (`requireUuid`), timelineStartNs is
    // non-negative (`checkTimelineInvariants`), opacity lies in [0,1] and gain is
    // non-negative (`validateClip`).
    //
    // NOT expressible in the ArgSpec vocabulary: `sourceOutNs > sourceInNs` is a
    // relation between two arguments, and the vocabulary has no cross-field
    // constraint, so the handler check below remains the only enforcement.
    t.schema
        .arg(uuidArg("trackId", true, "UUID of the target track."))
        .arg(uuidArg("assetId", true, "UUID of the media asset the clip references."))
        .arg(stringArg("sourcePath", false, "Optional informational source path/locator."))
        .arg(uuidArg("clipId", false, "Optional explicit clip UUID; generated when omitted."))
        .arg(intArg("timelineStartNs", false, "Timeline start position in nanoseconds.", 0))
        .arg(intArg("sourceInNs", false, "Source in-point in nanoseconds (default 0)."))
        .arg(intArg("sourceOutNs", true, "Source out-point in nanoseconds (> sourceInNs)."))
        .arg(ArgSpec{.name = "opacity",
                     .kind = JsonKind::Number,
                     .description = "Video opacity in [0,1] (default 1.0).",
                     .minNum = 0.0,
                     .maxNum = 1.0})
        .arg(ArgSpec{.name = "gain",
                     .kind = JsonKind::Number,
                     .description = "Audio gain >= 0 (default 1.0).",
                     .minNum = 0.0});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddClip));
        TimelineEngine& engine = session->engine();
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

Tool makeDeleteClipTool(ProjectSession* session) {
    Tool t;
    t.name = kDeleteClip;
    t.description = "Delete a clip by id from whichever track holds it.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to delete."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kDeleteClip));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine, std::make_unique<DeleteClipCommand>(clipId.value()),
                            std::move(out));
    };
    return t;
}

Tool makeMoveClipTool(ProjectSession* session) {
    Tool t;
    t.name = kMoveClip;
    t.description = "Move a clip to a new timeline start on its track (rejects overlaps).";
    // `MoveClipCommand` rejects a negative destination ("position must be >= 0");
    // that rule is now declared. Whether the destination overlaps another clip
    // depends on project state, so it stays with the command.
    t.schema
        .arg(uuidArg("clipId", true, "UUID of the clip to move."))
        .arg(intArg("timelineStartNs", true, "New timeline start position in nanoseconds.",
                   0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kMoveClip));
        TimelineEngine& engine = session->engine();
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

Tool makeTrimClipTool(ProjectSession* session) {
    Tool t;
    t.name = kTrimClip;
    t.description = "Trim a clip's start or end edge to a new source boundary.";
    // `edge` is a closed value set, so it is declared as one rather than re-checked
    // privately. `boundaryNs` carries no bound because the command *clamps* it into
    // the legal range instead of rejecting it.
    //
    // NOT expressible: "the source must be at least one frame long" compares
    // sourceDurationNs against the project frame rate, which is state the schema
    // cannot see, so it stays with `TrimClipCommand`.
    t.schema
        .arg(uuidArg("clipId", true, "UUID of the clip to trim."))
        .arg(ArgSpec{.name = "edge",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Which edge to trim: 'start' or 'end'.",
                     .enumValues = {"start", "end"}})
        .arg(intArg("boundaryNs", true, "New source boundary in nanoseconds."))
        .arg(intArg("sourceDurationNs", false,
                   "Full source media length in nanoseconds (defaults to the clip's "
                   "out-point).",
                   0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kTrimClip));
        TimelineEngine& engine = session->engine();
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

Tool makeSplitClipTool(ProjectSession* session) {
    Tool t;
    t.name = kSplitClip;
    t.description = "Split a clip at an interior playhead into two contiguous clips.";
    // NOT expressible: "the playhead lies strictly inside the clip" is a relation
    // between the argument and the clip's current timeline range, which the schema
    // cannot see; `SplitClipCommand` remains its enforcement point.
    t.schema
        .arg(uuidArg("clipId", true, "UUID of the clip to split."))
        .arg(intArg("playheadNs", true,
                   "Playhead position in nanoseconds (inside the clip).", 0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSplitClip));
        TimelineEngine& engine = session->engine();
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

Tool makeReorderClipsTool(ProjectSession* session) {
    Tool t;
    t.name = kReorderClips;
    t.description = "Reorder a track's clips into a new sequence (preserves clip count).";
    // NOT expressible: the vocabulary constrains an array's item *count* but not its
    // item *shape*, so "every entry is a canonical UUID string" is still checked in
    // the handler; and "the entries are a permutation of the track's clips" depends
    // on project state and stays with `ReorderClipsCommand`.
    t.schema
        .arg(uuidArg("trackId", true, "UUID of the track to reorder."))
        .arg(ArgSpec{.name = "order",
                     .kind = JsonKind::Array,
                     .required = true,
                     .description =
                         "Clip UUIDs, a permutation of the track's current clips."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kReorderClips));
        TimelineEngine& engine = session->engine();
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

Tool makeAddEffectTool(ProjectSession* session) {
    Tool t;
    t.name = kAddEffect;
    t.description = "Append an effect to a clip's effect chain.";
    // `type` is now a declared closed value set — the same list `parseEffectType`
    // accepts — including the ported `invert_colors` effect (Requirement 14.4).
    //
    // NOT expressible: `parameters` is an open map of names to numbers and the
    // vocabulary cannot constrain an object's member *values*. The handler is
    // correspondingly permissive (it takes the numeric members and ignores the
    // rest), so schema and handler still accept the same objects.
    t.schema
        .arg(uuidArg("clipId", true, "UUID of the target clip."))
        .arg(ArgSpec{.name = "type",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Effect type: brightness, contrast, blur, "
                                    "crop_transform, color_grade, invert_colors, custom.",
                     .enumValues = effectTypeValues()})
        .arg(ArgSpec{.name = "parameters",
                     .kind = JsonKind::Object,
                     .description = "Named numeric effect parameters."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddEffect));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> typeStr = requireString(in, "type");
        if (typeStr.isError()) return err<Json>(std::move(typeStr).error());

        // Exactly the declared value set: an unknown type is rejected rather than
        // silently becoming EffectType::Custom, so the published enum and the
        // handler accept the same inputs (Requirement 9.12).
        std::optional<EffectType> type = parseEffectType(typeStr.value());
        if (!type) {
            return err<Json>(invalidArgument("field 'type' is not a known effect type"));
        }

        Effect effect;
        effect.id = Uuid::generateV4();
        effect.type = *type;
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

Tool makeAddTransitionTool(ProjectSession* session) {
    Tool t;
    t.name = kAddTransition;
    t.description = "Set a clip's incoming transition (crossfade, wipe, slide, fade, ...).";
    // Both handler-private rules are now declared: `kind` is the closed set
    // `parseTransitionKind` accepts, and `durationNs >= 0`.
    t.schema
        .arg(uuidArg("clipId", true, "UUID of the clip whose incoming transition is set."))
        .arg(ArgSpec{.name = "kind",
                     .kind = JsonKind::String,
                     .required = true,
                     .description =
                         "Transition kind: crossfade, dip_to_color, wipe, slide, fade.",
                     .enumValues = transitionKindValues()})
        .arg(intArg("durationNs", true, "Transition region length in nanoseconds (>= 0).",
                   0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddTransition));
        TimelineEngine& engine = session->engine();
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

/// Wrap `handler` in the Requirement 3.5 guard: while no project is current the
/// tool refuses with "no project is open" before `handler` can run. Used by every
/// tool whose handler is not written inline (the hook-backed ones and the
/// session-backed defaults) except `project.create` and `project.open`, which
/// Requirement 3.5 exempts.
Tool::Handler sessionGuarded(ProjectSession* session, std::string_view tool,
                             Tool::Handler handler) {
    return [session, name = std::string(tool),
            handler = std::move(handler)](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(name));
        return handler(in);
    };
}

/// As above for a tool that has no default implementation: absent its hook the
/// capability is simply not configured in this build, which is an Unsupported
/// configuration report rather than an argument or state error. The no-project
/// refusal still comes first, so a caller with no project open always hears about
/// that rather than about the missing backend.
Tool::Handler guardedHookHandler(ProjectSession* session, std::string_view tool,
                                 Tool::Handler hook, std::string unavailable) {
    return sessionGuarded(
        session, tool,
        [hook = std::move(hook),
         unavailable = std::move(unavailable)](const Json& in) -> Result<Json> {
            if (!hook) return err<Json>(unsupported(unavailable));
            return hook(in);
        });
}

Tool makeGenerateTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kGenerate;
    t.description = "Trigger generative media (image/video) from a prompt and place it.";
    // The hook's rules, lifted: the prompt length gate (1..2000 characters), the
    // closed media-type set, a canonical-UUID `trackId` the hook *requires*, and a
    // non-negative `framePosition`. `params`, `sourceInTicks` and `sourceOutTicks`
    // are read by the composition root's hook but were never declared; they are
    // declared here so the advertised schema names every accepted argument
    // (Requirement 9.3) and they survive `additionalProperties: false`.
    //
    // NOT expressible: `sourceOutTicks > sourceInTicks` (a cross-field relation) and
    // "the model id is one the configured backend serves" (an open, backend-defined
    // set) remain with the coordinator.
    t.schema
        .arg(ArgSpec{.name = "prompt",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Generation prompt (1..2000 characters).",
                     .minLength = 1,
                     .maxLength = 2000})
        .arg(stringArg("model", true, "Selected SOTA model id (e.g. 'veo', 'gpt-image')."))
        .arg(ArgSpec{.name = "mediaType",
                     .kind = JsonKind::String,
                     .description = "Requested media type: 'video' or 'image'.",
                     .enumValues = {"video", "image"}})
        .arg(ArgSpec{.name = "params",
                     .kind = JsonKind::Object,
                     .description = "Model-specific string parameters."})
        .arg(uuidArg("trackId", true, "UUID of the track to place the generated clip on."))
        .arg(intArg("framePosition", false,
                   "Placement position in frames from timeline start.", 0))
        .arg(intArg("sourceInTicks", false,
                   "Source in-point of the generated clip in nanoseconds.", 0))
        .arg(intArg("sourceOutTicks", false,
                   "Source out-point of the generated clip in nanoseconds "
                   "(> sourceInTicks).",
                   1));
    t.handler = guardedHookHandler(
        session, kGenerate, std::move(hook),
        "generation.generate is not available: no generative backend is configured");
    return t;
}

Tool makeExportTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kExport;
    t.description = "Render the timeline to an output file at a selected format/resolution.";
    // No export hook is wired yet (the Export Engine is a later stage), so there is
    // no handler whose private rules could be lifted: the declaration is today's
    // argument list, unchanged, with a non-empty destination path and positive
    // output dimensions. The full container/codec/bitrate vocabulary arrives with
    // the hook.
    t.schema
        .arg(ArgSpec{.name = "outputPath",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Destination file path for the rendered output.",
                     .minLength = 1})
        .arg(stringArg("format", true, "Output container/codec format."))
        .arg(intArg("width", false, "Output width in pixels.", 1))
        .arg(intArg("height", false, "Output height in pixels.", 1));
    t.handler = guardedHookHandler(
        session, kExport, std::move(hook),
        "timeline.export is not available: no export engine is configured");
    return t;
}

// ---------------------------------------------------------------------------
// Session tools (task 4.3) — project.create / open / save / info
//
// These four are NOT EditCommands and are NOT undoable (design.md D1): each
// either commits a whole new project through ProjectSession (create/open) or only
// reads/persists the current one (info/save). ProjectSession builds the complete
// Project value locally and commits with TimelineEngine::reset only on full
// success, so a rejected create or open leaves the previous project, its document
// path, its modified flag and its undo history untouched (Requirements 3.9, 4.10).
//
// Each tool has a session-backed default implementation and accepts an optional
// hook override, so the headless sequence of Requirement 3.6 runs against a bare
// session while the GUI can still interpose its destination prompt on a save.
// ---------------------------------------------------------------------------

Tool makeProjectCreateTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectCreate;
    t.description = "Create a new project with the given name, frame rate, canvas and "
                    "colour space, and make it the current project.";
    // Every bound Requirement 3.8 names is declared here, so an out-of-range
    // argument is rejected by the published schema before ProjectSession is asked:
    // the name length (1..128 characters), the frame rate (1..240 fps), the canvas
    // (16..7680 x 16..4320 pixels) and the colour space (the closed set the domain
    // core exposes). ProjectSession re-checks each of them — it is reachable from
    // the GUI and the agent too — so the two agree by construction and the tool's
    // error names the rejected argument either way.
    t.schema
        .arg(ArgSpec{.name = "name",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Project name (1..128 characters).",
                     .minLength = kMinProjectNameLength,
                     .maxLength = kMaxProjectNameLength})
        .arg(ArgSpec{.name = "fps",
                     .kind = JsonKind::Number,
                     .required = true,
                     .description = "Timeline frame rate in frames per second (1..240).",
                     .minNum = static_cast<double>(kMinFramesPerSecond),
                     .maxNum = static_cast<double>(kMaxFramesPerSecond)})
        .arg(ArgSpec{.name = "width",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "Canvas width in pixels (16..7680).",
                     .minInt = static_cast<std::int64_t>(kMinCanvasWidth),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasWidth)})
        .arg(ArgSpec{.name = "height",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "Canvas height in pixels (16..4320).",
                     .minInt = static_cast<std::int64_t>(kMinCanvasHeight),
                     .maxInt = static_cast<std::int64_t>(kMaxCanvasHeight)})
        .arg(ArgSpec{.name = "colorSpace",
                     .kind = JsonKind::String,
                     .description = "Working colour space; defaults to Rec.709.",
                     .enumValues = colorSpaceValues()});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (hook) return hook(in);
        // Requirement 3.5 exempts this tool, so there is no "no project is open"
        // path: with neither a session nor a hook the capability is simply absent.
        if (session == nullptr) {
            return err<Json>(unsupported(
                "project.create is not available: no project session is bound"));
        }

        Result<std::string> name = requireString(in, "name");
        if (name.isError()) return err<Json>(std::move(name).error());
        Result<double> fps = requireNumber(in, "fps");
        if (fps.isError()) return err<Json>(std::move(fps).error());
        Result<std::int64_t> width = requireInt(in, "width");
        if (width.isError()) return err<Json>(std::move(width).error());
        Result<std::int64_t> height = requireInt(in, "height");
        if (height.isError()) return err<Json>(std::move(height).error());

        // The declared frame-rate bound again, for the same reason as the canvas
        // below: reached directly, an astronomically large number would otherwise
        // be converted to a rational through an out-of-range integer cast.
        if (!(fps.value() >= static_cast<double>(kMinFramesPerSecond)) ||
            fps.value() > static_cast<double>(kMaxFramesPerSecond)) {
            return err<Json>(outOfRange("rejected argument 'fps': the frame rate must lie "
                                        "between " + std::to_string(kMinFramesPerSecond) +
                                        " and " + std::to_string(kMaxFramesPerSecond) +
                                        " frames per second"));
        }

        const std::string colorSpaceName =
            in.stringOr("colorSpace", std::string(toStringView(kDefaultProjectColorSpace)));
        const std::optional<ColorSpace> colorSpace = parseColorSpace(colorSpaceName);
        if (!colorSpace) {
            return err<Json>(invalidArgument(
                "rejected argument 'colorSpace': '" + colorSpaceName +
                "' is not a colour space the domain core exposes"));
        }
        // The declared bounds again, because a handler is reachable without the
        // schema (the registry can be invoked directly) and the canvas fields are
        // unsigned: an unchecked cast of an out-of-range integer would wrap into a
        // plausible-looking canvas instead of being rejected (Requirement 3.8).
        if (width.value() < static_cast<std::int64_t>(kMinCanvasWidth) ||
            width.value() > static_cast<std::int64_t>(kMaxCanvasWidth)) {
            return err<Json>(outOfRange("rejected argument 'width': " +
                                        std::to_string(width.value()) + " is outside " +
                                        std::to_string(kMinCanvasWidth) + "-" +
                                        std::to_string(kMaxCanvasWidth) + " pixels"));
        }
        if (height.value() < static_cast<std::int64_t>(kMinCanvasHeight) ||
            height.value() > static_cast<std::int64_t>(kMaxCanvasHeight)) {
            return err<Json>(outOfRange("rejected argument 'height': " +
                                        std::to_string(height.value()) + " is outside " +
                                        std::to_string(kMinCanvasHeight) + "-" +
                                        std::to_string(kMaxCanvasHeight) + " pixels"));
        }

        const FrameRate  rate = frameRateFromFps(fps.value());
        const Resolution canvas{static_cast<std::uint32_t>(width.value()),
                                static_cast<std::uint32_t>(height.value())};

        Result<Uuid> created =
            session->createProject(name.value(), rate, canvas, *colorSpace);
        if (created.isError()) return err<Json>(std::move(created).error());

        Json out = Json::object();
        out.set("projectId", created.value().toString());
        out.set("name", name.value());
        out.set("fps", serializeFrameRate(rate));
        out.set("canvas", serializeCanvas(canvas));
        out.set("colorSpace", std::string(toStringView(*colorSpace)));
        out.set("modified", session->modified());
        out.set("documentPath", serializeDocumentPath(session->documentPath()));
        return out;
    };
    return t;
}

Tool makeProjectOpenTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectOpen;
    t.description = "Open a .palmier document and make it the current project.";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Filesystem path of the .palmier document to open.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (hook) return hook(in);
        if (session == nullptr) {
            return err<Json>(unsupported(
                "project.open is not available: no project session is bound"));
        }

        Result<std::string> path = requireString(in, "path");
        if (path.isError()) return err<Json>(std::move(path).error());

        // ProjectSession::openProject is all-or-nothing: a missing, unreadable,
        // malformed or unsupported-schema document is reported with the path and the
        // reason named, and the project that was current stays current, keeping its
        // document path, modified flag and undo history (Requirements 3.9, 4.10).
        Result<ProjectSession::Status> opened =
            session->openProject(std::filesystem::path(path.value()));
        if (opened.isError()) return err<Json>(std::move(opened).error());

        const ProjectSession::Status& status = opened.value();
        Json out = Json::object();
        out.set("projectId", status.projectId.toString());
        out.set("name", status.name);
        out.set("trackCount", static_cast<std::int64_t>(status.trackCount));
        out.set("clipCount", static_cast<std::int64_t>(status.clipCount));
        out.set("documentPath", serializeDocumentPath(status.documentPath));
        out.set("modified", status.modified);
        return out;
    };
    return t;
}

Tool makeProjectSaveTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectSave;
    t.description = "Write the current project to a .palmier document (defaults to its "
                    "recorded document path).";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .description = "Destination path; omitted means the project's "
                                        "recorded document path.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = sessionGuarded(
        session, kProjectSave,
        hook ? std::move(hook)
             : Tool::Handler([session](const Json& in) -> Result<Json> {
                   std::filesystem::path destination;
                   if (const Json* requested = in.find("path");
                       requested != nullptr && requested->isString()) {
                       destination = std::filesystem::path(requested->asString());
                   } else if (session->documentPath().has_value()) {
                       destination = *session->documentPath();
                   } else {
                       return err<Json>(failedPrecondition(
                           "project.save needs a destination: the project has no recorded "
                           "document path, so 'path' is required"));
                   }

                   // ProjectSession writes off the caller's thread (design.md D6,
                   // upstream PR 403) and applies the revision guard when the
                   // completion is pumped. A tool call is synchronous by contract —
                   // its result reports the bytes written — so this handler starts
                   // the save and then awaits exactly its own completion. The GUI
                   // does not take this path: it installs a `saveProject` hook that
                   // returns as soon as the write has been *started* and reports
                   // completion through the status bar instead.
                   auto captured = std::make_shared<
                       std::optional<ProjectSession::SaveCompletionInfo>>();
                   if (Result<void> started = session->requestSave(
                           destination,
                           [captured](const ProjectSession::SaveCompletionInfo& info) {
                               *captured = info;
                           });
                       started.isError()) {
                       return err<Json>(std::move(started).error());
                   }
                   while (!captured->has_value()) {
                       if (session->awaitSaveCompletions() == 0 && !captured->has_value()) {
                           return err<Json>(makeError(ErrorCode::Internal,
                                                      "project.save: the save completion "
                                                      "was never delivered"));
                       }
                   }

                   const ProjectSession::SaveCompletionInfo& info = **captured;
                   if (!info.succeeded) return err<Json>(info.error);

                   Json out = Json::object();
                   out.set("documentPath", info.path.string());
                   out.set("bytesWritten", static_cast<std::int64_t>(info.bytesWritten));
                   // Normally false; true only when the project was edited WHILE the
                   // write was running, in which case the written document is still
                   // valid but the session is legitimately still dirty (design.md D6).
                   out.set("modified", info.stillModified);
                   return out;
               }));
    return t;
}

Tool makeProjectInfoTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kProjectInfo;
    t.description = "Report the current project's identity, settings, counts, modified "
                    "state, document path and undo depth.";
    // No arguments.
    t.handler = sessionGuarded(
        session, kProjectInfo,
        hook ? std::move(hook)
             : Tool::Handler([session](const Json&) -> Result<Json> {
                   const Project                 project = session->engine().snapshot();
                   const ProjectSession::Status  status = session->status();

                   Json out = Json::object();
                   out.set("projectId", project.id.toString());
                   out.set("name", project.name);
                   out.set("fps", serializeFrameRate(project.timelineFps));
                   out.set("canvas", serializeCanvas(project.canvas));
                   out.set("colorSpace", std::string(toStringView(project.colorSpace)));
                   out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
                   out.set("clipCount", static_cast<std::int64_t>(countClips(project)));
                   // The media library is the authoritative asset catalogue of the
                   // current project (MediaImportService registers into it and
                   // ProjectSession rebuilds it from an opened document), so
                   // `project.info` and `media.list` always agree.
                   out.set("assetCount",
                           static_cast<std::int64_t>(session->mediaLibrary().assetCount()));
                   out.set("durationNs",
                           static_cast<std::int64_t>(timelineDuration(project).nanoseconds()));
                   out.set("modified", status.modified);
                   out.set("documentPath", serializeDocumentPath(status.documentPath));
                   out.set("undoDepth",
                           static_cast<std::int64_t>(session->engine().undoDepth()));
                   return out;
               }));
    return t;
}

// ---------------------------------------------------------------------------
// Media-library tools (task 4.4) — media.import / media.list
// ---------------------------------------------------------------------------

Tool makeMediaImportTool(ProjectSession* session, ToolRegistryHooks::MediaImportHook hook) {
    Tool t;
    t.name = kMediaImport;
    t.description = "Probe, validate and register a media file as an asset of the current "
                    "project's media library.";
    t.schema.arg(ArgSpec{.name = "path",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Filesystem path of the media file to import.",
                         .minLength = kMinPathLength,
                         .maxLength = kMaxPathLength});
    t.handler = [session, hook = std::move(hook)](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kMediaImport));
        if (!hook) {
            return err<Json>(unsupported(
                "media.import is not available: no media import service is configured"));
        }

        Result<std::string> path = requireString(in, "path");
        if (path.isError()) return err<Json>(std::move(path).error());

        Result<ImportedAsset> imported = hook(std::filesystem::path(path.value()));
        if (imported.isError()) return err<Json>(std::move(imported).error());

        // Requirement 2.2's result: the resolution and the frame rate are reported
        // only for an asset carrying a decodable video stream, and are ABSENT (not
        // null) otherwise, so a caller distinguishes "audio only" from "unknown".
        const ImportedAsset& asset = imported.value();
        Json out = Json::object();
        out.set("assetId", asset.assetId.toString());
        out.set("sourcePath", asset.sourcePath.string());
        out.set("containerFormat", asset.containerFormat);
        out.set("durationMs", asset.durationMs);
        if (asset.resolution.has_value()) {
            out.set("width", static_cast<std::int64_t>(asset.resolution->width));
            out.set("height", static_cast<std::int64_t>(asset.resolution->height));
        }
        if (asset.frameRate.has_value()) out.set("fps", asset.frameRate->toDouble());
        out.set("hasVideo", asset.hasVideo);
        out.set("hasAudio", asset.hasAudio);
        out.set("duplicate", asset.duplicate);
        return out;
    };
    return t;
}

Tool makeMediaListTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kMediaList;
    t.description = "List the assets registered in the current project's media library.";
    // No arguments.
    t.handler = sessionGuarded(
        session, kMediaList,
        hook ? std::move(hook)
             : Tool::Handler([session](const Json&) -> Result<Json> {
                   Json assets = Json::array();
                   for (const MediaAssetRef& ref : session->mediaLibrary().library()) {
                       Json entry = Json::object();
                       entry.set("assetId", ref.assetId.toString());
                       entry.set("sourcePath", ref.sourcePath);
                       entry.set("displayName",
                                 std::filesystem::path(ref.sourcePath).filename().string());
                       assets.push_back(std::move(entry));
                   }
                   const std::size_t count = assets.asArray().size();
                   Json out = Json::object();
                   out.set("assets", std::move(assets));
                   out.set("count", static_cast<std::int64_t>(count));
                   return out;
               }));
    return t;
}

// ---------------------------------------------------------------------------
// Track tools (task 4.5) — timeline.add_track / timeline.remove_track
//
// Unlike the session tools above, these two ARE ordinary edits: they are applied
// as the core AddTrackCommand / RemoveTrackCommand through TimelineEngine::apply,
// so they are atomic, invariant-checked, observable and undoable through exactly
// the same path as every clip edit (Requirements 3.3, 3.10).
// ---------------------------------------------------------------------------

Tool makeAddTrackTool(ProjectSession* session) {
    Tool t;
    t.name = kAddTrack;
    t.description = "Append a video or audio track after the last existing track of that "
                    "kind.";
    // `kind` is the closed set `parseTrackKind` accepts. NOT expressible: the
    // 64-tracks-per-kind cap of Requirement 3.8 counts the project's existing
    // tracks, which the schema never sees, so AddTrackCommand remains its
    // enforcement point.
    t.schema.arg(ArgSpec{.name = "kind",
                         .kind = JsonKind::String,
                         .required = true,
                         .description = "Track kind: 'video' or 'audio'.",
                         .enumValues = trackKindValues()});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddTrack));
        TimelineEngine& engine = session->engine();
        Result<std::string> kindStr = requireString(in, "kind");
        if (kindStr.isError()) return err<Json>(std::move(kindStr).error());
        const std::optional<TrackKind> kind = parseTrackKind(kindStr.value());
        if (!kind) {
            return err<Json>(invalidArgument(
                "rejected argument 'kind': '" + kindStr.value() +
                "' is not a track kind ('video' or 'audio')"));
        }

        auto           cmd = std::make_unique<AddTrackCommand>(*kind);
        AddTrackCommand* raw = cmd.get();  // owned by the undo stack after a successful apply
        CommandResult  result = engine.apply(std::move(cmd));
        if (result.isError()) return err<Json>(result.error());

        const Project project = engine.snapshot();
        Json out = Json::object();
        out.set("trackId", raw->trackId().toString());
        out.set("kind", std::string(trackKindName(*kind)));
        out.set("index", static_cast<std::int64_t>(raw->insertedIndex().value_or(0)));
        out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
        out.set("status", "applied");
        return out;
    };
    return t;
}

Tool makeRemoveTrackTool(ProjectSession* session) {
    Tool t;
    t.name = kRemoveTrack;
    t.description = "Remove a track and every clip on it, preserving the order of the "
                    "remaining tracks.";
    // NOT expressible: "the track is present in the current project" is a
    // state-dependent rule (Requirement 3.8), enforced by RemoveTrackCommand.
    t.schema.arg(uuidArg("trackId", true, "UUID of the track to remove."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRemoveTrack));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());

        CommandResult result =
            engine.apply(std::make_unique<RemoveTrackCommand>(trackId.value()));
        if (result.isError()) return err<Json>(result.error());

        const Project project = engine.snapshot();
        Json out = Json::object();
        out.set("trackId", trackId.value().toString());
        out.set("trackCount", static_cast<std::int64_t>(project.tracks.size()));
        out.set("clipCount", static_cast<std::int64_t>(countClips(project)));
        out.set("status", "applied");
        return out;
    };
    return t;
}

// ---------------------------------------------------------------------------
// Track mute tool (task 10.1) — timeline.set_track_muted
//
// The third track edit, and like the other two an ordinary undoable edit: it is
// applied as the core SetTrackMutedCommand through TimelineEngine::apply, so a
// mute is reversed by exactly one undo like any other edit. It exists because the
// offline interpreter's documented "mute track N" / "unmute track N" phrases must
// resolve to a tool that is actually in the Tool_Surface (Requirements 11.2,
// 11.3); the phrase's 1-based track ordinal is resolved to this identifier by the
// interpreter, which is the only party that knows the current project's tracks.
// ---------------------------------------------------------------------------

Tool makeSetTrackMutedTool(ProjectSession* session) {
    Tool t;
    t.name = kSetTrackMuted;
    t.description = "Mute or unmute a track, leaving its clips and every other track "
                    "untouched.";
    // NOT expressible: "the track is present in the current project" is a
    // state-dependent rule, enforced by SetTrackMutedCommand.
    t.schema.arg(uuidArg("trackId", true, "UUID of the track to mute or unmute."))
        .arg(ArgSpec{.name = "muted",
                     .kind = JsonKind::Bool,
                     .required = true,
                     .description = "True to mute the track, false to unmute it."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetTrackMuted));
        TimelineEngine& engine = session->engine();
        Result<Uuid> trackId = requireUuid(in, "trackId");
        if (trackId.isError()) return err<Json>(std::move(trackId).error());
        const Json* muted = in.find("muted");
        if (muted == nullptr || !muted->isBool()) {
            return err<Json>(invalidArgument("field 'muted' is required and must be a boolean"));
        }

        Json out = Json::object();
        out.set("trackId", trackId.value().toString());
        out.set("muted", muted->asBool());
        return applyCommand(
            engine, std::make_unique<SetTrackMutedCommand>(trackId.value(), muted->asBool()),
            std::move(out));
    };
    return t;
}

// ---------------------------------------------------------------------------
// History tools (task 10.1) — edit.undo / edit.redo
//
// These two are NOT EditCommands: they drive the engine's existing undo/redo
// stack, which is what makes them the tool-surface expression of Requirement 2.9.
// They exist for the same reason the mute tool does — the offline interpreter's
// documented `undo` and `redo` phrases have to resolve to real tools.
//
// An empty history is the engine's documented no-op-with-an-indication
// (Requirement 2.10), not a failure: the tool succeeds and reports `undone`/
// `redone` false plus the engine's indication, so a caller can tell "there was
// nothing to undo" from "the undo failed" without either being an error the
// executor would roll back.
// ---------------------------------------------------------------------------

Tool makeUndoTool(ProjectSession* session) {
    Tool t;
    t.name = kUndo;
    t.description = "Revert the most recently applied edit; reports a no-op when the undo "
                    "history is empty.";
    // No arguments.
    t.handler = [session](const Json&) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kUndo));
        TimelineEngine& engine = session->engine();
        const CommandResult result = engine.undo();
        if (result.isError()) return err<Json>(result.error());

        Json out = Json::object();
        out.set("undone", result.changed());
        out.set("indication", result.message());
        out.set("undoDepth", static_cast<std::int64_t>(engine.undoDepth()));
        out.set("redoDepth", static_cast<std::int64_t>(engine.redoDepth()));
        return out;
    };
    return t;
}

Tool makeRedoTool(ProjectSession* session) {
    Tool t;
    t.name = kRedo;
    t.description = "Re-apply the most recently undone edit; reports a no-op when the redo "
                    "history is empty.";
    // No arguments.
    t.handler = [session](const Json&) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRedo));
        TimelineEngine& engine = session->engine();
        const CommandResult result = engine.redo();
        if (result.isError()) return err<Json>(result.error());

        Json out = Json::object();
        out.set("redone", result.changed());
        out.set("indication", result.message());
        out.set("undoDepth", static_cast<std::int64_t>(engine.undoDepth()));
        out.set("redoDepth", static_cast<std::int64_t>(engine.redoDepth()));
        return out;
    };
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
        entry.set("inputSchema", t.inputSchema());
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

ToolRegistry buildDefaultToolRegistry(ProjectSession& session, ToolRegistryHooks hooks) {
    return buildDefaultToolRegistry(&session, std::move(hooks));
}

ToolRegistry buildDefaultToolRegistry(ProjectSession* session, ToolRegistryHooks hooks) {
    ToolRegistry registry;
    // Registration order is the order `tools/list` publishes, so it reads as the
    // life cycle of a project: read, then the session tools, the media library, the
    // track tools, the clip edits, generation and finally export.
    registry.add(makeReadTool(session));
    registry.add(makeProjectCreateTool(session, std::move(hooks.createProject)));
    registry.add(makeProjectOpenTool(session, std::move(hooks.openProject)));
    registry.add(makeProjectSaveTool(session, std::move(hooks.saveProject)));
    registry.add(makeProjectInfoTool(session, std::move(hooks.projectInfo)));
    registry.add(makeMediaImportTool(session, std::move(hooks.importMedia)));
    registry.add(makeMediaListTool(session, std::move(hooks.listMedia)));
    registry.add(makeAddTrackTool(session));
    registry.add(makeRemoveTrackTool(session));
    registry.add(makeSetTrackMutedTool(session));
    registry.add(makeAddClipTool(session));
    registry.add(makeDeleteClipTool(session));
    registry.add(makeMoveClipTool(session));
    registry.add(makeTrimClipTool(session));
    registry.add(makeSplitClipTool(session));
    registry.add(makeReorderClipsTool(session));
    registry.add(makeAddEffectTool(session));
    registry.add(makeAddTransitionTool(session));
    registry.add(makeUndoTool(session));
    registry.add(makeRedoTool(session));
    registry.add(makeGenerateTool(session, std::move(hooks.generate)));
    registry.add(makeExportTool(session, std::move(hooks.exportTimeline)));
    return registry;
}

}  // namespace palmier::services
