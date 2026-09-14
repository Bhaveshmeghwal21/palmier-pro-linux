// SPDX-License-Identifier: GPL-3.0-or-later
//
// Continuation of the mcp_protocol_property_test fixtures; see mcp_protocol_property_test_support1.hpp.

#include "mcp_protocol_property_test_support1.hpp"

// Part 2 of 2 of the shared fixtures for mcp_protocol_property_test, split because the
// fixtures alone exceed the 1000-line ceiling this repository enforces. Cut only at
// points where the brace depth is zero, so no entity is divided.

#ifndef PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT2_HPP_
#define PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT2_HPP_

namespace palmier::services {
namespace testsupport_mcpprotocolpropertytest {

[[nodiscard]] inline Json validValue(const ArgSpec& spec, const ScratchDir& scratch) {
    switch (spec.kind) {
        case JsonKind::Object: {
            Json object = Json::object();
            object.set("amount", Json(0.5));
            return object;
        }
        case JsonKind::Array: {
            Json items = Json::array();
            const std::size_t count =
                std::max<std::size_t>(spec.minLength.value_or(0), drawIndex(3));
            for (std::size_t i = 0; i < count; ++i) {
                items.push_back(Json(Uuid::generateV4().toString()));
            }
            return items;
        }
        case JsonKind::String: {
            if (!spec.enumValues.empty()) {
                return Json(spec.enumValues[drawIndex(spec.enumValues.size())]);
            }
            if (spec.uuid) return Json(Uuid::generateV4().toString());
            if (spec.name == "path" || spec.name == "outputPath") {
                return Json(scratch.file("generated").string());
            }
            const std::size_t low = std::max<std::size_t>(spec.minLength.value_or(1), 1);
            const std::size_t high =
                std::min<std::size_t>(spec.maxLength.value_or(low + 8), low + 8);
            return Json(drawAsciiText(low + drawIndex(high - low + 1)));
        }
        case JsonKind::Integer: {
            const std::int64_t low = spec.minInt.value_or(0);
            const std::int64_t high = spec.maxInt.value_or(low + 1'000'000);
            return Json(low >= high ? low : *rc::gen::inRange<std::int64_t>(low, high));
        }
        case JsonKind::Number: {
            const double low = spec.minNum.value_or(static_cast<double>(spec.minInt.value_or(0)));
            const double high =
                spec.maxNum.value_or(static_cast<double>(spec.maxInt.value_or(0)) + 100.0);
            const double span = high > low ? high - low : 1.0;
            return Json(low + span * (static_cast<double>(drawIndex(101)) / 100.0));
        }
        case JsonKind::Bool:
            return Json(*rc::gen::arbitrary<bool>());
    }
    return Json(nullptr);
}

/// A schema-valid argument object for `tool`: every required argument present,
/// each optional one included at random.
[[nodiscard]] inline Json schemaValidArgs(const Tool& tool, const ScratchDir& scratch) {
    Json args = Json::object();
    for (const ArgSpec& spec : tool.schema.args()) {
        if (!spec.required && !*rc::gen::arbitrary<bool>()) continue;
        args.set(spec.name, validValue(spec, scratch));
    }
    return args;
}

/// A value of a type the argument does not accept.
[[nodiscard]] inline Json wrongTypedValue(JsonKind kind) {
    if (kind == JsonKind::String) return Json(static_cast<std::int64_t>(7));
    if (kind == JsonKind::Bool) return Json("true");
    if (kind == JsonKind::Array || kind == JsonKind::Object) return Json("not-a-composite");
    return Json("not-a-number");
}

/// True iff `spec` declares a bound a value can fall outside of.
[[nodiscard]] inline bool hasBound(const ArgSpec& spec) {
    return spec.uuid || !spec.enumValues.empty() || spec.minInt.has_value() ||
           spec.maxInt.has_value() || spec.minNum.has_value() || spec.maxNum.has_value() ||
           spec.minLength.has_value() || spec.maxLength.has_value();
}

/// A value violating exactly one declared bound of `spec`.
[[nodiscard]] inline Json outOfBoundsValue(const ArgSpec& spec) {
    if (!spec.enumValues.empty()) return Json("not-a-member-of-the-enum");
    if (spec.uuid) return Json("not-a-uuid");
    if (spec.maxInt.has_value()) return Json(*spec.maxInt + 1);
    if (spec.minInt.has_value()) return Json(*spec.minInt - 1);
    if (spec.maxNum.has_value()) return Json(*spec.maxNum + 1.0);
    if (spec.minNum.has_value()) return Json(*spec.minNum - 1.0);
    if (spec.maxLength.has_value()) {
        return spec.kind == JsonKind::Array ? Json::array()
                                            : Json(drawAsciiText(*spec.maxLength + 1));
    }
    // A minLength bound: an empty string / empty array is one short of it.
    return spec.kind == JsonKind::Array ? Json::array() : Json(std::string{});
}

/// The enum value set a tool declares for `argument` (empty when it declares none).
[[nodiscard]] inline std::string drawEnumValue(const ToolRegistry& registry, std::string_view tool,
                                               std::string_view argument) {
    const Tool* entry = registry.find(tool);
    if (entry == nullptr) return {};
    const ArgSpec* spec = entry->schema.find(argument);
    if (spec == nullptr || spec->enumValues.empty()) return {};
    return spec->enumValues[drawIndex(spec->enumValues.size())];
}

// ---------------------------------------------------------------------------
// Property 46's invocation generator: a real tool with arguments that both
// validate against its advertised schema AND are semantically applicable to the
// current project, drawn from live project state.
// ---------------------------------------------------------------------------

struct Invocation {
    std::string tool;
    Json arguments;
};

/// A track holding at least one clip, or nullptr when the project has none.
[[nodiscard]] inline const Track* drawTrackWithClips(const Project& project) {
    std::vector<const Track*> candidates;
    for (const Track& track : project.tracks) {
        if (!track.clips.empty()) candidates.push_back(&track);
    }
    if (candidates.empty()) return nullptr;
    return candidates[drawIndex(candidates.size())];
}

[[nodiscard]] inline Invocation drawValidInvocation(const ToolRegistry& registry,
                                                    const ProjectSession& session,
                                                    const ScratchDir& scratch,
                                                    const fs::path& document) {
    const Tool& tool = registry.tools()[drawIndex(registry.size())];
    const std::string name = tool.name;
    const Project project = session.engine().snapshot();

    Json args = Json::object();

    // The arg-less tools, and the hook-backed tools, take their arguments straight
    // from the schema. `edit.undo` / `edit.redo` (task 10.1) belong here: they take
    // no arguments and an empty history is the engine's documented successful
    // no-op, never an error, so they are applicable to every generated project.
    if (name == kReadTimeline || name == kProjectInfo || name == kMediaList || name == kUndo ||
        name == kRedo || isHookBacked(name)) {
        return Invocation{name, schemaValidArgs(tool, scratch)};
    }

    // media.set_tags (usable-editor tasks.md task 15; no dedicated
    // Requirement) needs a REAL asset id from the project, exactly like
    // timeline.add_clip needs a real trackId/assetId — the seed project
    // always carries at least one asset (see drawSeedProject()).
    if (name == kMediaSetTags) {
        args.set("assetId",
                 Json(project.assets[drawIndex(project.assets.size())].assetId.toString()));
        Json tags = Json::array();
        const int tagCount = *rc::gen::inRange(0, 4);
        for (int i = 0; i < tagCount; ++i) {
            tags.push_back(Json(drawAsciiText(1 + drawIndex(12))));
        }
        args.set("tags", std::move(tags));
        return Invocation{name, std::move(args)};
    }

    if (name == kProjectCreate) {
        args.set("name", Json(drawAsciiText(1 + drawIndex(24))));
        args.set("fps", Json(static_cast<double>(*rc::gen::inRange(1, 241))));
        args.set("width", Json(static_cast<std::int64_t>(*rc::gen::inRange(16, 7681))));
        args.set("height", Json(static_cast<std::int64_t>(*rc::gen::inRange(16, 4321))));
        if (*rc::gen::arbitrary<bool>()) {
            args.set("colorSpace", Json(drawEnumValue(registry, kProjectCreate, "colorSpace")));
        }
        return Invocation{name, std::move(args)};
    }
    if (name == kProjectOpen) {
        args.set("path", Json(document.string()));
        return Invocation{name, std::move(args)};
    }
    if (name == kProjectSave) {
        args.set("path", Json(scratch.file("saved").string()));
        return Invocation{name, std::move(args)};
    }
    // Mutable project settings (task 10; Requirement 7): at least one field must
    // be given, so the generic schemaValidArgs() fallback (which may supply none)
    // would not be applicable here. A distinct fps from the seed's own rate proves
    // this changes the setting rather than reporting a no-op-shaped success.
    if (name == kSetProjectSettings) {
        const double newFps = project.timelineFps.toDouble() >= 60.0 ? 24.0 : 60.0;
        args.set("fps", Json(newFps));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddTrack) {
        args.set("kind", Json(drawEnumValue(registry, kAddTrack, "kind")));
        return Invocation{name, std::move(args)};
    }
    if (name == kRemoveTrack) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        return Invocation{name, std::move(args)};
    }
    // Task 10.1: like every other track-addressed edit, applicable exactly when the
    // identifier names a track of the current project. Either flag value applies.
    if (name == kSetTrackMuted) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        args.set("muted", Json(*rc::gen::arbitrary<bool>()));
        return Invocation{name, std::move(args)};
    }
    // Upstream parity: the `timeline` tool category's marker gap. `add_marker` needs
    // no pre-existing state; `remove_marker` draws the guaranteed seed marker (see
    // drawSeedProject()) the same way kRemoveCaptionCue above draws its guaranteed cue.
    if (name == kAddMarker) {
        args.set("timelineNs", Json(projectEndNs(project)));
        if (*rc::gen::arbitrary<bool>()) {
            args.set("label", Json(drawAsciiText(1 + drawIndex(24))));
        }
        return Invocation{name, std::move(args)};
    }
    if (name == kRemoveMarker) {
        const Marker& marker = project.markers[drawIndex(project.markers.size())];
        args.set("markerId", Json(marker.id.toString()));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddClip) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        args.set("assetId",
                 Json(project.assets[drawIndex(project.assets.size())].assetId.toString()));
        // Beyond everything already on the timeline, so the placement cannot
        // overlap an existing clip.
        args.set("timelineStartNs", Json(projectEndNs(project) +
                                         Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("sourceInNs", Json(static_cast<std::int64_t>(0)));
        args.set("sourceOutNs", Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        if (*rc::gen::arbitrary<bool>()) {
            args.set("opacity", Json(static_cast<double>(drawIndex(101)) / 100.0));
        }
        return Invocation{name, std::move(args)};
    }

    // Everything below needs a clip. The seed project always has at least two on
    // its first track, so this is reachable for every generated project.
    const Track* track = drawTrackWithClips(project);
    RC_ASSERT(track != nullptr);

    if (name == kReorderClips) {
        std::vector<std::size_t> order(track->clips.size());
        std::iota(order.begin(), order.end(), 0u);
        for (std::size_t i = order.size(); i > 1; --i) {  // Fisher-Yates
            std::swap(order[i - 1], order[drawIndex(i)]);
        }
        Json entries = Json::array();
        for (const std::size_t index : order) {
            entries.push_back(Json(track->clips[index].id.toString()));
        }
        args.set("trackId", Json(track->id.toString()));
        args.set("order", std::move(entries));
        return Invocation{name, std::move(args)};
    }

    // A start-edge trim shifts the clip later on its track, so trimming targets
    // the LAST clip of the track, where no successor can be displaced.
    const Clip& lastClip = track->clips.back();
    const Clip& anyClip = track->clips[drawIndex(track->clips.size())];

    if (name == kTrimClip) {
        args.set("clipId", Json(lastClip.id.toString()));
        args.set("edge", Json(drawEnumValue(registry, kTrimClip, "edge")));
        // Deliberately unclamped: TrimClipCommand clamps the boundary into the
        // legal range rather than rejecting it, so any value is applicable.
        args.set("boundaryNs",
                 Json(Duration::fromMilliseconds(
                          *rc::gen::inRange<std::int64_t>(-kClipLengthMs, 3 * kClipLengthMs))
                          .nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kDeleteClip) {
        args.set("clipId", Json(anyClip.id.toString()));
        return Invocation{name, std::move(args)};
    }
    // Task 8.2 — ripple editing. Every seed clip is a full second long and is
    // separated from its neighbour by a gap, which is what makes these applicable.
    if (name == kRippleDelete) {
        // Closing a one-second hole only ever moves later clips EARLIER by one
        // second, and each already begins a gap after its predecessor's end, so no
        // choice of clip can overlap or cross zero.
        args.set("clipId", Json(anyClip.id.toString()));
        return Invocation{name, std::move(args)};
    }
    if (name == kRippleTrim) {
        // Applicable for either edge and any boundary, because the command clamps:
        // `sourceDurationNs` defaults to the clip's out-point, so an end-edge trim
        // can only SHORTEN the clip (its followers move earlier by the same amount,
        // preserving every gap) and a start-edge trim leaves the trailing edge fixed
        // (so no follower moves at all). The seed project declares no clip groups,
        // so no cross-track propagation is in play here.
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("edge", Json(drawEnumValue(registry, kRippleTrim, "edge")));
        args.set("boundaryNs",
                 Json(Duration::fromMilliseconds(
                          *rc::gen::inRange<std::int64_t>(-kClipLengthMs, 3 * kClipLengthMs))
                          .nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kCloseGap) {
        // A gap can only be closed after a clip that HAS a successor, so this
        // deliberately targets the first track, which the seed project always fills
        // with at least two clips, and never its last clip.
        const Track* twoOrMore = nullptr;
        for (const Track& candidate : project.tracks) {
            if (candidate.clips.size() >= 2) {
                twoOrMore = &candidate;
                break;
            }
        }
        RC_ASSERT(twoOrMore != nullptr);
        const Clip& withSuccessor = twoOrMore->clips[drawIndex(twoOrMore->clips.size() - 1)];
        args.set("clipId", Json(withSuccessor.id.toString()));
        return Invocation{name, std::move(args)};
    }
    if (name == kMoveClip) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("timelineStartNs", Json(projectEndNs(project) +
                                         Duration::fromMilliseconds(2 * kClipGapMs).nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetKeyframes) {
        // Two keyframes on a drawn property, so the generated call exercises a real animation
        // rather than the empty-list clear.
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("property", Json(drawEnumValue(registry, kSetKeyframes, "property")));
        Json points = Json::array();
        Json first = Json::object();
        first.set("timeNs", Json(static_cast<std::int64_t>(0)));
        first.set("value", Json(static_cast<double>(drawIndex(101)) / 100.0));
        points.push_back(std::move(first));
        Json second = Json::object();
        second.set("timeNs", Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        second.set("value", Json(static_cast<double>(drawIndex(101)) / 100.0));
        points.push_back(std::move(second));
        args.set("keyframes", std::move(points));
        return Invocation{name, std::move(args)};
    }
    if (name == kApplyLayout) {
        // One clip is enough for the arrangement to be a legal edit, and it keeps the generator
        // independent of how many clips the drawn project happens to hold.
        Json ids = Json::array();
        ids.push_back(Json(anyClip.id.toString()));
        args.set("clipIds", std::move(ids));
        args.set("layout", Json(drawEnumValue(registry, kApplyLayout, "layout")));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetClipProperties) {
        // Both properties, so each optional field of the rendered payload is exercised.
        // opacity is bounded to [0,1] and gain to >= 0 by the advertised schema.
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("opacity", Json(static_cast<double>(drawIndex(101)) / 100.0));
        args.set("gain", Json(static_cast<double>(drawIndex(201)) / 100.0));
        // Only speeds >= 1.0, which SHORTEN the clip. A slower speed lengthens it and is
        // legitimately refused when the clip has a neighbour to run into, and this property
        // asserts the shape of a SUCCESSFUL call -- so the invocation it draws has to be one
        // that always applies. That refusal is covered directly in clip_speed_test.cpp.
        args.set("speed", Json(static_cast<double>(100 + drawIndex(301)) / 100.0));
        return Invocation{name, std::move(args)};
    }
    if (name == kSplitClip) {
        // Strictly interior: the clip is a full second long, so the midpoint is
        // several frames from either edge at every generated frame rate.
        const std::int64_t interior =
            anyClip.timelineStart.nanoseconds() +
            (anyClip.timelineEnd().nanoseconds() - anyClip.timelineStart.nanoseconds()) / 2;
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("playheadNs", Json(interior));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddEffect) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("type", Json(drawEnumValue(registry, kAddEffect, "type")));
        if (*rc::gen::arbitrary<bool>()) {
            Json parameters = Json::object();
            parameters.set("amount", Json(static_cast<double>(drawIndex(101)) / 100.0));
            args.set("parameters", std::move(parameters));
        }
        return Invocation{name, std::move(args)};
    }
    if (name == kAddTransition) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("kind", Json(drawEnumValue(registry, kAddTransition, "kind")));
        args.set(
            "durationNs",
            Json(
                Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 200)).nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    // Effect lifecycle (task 9.2; Requirement 6). drawSeedProject() guarantees the
    // first clip of track 0 carries exactly one effect, which is what these three
    // target: track 0 always exists and always has at least one clip, so this does
    // not depend on which track/clip drawEnumValue or anyClip happened to pick.
    if (name == kRemoveEffect || name == kReorderEffects || name == kSetEffectParameter) {
        const Clip& seeded = project.tracks[0].clips[0];
        const Uuid effectId = seeded.effects.front().id;
        args.set("clipId", Json(seeded.id.toString()));
        if (name == kRemoveEffect) {
            args.set("effectId", Json(effectId.toString()));
        } else if (name == kReorderEffects) {
            // A permutation of exactly the clip's one effect id is the only
            // permutation there is, and it is trivially valid.
            Json order = Json::array();
            order.push_back(Json(effectId.toString()));
            args.set("order", std::move(order));
        } else {
            args.set("effectId", Json(effectId.toString()));
            args.set("parameter", Json(std::string{"amount"}));
            args.set("value", Json(static_cast<double>(drawIndex(201)) / 100.0));
        }
        return Invocation{name, std::move(args)};
    }

    // Tone-curve control points (monitoring-and-grading Requirement 5.7). Targets the
    // same guaranteed seeded effect. The effect's TYPE does not matter: a control point
    // is a parameter like any other, and the command deliberately does not police which
    // effect kind carries one -- exactly as set_effect_parameter does not.
    // A resource path on the same guaranteed seeded effect (Requirement 7.9). The path is
    // deliberately one that does not exist: the tool must not check, because 7.8 requires a
    // missing LUT to leave the effect in the chain rather than to refuse the edit.
    if (name == kSetEffectResource) {
        const Clip& seeded = project.tracks[0].clips[0];
        args.set("clipId", Json(seeded.id.toString()));
        args.set("effectId", Json(seeded.effects.front().id.toString()));
        args.set("path", Json(std::string{"/luts/generated.cube"}));
        return Invocation{name, std::move(args)};
    }
    if (name == kEditCurvePoint) {
        const Clip& seeded = project.tracks[0].clips[0];
        args.set("clipId", Json(seeded.id.toString()));
        args.set("effectId", Json(seeded.effects.front().id.toString()));
        args.set("channel", Json(drawEnumValue(registry, kEditCurvePoint, "channel")));
        // "add" only: move and remove need a point that already exists, and this draws
        // one invocation against a fresh project every case. The undo/redo and
        // renumbering behaviour of the other two is pinned in the core command's own
        // tests, where a curve can be built up first.
        args.set("operation", Json(std::string{"add"}));
        args.set("x", Json(static_cast<double>(drawIndex(101)) / 100.0));
        args.set("y", Json(static_cast<double>(drawIndex(101)) / 100.0));
        return Invocation{name, std::move(args)};
    }

    // Text and titles (usable-editor task 12; Requirement 9). The seed project's
    // guaranteed text track/clip (see drawSeedProject()) is the stable target,
    // exactly like the effect-lifecycle tools above target track 0's guaranteed
    // effect: these three need a REAL text track/clip, which a random
    // schema-valid uuid from the generic fallback below would essentially never
    // draw, and this property's own premise (every non-hook-backed invocation
    // succeeds) needs that.
    const Track* textTrack = nullptr;
    for (const Track& candidate : project.tracks) {
        if (candidate.kind == TrackKind::Text) {
            textTrack = &candidate;
            break;
        }
    }
    RC_ASSERT(textTrack != nullptr);

    if (name == kAddTextClip) {
        args.set("trackId", Json(textTrack->id.toString()));
        args.set("timelineStartNs", Json(projectEndNs(project) +
                                         Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("durationNs", Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        args.set("content", Json(drawAsciiText(1 + drawIndex(24))));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetTextContent || name == kSetTextStyle) {
        const Clip& textClip = textTrack->clips.front();
        args.set("clipId", Json(textClip.id.toString()));
        if (name == kSetTextContent) {
            args.set("content", Json(drawAsciiText(1 + drawIndex(24))));
        } else {
            args.set("pointSize", Json(static_cast<double>(1 + drawIndex(200))));
        }
        return Invocation{name, std::move(args)};
    }

    // Captions (usable-editor task 13; Requirement 10.2). The seed project's
    // guaranteed caption track/cue (see drawSeedProject()) is the stable
    // target, for the identical reason the text track/clip above is: these
    // four need a REAL caption track/cue, which a random schema-valid uuid
    // from the generic fallback would essentially never draw.
    // timeline.transcribe_to_captions is NOT handled here — it is
    // isHookBacked() (see that function's own doc comment): even in a real
    // build no recognizer backend is bundled, so it always reports
    // Unsupported by name regardless of its arguments, exactly like
    // generation.generate/list_models/timeline.export/media.import, and
    // ToolsCallSuccessShape's isHookBacked() branch is what actually checks
    // its result shape.
    const Track* captionTrack = nullptr;
    for (const Track& candidate : project.tracks) {
        if (candidate.kind == TrackKind::Caption) {
            captionTrack = &candidate;
            break;
        }
    }
    RC_ASSERT(captionTrack != nullptr);

    if (name == kAddCaptionCue) {
        args.set("trackId", Json(captionTrack->id.toString()));
        args.set("timelineStartNs", Json(projectEndNs(project) +
                                         Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("durationNs", Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        args.set("text", Json(drawAsciiText(1 + drawIndex(24))));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetCaptionText || name == kRetimeCaptionCue || name == kRemoveCaptionCue) {
        const Clip& captionCue = captionTrack->clips.front();
        args.set("clipId", Json(captionCue.id.toString()));
        if (name == kSetCaptionText) {
            args.set("text", Json(drawAsciiText(1 + drawIndex(24))));
        } else if (name == kRetimeCaptionCue) {
            // At least one of timelineStartNs/durationNs is required; supply
            // durationNs only, mirroring setTextStyle's own "supply one field"
            // pattern above.
            args.set("durationNs",
                     Json(Duration::fromMilliseconds(kClipLengthMs / 2).nanoseconds()));
        }
        // kRemoveCaptionCue needs only clipId, already set above.
        return Invocation{name, std::move(args)};
    }

    // A tool added later without a case here still exercises the property through
    // its own schema.
    return Invocation{name, schemaValidArgs(tool, scratch)};
}

}  // namespace testsupport_mcpprotocolpropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT2_HPP_
