// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TimelineClipTools.cpp — the timeline read tool and the clip structural edits: add, delete,
// move, trim, split, reorder, and the ripple variants.
//
// Split out of services/ToolRegistry.cpp, which held the entire tool surface in
// one 2767-line translation unit. The internal contract these units share is
// services/ToolRegistryInternal.hpp; the public one is unchanged and remains
// services/ToolRegistry.hpp. Every definition below was moved rather than
// rewritten, so the surface this file contributes is byte-identical to before.
//
// One include, because ToolRegistryInternal.hpp states the surface's shared
// dependency set once for all of these files.

#include "services/ToolRegistryInternal.hpp"

namespace palmier::services::detail {

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
    t.schema.arg(uuidArg("trackId", true, "UUID of the target track."))
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
        return applyCommand(engine,
                            std::make_unique<AddClipCommand>(trackId.value(), std::move(clip)),
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
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to move."))
        .arg(intArg("timelineStartNs", true, "New timeline start position in nanoseconds.", 0));
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

Tool makeSetClipPropertiesTool(ProjectSession* session) {
    Tool t;
    t.name = kSetClipProperties;
    t.description = "Change a clip's opacity or audio gain after it has been created.";
    // Both fields are optional and an absent field leaves the clip's value alone, so a caller
    // can change one property without knowing the other. The bounds match Clip's invariants
    // (ProjectValidation: opacity within [0,1], gain >= 0) so an out-of-range value is refused
    // by the schema with the range in the message, rather than by validation after the edit has
    // been built.
    //
    // A call naming neither property is an applied no-op, not a refusal: "at least one of
    // these optional arguments" is a relation between arguments that the schema cannot state,
    // and the suite asserts that whatever the advertised schema accepts the handler accepts.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to change."))
        .arg(ArgSpec{.name = "opacity",
                     .kind = JsonKind::Number,
                     .description = "Video opacity in [0,1]. Absent leaves it unchanged.",
                     .minNum = 0.0,
                     .maxNum = 1.0})
        .arg(ArgSpec{.name = "gain",
                     .kind = JsonKind::Number,
                     .description = "Audio gain, unity = 1.0. Absent leaves it unchanged.",
                     .minNum = 0.0})
        .arg(ArgSpec{.name = "centerX",
                     .kind = JsonKind::Number,
                     .description = "Picture centre across the canvas; 0.5 is the middle.",
                     .minNum = -10.0,
                     .maxNum = 10.0})
        .arg(ArgSpec{.name = "centerY",
                     .kind = JsonKind::Number,
                     .description = "Picture centre down the canvas; 0.5 is the middle.",
                     .minNum = -10.0,
                     .maxNum = 10.0})
        .arg(ArgSpec{.name = "scale",
                     .kind = JsonKind::Number,
                     .description = "Multiplies the aspect-preserving fitted size; 1.0 fills.",
                     .minNum = 0.001,
                     .maxNum = 100.0})
        .arg(ArgSpec{.name = "rotationDegrees",
                     .kind = JsonKind::Number,
                     .description = "Clockwise rotation about the picture's own centre.",
                     .minNum = -360.0,
                     .maxNum = 360.0})
        .arg(ArgSpec{.name = "flipHorizontal",
                     .kind = JsonKind::Bool,
                     .description = "Mirror the picture left to right."})
        .arg(ArgSpec{.name = "flipVertical",
                     .kind = JsonKind::Bool,
                     .description = "Mirror the picture top to bottom."})
        .arg(intArg("fadeInNs", false,
                    "Ramp up over this long at the clip's leading edge, in nanoseconds.", 0))
        .arg(intArg("fadeOutNs", false,
                    "Ramp down over this long at the clip's trailing edge, in nanoseconds.", 0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetClipProperties));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());

        SetClipPropertiesCommand::Properties props;
        if (in.find("opacity") != nullptr) props.opacity = in.doubleOr("opacity", 1.0);
        if (in.find("gain") != nullptr) props.gain = in.doubleOr("gain", 1.0);
        if (in.find("centerX") != nullptr) props.centerX = in.doubleOr("centerX", 0.5);
        if (in.find("centerY") != nullptr) props.centerY = in.doubleOr("centerY", 0.5);
        if (in.find("scale") != nullptr) props.scale = in.doubleOr("scale", 1.0);
        if (in.find("rotationDegrees") != nullptr) {
            props.rotationDegrees = in.doubleOr("rotationDegrees", 0.0);
        }
        if (in.find("flipHorizontal") != nullptr) {
            props.flipHorizontal = in.boolOr("flipHorizontal", false);
        }
        if (in.find("flipVertical") != nullptr) {
            props.flipVertical = in.boolOr("flipVertical", false);
        }
        if (in.find("fadeInNs") != nullptr) {
            props.fadeIn = Duration::fromNanoseconds(in.intOr("fadeInNs", 0));
        }
        if (in.find("fadeOutNs") != nullptr) {
            props.fadeOut = Duration::fromNanoseconds(in.intOr("fadeOutNs", 0));
        }

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        if (props.opacity.has_value()) out.set("opacity", *props.opacity);
        if (props.gain.has_value()) out.set("gain", *props.gain);
        return applyCommand(engine,
                            std::make_unique<SetClipPropertiesCommand>(clipId.value(), props),
                            std::move(out));
    };
    return t;
}

Tool makeApplyLayoutTool(ProjectSession* session) {
    Tool t;
    t.name = kApplyLayout;
    t.description = "Arrange several clips on the canvas in a named layout.";
    // `layout` is a closed value set so it is declared as one. That the array holds UUID strings
    // is checked by the handler, and that each names a real clip by the command — which resolves
    // every id before moving anything, so a bad id refuses the whole arrangement.
    t.schema
        .arg(ArgSpec{.name = "clipIds",
                     .kind = JsonKind::Array,
                     .required = true,
                     .description = "UUIDs of the clips to arrange, in the order they should "
                                    "be placed.",
                     .minLength = 1})
        .arg(ArgSpec{.name = "layout",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "The arrangement to apply.",
                     .enumValues = layoutKindValues()});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kApplyLayout));
        TimelineEngine& engine = session->engine();

        const Json* ids = in.find("clipIds");
        if (ids == nullptr || !ids->isArray()) {
            return err<Json>(invalidArgument("missing or non-array field 'clipIds'"));
        }
        // No empty check here: the schema declares minItems 1 and the executor validates before
        // dispatching, so an empty array never reaches this handler.
        std::vector<Uuid> clipIds;
        clipIds.reserve(ids->asArray().size());
        for (const Json& entry : ids->asArray()) {
            if (!entry.isString()) {
                return err<Json>(invalidArgument("field 'clipIds' must contain UUID strings"));
            }
            std::optional<Uuid> parsed = Uuid::parse(entry.asString());
            if (!parsed) {
                return err<Json>(invalidArgument("field 'clipIds' contains an invalid UUID"));
            }
            clipIds.push_back(*parsed);
        }

        const std::optional<LayoutKind> layout = parseLayoutKind(in.stringOr("layout", ""));
        if (!layout.has_value()) {
            return err<Json>(invalidArgument("field 'layout' does not name a known layout"));
        }

        const std::vector<Transform> transforms = layoutTransforms(*layout, clipIds.size());
        std::vector<SetClipTransformsCommand::Placement> placements;
        placements.reserve(transforms.size());
        for (std::size_t i = 0; i < transforms.size(); ++i) {
            placements.push_back(SetClipTransformsCommand::Placement{clipIds[i], transforms[i]});
        }

        Json out = Json::object();
        out.set("clipCount", static_cast<std::int64_t>(placements.size()));
        return applyCommand(engine,
                            std::make_unique<SetClipTransformsCommand>(std::move(placements)),
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
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to trim."))
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
        if (edgeStr.value() == "start")
            edge = TrimClipCommand::Edge::Start;
        else if (edgeStr.value() == "end")
            edge = TrimClipCommand::Edge::End;
        else
            return err<Json>(invalidArgument("field 'edge' must be 'start' or 'end'"));

        // Defaults are derived from the current project state / frame rate.
        std::optional<Clip> clip = engine.clip(clipId.value());
        if (!clip) {
            return err<Json>(
                notFound("trim_clip: clip " + clipId.value().toString() + " not found"));
        }
        const FrameRate fps = engine.snapshot().timelineFps;
        const Duration sourceDuration =
            in.contains("sourceDurationNs")
                ? Duration::fromNanoseconds(in.intOr("sourceDurationNs", 0))
                : clip->sourceOut;

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine,
                            std::make_unique<TrimClipCommand>(
                                clipId.value(), edge, Duration::fromNanoseconds(boundary.value()),
                                fps, sourceDuration),
                            std::move(out));
    };
    return t;
}

// ---------------------------------------------------------------------------
// Ripple editing and gap management (usable-editor Phase 3 task 8; Requirement 5)
// ---------------------------------------------------------------------------

Tool makeRippleDeleteTool(ProjectSession* session) {
    Tool t;
    t.name = kRippleDelete;
    t.description = "Delete a clip and shift every later clip on the same track earlier by the "
                    "removed clip's duration, closing the gap it leaves.";
    // The only argument is the subject: how far the track closes up is the removed
    // clip's own duration, which is project state rather than a caller's choice.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to ripple-delete."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRippleDelete));
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(session->engine(),
                            std::make_unique<RippleDeleteCommand>(clipId.value()), std::move(out));
    };
    return t;
}

Tool makeRippleTrimTool(ProjectSession* session) {
    Tool t;
    t.name = kRippleTrim;
    t.description = "Trim a clip's edge and shift every later clip on the same track by the "
                    "change in duration, keeping grouped multicam angles synchronised.";
    // Mirrors `timeline.trim_clip`'s declaration, because the two commands share
    // their clamping rules: `edge` is a closed set, `boundaryNs` carries no bound
    // because the command clamps rather than rejects, and `sourceDurationNs`
    // defaults to the clip's out-point.
    //
    // NOT expressible: that a grouped angle can absorb the same trim is a relation
    // between the argument and every other member of the clip's group, which the
    // schema cannot see; `RippleTrimCommand` remains its enforcement point.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to ripple-trim."))
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
        if (session == nullptr) return err<Json>(noProjectOpen(kRippleTrim));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::string> edgeStr = requireString(in, "edge");
        if (edgeStr.isError()) return err<Json>(std::move(edgeStr).error());
        Result<std::int64_t> boundary = requireInt(in, "boundaryNs");
        if (boundary.isError()) return err<Json>(std::move(boundary).error());

        RippleTrimCommand::Edge edge;
        if (edgeStr.value() == "start")
            edge = RippleTrimCommand::Edge::Start;
        else if (edgeStr.value() == "end")
            edge = RippleTrimCommand::Edge::End;
        else
            return err<Json>(invalidArgument("field 'edge' must be 'start' or 'end'"));

        std::optional<Clip> clip = engine.clip(clipId.value());
        if (!clip) {
            return err<Json>(
                notFound("ripple_trim: clip " + clipId.value().toString() + " not found"));
        }
        const FrameRate fps = engine.snapshot().timelineFps;
        const Duration sourceDuration =
            in.contains("sourceDurationNs")
                ? Duration::fromNanoseconds(in.intOr("sourceDurationNs", 0))
                : clip->sourceOut;

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(engine,
                            std::make_unique<RippleTrimCommand>(
                                clipId.value(), edge, Duration::fromNanoseconds(boundary.value()),
                                fps, sourceDuration),
                            std::move(out));
    };
    return t;
}

Tool makeCloseGapTool(ProjectSession* session) {
    Tool t;
    t.name = kCloseGap;
    t.description = "Close the gap immediately following a clip by shifting later clips on the "
                    "same track earlier, leaving every clip's duration unchanged.";
    // NOT expressible: "a gap follows this clip" is a relation between the clip and
    // its successor's position, which the schema cannot see; `CloseGapCommand`
    // refuses when the clip is last on its track or its successor already abuts it.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip whose following gap is closed."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kCloseGap));
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        return applyCommand(session->engine(), std::make_unique<CloseGapCommand>(clipId.value()),
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
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip to split."))
        .arg(intArg("playheadNs", true, "Playhead position in nanoseconds (inside the clip).", 0));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSplitClip));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<std::int64_t> playhead = requireInt(in, "playheadNs");
        if (playhead.isError()) return err<Json>(std::move(playhead).error());

        auto cmd = std::make_unique<SplitClipCommand>(clipId.value(),
                                                      Duration::fromNanoseconds(playhead.value()));
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
    t.schema.arg(uuidArg("trackId", true, "UUID of the track to reorder."))
        .arg(ArgSpec{.name = "order",
                     .kind = JsonKind::Array,
                     .required = true,
                     .description = "Clip UUIDs, a permutation of the track's current clips."});
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
        return applyCommand(
            engine, std::make_unique<ReorderClipsCommand>(trackId.value(), std::move(newOrder)),
            std::move(out));
    };
    return t;
}

}  // namespace palmier::services::detail
