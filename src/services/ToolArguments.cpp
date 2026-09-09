// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolArguments.cpp — extracting and validating tool arguments, the ArgSpec shapes that
// recur across the surface, the shared no-project-open refusal, and the
// atomic apply every structural-edit handler funnels through.
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
    return failedPrecondition("no project is open: cannot execute tool '" + std::string(tool) +
                              "'");
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
        double value;
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

// ---------------------------------------------------------------------------
// Argument parsing helpers — each yields a descriptive InvalidArgument on failure
// so the executor (task 15.3) can validate inputs before creating a command.
// ---------------------------------------------------------------------------

Result<Uuid> requireUuid(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isString()) {
        return err<Uuid>(
            invalidArgument(std::string("missing or non-string field '") + std::string(key) + "'"));
    }
    std::optional<Uuid> parsed = Uuid::parse(m->asString());
    if (!parsed) {
        return err<Uuid>(
            invalidArgument(std::string("field '") + std::string(key) + "' is not a valid UUID"));
    }
    return *parsed;
}

Result<std::int64_t> requireInt(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isNumber()) {
        return err<std::int64_t>(invalidArgument(std::string("missing or non-numeric field '") +
                                                 std::string(key) + "'"));
    }
    return m->isInt() ? m->asInt() : static_cast<std::int64_t>(m->asDouble());
}

Result<double> requireNumber(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isNumber()) {
        return err<double>(invalidArgument(std::string("missing or non-numeric field '") +
                                           std::string(key) + "'"));
    }
    return m->isInt() ? static_cast<double>(m->asInt()) : m->asDouble();
}

Result<std::string> requireString(const Json& in, std::string_view key) {
    const Json* m = in.find(key);
    if (m == nullptr || !m->isString()) {
        return err<std::string>(
            invalidArgument(std::string("missing or non-string field '") + std::string(key) + "'"));
    }
    return m->asString();
}

// ---------------------------------------------------------------------------
// Apply a command and translate the CommandResult into a Result<Json>.
//   * Failed -> the engine's Error, forwarded verbatim (the executor rolls back).
//   * NoOp   -> success payload annotated with the engine's indication.
//   * Applied-> success payload.
// ---------------------------------------------------------------------------

Result<Json> applyCommand(TimelineEngine& engine, std::unique_ptr<EditCommand> cmd, Json success) {
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
               std::optional<std::int64_t> minimum) {
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

}  // namespace palmier::services::detail
