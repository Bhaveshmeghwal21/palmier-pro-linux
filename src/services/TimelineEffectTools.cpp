// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TimelineEffectTools.cpp — the effect, tone-curve and transition tools.
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
    t.schema.arg(uuidArg("clipId", true, "UUID of the target clip."))
        .arg(ArgSpec{.name = "type",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Effect type: brightness, contrast, blur, "
                                    "crop_transform, color_grade, invert_colors, "
                                    "tone_curve, custom.",
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
                            std::make_unique<AddEffectCommand>(clipId.value(), std::move(effect)),
                            std::move(out));
    };
    return t;
}

// ---------------------------------------------------------------------------
// Effect lifecycle management (task 9.2; Requirement 6). These three sit right
// after the append that AddEffectCommand publishes, covering the rest of a
// clip's effect chain: remove one by id, reorder the chain, and change an
// existing effect's parameter. All three refuse (leaving the project untouched)
// when the clip or the named effect is not found (Requirement 6.5).
// ---------------------------------------------------------------------------

Tool makeRemoveEffectTool(ProjectSession* session) {
    Tool t;
    t.name = kRemoveEffect;
    t.description = "Remove one effect from a clip's effect chain by id.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip carrying the effect."))
        .arg(uuidArg("effectId", true, "UUID of the effect to remove."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRemoveEffect));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<Uuid> effectId = requireUuid(in, "effectId");
        if (effectId.isError()) return err<Json>(std::move(effectId).error());

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        out.set("effectId", effectId.value().toString());
        return applyCommand(engine,
                            std::make_unique<RemoveEffectCommand>(clipId.value(), effectId.value()),
                            std::move(out));
    };
    return t;
}

Tool makeReorderEffectsTool(ProjectSession* session) {
    Tool t;
    t.name = kReorderEffects;
    t.description = "Reorder a clip's effect chain (preserves effect count).";
    // NOT expressible: the vocabulary constrains an array's item count but not its
    // item shape, so "every entry is a canonical UUID string" is checked in the
    // handler, and "the entries are a permutation of the clip's effects" depends
    // on project state and stays with ReorderEffectsCommand.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip whose effect chain is reordered."))
        .arg(ArgSpec{.name = "order",
                     .kind = JsonKind::Array,
                     .required = true,
                     .description = "Effect UUIDs, a permutation of the clip's current effects."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kReorderEffects));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        const Json* order = in.find("order");
        if (order == nullptr || !order->isArray()) {
            return err<Json>(invalidArgument("missing or non-array field 'order'"));
        }
        std::vector<Uuid> newOrder;
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
        out.set("clipId", clipId.value().toString());
        return applyCommand(
            engine, std::make_unique<ReorderEffectsCommand>(clipId.value(), std::move(newOrder)),
            std::move(out));
    };
    return t;
}

Tool makeSetEffectParameterTool(ProjectSession* session) {
    Tool t;
    t.name = kSetEffectParameter;
    t.description = "Set (or insert) a named numeric parameter on one of a clip's effects.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip carrying the effect."))
        .arg(uuidArg("effectId", true, "UUID of the effect to change."))
        .arg(ArgSpec{.name = "parameter",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Name of the parameter to set."})
        .arg(ArgSpec{.name = "value",
                     .kind = JsonKind::Number,
                     .required = true,
                     .description = "New numeric value for the parameter."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetEffectParameter));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<Uuid> effectId = requireUuid(in, "effectId");
        if (effectId.isError()) return err<Json>(std::move(effectId).error());
        Result<std::string> parameter = requireString(in, "parameter");
        if (parameter.isError()) return err<Json>(std::move(parameter).error());
        Result<double> value = requireNumber(in, "value");
        if (value.isError()) return err<Json>(std::move(value).error());

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        out.set("effectId", effectId.value().toString());
        return applyCommand(engine,
                            std::make_unique<SetEffectParameterCommand>(
                                clipId.value(), effectId.value(), parameter.value(), value.value()),
                            std::move(out));
    };
    return t;
}

Tool makeSetEffectResourceTool(ProjectSession* session) {
    Tool t;
    t.name = kSetEffectResource;
    t.description = "Point one of a clip's effects at an external resource file, such as a "
                    "`.cube` LUT, or clear it with an empty path.";
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip carrying the effect."))
        .arg(uuidArg("effectId", true, "UUID of the effect to change."))
        .arg(ArgSpec{.name = "path",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Filesystem path to the resource, or \"\" to clear it."});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kSetEffectResource));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<Uuid> effectId = requireUuid(in, "effectId");
        if (effectId.isError()) return err<Json>(std::move(effectId).error());
        Result<std::string> path = requireString(in, "path");
        if (path.isError()) return err<Json>(std::move(path).error());

        // The path is NOT checked for existence. Requirement 7.8 requires a missing LUT to
        // leave the effect in the chain and render un-graded, so refusing an unreadable path
        // here would make a project that opens on one machine un-editable on another -- and
        // an agent setting up a project before its assets arrive is an ordinary workflow.
        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        out.set("effectId", effectId.value().toString());
        out.set("path", path.value());
        return applyCommand(engine,
                            std::make_unique<SetEffectResourceCommand>(
                                clipId.value(), effectId.value(), path.value()),
                            std::move(out));
    };
    return t;
}

Tool makeEditCurvePointTool(ProjectSession* session) {
    Tool t;
    t.name = kEditCurvePoint;
    t.description = "Add, move or remove one control point on a tone-curve effect's "
                    "master, red, green or blue curve, as a single undoable edit.";
    // Two handler-private rules are declared here as far as the vocabulary allows and
    // documented in the schema/handler conformance property as Class 1 (cross-field)
    // beyond that: which of index/x/y are required depends on `operation`'s VALUE,
    // which no per-argument constraint can express.
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip carrying the effect."))
        .arg(uuidArg("effectId", true, "UUID of the tone-curve effect to change."))
        .arg(ArgSpec{.name = "channel",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Curve to edit: master, red, green, blue.",
                     .enumValues = curveChannelValues()})
        .arg(ArgSpec{.name = "operation",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "add (append a point), move (replace point `index`), "
                                    "remove (delete point `index`).",
                     .enumValues = curvePointOperationValues()})
        .arg(intArg("index", false,
                    "Zero-based point index. Required for move and remove; ignored by add, "
                    "which always appends.",
                    0))
        .arg(ArgSpec{.name = "x",
                     .kind = JsonKind::Number,
                     .required = false,
                     .description = "Input coordinate in [0,1]. Required for add and move.",
                     .minNum = 0.0,
                     .maxNum = 1.0})
        .arg(ArgSpec{.name = "y",
                     .kind = JsonKind::Number,
                     .required = false,
                     .description = "Output coordinate in [0,1]. Required for add and move.",
                     .minNum = 0.0,
                     .maxNum = 1.0});
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kEditCurvePoint));
        TimelineEngine& engine = session->engine();
        Result<Uuid> clipId = requireUuid(in, "clipId");
        if (clipId.isError()) return err<Json>(std::move(clipId).error());
        Result<Uuid> effectId = requireUuid(in, "effectId");
        if (effectId.isError()) return err<Json>(std::move(effectId).error());
        Result<std::string> channelStr = requireString(in, "channel");
        if (channelStr.isError()) return err<Json>(std::move(channelStr).error());
        const std::optional<CurveChannel> channel = parseCurveChannel(channelStr.value());
        if (!channel) {
            return err<Json>(invalidArgument("channel must be one of master, red, green, blue"));
        }
        Result<std::string> operationStr = requireString(in, "operation");
        if (operationStr.isError()) return err<Json>(std::move(operationStr).error());
        const std::optional<EditCurvePointCommand::Operation> operation =
            parseCurvePointOperation(operationStr.value());
        if (!operation) {
            return err<Json>(invalidArgument("operation must be one of add, move, remove"));
        }

        // Which coordinates matter depends on the operation, so each is required only
        // where it means something. Defaulting a missing index to 0 would silently move
        // or delete the wrong point, which is exactly the kind of plausible wrong answer
        // that is worse than a rejection.
        const bool needsIndex = *operation != EditCurvePointCommand::Operation::Add;
        const bool needsPoint = *operation != EditCurvePointCommand::Operation::Remove;
        std::int64_t index = 0;
        if (needsIndex) {
            Result<std::int64_t> parsed = requireInt(in, "index");
            if (parsed.isError()) return err<Json>(std::move(parsed).error());
            index = parsed.value();
        }
        double x = 0.0;
        double y = 0.0;
        if (needsPoint) {
            Result<double> parsedX = requireNumber(in, "x");
            if (parsedX.isError()) return err<Json>(std::move(parsedX).error());
            Result<double> parsedY = requireNumber(in, "y");
            if (parsedY.isError()) return err<Json>(std::move(parsedY).error());
            x = parsedX.value();
            y = parsedY.value();
        }

        Json out = Json::object();
        out.set("clipId", clipId.value().toString());
        out.set("effectId", effectId.value().toString());
        out.set("channel", channelStr.value());
        out.set("operation", operationStr.value());
        return applyCommand(engine,
                            std::make_unique<EditCurvePointCommand>(
                                clipId.value(), effectId.value(), *channel, *operation,
                                static_cast<std::size_t>(index), x, y),
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
    t.schema.arg(uuidArg("clipId", true, "UUID of the clip whose incoming transition is set."))
        .arg(ArgSpec{.name = "kind",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Transition kind: crossfade, dip_to_color, wipe, slide, fade.",
                     .enumValues = transitionKindValues()})
        .arg(intArg("durationNs", true, "Transition region length in nanoseconds (>= 0).", 0));
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
        return applyCommand(
            engine, std::make_unique<SetTransitionCommand>(clipId.value(), std::move(transition)),
            std::move(out));
    };
    return t;
}

}  // namespace palmier::services::detail
