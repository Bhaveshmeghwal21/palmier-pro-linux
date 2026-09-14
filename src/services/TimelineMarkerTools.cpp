// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TimelineMarkerTools.cpp — the timeline marker tools (upstream parity: the
// `timeline` tool category's marker gap).
//
// Two tools, following AddTrackTool/RemoveTrackTool's own shape exactly: an add that
// names its command's result field and a remove that takes only the target id. Listing
// is not a third tool — a project's markers are part of `timeline.read`'s existing
// result, the same way its tracks and clips already are, so there is nothing to add a
// separate read path for.

#include "services/ToolRegistryInternal.hpp"

namespace palmier::services::detail {

Tool makeAddMarkerTool(ProjectSession* session) {
    Tool t;
    t.name = kAddMarker;
    t.description = "Add a named marker at a position on the shared timeline.";
    t.schema.arg(intArg("timelineNs", true, "Timeline position in nanoseconds (>= 0)."))
        .arg(stringArg("label", false, "Human-readable annotation (default \"\")."))
        .arg(uuidArg("markerId", false, "Optional explicit marker UUID; generated when omitted."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kAddMarker));
        TimelineEngine& engine = session->engine();
        Result<std::int64_t> timelineNs = requireInt(in, "timelineNs");
        if (timelineNs.isError()) return err<Json>(std::move(timelineNs).error());
        if (timelineNs.value() < 0) {
            return err<Json>(invalidArgument("timelineNs must be >= 0"));
        }
        const std::string label = in.stringOr("label");

        std::unique_ptr<AddMarkerCommand> command;
        if (const Json* mid = in.find("markerId"); mid != nullptr && mid->isString()) {
            std::optional<Uuid> parsed = Uuid::parse(mid->asString());
            if (!parsed) return err<Json>(invalidArgument("field 'markerId' is not a valid UUID"));
            command = std::make_unique<AddMarkerCommand>(
                *parsed, Duration::fromNanoseconds(timelineNs.value()), label);
        } else {
            command = std::make_unique<AddMarkerCommand>(
                Duration::fromNanoseconds(timelineNs.value()), label);
        }
        const Uuid markerId = command->markerId();

        Json out = Json::object();
        out.set("markerId", markerId.toString());
        return applyCommand(engine, std::move(command), std::move(out));
    };
    return t;
}

Tool makeRemoveMarkerTool(ProjectSession* session) {
    Tool t;
    t.name = kRemoveMarker;
    t.description = "Remove one timeline marker by id.";
    t.schema.arg(uuidArg("markerId", true, "UUID of the marker to remove."));
    t.handler = [session](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(kRemoveMarker));
        TimelineEngine& engine = session->engine();
        Result<Uuid> markerId = requireUuid(in, "markerId");
        if (markerId.isError()) return err<Json>(std::move(markerId).error());

        Json out = Json::object();
        out.set("markerId", markerId.value().toString());
        return applyCommand(engine, std::make_unique<RemoveMarkerCommand>(markerId.value()),
                            std::move(out));
    };
    return t;
}

}  // namespace palmier::services::detail
