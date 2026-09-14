// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mcp_protocol_property_test cases that stayed here after the split.
//
// Suites here: McpProtocolProperties.
// Same CMake target as its siblings, so every case keeps the name CTest already knew.

#include "mcp_protocol_property_test_support2.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::
    testsupport_mcpprotocolpropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 43: JSON-RPC envelope
// round-trip — for all well-formed JSON-RPC 2.0 requests carrying an `id`, the
// response parses as JSON, carries `"jsonrpc":"2.0"`, echoes the `id` unchanged in
// both type and value, carries exactly one of `result` or `error`, and is produced
// within 1000 milliseconds; and for all well-formed notifications, which carry no
// `id`, the response body is zero bytes.
//
// Requirement 9.1: "WHEN the MCP_Endpoint receives a POST request on `/mcp` whose
// body is a JSON-RPC 2.0 request of at most 1 MiB, THE MCP_Protocol_Handler SHALL
// respond within 1000 milliseconds with a JSON-RPC 2.0 response object that
// carries `"jsonrpc":"2.0"`, carries the request `id` unchanged, and carries
// exactly one of `result` or `error`."
// Requirement 9.13: "FOR ALL well-formed JSON-RPC requests that carry an `id`, THE
// MCP_Protocol_Handler SHALL produce a response that parses as JSON and echoes the
// request `id` unchanged in both type and value; FOR ALL well-formed
// notifications, which carry no `id`, THE MCP_Protocol_Handler SHALL produce no
// response body (round-trip property)."
//
// The request arm draws over the three *request* methods; `notifications/initialized`
// is the protocol's one notification, and carrying an `id` on it is not well formed
// (Requirement 9.10 fixes its answer as a zero-byte body regardless), so it is the
// subject of the notification arm instead.
//
// **Validates: Requirements 9.1, 9.13**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, JsonRpcEnvelopeRoundTrip, ()) {
    ScratchDir scratch;

    McpSessionRegistry::Options options;
    options.maxSessions = 32;  // so a case may draw several initializes
    Stack             stack(drawSeedProject(), {}, options);
    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const int requests = *rc::gen::inRange(1, 5);
    for (int i = 0; i < requests; ++i) {
        // --- the notification arm ------------------------------------------
        if (*rc::gen::inRange(0, 4) == 0) {
            std::optional<Json> params;
            if (*rc::gen::arbitrary<bool>()) params = Json::object();
            const std::string body =
                envelope("notifications/initialized", std::nullopt, std::move(params)).dump();

            const Timed answered = handleTimed(stack.handler(), Stack::context(session), body);
            RC_ASSERT(answered.reply.body.empty());
            RC_ASSERT(answered.reply.body.size() == 0u);
            RC_ASSERT(answered.reply.httpStatus == 202);
            RC_ASSERT(answered.elapsed < 1000ms);
            continue;
        }

        // --- the request arm ------------------------------------------------
        Json id(nullptr);
        switch (*rc::gen::inRange(0, 3)) {
            case 0:
                id = Json(drawAsciiText(1 + drawIndex(12)));
                break;
            case 1:
                id = Json(*rc::gen::inRange<std::int64_t>(-4096, 1'000'001));
                break;
            default:
                id = Json(nullptr);
                break;
        }

        std::string        method;
        std::optional<Json> params;
        switch (*rc::gen::inRange(0, 3)) {
            case 0: {
                method = "initialize";
                if (*rc::gen::arbitrary<bool>()) {
                    Json p = Json::object();
                    p.set("protocolVersion", Json(drawAsciiText(1 + drawIndex(10))));
                    params = std::move(p);
                }
                break;
            }
            case 1:
                method = "tools/list";
                if (*rc::gen::arbitrary<bool>()) params = Json::object();
                break;
            default: {
                method = "tools/call";
                const Tool& tool = stack.registry().tools()[drawIndex(stack.registry().size())];
                switch (*rc::gen::inRange(0, 3)) {
                    case 0:  // a schema-valid argument object
                        params = callParams(tool.name, schemaValidArgs(tool, scratch));
                        break;
                    case 1:  // an unregistered tool name
                        params = callParams("timeline." + drawAsciiText(6), Json::object());
                        break;
                    default:  // arguments the schema rejects
                        params = callParams(tool.name, Json::object());
                        break;
                }
                break;
            }
        }

        const std::string body = envelope(method, id, std::move(params)).dump();
        const Timed       answered = handleTimed(stack.handler(), Stack::context(session), body);

        // Produced within the stated bound.
        RC_ASSERT(answered.elapsed < 1000ms);

        // Parses as JSON, and is a JSON-RPC 2.0 response with exactly one of
        // result / error.
        const std::optional<Json> parsedBody = parseBody(answered.reply);
        RC_ASSERT(parsedBody.has_value());
        RC_ASSERT(isJsonRpcEnvelope(*parsedBody));

        // Echoes the id unchanged in both type and value.
        const Json* echoed = parsedBody->find("id");
        RC_ASSERT(echoed != nullptr);
        RC_ASSERT(echoed->type() == id.type());
        RC_ASSERT(*echoed == id);
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 44: initialize negotiates a
// supported protocol version — for any client-requested protocol version string,
// the `initialize` result carries a negotiated version equal to the request when
// that version is supported and equal to the highest supported version otherwise,
// together with a non-empty server name, a server version, and a capabilities
// object declaring the `tools` capability.
//
// Requirement 9.2: "WHEN the MCP_Protocol_Handler receives the `initialize`
// method, THE MCP_Protocol_Handler SHALL return a negotiated protocol version
// equal to the client-requested version when that version is one the
// MCP_Protocol_Handler supports and otherwise equal to the highest version it
// supports, a non-empty server name, a server version, and a capabilities object
// that declares the `tools` capability."
//
// **Validates: Requirements 9.2**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, InitializeNegotiatesASupportedProtocolVersion, ()) {
    McpSessionRegistry::Options options;
    options.maxSessions = 32;
    Stack stack(drawSeedProject(), {}, options);

    const int attempts = *rc::gen::inRange(1, 6);
    for (int i = 0; i < attempts; ++i) {
        // Supported versions, unsupported date-shaped strings, the empty string,
        // non-date garbage, and the omitted-parameter case.
        std::optional<std::string> requested;
        switch (*rc::gen::inRange(0, 5)) {
            case 0:
                requested = std::string(
                    McpProtocolHandler::kSupportedProtocolVersions[drawIndex(
                        std::size(McpProtocolHandler::kSupportedProtocolVersions))]);
                break;
            case 1: {
                char buffer[16] = {};
                std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d",
                              *rc::gen::inRange(1990, 2101), *rc::gen::inRange(1, 13),
                              *rc::gen::inRange(1, 29));
                requested = std::string(buffer);
                break;
            }
            case 2:
                requested = std::string{};
                break;
            case 3:
                requested = drawAsciiText(1 + drawIndex(20));
                break;
            default:
                requested = std::nullopt;  // no protocolVersion supplied
                break;
        }

        std::optional<Json> params;
        if (requested.has_value()) {
            Json p = Json::object();
            p.set("protocolVersion", Json(*requested));
            params = std::move(p);
        }

        const std::string body =
            envelope("initialize", Json(static_cast<std::int64_t>(i + 1)), std::move(params))
                .dump();
        const McpReply reply = stack.handler().handle(Stack::context(std::nullopt), body);

        const std::optional<Json> parsedBody = parseBody(reply);
        RC_ASSERT(parsedBody.has_value());
        RC_ASSERT(isJsonRpcEnvelope(*parsedBody));
        const Json* result = parsedBody->find("result");
        RC_ASSERT(result != nullptr);

        // The requested version when supported; the highest supported otherwise.
        const std::string expected =
            requested.has_value() && McpProtocolHandler::isSupportedProtocolVersion(*requested)
                ? *requested
                : std::string(McpProtocolHandler::latestProtocolVersion());
        RC_ASSERT(result->stringOr("protocolVersion") == expected);
        RC_ASSERT(McpProtocolHandler::isSupportedProtocolVersion(
            result->stringOr("protocolVersion")));

        // A non-empty server name and a server version.
        const Json* serverInfo = result->find("serverInfo");
        RC_ASSERT(serverInfo != nullptr);
        RC_ASSERT(!serverInfo->stringOr("name").empty());
        RC_ASSERT(!serverInfo->stringOr("version").empty());

        // A capabilities object declaring `tools`.
        const Json* capabilities = result->find("capabilities");
        RC_ASSERT(capabilities != nullptr);
        RC_ASSERT(capabilities->isObject());
        const Json* tools = capabilities->find("tools");
        RC_ASSERT(tools != nullptr);
        RC_ASSERT(tools->isObject());
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 45: tools/list describes every
// registered tool — for any registry contents, the `tools/list` result contains
// exactly one entry per registered tool, and every entry carries a non-empty
// `name` of at most 64 characters, a non-empty `description`, and an `inputSchema`
// object of type `object` naming each accepted argument and listing the required
// arguments.
//
// Requirement 9.3: "WHEN the MCP_Protocol_Handler receives the `tools/list`
// method, THE MCP_Protocol_Handler SHALL return one entry per tool registered in
// the Tool_Surface — so that the entry count equals the Tool_Surface tool count —
// with each entry containing a non-empty `name` of at most 64 characters, a
// non-empty `description`, and an `inputSchema` object that is a JSON Schema of
// type object naming each accepted argument and listing the required arguments."
//
// **Validates: Requirements 9.3**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ToolsListDescribesEveryRegisteredTool, ()) {
    // A generated subset, in a generated order, of the full advertised surface.
    // The null-session overload advertises the identical surface, so it can be
    // consulted for the tool count before the session under test exists.
    const ToolRegistry advertised =
        buildDefaultToolRegistry(static_cast<ProjectSession*>(nullptr));
    const std::size_t total = advertised.size();
    RC_ASSERT(total > 0u);

    std::vector<std::size_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    for (std::size_t i = total; i > 1; --i) {  // Fisher-Yates
        std::swap(order[i - 1], order[drawIndex(i)]);
    }
    order.resize(*rc::gen::inRange<std::size_t>(0, total + 1));

    Stack stack(drawSeedProject(), [order](ProjectSession& session) {
        const ToolRegistry full = buildDefaultToolRegistry(session);
        ToolRegistry       subset;
        for (const std::size_t index : order) subset.add(full.tools()[index]);
        return subset;
    });
    RC_ASSERT(stack.registry().size() == order.size());

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const McpReply reply = stack.handler().handle(
        Stack::context(session),
        envelope("tools/list", Json(static_cast<std::int64_t>(7))).dump());

    const std::optional<Json> body = parseBody(reply);
    RC_ASSERT(body.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*body));
    const Json* result = body->find("result");
    RC_ASSERT(result != nullptr);
    const Json* entries = result->find("tools");
    RC_ASSERT(entries != nullptr);
    RC_ASSERT(entries->isArray());

    // Exactly one entry per registered tool.
    RC_ASSERT(entries->asArray().size() == stack.registry().size());

    std::set<std::string> described;
    for (const Json& entry : entries->asArray()) {
        RC_ASSERT(entry.isObject());

        const std::string name = entry.stringOr("name");
        RC_ASSERT(!name.empty());
        RC_ASSERT(name.size() <= 64u);
        RC_ASSERT(described.insert(name).second);  // one entry, not several
        RC_ASSERT(!entry.stringOr("description").empty());

        const Tool* tool = stack.registry().find(name);
        RC_ASSERT(tool != nullptr);

        const Json* schema = entry.find("inputSchema");
        RC_ASSERT(schema != nullptr);
        RC_ASSERT(schema->isObject());
        RC_ASSERT(schema->stringOr("type") == "object");

        // Naming each accepted argument ...
        const Json* properties = schema->find("properties");
        RC_ASSERT(properties != nullptr);
        RC_ASSERT(properties->isObject());
        RC_ASSERT(properties->asObject().size() == tool->schema.size());
        for (const ArgSpec& spec : tool->schema.args()) {
            RC_ASSERT(properties->contains(spec.name));
        }

        // ... and listing the required ones, and only those.
        std::set<std::string> requiredBySchema;
        if (const Json* required = schema->find("required"); required != nullptr) {
            RC_ASSERT(required->isArray());
            for (const Json& item : required->asArray()) {
                RC_ASSERT(item.isString());
                RC_ASSERT(requiredBySchema.insert(item.asString()).second);
            }
        }
        std::set<std::string> requiredByTool;
        for (const ArgSpec& spec : tool->schema.args()) {
            if (spec.required) requiredByTool.insert(spec.name);
        }
        RC_ASSERT(requiredBySchema == requiredByTool);
    }
    RC_ASSERT(described.size() == stack.registry().size());
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 46: tools/call success shape —
// for any registered tool and any argument object that validates against its
// advertised `inputSchema`, the `tools/call` result carries a `content` array
// whose first entry has `"type":"text"` and carries `isError` set to false.
//
// Requirement 9.4: "WHEN the MCP_Protocol_Handler receives the `tools/call` method
// with a tool name registered in the Tool_Surface and an arguments object that
// validates against that tool's advertised `inputSchema`, THE MCP_Protocol_Handler
// SHALL execute the tool through the same execution policy used for GUI and
// in-app agent invocations — argument validation, rollback on failure, and
// undo-history recording — and return a result object containing a `content` array
// whose first entry has `"type":"text"`, plus `isError` set to false."
//
// `generation.generate`, `generation.list_models`, `timeline.export` and
// `media.import` are hook-backed and no hook is wired in a test binary, so their
// capability is absent from this build:
// they are required to answer in the identical result shape with `isError` true
// naming the tool, which is what "the capability is not configured" looks like
// through the protocol rather than a protocol fault.
//
// **Validates: Requirements 9.4**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ToolsCallSuccessShape, ()) {
    ScratchDir    scratch;
    const Project seed = drawSeedProject();

    // A readable document for `project.open`, written through the project store.
    const fs::path document = scratch.file("document");
    RC_ASSERT(saveProjectToFile(seed, document).isOk());

    Stack             stack(seed);
    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const Invocation invocation =
        drawValidInvocation(stack.registry(), stack.session(), scratch, document);
    const Tool* tool = stack.registry().find(invocation.tool);
    RC_ASSERT(tool != nullptr);

    // The arguments validate against the very schema `tools/list` advertises.
    RC_ASSERT(tool->schema.validate(invocation.arguments).isOk());

    const std::string body =
        envelope("tools/call", Json(static_cast<std::int64_t>(11)),
                 callParams(invocation.tool, invocation.arguments))
            .dump();
    const McpReply reply = stack.handler().handle(Stack::context(session), body);

    const std::optional<Json> parsedBody = parseBody(reply);
    RC_ASSERT(parsedBody.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*parsedBody));

    // The call reached the tool: the answer is a result, never a protocol error.
    const Json* result = parsedBody->find("result");
    RC_ASSERT(result != nullptr);

    // A `content` array whose first entry is a text entry.
    const std::optional<std::string> text = firstTextContent(*result);
    RC_ASSERT(text.has_value());
    RC_ASSERT(!text->empty());

    const Json* isError = result->find("isError");
    RC_ASSERT(isError != nullptr);
    RC_ASSERT(isError->isBool());

    if (isHookBacked(invocation.tool)) {
        // The capability is not configured in this build: reported as a tool
        // failure naming the tool, in the same result shape.
        RC_ASSERT(isError->asBool());
        RC_ASSERT(contains(*text, invocation.tool));
    } else {
        RC_ASSERT(!isError->asBool());
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 47: A failing tool leaves the
// project and history untouched — for any project and any tool invocation forced to
// fail, the `tools/call` result carries `isError` set to true with a `content`
// entry naming the failing tool and the failure reason, and the project's track
// count, clip set, clip source ranges, effects, transitions, asset references and
// undo history compare equal to their pre-invocation values.
//
// Requirement 9.5: "IF a tool invoked through `tools/call` fails, THEN THE
// MCP_Protocol_Handler SHALL return a JSON-RPC result with `isError` set to true
// and a `content` entry naming the failing tool and the failure reason, and SHALL
// leave the project — its track count, clip set, clip source ranges, effects,
// transitions, asset references and undo history — in its pre-invocation state."
//
// The failure is injected at three points: before any command is created, after
// exactly one applied command, and after several applied commands — so the
// rollback obligation is exercised from "nothing to undo" through "undo a whole
// batch". `InvalidArgument` and `NotFound` failures keep their −32602
// classification instead of becoming an `isError` result, so they are Property 48's
// subject rather than this one's.
//
// **Validates: Requirements 9.5**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, FailingToolLeavesProjectAndHistoryUntouched, ()) {
    const Project seed = drawSeedProject();

    // 0 = fail before any command, 1 = fail after one, 2 = fail after several.
    const int         appliedBeforeFailure = *rc::gen::inRange(0, 3);
    const std::string reason = "forced failure " + drawAsciiText(1 + drawIndex(12));
    const std::string failingToolName = "test.forced_failure";

    Stack stack(seed, [appliedBeforeFailure, reason,
                       failingToolName](ProjectSession& session) {
        ToolRegistry registry = buildDefaultToolRegistry(session);

        Tool failing;
        failing.name = failingToolName;
        failing.description = "Applies a generated number of edits and then fails.";
        failing.handler = [&session, appliedBeforeFailure,
                           reason](const Json&) -> Result<Json> {
            TimelineEngine& engine = session.engine();
            for (int i = 0; i < appliedBeforeFailure; ++i) {
                const Project project = engine.snapshot();
                if (project.tracks.empty()) break;

                Clip clip;
                clip.id = Uuid::generateV4();
                clip.assetRef = project.assets.empty()
                                    ? MediaAssetRef(Uuid::generateV4(), "/media/forced.mp4")
                                    : project.assets.front();
                clip.timelineStart = Duration::fromNanoseconds(
                    projectEndNs(project) +
                    Duration::fromMilliseconds(kClipGapMs).nanoseconds());
                clip.sourceIn = Duration::zero();
                clip.sourceOut = Duration::fromMilliseconds(kClipLengthMs);
                const CommandResult applied = engine.apply(
                    std::make_unique<AddClipCommand>(project.tracks.front().id,
                                                     std::move(clip)));
                if (applied.isError()) break;
            }
            // A FailedPrecondition (rather than InvalidArgument / NotFound) is what
            // Requirement 9.5 classifies as a tool failure.
            return err<Json>(failedPrecondition(reason));
        };
        registry.add(std::move(failing));
        return registry;
    });

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const ProjectFingerprint before = fingerprint(stack.session());

    const McpReply reply = stack.handler().handle(
        Stack::context(session),
        envelope("tools/call", Json(static_cast<std::int64_t>(13)),
                 callParams(failingToolName, Json::object()))
            .dump());

    const std::optional<Json> body = parseBody(reply);
    RC_ASSERT(body.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*body));
    const Json* result = body->find("result");
    RC_ASSERT(result != nullptr);

    // isError true, with a content entry naming the tool and the reason.
    RC_ASSERT(result->boolOr("isError", false));
    const std::optional<std::string> text = firstTextContent(*result);
    RC_ASSERT(text.has_value());
    RC_ASSERT(contains(*text, failingToolName));
    RC_ASSERT(contains(*text, reason));

    // The project and the undo history are exactly as they were.
    const ProjectFingerprint after = fingerprint(stack.session());
    RC_ASSERT(after.trackCount == before.trackCount);
    RC_ASSERT(after.clipCount == before.clipCount);
    RC_ASSERT(after.clipFacts == before.clipFacts);
    RC_ASSERT(after.assetRefs == before.assetRefs);
    RC_ASSERT(after.undoDepth == before.undoDepth);
    RC_ASSERT(after.canUndo == before.canUndo);
    RC_ASSERT(after.bytes == before.bytes);
    RC_ASSERT(after == before);
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 48: Protocol faults map to
// their assigned codes and create no edit command — for any request violating
// exactly one protocol rule, the response carries the code that rule is assigned —
// −32700 for an unparsable or over-1-MiB body, −32600 for a missing
// `"jsonrpc":"2.0"`, a missing `method`, or a `method`/`id` of a disallowed JSON
// type, −32601 for an unsupported method (with the method named), −32602 for an
// unknown tool, a missing required argument, a wrong-typed argument or an
// out-of-bounds argument value (with the tool or argument named) — and no edit
// command is created and the project is byte-identical.
//
// Requirement 9.6: "IF a request body is not parsable JSON, or exceeds 1 MiB, THEN
// THE MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32700 and SHALL
// create no edit command."
// Requirement 9.7: "IF a request is not a valid JSON-RPC 2.0 request object —
// because it omits `"jsonrpc":"2.0"`, omits `method`, or supplies `method` or `id`
// with a JSON type other than string (or, for `id`, number or null) — THEN THE
// MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32600."
// Requirement 9.8: "IF a request names a method other than `initialize`,
// `notifications/initialized`, `tools/list` or `tools/call`, THEN THE
// MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32601 and an error
// message naming the unsupported method."
// Requirement 9.9: "IF a `tools/call` request names a tool absent from the
// Tool_Surface, omits a required argument, supplies an argument of the wrong JSON
// type, or supplies an argument value outside the bounds declared by the tool's
// `inputSchema`, THEN THE MCP_Protocol_Handler SHALL respond with JSON-RPC error
// code -32602 naming the offending tool or argument, SHALL create no edit command,
// and SHALL leave the project state unchanged."
//
// **Validates: Requirements 9.6, 9.7, 9.8, 9.9**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ProtocolFaultsMapToTheirAssignedCodes, ()) {
    ScratchDir scratch;
    Stack      stack(drawSeedProject());

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const ProjectFingerprint before = fingerprint(stack.session());

    enum class Fault {
        Unparsable,
        OversizeDeclared,
        OversizeActual,
        NotAnObject,
        MissingJsonrpc,
        WrongJsonrpc,
        MissingMethod,
        NonStringMethod,
        DisallowedIdType,
        UnsupportedMethod,
        UnknownTool,
        MissingRequiredArgument,
        WrongTypedArgument,
        OutOfBoundsArgument,
    };
    static constexpr Fault kFaults[] = {
        Fault::Unparsable,         Fault::OversizeDeclared,   Fault::OversizeActual,
        Fault::NotAnObject,        Fault::MissingJsonrpc,     Fault::WrongJsonrpc,
        Fault::MissingMethod,      Fault::NonStringMethod,    Fault::DisallowedIdType,
        Fault::UnsupportedMethod,  Fault::UnknownTool,        Fault::MissingRequiredArgument,
        Fault::WrongTypedArgument, Fault::OutOfBoundsArgument};
    const Fault fault = kFaults[drawIndex(std::size(kFaults))];

    McpRequestContext context = Stack::context(session);
    std::string       body;
    std::int64_t      expectedCode = 0;
    std::string       mustName;      ///< the method, tool or argument the message must name
    std::string       alsoMustName;  ///< a second name (the tool, for argument faults)

    // A well-formed `tools/call` on a real tool is the base every argument fault
    // perturbs in exactly one place.
    const Tool& tool = stack.registry().tools()[drawIndex(stack.registry().size())];

    switch (fault) {
        case Fault::Unparsable: {
            static const char* const unparsable[] = {
                "{", "[", "}", "{\"jsonrpc\"", "{\"jsonrpc\":\"2.0\",", "not json at all",
                "\"unterminated", "{,}", "{\"a\":}", "tru"};
            body = unparsable[drawIndex(std::size(unparsable))];
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::OversizeDeclared: {
            body = envelope("tools/list", Json(static_cast<std::int64_t>(1))).dump();
            context.bodyBytes = McpProtocolHandler::kDefaultMaxBodyBytes +
                                static_cast<std::size_t>(*rc::gen::inRange(1, 4096));
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::OversizeActual: {
            Json padded = Json::object();
            padded.set("name", Json(std::string(kReadTimeline)));
            padded.set("padding",
                       Json(std::string(McpProtocolHandler::kDefaultMaxBodyBytes + 64, 'p')));
            body = envelope("tools/call", Json(static_cast<std::int64_t>(1)),
                            std::move(padded))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::NotAnObject: {
            switch (*rc::gen::inRange(0, 3)) {
                case 0:
                    body = R"(["jsonrpc","2.0"])";
                    break;
                case 1:
                    body = "42";
                    break;
                default:
                    body = "\"tools/list\"";
                    break;
            }
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::MissingJsonrpc: {
            Json request = Json::object();
            request.set("id", Json(static_cast<std::int64_t>(3)));
            request.set("method", Json("tools/list"));
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::WrongJsonrpc: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(3)));
            request.set("jsonrpc", *rc::gen::arbitrary<bool>()
                                       ? Json("1.0")
                                       : Json(2.0));  // wrong value / wrong type
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::MissingMethod: {
            Json request = Json::object();
            request.set("jsonrpc", Json("2.0"));
            request.set("id", Json(static_cast<std::int64_t>(4)));
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::NonStringMethod: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(4)));
            switch (*rc::gen::inRange(0, 3)) {
                case 0:
                    request.set("method", Json(static_cast<std::int64_t>(42)));
                    break;
                case 1:
                    request.set("method", Json::array());
                    break;
                default:
                    request.set("method", Json(true));
                    break;
            }
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::DisallowedIdType: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(5)));
            switch (*rc::gen::inRange(0, 3)) {
                case 0: {
                    Json object = Json::object();
                    object.set("a", Json(static_cast<std::int64_t>(1)));
                    request.set("id", std::move(object));
                    break;
                }
                case 1: {
                    Json array = Json::array();
                    array.push_back(Json(static_cast<std::int64_t>(1)));
                    request.set("id", std::move(array));
                    break;
                }
                default:
                    request.set("id", Json(*rc::gen::arbitrary<bool>()));
                    break;
            }
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::UnsupportedMethod: {
            static const char* const prefixes[] = {"resources/", "prompts/", "completion/",
                                                   "logging/",   "tools/",   ""};
            mustName = std::string(prefixes[drawIndex(std::size(prefixes))]) +
                       drawAsciiText(1 + drawIndex(10));
            RC_PRE(mustName != "initialize" && mustName != "tools/list" &&
                   mustName != "tools/call" && mustName != "notifications/initialized");
            body = envelope(mustName, Json(static_cast<std::int64_t>(6))).dump();
            expectedCode = McpProtocolHandler::kErrorMethodNotFound;
            break;
        }
        case Fault::UnknownTool: {
            mustName = "timeline." + drawAsciiText(1 + drawIndex(10));
            RC_PRE(!stack.registry().has(mustName));
            body = envelope("tools/call", Json(static_cast<std::int64_t>(7)),
                            callParams(mustName, Json::object()))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::MissingRequiredArgument: {
            std::vector<const ArgSpec*> required;
            for (const ArgSpec& spec : tool.schema.args()) {
                if (spec.required) required.push_back(&spec);
            }
            RC_PRE(!required.empty());
            const ArgSpec& dropped = *required[drawIndex(required.size())];

            Json args = schemaValidArgs(tool, scratch);
            for (const ArgSpec& spec : tool.schema.args()) {  // ensure every other one is present
                if (spec.name != dropped.name && !args.contains(spec.name)) {
                    args.set(spec.name, validValue(spec, scratch));
                }
            }
            Json trimmed = Json::object();
            for (const auto& [key, value] : args.asObject()) {
                if (key != dropped.name) trimmed.set(key, value);
            }
            mustName = dropped.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(8)),
                            callParams(tool.name, std::move(trimmed)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::WrongTypedArgument: {
            RC_PRE(tool.schema.size() > 0u);
            const ArgSpec& spec = tool.schema.args()[drawIndex(tool.schema.size())];
            Json           args = schemaValidArgs(tool, scratch);
            args.set(spec.name, wrongTypedValue(spec.kind));
            mustName = spec.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(9)),
                            callParams(tool.name, std::move(args)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::OutOfBoundsArgument: {
            std::vector<const ArgSpec*> bounded;
            for (const ArgSpec& spec : tool.schema.args()) {
                if (hasBound(spec)) bounded.push_back(&spec);
            }
            RC_PRE(!bounded.empty());
            const ArgSpec& spec = *bounded[drawIndex(bounded.size())];
            Json           args = schemaValidArgs(tool, scratch);
            args.set(spec.name, outOfBoundsValue(spec));
            mustName = spec.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(10)),
                            callParams(tool.name, std::move(args)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
    }

    const McpReply            reply = stack.handler().handle(context, body);
    const std::optional<Json> parsedBody = parseBody(reply);
    RC_ASSERT(parsedBody.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*parsedBody));
    RC_ASSERT(parsedBody->contains("error"));

    // The code that rule is assigned.
    RC_ASSERT(errorCode(*parsedBody) == expectedCode);

    // Naming the method, the tool or the argument.
    const std::string message = errorMessage(*parsedBody);
    if (!mustName.empty()) RC_ASSERT(contains(message, mustName));
    if (!alsoMustName.empty()) RC_ASSERT(contains(message, alsoMustName));

    // No edit command was created, and the project is byte-identical.
    const ProjectFingerprint after = fingerprint(stack.session());
    RC_ASSERT(after.bytes == before.bytes);
    RC_ASSERT(after.undoDepth == before.undoDepth);
    RC_ASSERT(!stack.engine().canUndo());
    RC_ASSERT(after == before);
}

}  // namespace
}  // namespace palmier::services
