// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpToolExecutor.cpp — the MCP tool-execution policy (task 15.3).
//
// See McpToolExecutor.hpp for the behavioural contract (Requirements 7.4-7.7,
// 7.10). The order of checks in executeTool mirrors the requirement structure:
// recognize the tool (7.5) -> a project must be open (7.10) -> validate inputs
// against the schema before any command is created (7.10) -> run the tool under
// the time budget, rolling back on failure (7.6) or timeout (7.7), otherwise
// returning the result (7.4).

#include "services/McpToolExecutor.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include "core/ChangeSet.hpp"
#include "core/CommandResult.hpp"
#include "core/Error.hpp"
#include "core/Subscription.hpp"
#include "core/TimelineEngine.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {

namespace {

// True iff the JSON value `v` satisfies a JSON-Schema primitive `type` name.
// "number" accepts any numeric; "integer" accepts an integer payload or a double
// with an exact integral value (JSON has no separate integer literal, so 5.0 is a
// legitimate integer). An unrecognized type constraint is treated permissively.
bool matchesJsonType(const Json& v, std::string_view type) {
    if (type == "object")  return v.isObject();
    if (type == "array")   return v.isArray();
    if (type == "string")  return v.isString();
    if (type == "boolean") return v.isBool();
    if (type == "null")    return v.isNull();
    if (type == "number")  return v.isNumber();
    if (type == "integer") {
        if (v.isInt()) return true;
        if (v.isDouble()) {
            const double d = v.asDouble();
            return std::isfinite(d) && d == std::floor(d);
        }
        return false;
    }
    return true;  // unknown constraint -> do not reject
}

}  // namespace

McpToolExecutor::McpToolExecutor(const ToolRegistry& registry, TimelineEngine* engine)
    : McpToolExecutor(registry, engine, Options{}) {}

McpToolExecutor::McpToolExecutor(const ToolRegistry& registry, TimelineEngine* engine,
                                 Options options)
    : registry_(&registry),
      engine_(engine),
      budget_(options.timeBudget),
      clock_(options.clock ? std::move(options.clock)
                           : Clock([] { return std::chrono::steady_clock::now(); })) {}

Result<void> McpToolExecutor::validateAgainstSchema(const Json& input, const Json& schema) {
    // A non-object schema declares no constraints we understand -> accept.
    if (!schema.isObject()) {
        return ok();
    }

    // Root type: an "object" schema requires an object input.
    if (const Json* type = schema.find("type");
        type != nullptr && type->isString() && type->asString() == "object") {
        if (!input.isObject()) {
            return err(invalidArgument("tool input must be a JSON object"));
        }
    }

    // Required members must all be present.
    if (const Json* required = schema.find("required");
        required != nullptr && required->isArray()) {
        for (const Json& entry : required->asArray()) {
            if (!entry.isString()) {
                continue;
            }
            if (!input.contains(entry.asString())) {
                return err(invalidArgument("missing required field '" + entry.asString() +
                                           "'"));
            }
        }
    }

    // Declared property types must match for any supplied property.
    if (const Json* props = schema.find("properties");
        props != nullptr && props->isObject()) {
        for (const auto& [propName, propSchema] : props->asObject()) {
            const Json* value = input.find(propName);
            if (value == nullptr || !propSchema.isObject()) {
                continue;  // absence is covered by "required"; nothing to check
            }
            const Json* propType = propSchema.find("type");
            if (propType == nullptr || !propType->isString()) {
                continue;
            }
            if (!matchesJsonType(*value, propType->asString())) {
                return err(invalidArgument("field '" + propName + "' must be of type " +
                                           propType->asString()));
            }
        }
    }

    return ok();
}

void McpToolExecutor::rollback(int appliedCount) {
    if (engine_ == nullptr) {
        return;
    }
    // Undo exactly the commands this invocation applied, restoring the
    // pre-invocation project state and removing them from the undo history.
    for (int i = 0; i < appliedCount; ++i) {
        const CommandResult result = engine_->undo();
        if (!result.changed()) {
            break;  // nothing left to undo (defensive)
        }
    }
}

Result<Json> McpToolExecutor::executeTool(std::string_view name, const Json& input) {
    // 7.5 — an unrecognized tool name leaves the project unchanged and reports an
    // unknown-tool error. Checked first: the tool surface exists independently of
    // whether a project is open, and no mutation or command creation happens here.
    const Tool* tool = registry_->find(name);
    if (tool == nullptr) {
        return err<Json>(notFound("unknown tool '" + std::string(name) + "'"));
    }

    // 7.10 — a project operation while no project is open returns a no-project
    // error. The registry (and its handlers) are never invoked in this state.
    if (engine_ == nullptr) {
        return err<Json>(failedPrecondition(
            "no project is open: cannot execute tool '" + std::string(name) + "'"));
    }

    // 7.10 — validate inputs against the tool's declared schema BEFORE any
    // EditCommand is created. A rejection leaves the project untouched.
    if (Result<void> validation = validateAgainstSchema(input, tool->inputSchema);
        validation.isError()) {
        return err<Json>(std::move(validation).error());
    }

    // Observe how many commands this invocation applies, so an abort can undo
    // exactly those and restore the pre-invocation state.
    int appliedCount = 0;
    Subscription observing = engine_->observe([&appliedCount](const ChangeSet& change) {
        if (change.origin == ChangeOrigin::Apply) {
            ++appliedCount;
        }
    });

    // Run the tool, measuring elapsed time on the (injectable) monotonic clock.
    const std::chrono::steady_clock::time_point start = clock_();
    Result<Json> result = registry_->invoke(name, input);
    const std::chrono::steady_clock::time_point finish = clock_();

    // Stop observing before any rollback so undo() emissions are not counted.
    observing.reset();

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);

    // 7.7 — overran the time budget: abort, roll back, return a timeout error.
    // Checked before the success/failure branches so a result that arrived too
    // late is discarded rather than returned.
    if (elapsed > budget_) {
        rollback(appliedCount);
        return err<Json>(makeError(
            ErrorCode::Timeout,
            "tool '" + std::string(name) + "' execution timed out after " +
                std::to_string(elapsed.count()) + " ms (budget " +
                std::to_string(budget_.count()) + " ms)"));
    }

    // 7.6 — the tool's execution failed: roll back any commands it applied and
    // surface the failure verbatim.
    if (result.isError()) {
        rollback(appliedCount);
        return result;
    }

    // 7.4 — success within budget: return the tool's payload.
    return result;
}

Json McpToolExecutor::execute(const Json& request) {
    // Accept the tool name/arguments either at the top level or nested under
    // "params" (JSON-RPC-style MCP tools/call). Prefer the nested form when both
    // a "params" object and a top-level field are present.
    const Json* source = &request;
    if (const Json* params = request.find("params");
        params != nullptr && params->isObject()) {
        source = params;
    }

    std::string name = source->stringOr("name");
    if (name.empty() && source != &request) {
        name = request.stringOr("name");
    }

    Json out = Json::object();
    if (name.empty()) {
        Json error = Json::object();
        error.set("code", std::string(toStringView(ErrorCode::InvalidArgument)));
        error.set("message", "request is missing the tool 'name'");
        out.set("ok", false);
        out.set("error", std::move(error));
        return out;
    }

    Json arguments = Json::object();
    if (const Json* args = source->find("arguments"); args != nullptr) {
        arguments = *args;
    } else if (const Json* topArgs = request.find("arguments"); topArgs != nullptr) {
        arguments = *topArgs;
    }

    Result<Json> result = executeTool(name, arguments);
    if (result.isError()) {
        Json error = Json::object();
        error.set("code", std::string(toStringView(result.error().code())));
        error.set("message", result.error().message());
        out.set("ok", false);
        out.set("error", std::move(error));
        return out;
    }

    out.set("ok", true);
    out.set("result", std::move(result).value());
    return out;
}

}  // namespace palmier::services
