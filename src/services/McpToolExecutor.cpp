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
#include <string>
#include <utility>

#include "core/ChangeSet.hpp"
#include "core/CommandResult.hpp"
#include "core/Error.hpp"
#include "core/Subscription.hpp"
#include "core/TimelineEngine.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {

std::string_view invocationSourceName(InvocationSource source) noexcept {
    switch (source) {
        case InvocationSource::Gui:   return "gui";
        case InvocationSource::Mcp:   return "mcp";
        case InvocationSource::Agent: return "agent";
    }
    return "mcp";
}

McpToolExecutor::McpToolExecutor(const ToolRegistry& registry, ProjectSession* session)
    : McpToolExecutor(registry, session, Options{}) {}

McpToolExecutor::McpToolExecutor(const ToolRegistry& registry, ProjectSession* session,
                                 Options options)
    : registry_(&registry),
      session_(session),
      budget_(options.timeBudget),
      clock_(options.clock ? std::move(options.clock)
                           : Clock([] { return std::chrono::steady_clock::now(); })),
      invocationLog_(std::move(options.invocationLog)) {}

Result<void> McpToolExecutor::validateAgainstSchema(const Json& input,
                                                    const ToolSchema& schema) {
    // One declaration, one validator: the constraints enforced here are exactly
    // the ones `toJsonSchema()` publishes (design.md D3; Requirements 9.9, 9.12).
    return schema.validate(input);
}

void McpToolExecutor::rollback(int appliedCount) {
    if (session_ == nullptr) {
        return;
    }
    // Undo exactly the commands this invocation applied, restoring the
    // pre-invocation project state and removing them from the undo history.
    for (int i = 0; i < appliedCount; ++i) {
        const CommandResult result = session_->engine().undo();
        if (!result.changed()) {
            break;  // nothing left to undo (defensive)
        }
    }
}

Result<Json> McpToolExecutor::executeTool(std::string_view name, const Json& input,
                                          InvocationSource source) {
    // Logging only (task 3.4): `source` names the surface that issued the call and
    // never influences any decision below.
    const auto record = [this, name, source](bool succeeded,
                                             std::chrono::milliseconds elapsed) {
        if (invocationLog_) {
            invocationLog_(source, name, succeeded, elapsed);
        }
    };

    // 7.5 — an unrecognized tool name leaves the project unchanged and reports an
    // unknown-tool error. Checked first: the tool surface exists independently of
    // whether a project is open, and no mutation or command creation happens here.
    const Tool* tool = registry_->find(name);
    if (tool == nullptr) {
        record(false, std::chrono::milliseconds::zero());
        return err<Json>(notFound("unknown tool '" + std::string(name) + "'"));
    }

    // 7.10 — a project operation while no project is open returns a no-project
    // error. The registry (and its handlers) are never invoked in this state.
    if (session_ == nullptr) {
        record(false, std::chrono::milliseconds::zero());
        return err<Json>(failedPrecondition(
            "no project is open: cannot execute tool '" + std::string(name) + "'"));
    }

    // 7.10 — validate inputs against the tool's declared schema BEFORE any
    // EditCommand is created. A rejection leaves the project untouched.
    if (Result<void> validation = validateAgainstSchema(input, tool->schema);
        validation.isError()) {
        record(false, std::chrono::milliseconds::zero());
        return err<Json>(std::move(validation).error());
    }

    // The engine is resolved HERE, per invocation, exactly as the registry's
    // handlers resolve it (design.md D1), so both observe the same object even
    // when the session loaded its project after this executor was constructed.
    TimelineEngine& engine = session_->engine();

    // Observe how many commands this invocation applies, so an abort can undo
    // exactly those and restore the pre-invocation state.
    int appliedCount = 0;
    Subscription observing = engine.observe([&appliedCount](const ChangeSet& change) {
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
        record(false, elapsed);
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
        record(false, elapsed);
        return result;
    }

    // 7.4 — success within budget: return the tool's payload.
    record(true, elapsed);
    return result;
}

Json McpToolExecutor::execute(const Json& request, InvocationSource source) {
    // Accept the tool name/arguments either at the top level or nested under
    // "params" (JSON-RPC-style MCP tools/call). Prefer the nested form when both
    // a "params" object and a top-level field are present.
    const Json* fields = &request;
    if (const Json* params = request.find("params");
        params != nullptr && params->isObject()) {
        fields = params;
    }

    std::string name = fields->stringOr("name");
    if (name.empty() && fields != &request) {
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
    if (const Json* args = fields->find("arguments"); args != nullptr) {
        arguments = *args;
    } else if (const Json* topArgs = request.find("arguments"); topArgs != nullptr) {
        arguments = *topArgs;
    }

    Result<Json> result = executeTool(name, arguments, source);
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
