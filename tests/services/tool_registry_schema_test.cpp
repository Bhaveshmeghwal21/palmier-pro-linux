// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/tool_registry_schema_test.cpp — the tool surface's argument
// declarations (task 3.2; Requirements 3.1, 9.3, 9.9, 9.12, 14.4).
//
// Every tool in the default surface now declares its arguments exactly once as a
// `ToolSchema`, and `Tool::inputSchema()` renders the draft-07 schema `tools/list`
// publishes from that one declaration (design.md D3, "Schema/handler agreement").
// These tests pin the outcome of that conversion:
//
//   * the rendered schema of every tool still names exactly the arguments the
//     hand-written schema named, in the same order, with the same JSON types and
//     the same required set (Requirement 9.3) — the conversion is behaviour-
//     preserving on the published surface, apart from the two deliberate changes
//     below;
//   * `additionalProperties` is now false, so an undeclared key is rejected
//     instead of silently ignored (Requirement 9.9);
//   * the acceptance rules the handlers used to enforce privately — canonical
//     UUIDs, the `edge`/`kind`/`type` closed value sets, `timelineStartNs >= 0`,
//     `durationNs >= 0`, opacity in [0,1], gain >= 0, the 1..2000-character
//     generation prompt — are published as schema constraints and enforced by
//     `ToolSchema::validate`, and the handlers agree with them (Requirement 9.12);
//   * `timeline.add_effect` accepts the ported `invert_colors` effect end to end
//     (Requirement 14.4).
//
// One handler rule remains unexpressible in the `ArgSpec` vocabulary and is
// therefore still handler-only: `timeline.add_clip`'s `sourceOutNs > sourceInNs`
// is a relation between two arguments. The last test pins the handler's rejection
// so the gap is visible rather than silent.

#include "services/ToolRegistry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

// The default surface is now built over a ProjectSession, whose handlers resolve
// the engine at invocation time (task 3.4; design.md D1). A test seeds its fixture
// project into the session's one engine the way `project.open` will.
void seedSession(ProjectSession& session, Project project) {
    (void)session.engine().reset(std::move(project));
}

// --- Fixtures --------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Tool Schema Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    MediaAssetRef asset(Uuid::generateV4(), "/media/a.mp4");
    project.assets.push_back(asset);
    project.tracks.push_back(track);

    trackId = track.id;
    assetId = asset.assetId;
    return project;
}

Json addClipArgs(const Uuid& trackId, const Uuid& assetId) {
    Json args = Json::object();
    args.set("trackId", trackId.toString());
    args.set("assetId", assetId.toString());
    args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
    return args;
}

/// The `properties` names of a rendered schema, in rendered order.
std::vector<std::string> propertyNames(const Json& schema) {
    std::vector<std::string> names;
    const Json* props = schema.find("properties");
    if (props != nullptr && props->isObject()) {
        for (const Json::Member& member : props->asObject()) names.push_back(member.first);
    }
    return names;
}

std::vector<std::string> requiredNames(const Json& schema) {
    std::vector<std::string> names;
    const Json* required = schema.find("required");
    if (required != nullptr && required->isArray()) {
        for (const Json& entry : required->asArray()) names.push_back(entry.asString());
    }
    return names;
}

std::vector<std::string> enumValues(const Json& property) {
    std::vector<std::string> values;
    const Json* values_ = property.find("enum");
    if (values_ != nullptr && values_->isArray()) {
        for (const Json& entry : values_->asArray()) values.push_back(entry.asString());
    }
    return values;
}

const std::string kNotAUuid = "not-a-uuid";

// --- The published surface -------------------------------------------------

struct ArgExpectation {
    const char* name;
    const char* type;
    bool        required;
};

struct ToolExpectation {
    const char*                 tool;
    std::vector<ArgExpectation> args;
};

/// Exactly the arguments each tool advertised before the conversion, in the same
/// order, with the same types and required flags — plus the arguments the
/// `generation.generate` hook has always read but never declared (`params`,
/// `sourceInTicks`, `sourceOutTicks`) and its `trackId`, which the hook requires
/// and now says so.
std::vector<ToolExpectation> expectedSurface() {
    return {
        {"timeline.read", {}},
        // Tasks 4.3-4.5 — the session, media-library and track tools, with exactly
        // the arguments the design's tool table gives them (Requirement 3.1).
        {"project.create",
         {{"name", "string", true},
          {"fps", "number", true},
          {"width", "integer", true},
          {"height", "integer", true},
          {"colorSpace", "string", false}}},
        {"project.open", {{"path", "string", true}}},
        {"project.save", {{"path", "string", false}}},
        {"project.info", {}},
        {"media.import", {{"path", "string", true}}},
        {"media.list", {}},
        {"timeline.add_track", {{"kind", "string", true}}},
        {"timeline.remove_track", {{"trackId", "string", true}}},
        // Task 10.1 — the third track edit, added so the offline interpreter's
        // documented "mute track N" / "unmute track N" phrases resolve to a tool
        // that is really in the surface (Requirements 11.2, 11.3).
        {"timeline.set_track_muted",
         {{"trackId", "string", true}, {"muted", "boolean", true}}},
        {"timeline.add_clip",
         {{"trackId", "string", true},
          {"assetId", "string", true},
          {"sourcePath", "string", false},
          {"clipId", "string", false},
          {"timelineStartNs", "integer", false},
          {"sourceInNs", "integer", false},
          {"sourceOutNs", "integer", true},
          {"opacity", "number", false},
          {"gain", "number", false}}},
        {"timeline.delete_clip", {{"clipId", "string", true}}},
        {"timeline.move_clip",
         {{"clipId", "string", true}, {"timelineStartNs", "integer", true}}},
        {"timeline.trim_clip",
         {{"clipId", "string", true},
          {"edge", "string", true},
          {"boundaryNs", "integer", true},
          {"sourceDurationNs", "integer", false}}},
        {"timeline.split_clip",
         {{"clipId", "string", true}, {"playheadNs", "integer", true}}},
        {"timeline.reorder_clips",
         {{"trackId", "string", true}, {"order", "array", true}}},
        {"timeline.add_effect",
         {{"clipId", "string", true},
          {"type", "string", true},
          {"parameters", "object", false}}},
        {"timeline.add_transition",
         {{"clipId", "string", true},
          {"kind", "string", true},
          {"durationNs", "integer", true}}},
        {"generation.generate",
         {{"prompt", "string", true},
          {"model", "string", true},
          {"mediaType", "string", false},
          {"params", "object", false},
          {"trackId", "string", true},
          {"framePosition", "integer", false},
          {"sourceInTicks", "integer", false},
          {"sourceOutTicks", "integer", false}}},
        // Task 9.7 — `timeline.export` is now wired to services::ExportCoordinator,
        // and Requirement 7.2 lists what a caller may ask for: "an output path,
        // container format, video codec, resolution, frame rate and bit rate".
        // `codec`, `fps` and `bitrateKbps` had no argument to arrive in, and the
        // three booleans below express rules the export requirements make the caller
        // responsible for: the audio stream, hardware encoding (Requirement 8.2) and
        // the overwrite acknowledgement (Requirement 7.11), which must be
        // expressible or an existing destination could never be replaced through the
        // tool surface. Every addition is OPTIONAL, so every argument object that
        // validated before task 9.7 still validates and still means the same thing;
        // `outputPath` and `format` are untouched, including their required flags.
        {"timeline.export",
         {{"outputPath", "string", true},
          {"format", "string", true},
          {"width", "integer", false},
          {"height", "integer", false},
          {"codec", "string", false},
          {"fps", "number", false},
          {"bitrateKbps", "integer", false},
          {"includeAudio", "boolean", false},
          {"preferHardware", "boolean", false},
          {"overwrite", "boolean", false}}},
        // Task 10.1 — the tool-surface expression of the engine's undo/redo stack,
        // which the offline interpreter's `undo` and `redo` phrases resolve to.
        // Neither takes an argument.
        {"edit.undo", {}},
        {"edit.redo", {}},
    };
}

// ---------------------------------------------------------------------------
// 9.3 — every tool publishes an object schema naming its accepted arguments and
// listing the required ones, rendered from its single ToolSchema.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, EveryToolPublishesItsDeclaredArguments) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const std::vector<ToolExpectation> expected = expectedSurface();
    ASSERT_EQ(registry.size(), expected.size());

    for (const ToolExpectation& e : expected) {
        const Tool* tool = registry.find(e.tool);
        ASSERT_NE(tool, nullptr) << "tool " << e.tool << " is not registered";
        EXPECT_FALSE(tool->description.empty()) << e.tool;

        const Json schema = tool->inputSchema();
        EXPECT_EQ(schema.stringOr("type"), "object") << e.tool;

        std::vector<std::string> names, required;
        for (const ArgExpectation& a : e.args) {
            names.push_back(a.name);
            if (a.required) required.push_back(a.name);
        }
        EXPECT_EQ(propertyNames(schema), names) << e.tool;
        EXPECT_EQ(requiredNames(schema), required) << e.tool;

        const Json* props = schema.find("properties");
        ASSERT_NE(props, nullptr) << e.tool;
        for (const ArgExpectation& a : e.args) {
            const Json* property = props->find(a.name);
            ASSERT_NE(property, nullptr) << e.tool << '.' << a.name;
            EXPECT_EQ(property->stringOr("type"), a.type) << e.tool << '.' << a.name;
            EXPECT_FALSE(property->stringOr("description").empty())
                << e.tool << '.' << a.name << " has no description";
        }

        // The rendering is a pure function of the declaration.
        EXPECT_EQ(tool->inputSchema().dump(), schema.dump()) << e.tool;
        EXPECT_EQ(tool->schema.size(), e.args.size()) << e.tool;
    }
}

// ---------------------------------------------------------------------------
// 9.9 — the declared set is closed: an undeclared key is rejected rather than
// silently ignored (the `additionalProperties: true` hole the hand-written
// schemas left open).
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, UndeclaredArgumentsAreRejectedByEveryTool) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    for (const Tool& tool : registry.tools()) {
        const Json schema = tool.inputSchema();
        const Json* additional = schema.find("additionalProperties");
        ASSERT_NE(additional, nullptr) << tool.name;
        ASSERT_TRUE(additional->isBool()) << tool.name;
        EXPECT_FALSE(additional->asBool()) << tool.name;

        // An object carrying every required argument (so the required check passes)
        // plus one undeclared key must be rejected, naming the offending key.
        Json unknown = Json::object();
        for (const ArgSpec& spec : tool.schema.args()) {
            if (!spec.required) continue;
            switch (spec.kind) {
                case JsonKind::String:
                    // A declared minimum length is part of "its own required
                    // arguments": `project.create`'s name and the path arguments
                    // declare one, so the filler honours it.
                    unknown.set(spec.name,
                                spec.uuid ? Uuid::generateV4().toString()
                                          : (spec.enumValues.empty()
                                                 ? std::string(std::max<std::size_t>(
                                                                   spec.minLength.value_or(1), 1),
                                                               'x')
                                                 : spec.enumValues.front()));
                    break;
                case JsonKind::Integer:
                    // Likewise for a declared lower bound (`project.create`'s canvas
                    // dimensions start at 16 pixels).
                    unknown.set(spec.name, std::max<std::int64_t>(spec.minInt.value_or(1), 1));
                    break;
                case JsonKind::Number:
                    unknown.set(spec.name, std::max<double>(spec.minNum.value_or(1.0), 1.0));
                    break;
                case JsonKind::Bool:    unknown.set(spec.name, true); break;
                case JsonKind::Array:   unknown.set(spec.name, Json::array()); break;
                case JsonKind::Object:  unknown.set(spec.name, Json::object()); break;
            }
        }
        ASSERT_TRUE(tool.schema.validate(unknown).isOk())
            << tool.name << " rejected its own required arguments";

        unknown.set("definitelyNotAnArgument", true);
        const Result<void> rejected = tool.schema.validate(unknown);
        ASSERT_TRUE(rejected.isError()) << tool.name;
        EXPECT_NE(rejected.error().message().find("definitelyNotAnArgument"),
                  std::string::npos)
            << tool.name;
    }
}

// ---------------------------------------------------------------------------
// 9.12 — the UUID rule every handler enforced through `requireUuid` is now
// declared, so it is published and enforced before execution.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, IdentifierArgumentsPublishAndEnforceTheUuidFormat) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    struct Case {
        const char* tool;
        const char* arg;
    };
    const Case cases[] = {
        {"timeline.add_clip", "trackId"},      {"timeline.add_clip", "assetId"},
        {"timeline.add_clip", "clipId"},       {"timeline.delete_clip", "clipId"},
        {"timeline.move_clip", "clipId"},      {"timeline.trim_clip", "clipId"},
        {"timeline.split_clip", "clipId"},     {"timeline.reorder_clips", "trackId"},
        {"timeline.add_effect", "clipId"},     {"timeline.add_transition", "clipId"},
        {"generation.generate", "trackId"},
    };

    for (const Case& c : cases) {
        const Tool* tool = registry.find(c.tool);
        ASSERT_NE(tool, nullptr) << c.tool;
        const Json* property = tool->inputSchema().find("properties")->find(c.arg);
        ASSERT_NE(property, nullptr) << c.tool << '.' << c.arg;
        EXPECT_EQ(property->stringOr("format"), "uuid") << c.tool << '.' << c.arg;

        const ArgSpec* spec = tool->schema.find(c.arg);
        ASSERT_NE(spec, nullptr) << c.tool << '.' << c.arg;
        EXPECT_TRUE(spec->uuid) << c.tool << '.' << c.arg;

        Json args = Json::object();
        args.set(c.arg, kNotAUuid);
        const Result<void> rejected = tool->schema.validate(args);
        ASSERT_TRUE(rejected.isError()) << c.tool << '.' << c.arg;
        EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// 9.12 — the closed value sets that used to live inside the handlers.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, ClosedValueSetsArePublished) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json trim = registry.find("timeline.trim_clip")->inputSchema();
    EXPECT_EQ(enumValues(*trim.find("properties")->find("edge")),
              (std::vector<std::string>{"start", "end"}));

    const Json transition = registry.find("timeline.add_transition")->inputSchema();
    EXPECT_EQ(enumValues(*transition.find("properties")->find("kind")),
              (std::vector<std::string>{"crossfade", "dip_to_color", "wipe", "slide",
                                        "fade"}));

    // Requirement 14.4 — the ported invert-colors effect is an accepted type.
    const Json effect = registry.find("timeline.add_effect")->inputSchema();
    EXPECT_EQ(enumValues(*effect.find("properties")->find("type")),
              (std::vector<std::string>{"brightness", "contrast", "blur", "crop_transform",
                                        "color_grade", "invert_colors", "custom"}));

    const Json generate = registry.find("generation.generate")->inputSchema();
    EXPECT_EQ(enumValues(*generate.find("properties")->find("mediaType")),
              (std::vector<std::string>{"video", "image"}));
}

TEST(ToolRegistrySchema, NumericAndLengthBoundsArePublished) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Json addClip = registry.find("timeline.add_clip")->inputSchema();
    const Json* addClipProps = addClip.find("properties");
    // timelineStart >= 0 (checkTimelineInvariants), opacity in [0,1] and gain >= 0
    // (validateClip) — all engine rules a caller previously discovered only by
    // having its edit rejected after the fact.
    EXPECT_EQ(addClipProps->find("timelineStartNs")->find("minimum")->asInt(), 0);
    EXPECT_EQ(addClipProps->find("opacity")->find("minimum")->asDouble(), 0.0);
    EXPECT_EQ(addClipProps->find("opacity")->find("maximum")->asDouble(), 1.0);
    EXPECT_EQ(addClipProps->find("gain")->find("minimum")->asDouble(), 0.0);

    // MoveClipCommand: "destination position must be >= 0".
    const Json move = registry.find("timeline.move_clip")->inputSchema();
    EXPECT_EQ(move.find("properties")->find("timelineStartNs")->find("minimum")->asInt(), 0);

    // add_transition: "durationNs must be >= 0".
    const Json transition = registry.find("timeline.add_transition")->inputSchema();
    EXPECT_EQ(transition.find("properties")->find("durationNs")->find("minimum")->asInt(), 0);

    // The generation prompt gate (1..2000 characters).
    const Json generate = registry.find("generation.generate")->inputSchema();
    const Json* prompt = generate.find("properties")->find("prompt");
    EXPECT_EQ(prompt->find("minLength")->asInt(), 1);
    EXPECT_EQ(prompt->find("maxLength")->asInt(), 2000);
}

TEST(ToolRegistrySchema, PublishedBoundsAreEnforcedByValidate) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const ToolSchema& addClip = registry.find("timeline.add_clip")->schema;
    Json args = addClipArgs(trackId, assetId);
    EXPECT_TRUE(addClip.validate(args).isOk());

    Json tooOpaque = args;
    tooOpaque.set("opacity", 1.5);
    EXPECT_TRUE(addClip.validate(tooOpaque).isError());

    Json negativeGain = args;
    negativeGain.set("gain", -0.5);
    EXPECT_TRUE(addClip.validate(negativeGain).isError());

    Json negativeStart = args;
    negativeStart.set("timelineStartNs", static_cast<std::int64_t>(-1));
    EXPECT_TRUE(addClip.validate(negativeStart).isError());

    // A fractional tick count is not an integer position.
    Json fractional = args;
    fractional.set("sourceOutNs", 1.5);
    EXPECT_TRUE(addClip.validate(fractional).isError());

    const ToolSchema& transition = registry.find("timeline.add_transition")->schema;
    Json negativeDuration = Json::object();
    negativeDuration.set("clipId", Uuid::generateV4().toString());
    negativeDuration.set("kind", "crossfade");
    negativeDuration.set("durationNs", static_cast<std::int64_t>(-1));
    EXPECT_TRUE(transition.validate(negativeDuration).isError());
    negativeDuration.set("durationNs", static_cast<std::int64_t>(0));
    EXPECT_TRUE(transition.validate(negativeDuration).isOk());

    const ToolSchema& generate = registry.find("generation.generate")->schema;
    Json generateArgs = Json::object();
    generateArgs.set("prompt", "a duck on a bicycle");
    generateArgs.set("model", "veo");
    generateArgs.set("trackId", trackId.toString());
    EXPECT_TRUE(generate.validate(generateArgs).isOk());
    Json emptyPrompt = generateArgs;
    emptyPrompt.set("prompt", "");
    EXPECT_TRUE(generate.validate(emptyPrompt).isError());
    Json longPrompt = generateArgs;
    longPrompt.set("prompt", std::string(2001, 'x'));
    EXPECT_TRUE(generate.validate(longPrompt).isError());
}

// ---------------------------------------------------------------------------
// 9.12 / 14.4 — the effect-type set the schema publishes is exactly the set the
// handler accepts, and `invert_colors` reaches the project.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, AddEffectAcceptsInvertColorsAndRejectsUnknownTypes) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    Result<Json> added = registry.invoke("timeline.add_clip", addClipArgs(trackId, assetId));
    ASSERT_TRUE(added.isOk());
    const std::string clipId = added.value().stringOr("clipId");
    ASSERT_FALSE(clipId.empty());

    Json effectArgs = Json::object();
    effectArgs.set("clipId", clipId);
    effectArgs.set("type", "invert_colors");
    ASSERT_TRUE(registry.find("timeline.add_effect")->schema.validate(effectArgs).isOk());
    ASSERT_TRUE(registry.invoke("timeline.add_effect", effectArgs).isOk());

    // The effect is on the clip, reported under its published type name.
    const Result<Json> read = registry.invoke("timeline.read", Json::object());
    ASSERT_TRUE(read.isOk());
    const Json& clips = *read.value().find("tracks")->asArray()[0].find("clips");
    ASSERT_EQ(clips.asArray().size(), 1u);
    const Json& effects = *clips.asArray()[0].find("effects");
    ASSERT_EQ(effects.asArray().size(), 1u);
    EXPECT_EQ(effects.asArray()[0].stringOr("type"), "invert_colors");

    // A type outside the published set is rejected by the schema AND by the
    // handler: an unknown value no longer degrades silently to "custom".
    Json unknownType = Json::object();
    unknownType.set("clipId", clipId);
    unknownType.set("type", "sepia");
    EXPECT_TRUE(registry.find("timeline.add_effect")->schema.validate(unknownType).isError());
    const Result<Json> rejected = registry.invoke("timeline.add_effect", unknownType);
    ASSERT_TRUE(rejected.isError());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// The one rule the ArgSpec vocabulary cannot express: a relation between two
// arguments. `sourceOutNs > sourceInNs` therefore stays inside the handler, and
// this test pins it so the gap stays visible.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, CrossFieldSourceRangeRuleRemainsHandlerEnforced) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    const Tool* addClip = registry.find("timeline.add_clip");
    ASSERT_NE(addClip, nullptr);

    Json inverted = addClipArgs(trackId, assetId);
    inverted.set("sourceInNs", static_cast<std::int64_t>(2'000'000'000));
    inverted.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));

    // The published schema accepts it — no cross-field constraint exists.
    EXPECT_TRUE(addClip->schema.validate(inverted).isOk());

    // The handler rejects it, as it always has.
    const Result<Json> rejected = registry.invoke("timeline.add_clip", inverted);
    ASSERT_TRUE(rejected.isError());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Task 9.7 — the arguments `timeline.export` gained when the hook was wired to
// services::ExportCoordinator, and the bounds `width`/`height` gained.
//
// The additions are asserted here, next to the surface expectation above, so the
// schema change is described by a test rather than only by a comment: what is
// published, that it is optional, and — the compatibility claim that matters —
// that the argument object accepted before task 9.7 is still accepted unchanged.
// ---------------------------------------------------------------------------

TEST(ToolRegistrySchema, ExportPublishesTheRequirement72ArgumentsAndTheirRanges) {
    Uuid trackId, assetId;
    ProjectSession session;
    seedSession(session, makeProject(trackId, assetId));
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const Tool* exportTool = registry.find("timeline.export");
    ASSERT_NE(exportTool, nullptr);
    const Json  schema = exportTool->inputSchema();
    const Json* props = schema.find("properties");
    ASSERT_NE(props, nullptr);

    // Requirement 7.1's accepted ranges, published rather than only enforced.
    EXPECT_EQ(props->find("width")->find("minimum")->asInt(), 128);
    EXPECT_EQ(props->find("width")->find("maximum")->asInt(), 3840);
    EXPECT_EQ(props->find("height")->find("minimum")->asInt(), 128);
    EXPECT_EQ(props->find("height")->find("maximum")->asInt(), 2160);
    EXPECT_EQ(props->find("fps")->find("minimum")->asDouble(), 1.0);
    EXPECT_EQ(props->find("fps")->find("maximum")->asDouble(), 120.0);
    EXPECT_EQ(props->find("bitrateKbps")->find("minimum")->asInt(), 100);
    EXPECT_EQ(props->find("bitrateKbps")->find("maximum")->asInt(), 200'000);

    // The three codecs the Encoder_Selector supports (Requirement 8.2).
    EXPECT_EQ(enumValues(*props->find("codec")),
              (std::vector<std::string>{"h264", "hevc", "vp9"}));

    // Only `outputPath` and `format` are required: nothing task 9.7 added is.
    EXPECT_EQ(requiredNames(schema), (std::vector<std::string>{"outputPath", "format"}));

    // Backwards compatibility, asserted rather than asserted-about: the exact
    // argument object callers sent before task 9.7 still validates.
    Json legacy = Json::object();
    legacy.set("outputPath", std::string("/tmp/palmier-schema-test-out.mp4"));
    legacy.set("format", std::string("mp4"));
    EXPECT_TRUE(exportTool->schema.validate(legacy).isOk());

    // And the new arguments validate together, at their bounds.
    Json full = legacy;
    full.set("width", static_cast<std::int64_t>(1920));
    full.set("height", static_cast<std::int64_t>(1080));
    full.set("codec", std::string("hevc"));
    full.set("fps", 120.0);
    full.set("bitrateKbps", static_cast<std::int64_t>(100));
    full.set("includeAudio", false);
    full.set("preferHardware", false);
    full.set("overwrite", true);
    EXPECT_TRUE(exportTool->schema.validate(full).isOk());

    // Each published bound is enforced by the same declaration.
    Json tooSmall = full;
    tooSmall.set("width", static_cast<std::int64_t>(127));
    EXPECT_TRUE(exportTool->schema.validate(tooSmall).isError());
    Json tooFast = full;
    tooFast.set("fps", 121.0);
    EXPECT_TRUE(exportTool->schema.validate(tooFast).isError());
    Json unknownCodec = full;
    unknownCodec.set("codec", std::string("av1"));
    EXPECT_TRUE(exportTool->schema.validate(unknownCodec).isError());
    Json notABool = full;
    notABool.set("overwrite", std::string("yes"));
    EXPECT_TRUE(exportTool->schema.validate(notABool).isError());
}

}  // namespace
}  // namespace palmier::services
