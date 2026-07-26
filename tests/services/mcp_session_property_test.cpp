// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_session_property_test.cpp — the universally quantified
// properties of MCP session identity and session-state enforcement (task 5.5).
//
// Two properties live here:
//
//   Property 49 — every identifier `initialize` hands out is opaque and at least
//                 32 characters, no identifier is ever reissued for the whole
//                 process lifetime (including after its session was closed or
//                 expired), and a live identifier is accepted on every later
//                 request from the same client until it is closed or expires
//                 (Requirement 9.11).
//   Property 51 — `tools/list` and `tools/call` presented on a session the
//                 endpoint does not recognise, on an expired session, or on a
//                 session that never completed `initialize` are refused with the
//                 respective error, create no edit command, and leave the project
//                 byte-identical (Requirements 9.14, 9.15).
//
// Tasks 6.5 add Properties 55 (session limit) and 57 (idle expiry) to this same
// target.
//
// Time is injected, never slept on
// -------------------------------
// `McpSessionRegistry::Options::clock` is the registry's only time source, so both
// properties drive expiry by advancing a variable. A property that slept would
// need thousands of seconds to reach the 30-3600 s timeout range these cases
// generate; with an injected clock a case that straddles a 3600-second timeout
// costs nothing and the strict boundary (Requirement 10.11 closes a session idle
// for LONGER than the timeout) is reproducible.
//
// The Property 49 model
// ---------------------
// The registry prunes expired records lazily — `create()` and `expireIdle()` sweep
// every record, `touch()` only the one it was asked about — so the model tracks
// which records are still PHYSICALLY present alongside each record's `lastSeen`,
// and derives liveness from the two. That is what lets the property assert the
// exact outcome of every operation (Ok / NotFound / Timeout) instead of merely
// "something was refused".
//
// Property 51 drives the real stack — ProjectSession, the real default tool
// surface, the real McpToolExecutor and the real McpProtocolHandler — so "no edit
// command was created" is observed on the production execution path rather than a
// stand-in: the project is compared through `serializeProject`, and the undo depth
// is compared against a baseline deliberately raised to 1 by a legitimate edit
// first, so a case cannot pass merely because nothing had ever been recorded.
//
// _Requirements: 9.11, 9.14, 9.15_

#include "services/McpSessionRegistry.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpProtocolHandler.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

using namespace std::chrono_literals;
using TimePoint = std::chrono::steady_clock::time_point;

// ---------------------------------------------------------------------------
// A manually advanced monotonic clock
// ---------------------------------------------------------------------------

/// The injected time source both properties use. Starting an hour past the epoch
/// keeps every `now() - lastSeen` subtraction inside the representable range even
/// for the largest generated timeout.
class ManualClock {
public:
    [[nodiscard]] TimePoint now() const { return now_; }
    void advance(std::chrono::seconds delta) { now_ += delta; }

    [[nodiscard]] McpSessionRegistry::Clock source() {
        return [this] { return now_; };
    }

private:
    TimePoint now_ = TimePoint{} + 1h;
};

// ---------------------------------------------------------------------------
// The protocol stack Property 51 drives
// ---------------------------------------------------------------------------

Project makeProject(Uuid& trackId, Uuid& assetId) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP Session Property Test";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;

    const MediaAssetRef asset(Uuid::generateV4(), "/media/a.mp4");
    project.assets.push_back(asset);
    project.tracks.push_back(track);

    trackId = track.id;
    assetId = asset.assetId;
    return project;
}

/// session registry -> tool surface -> executor -> protocol handler, over a real
/// ProjectSession and the injected clock.
class Stack {
public:
    explicit Stack(std::chrono::seconds idleTimeout) {
        McpSessionRegistry::Options options;
        options.idleTimeout = idleTimeout;
        options.clock = clock_.source();
        sessions_ = std::make_unique<McpSessionRegistry>(std::move(options));

        session_ = std::make_unique<ProjectSession>();
        (void)session_->engine().reset(makeProject(trackId_, assetId_));
        registry_ = std::make_unique<ToolRegistry>(buildDefaultToolRegistry(*session_));
        executor_ = std::make_unique<McpToolExecutor>(*registry_, session_.get());
        handler_ = std::make_unique<McpProtocolHandler>(*registry_, *executor_, *sessions_,
                                                       inlineMainThreadInvoker());
    }

    McpProtocolHandler& handler() { return *handler_; }
    McpSessionRegistry& sessions() { return *sessions_; }
    ManualClock&        clock() { return clock_; }
    TimelineEngine&     engine() { return session_->engine(); }
    const Uuid&         trackId() const { return trackId_; }
    const Uuid&         assetId() const { return assetId_; }

    /// `initialize`, returning the minted identifier (empty on refusal).
    std::string initialize(std::int64_t requestId) {
        const McpReply reply = handler().handle(context(std::nullopt), initializeBody(requestId));
        return reply.newSessionId.value_or(std::string{});
    }

    /// `notifications/initialized` on `id`.
    void markInitialized(const std::string& id) {
        (void)handler().handle(context(id),
                               R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    }

    static McpRequestContext context(std::optional<std::string> sessionId) {
        McpRequestContext ctx;
        ctx.sourceAddress = "127.0.0.1";
        ctx.sessionId = std::move(sessionId);
        return ctx;
    }

    static std::string initializeBody(std::int64_t requestId) {
        Json params = Json::object();
        params.set("protocolVersion", Json("2025-06-18"));
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(requestId));
        request.set("method", Json("initialize"));
        request.set("params", std::move(params));
        return request.dump();
    }

    static std::string listBody(std::int64_t requestId) {
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(requestId));
        request.set("method", Json("tools/list"));
        return request.dump();
    }

    static std::string callBody(std::int64_t requestId, const std::string& tool, Json arguments) {
        Json params = Json::object();
        params.set("name", Json(tool));
        params.set("arguments", std::move(arguments));
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(requestId));
        request.set("method", Json("tools/call"));
        request.set("params", std::move(params));
        return request.dump();
    }

    Json addClipArgs() const {
        Json args = Json::object();
        args.set("trackId", trackId_.toString());
        args.set("assetId", assetId_.toString());
        args.set("sourceOutNs", static_cast<std::int64_t>(1'000'000'000));
        return args;
    }

private:
    ManualClock                         clock_;
    Uuid                                trackId_;
    Uuid                                assetId_;
    std::unique_ptr<McpSessionRegistry> sessions_;
    std::unique_ptr<ProjectSession>     session_;
    std::unique_ptr<ToolRegistry>       registry_;
    std::unique_ptr<McpToolExecutor>    executor_;
    std::unique_ptr<McpProtocolHandler> handler_;
};

/// The JSON-RPC error code carried by a reply body, or 0 when the reply carries a
/// result. Also checks the envelope of Requirement 9.1 along the way.
std::int64_t errorCodeOf(const McpReply& reply, std::string& messageOut) {
    const Result<Json> parsed = Json::parse(reply.body);
    RC_ASSERT(parsed.isOk());
    const Json& body = parsed.value();
    RC_ASSERT(body.isObject());
    RC_ASSERT(body.stringOr("jsonrpc") == "2.0");
    RC_ASSERT(body.contains("error") != body.contains("result"));
    const Json* error = body.find("error");
    if (error == nullptr) return 0;
    messageOut = error->stringOr("message");
    return error->intOr("code");
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 49: Session identifiers are
// opaque and unique for the process lifetime — for any sequence of successful
// `initialize` calls, every returned session identifier is a string of at least 32
// characters, all identifiers issued since application start are pairwise
// distinct, and each is accepted on a subsequent request from the same client
// until the session is closed or expires.
//
// Requirement 9.11: "WHEN a client completes `initialize`, THE MCP_Endpoint SHALL
// return a session identifier header whose value is an opaque string of at least
// 32 characters that is unique among all sessions active since application start,
// and SHALL accept that identifier on subsequent requests from the same client
// until the session is closed or expires."
//
// The sequence interleaves creations with closures, expiries (the clock jumps past
// the timeout) and later requests, so distinctness is asserted across identifiers
// whose sessions are long gone — exactly what the registry's never-pruned issued
// set exists for — and acceptance is asserted for identifiers that are still live
// after arbitrary amounts of intervening activity.
//
// **Validates: Requirements 9.11**
// ===========================================================================
RC_GTEST_PROP(McpSessionProperties, IdentifiersAreOpaqueAndUniqueForTheProcessLifetime, ()) {
    enum class Op { Initialize, Request, Close, Advance };

    const auto maxSessions = *rc::gen::inRange<std::size_t>(1, 33);
    const std::chrono::seconds timeout(*rc::gen::inRange<std::int64_t>(30, 3601));

    ManualClock                 clock;
    McpSessionRegistry::Options options;
    options.maxSessions = maxSessions;
    options.idleTimeout = timeout;
    options.clock = clock.source();
    McpSessionRegistry registry(std::move(options));
    RC_ASSERT(registry.maxSessions() == maxSessions);
    RC_ASSERT(registry.idleTimeout() == timeout);

    // Model. `present` holds the records still physically in the registry, keyed by
    // identifier and carrying that record's lastSeen; `issuedOrder` / `issuedSet`
    // hold every identifier ever minted, and `origin` the client each was minted
    // for, so acceptance can be asserted "from the same client".
    std::map<std::string, TimePoint>    present;
    std::vector<std::string>            issuedOrder;
    std::set<std::string>               issuedSet;
    std::map<std::string, std::string>  origin;

    const auto pruneModel = [&present, &clock, timeout] {
        for (auto it = present.begin(); it != present.end();) {
            it = (clock.now() - it->second > timeout) ? present.erase(it) : std::next(it);
        }
    };

    const int steps = *rc::gen::inRange(1, 201);
    for (int step = 0; step < steps; ++step) {
        const Op op = *rc::gen::element(Op::Initialize, Op::Initialize, Op::Initialize, Op::Request,
                                        Op::Request, Op::Close, Op::Advance, Op::Advance);

        switch (op) {
            case Op::Initialize: {
                // `create()` sweeps every expired record before deciding, so the
                // model sweeps too.
                pruneModel();
                const std::string client =
                    "127.0.0." + std::to_string(1 + (issuedOrder.size() % 250));
                const Result<std::string> created = registry.create(client, "2025-06-18");

                if (present.size() >= maxSessions) {
                    // Refused, and nothing was minted — Property 55 owns the limit
                    // itself; here it only bounds which calls are "successful".
                    RC_ASSERT(created.isError());
                    RC_ASSERT(created.error().code() == ErrorCode::FailedPrecondition);
                    RC_ASSERT(registry.issuedCount() == issuedSet.size());
                    break;
                }

                RC_ASSERT(created.isOk());
                const std::string& id = created.value();

                // Opaque, and at least 32 characters.
                RC_ASSERT(id.size() >= 32);
                RC_ASSERT(McpSessionRegistry::isWellFormedId(id));

                // Pairwise distinct from EVERY identifier issued since start,
                // including those whose sessions were closed or expired.
                RC_ASSERT(issuedSet.insert(id).second);
                RC_ASSERT(registry.wasIssued(id));
                issuedOrder.push_back(id);
                origin.emplace(id, client);
                present.emplace(id, clock.now());
                RC_ASSERT(registry.activeCount() == present.size());

                // Accepted on the next request from that client.
                const Result<McpSessionRecord*> accepted = registry.touch(id);
                RC_ASSERT(accepted.isOk());
                RC_ASSERT(accepted.value()->id == id);
                RC_ASSERT(accepted.value()->sourceAddress == client);
                break;
            }

            case Op::Request: {
                if (issuedOrder.empty()) break;
                const std::string& id =
                    issuedOrder[*rc::gen::inRange<std::size_t>(0, issuedOrder.size())];

                const auto known = present.find(id);
                const Result<McpSessionRecord*> touched = registry.touch(id);

                if (known == present.end()) {
                    // Closed, or already pruned after expiring: the client must
                    // repeat initialize, and the identifier is still never reused.
                    RC_ASSERT(touched.isError());
                    RC_ASSERT(touched.error().code() == ErrorCode::NotFound);
                    RC_ASSERT(registry.wasIssued(id));
                } else if (clock.now() - known->second > timeout) {
                    RC_ASSERT(touched.isError());
                    RC_ASSERT(touched.error().code() == ErrorCode::Timeout);
                    RC_ASSERT(registry.wasIssued(id));
                    present.erase(known);
                } else {
                    RC_ASSERT(touched.isOk());
                    RC_ASSERT(touched.value()->id == id);
                    RC_ASSERT(touched.value()->sourceAddress == origin[id]);
                    known->second = clock.now();  // the request refreshed lastSeen
                }
                break;
            }

            case Op::Close: {
                if (issuedOrder.empty()) break;
                const std::string& id =
                    issuedOrder[*rc::gen::inRange<std::size_t>(0, issuedOrder.size())];
                // close() reports whether a record was physically removed; an
                // expired-but-unswept record is still there to remove.
                RC_ASSERT(registry.close(id) == (present.find(id) != present.end()));
                present.erase(id);
                RC_ASSERT(registry.wasIssued(id));
                break;
            }

            case Op::Advance: {
                const std::int64_t seconds = *rc::gen::element<std::int64_t>(
                    1, timeout.count() / 2, timeout.count(), timeout.count() + 1,
                    2 * timeout.count());
                clock.advance(std::chrono::seconds(seconds));
                break;
            }
        }
    }

    // Nothing was minted that the model did not see, and every identifier ever
    // issued is still recorded as issued — so none of them can be minted again.
    RC_ASSERT(registry.issuedCount() == issuedSet.size());
    RC_ASSERT(issuedOrder.size() == issuedSet.size());
    for (const std::string& id : issuedOrder) {
        RC_ASSERT(registry.wasIssued(id));
        RC_ASSERT(McpSessionRegistry::isWellFormedId(id));
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 51: Session-state violations
// are rejected without touching the project — for any `tools/list` or `tools/call`
// request presented on a session that has not completed `initialize`, on a session
// identifier the endpoint does not recognise, or on a session that has expired, the
// response is an error indicating respectively that the session is not initialized
// or that the client must repeat `initialize`, no edit command is created, and the
// project state is unchanged.
//
// Requirement 9.14: "IF a `tools/list` or `tools/call` request arrives on a session
// that has not completed `initialize`, THEN THE MCP_Protocol_Handler SHALL respond
// with a JSON-RPC error indicating that the session is not initialized, SHALL
// create no edit command, and SHALL leave the project state unchanged."
//
// Requirement 9.15: "IF a request presents a session identifier that the
// MCP_Endpoint does not recognise or that has expired, THEN THE MCP_Endpoint SHALL
// reject the request with an error indicating that the client must repeat
// `initialize`, and SHALL create no edit command."
//
// The generated space is presented-identifier kind (absent, random garbage,
// well-formed-but-never-issued, the real one) x session state (never initialized,
// initialized, idle past the timeout under the injected clock) x method
// (`tools/list`, `tools/call`), minus the one combination that is not a violation
// — the real identifier on a live, initialized session. `tools/call` is generated
// with both schema-valid and schema-invalid arguments, because the session check
// runs BEFORE argument validation and a valid-argument call is precisely the one
// that would have created an edit command.
//
// **Validates: Requirements 9.14, 9.15**
// ===========================================================================
RC_GTEST_PROP(McpSessionProperties, SessionStateViolationsAreRejectedWithoutTouchingTheProject,
              ()) {
    enum class IdKind { Absent, Garbage, UnknownWellFormed, Real };
    enum class State { NeverInitialized, Initialized, Expired };

    const auto combination = *rc::gen::suchThat(
        rc::gen::pair(rc::gen::element(IdKind::Absent, IdKind::Garbage,
                                       IdKind::UnknownWellFormed, IdKind::Real),
                      rc::gen::element(State::NeverInitialized, State::Initialized,
                                       State::Expired)),
        [](const std::pair<IdKind, State>& candidate) {
            // The real identifier on a live, initialized session is legal, so it
            // is not part of this property's space.
            return !(candidate.first == IdKind::Real && candidate.second == State::Initialized);
        });
    const IdKind      idKind = combination.first;
    const State       state = combination.second;
    const bool        callMethod = *rc::gen::element(true, false);
    const bool        validArguments = *rc::gen::element(true, false);
    const std::chrono::seconds timeout(*rc::gen::inRange<std::int64_t>(30, 3601));

    Stack stack(timeout);
    RC_ASSERT(stack.sessions().idleTimeout() == timeout);

    // A legitimate edit through a properly opened session, so the baseline the
    // violation is measured against is a project that HAS an edit history: a case
    // cannot pass merely because nothing had ever been recorded.
    const std::string seedSession = stack.initialize(1);
    RC_ASSERT(!seedSession.empty());
    stack.markInitialized(seedSession);
    std::string ignoredMessage;
    RC_ASSERT(errorCodeOf(stack.handler().handle(Stack::context(seedSession),
                                                Stack::callBody(2, "timeline.add_clip",
                                                                stack.addClipArgs())),
                          ignoredMessage) == 0);
    RC_ASSERT(stack.engine().undoDepth() == 1);

    // The session the request will claim to be on.
    const std::string realSession = stack.initialize(3);
    RC_ASSERT(!realSession.empty());
    if (state != State::NeverInitialized) stack.markInitialized(realSession);
    if (state == State::Expired) {
        // Strictly past the timeout, so Requirement 10.11's "longer than" holds.
        stack.clock().advance(timeout + std::chrono::seconds(*rc::gen::inRange(1, 600)));
    }

    // The identifier presented on the request.
    std::optional<std::string> presented;
    switch (idKind) {
        case IdKind::Absent:
            break;
        case IdKind::Garbage: {
            std::string garbage = *rc::gen::container<std::string>(rc::gen::inRange<char>(33, 127));
            if (stack.sessions().wasIssued(garbage)) garbage += "!never-issued";
            presented = garbage;
            break;
        }
        case IdKind::UnknownWellFormed: {
            std::string hex;
            for (std::size_t i = 0; i < McpSessionRegistry::kSessionIdLength; ++i) {
                hex.push_back(*rc::gen::element('0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                                'a', 'b', 'c', 'd', 'e', 'f'));
            }
            RC_PRE(!stack.sessions().wasIssued(hex));
            RC_ASSERT(McpSessionRegistry::isWellFormedId(hex));
            presented = hex;
            break;
        }
        case IdKind::Real:
            presented = realSession;
            break;
    }

    // The pre-request project state, byte for byte, plus the edit-command count.
    const std::string   projectBefore = serializeProject(stack.engine().snapshot());
    const std::size_t   undoDepthBefore = stack.engine().undoDepth();

    Json arguments = validArguments ? stack.addClipArgs() : Json::object();
    if (!validArguments) arguments.set("trackId", std::string("not-a-uuid"));
    const std::string body = callMethod ? Stack::callBody(4, "timeline.add_clip",
                                                         std::move(arguments))
                                        : Stack::listBody(4);

    const McpReply reply = stack.handler().handle(Stack::context(presented), body);

    // The response is an error, and it is the error this violation calls for.
    std::string        message;
    const std::int64_t code = errorCodeOf(reply, message);
    if (idKind == IdKind::Real && state == State::NeverInitialized) {
        // Requirement 9.14: the session is not initialized.
        RC_ASSERT(code == McpProtocolHandler::kErrorSessionNotInitialized);
        RC_ASSERT(contains(message, "not initialized"));
    } else {
        // Requirement 9.15: unrecognised or expired — repeat initialize.
        RC_ASSERT(code == McpProtocolHandler::kErrorSessionUnknown);
        RC_ASSERT(contains(message, "initialize"));
    }

    // No edit command was created, and the project is byte-identical.
    RC_ASSERT(stack.engine().undoDepth() == undoDepthBefore);
    RC_ASSERT(serializeProject(stack.engine().snapshot()) == projectBefore);
}

}  // namespace
}  // namespace palmier::services
