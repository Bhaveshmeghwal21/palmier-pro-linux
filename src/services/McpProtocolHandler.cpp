// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpProtocolHandler.cpp — implementation of JSON-RPC 2.0 dispatch for
// the MCP endpoint (task 5.2). See the header for the contract, the fixed dispatch
// order and the mapping to design.md D3/D5 and Requirements 9.1-9.16.

#include "services/McpProtocolHandler.hpp"

#include <string>
#include <utility>

#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

constexpr std::string_view kMethodInitialize  = "initialize";
constexpr std::string_view kMethodInitialized = "notifications/initialized";
constexpr std::string_view kMethodToolsList   = "tools/list";
constexpr std::string_view kMethodToolsCall   = "tools/call";

/// A JSON-RPC `id` must be a string, a number, or null (Requirement 9.7).
bool isAcceptableId(const Json& id) {
    return id.isString() || id.isInt() || id.isDouble() || id.isNull();
}

/// The `content` + `isError` result shape of Requirements 9.4, 9.5 and 9.16: a
/// `content` array whose first entry is a text entry, plus the error flag.
Json toolResult(std::string text, bool isError) {
    Json entry = Json::object();
    entry.set("type", Json("text"));
    entry.set("text", Json(std::move(text)));

    Json content = Json::array();
    content.push_back(std::move(entry));

    Json result = Json::object();
    result.set("content", std::move(content));
    result.set("isError", Json(isError));
    return result;
}

}  // namespace

MainThreadInvoker inlineMainThreadInvoker() {
    return [](std::function<Result<Json>()> work,
              std::chrono::milliseconds) -> Result<Json> { return work(); };
}

McpProtocolHandler::McpProtocolHandler(const ToolRegistry& registry, McpToolExecutor& executor,
                                       McpSessionRegistry& sessions, MainThreadInvoker invoker)
    : McpProtocolHandler(registry, executor, sessions, std::move(invoker), Options{}) {}

McpProtocolHandler::McpProtocolHandler(const ToolRegistry& registry, McpToolExecutor& executor,
                                       McpSessionRegistry& sessions, MainThreadInvoker invoker,
                                       Options options)
    : registry_(&registry),
      executor_(&executor),
      sessions_(&sessions),
      invoker_(std::move(invoker)),
      options_(std::move(options)) {
    if (!invoker_) invoker_ = inlineMainThreadInvoker();
}

bool McpProtocolHandler::isSupportedProtocolVersion(std::string_view version) noexcept {
    for (const std::string_view supported : kSupportedProtocolVersions) {
        if (supported == version) return true;
    }
    return false;
}

McpReply McpProtocolHandler::successReply(const Json& id, Json result, int httpStatus) {
    Json envelope = Json::object();
    envelope.set("jsonrpc", Json("2.0"));
    envelope.set("id", id);
    envelope.set("result", std::move(result));

    McpReply reply;
    reply.httpStatus = httpStatus;
    reply.body = envelope.dump();
    return reply;
}

McpReply McpProtocolHandler::errorReply(const Json& id, std::int64_t code, std::string message,
                                        int httpStatus) {
    Json error = Json::object();
    error.set("code", Json(code));
    error.set("message", Json(std::move(message)));

    Json envelope = Json::object();
    envelope.set("jsonrpc", Json("2.0"));
    envelope.set("id", id);
    envelope.set("error", std::move(error));

    McpReply reply;
    reply.httpStatus = httpStatus;
    reply.body = envelope.dump();
    return reply;
}

McpReply McpProtocolHandler::handle(const McpRequestContext& context, std::string_view rawBody) {
    const Json nullId(nullptr);

    // --- Stage 1: size cap and JSON parse (Requirement 9.6) ----------------
    const std::size_t declared = context.bodyBytes > 0 ? context.bodyBytes : rawBody.size();
    if (declared > options_.maxBodyBytes || rawBody.size() > options_.maxBodyBytes) {
        return errorReply(nullId, kErrorParse,
                          "Parse error: request body of " + std::to_string(declared) +
                              " bytes exceeds the " + std::to_string(options_.maxBodyBytes) +
                              "-byte limit",
                          400);
    }

    Result<Json> parsed = Json::parse(rawBody);
    if (parsed.isError()) {
        return errorReply(nullId, kErrorParse,
                          "Parse error: " + parsed.error().message(), 400);
    }
    const Json request = std::move(parsed).value();

    // --- Stage 2: JSON-RPC 2.0 envelope shape (Requirement 9.7) -------------
    if (!request.isObject()) {
        return errorReply(nullId, kErrorInvalidRequest,
                          "Invalid Request: the body is not a JSON-RPC request object", 400);
    }

    const Json* jsonrpc = request.find("jsonrpc");
    if (jsonrpc == nullptr || !jsonrpc->isString() || jsonrpc->asString() != "2.0") {
        return errorReply(nullId, kErrorInvalidRequest,
                          "Invalid Request: \"jsonrpc\":\"2.0\" is required", 400);
    }

    const Json* idMember = request.find("id");
    if (idMember != nullptr && !isAcceptableId(*idMember)) {
        return errorReply(nullId, kErrorInvalidRequest,
                          "Invalid Request: \"id\" must be a string, a number or null", 400);
    }
    // A request without an `id` member is a notification; its echoed id is null
    // for the fault replies the requirements still demand (Requirement 9.13).
    const Json id = idMember != nullptr ? *idMember : nullId;

    const Json* method = request.find("method");
    if (method == nullptr || !method->isString()) {
        return errorReply(id, kErrorInvalidRequest,
                          "Invalid Request: a string \"method\" member is required", 400);
    }
    const std::string& methodName = method->asString();

    const Json* params = request.find("params");

    // --- Stage 3: method recognition (Requirement 9.8) ----------------------
    if (methodName == kMethodInitialize) {
        return handleInitialize(context, id, params);
    }
    if (methodName == kMethodInitialized) {
        return handleInitializedNotification(context);
    }
    if (methodName == kMethodToolsList) {
        return handleToolsList(context, id);
    }
    if (methodName == kMethodToolsCall) {
        return handleToolsCall(context, id, params);
    }
    // Requirement 9.8 names an unsupported method explicitly, and it does so for
    // "a request", so an unsupported *notification* is answered the same way
    // rather than silently dropped.
    return errorReply(id, kErrorMethodNotFound,
                      "Method not found: '" + methodName + "' is not supported by this MCP "
                      "endpoint (supported: initialize, notifications/initialized, tools/list, "
                      "tools/call)");
}

McpReply McpProtocolHandler::handleInitialize(const McpRequestContext& context, const Json& id,
                                              const Json* params) {
    // Requirement 9.2: the client's requested version when supported, otherwise
    // the highest version this handler supports.
    std::string negotiated{latestProtocolVersion()};
    if (params != nullptr && params->isObject()) {
        const Json* requested = params->find("protocolVersion");
        if (requested != nullptr && requested->isString() &&
            isSupportedProtocolVersion(requested->asString())) {
            negotiated = requested->asString();
        }
    }

    Result<std::string> minted = sessions_->create(context.sourceAddress, negotiated);
    if (minted.isError()) {
        // Requirement 10.9: only this request is refused; established sessions
        // stay active, and nothing about the project is touched.
        const std::int64_t code = minted.error().code() == ErrorCode::FailedPrecondition
                                      ? kErrorSessionLimit
                                      : kErrorInternal;
        return errorReply(id, code, minted.error().message());
    }

    Json tools = Json::object();
    tools.set("listChanged", Json(false));
    Json capabilities = Json::object();
    capabilities.set("tools", std::move(tools));

    Json serverInfo = Json::object();
    serverInfo.set("name", Json(options_.serverName));
    serverInfo.set("version", Json(options_.serverVersion));

    Json result = Json::object();
    result.set("protocolVersion", Json(negotiated));
    result.set("capabilities", std::move(capabilities));
    result.set("serverInfo", std::move(serverInfo));

    McpReply reply = successReply(id, std::move(result));
    reply.newSessionId = minted.value();  // emitted as Mcp-Session-Id (Req 9.11)
    return reply;
}

McpReply McpProtocolHandler::handleInitializedNotification(const McpRequestContext& context) {
    const Json nullId(nullptr);

    if (!context.sessionId.has_value()) {
        return errorReply(nullId, kErrorSessionUnknown,
                          "no MCP session identifier was presented; the client must repeat "
                          "initialize");
    }

    const Result<McpSessionRecord*> record = sessions_->touch(*context.sessionId);
    if (record.isError()) {
        return errorReply(nullId, kErrorSessionUnknown, record.error().message());
    }
    sessions_->markInitialized(*context.sessionId);

    // Requirement 9.10: accepted, zero-byte body, HTTP 202.
    McpReply reply;
    reply.httpStatus = 202;
    reply.body.clear();
    return reply;
}

std::optional<McpReply> McpProtocolHandler::checkSession(const McpRequestContext& context,
                                                        const Json& id) {
    if (!context.sessionId.has_value()) {
        return errorReply(id, kErrorSessionUnknown,
                          "no MCP session identifier was presented; the client must repeat "
                          "initialize");
    }
    const Result<McpSessionRecord*> record = sessions_->touch(*context.sessionId);
    if (record.isError()) {
        // Requirement 9.15: unrecognised or expired -> repeat initialize.
        return errorReply(id, kErrorSessionUnknown, record.error().message());
    }
    if (!record.value()->initialized) {
        // Requirement 9.14: the session has not completed initialization.
        return errorReply(id, kErrorSessionNotInitialized,
                          "the MCP session is not initialized; send the "
                          "notifications/initialized notification before tools/list or "
                          "tools/call");
    }
    return std::nullopt;
}

McpReply McpProtocolHandler::handleToolsList(const McpRequestContext& context, const Json& id) {
    if (std::optional<McpReply> refusal = checkSession(context, id); refusal.has_value()) {
        return *std::move(refusal);
    }

    // Requirement 9.3: one entry per registered tool, each with name, description
    // and the JSON Schema rendered from that tool's single ToolSchema declaration.
    Json result = Json::object();
    result.set("tools", registry_->describe());
    return successReply(id, std::move(result));
}

McpReply McpProtocolHandler::handleToolsCall(const McpRequestContext& context, const Json& id,
                                             const Json* params) {
    if (std::optional<McpReply> refusal = checkSession(context, id); refusal.has_value()) {
        return *std::move(refusal);
    }

    if (params == nullptr || !params->isObject()) {
        return errorReply(id, kErrorInvalidParams,
                          "Invalid params: tools/call requires a params object carrying the "
                          "tool \"name\"");
    }
    const Json* nameMember = params->find("name");
    if (nameMember == nullptr || !nameMember->isString() || nameMember->asString().empty()) {
        return errorReply(id, kErrorInvalidParams,
                          "Invalid params: tools/call requires a non-empty string \"name\"");
    }
    const std::string toolName = nameMember->asString();

    Json arguments = Json::object();
    if (const Json* argsMember = params->find("arguments"); argsMember != nullptr) {
        if (!argsMember->isNull()) {
            if (!argsMember->isObject()) {
                return errorReply(id, kErrorInvalidParams,
                                  "Invalid params: \"arguments\" for tool '" + toolName +
                                      "' must be an object");
            }
            arguments = *argsMember;
        }
    }

    // Requirement 9.9, first clause: a tool absent from the surface.
    const Tool* tool = registry_->find(toolName);
    if (tool == nullptr) {
        return errorReply(id, kErrorInvalidParams,
                          "Invalid params: tool '" + toolName +
                              "' is not registered in this editor's tool surface");
    }

    // Requirement 9.9, remaining clauses: a missing required argument, a wrong
    // JSON type, or a value outside a declared bound — checked against the very
    // schema `tools/list` advertised, before any edit command can be created.
    if (const Result<void> valid = tool->schema.validate(arguments); valid.isError()) {
        return errorReply(id, kErrorInvalidParams,
                          "Invalid params for tool '" + toolName + "': " +
                              valid.error().message());
    }

    // Requirement 9.4: execute through the one execution policy the GUI and the
    // in-app agent use, marshalled onto the thread that owns the project with the
    // 60-second budget of Requirement 9.16.
    McpToolExecutor* executor = executor_;
    const Json       argumentsCopy = arguments;
    Result<Json>     outcome = invoker_(
        [executor, toolName, argumentsCopy]() -> Result<Json> {
            return executor->executeTool(toolName, argumentsCopy, InvocationSource::Mcp);
        },
        options_.toolBudget);

    if (outcome.isOk()) {
        Json payload = std::move(outcome).value();
        Json result = toolResult(payload.dump(), /*isError=*/false);
        result.set("structuredContent", std::move(payload));
        return successReply(id, std::move(result));
    }

    const Error& failure = outcome.error();

    // Requirement 9.16: the budget was exceeded — a result naming the tool and the
    // limit, not a transport failure. The executor has already restored the
    // pre-invocation project state.
    if (failure.code() == ErrorCode::Timeout) {
        return successReply(
            id, toolResult("tool '" + toolName + "' exceeded the execution time limit of " +
                               std::to_string(options_.toolBudget.count()) +
                               " ms and was abandoned; the project is unchanged: " +
                               failure.message(),
                           /*isError=*/true));
    }

    // An argument or tool-identity fault the executor detected keeps its -32602
    // classification (Requirement 9.9) rather than degrading into a tool failure.
    if (failure.code() == ErrorCode::InvalidArgument || failure.code() == ErrorCode::NotFound) {
        return errorReply(id, kErrorInvalidParams,
                          "Invalid params for tool '" + toolName + "': " + failure.message());
    }

    // Requirement 9.5: the tool failed. Name the tool and the reason; the executor
    // has already rolled the project back to its pre-invocation state.
    return successReply(id, toolResult("tool '" + toolName + "' failed: " + failure.toString(),
                                       /*isError=*/true));
}

McpProtocolDelegate protocolDelegateFor(McpProtocolHandler& handler) {
    return [&handler](const McpRequestContext& context, std::string_view rawBody) {
        return handler.handle(context, rawBody);
    };
}

}  // namespace palmier::services
