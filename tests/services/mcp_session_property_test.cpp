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
// Task 6.5 added two more to this same file and target:
//
//   Property 55 — a session-initiating request is accepted while the active count
//                 is below the configured maximum, each request arriving at the
//                 maximum is rejected naming the limit, and every established
//                 session stays active and usable (Requirement 10.9).
//   Property 57 — an established session is closed if and only if its idle
//                 interval EXCEEDS the configured timeout, and a later request
//                 carrying that identifier is told the session is no longer valid
//                 (Requirement 10.11).
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
// _Requirements: 9.11, 9.14, 9.15, 10.9, 10.11_

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

// ---------------------------------------------------------------------------
// A registry on the manual clock (tasks 5.5 and 6.5)
// ---------------------------------------------------------------------------

/// `clock` must outlive the returned registry: `ManualClock::source()` captures it
/// by pointer, so the caller keeps the clock as a named local and never moves it.
std::unique_ptr<McpSessionRegistry> makeRegistry(ManualClock&         clock,
                                                std::chrono::seconds timeout,
                                                std::size_t          maxSessions) {
    McpSessionRegistry::Options options;
    options.maxSessions = maxSessions;
    options.idleTimeout = timeout;
    options.clock = clock.source();
    return std::make_unique<McpSessionRegistry>(std::move(options));
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 55: The session limit rejects
// only the excess request — for any configured maximum in 1-32 and any sequence of
// session-initiating requests, requests are accepted while the active session count
// is below the maximum, each request arriving at the maximum is rejected with an
// error indicating the session limit was reached, and every established session
// remains active and usable.
//
// Requirement 10.9: "THE Remote_Access_Gate SHALL limit concurrent MCP sessions to
// a configurable maximum in the range 1 to 32 with a default of 8, and IF a
// session-initiating request arrives while that maximum is reached, THEN THE
// Remote_Access_Gate SHALL reject only that request with an error indicating the
// session limit was reached and SHALL keep all established sessions active."
//
// "Only the excess" is what this property is really about, so a rejection is never
// observed in isolation. After every refusal the case asserts three further things:
// nothing was minted (`issuedCount` unmoved, so a refused request consumed no
// identifier), every session established before the refusal is still counted
// active, and every one of them still `touch()`es successfully — active *and*
// usable, not merely present. Freeing a slot by closing a session is generated as
// an operation of its own, and the loop is followed by a saturate → refuse → close →
// accept coda, so the refusal is also shown not to be sticky: the limit refuses the
// request, not the client.
//
// The idle timeout is pinned to its maximum and the clock never advances, so no
// record can expire underneath this property — the only thing that removes a
// session here is an explicit close, which keeps the model exact. Property 57 owns
// expiry.
//
// **Validates: Requirements 10.9**
// ===========================================================================
RC_GTEST_PROP(McpSessionProperties, TheSessionLimitRejectsOnlyTheExcessRequest, ()) {
    const auto maxSessions = *rc::gen::inRange<std::size_t>(1, 33);

    ManualClock clock;
    auto        registry = makeRegistry(clock, McpSessionRegistry::kMaxIdleTimeout, maxSessions);
    RC_ASSERT(registry->maxSessions() == maxSessions);

    // Model: the sessions currently established, in creation order.
    std::vector<std::string> live;
    std::set<std::string>    issued;

    /// Every established session is still counted active and still usable.
    const auto assertEstablishedStillUsable = [&registry](const std::vector<std::string>& ids) {
        RC_ASSERT(registry->activeCount() == ids.size());
        for (const std::string& id : ids) {
            const Result<McpSessionRecord*> touched = registry->touch(id);
            RC_ASSERT(touched.isOk());
            RC_ASSERT(touched.value()->id == id);
            RC_ASSERT(touched.value()->protocolVersion == "2025-06-18");
        }
    };

    const int operations = *rc::gen::inRange(1, 65);
    for (int step = 0; step < operations; ++step) {
        // Closing an empty registry is not an operation, so an empty model always
        // initiates; otherwise initiations outnumber closures 3:1, which keeps the
        // count pressed against the maximum where the interesting cases live.
        const bool initiate = live.empty() || *rc::gen::element(true, true, true, false);

        if (initiate) {
            const std::vector<std::string> established = live;
            const std::size_t              issuedBefore = registry->issuedCount();
            const std::string client = "203.0.113." + std::to_string(1 + (issued.size() % 250));

            const Result<std::string> created = registry->create(client, "2025-06-18");

            if (established.size() < maxSessions) {
                // Below the maximum: accepted.
                RC_ASSERT(created.isOk());
                const std::string& id = created.value();
                RC_ASSERT(McpSessionRegistry::isWellFormedId(id));
                RC_ASSERT(issued.insert(id).second);
                RC_ASSERT(registry->issuedCount() == issuedBefore + 1);
                live.push_back(id);
                RC_ASSERT(registry->activeCount() == live.size());
                RC_ASSERT(live.size() <= maxSessions);
            } else {
                // At the maximum: this request, and only this request, is refused.
                RC_ASSERT(created.isError());
                RC_ASSERT(created.error().code() == ErrorCode::FailedPrecondition);
                RC_ASSERT(contains(created.error().message(), "session limit"));
                RC_ASSERT(contains(created.error().message(), std::to_string(maxSessions)));
                // A refused request mints nothing, so it cannot consume an identifier.
                RC_ASSERT(registry->issuedCount() == issuedBefore);
                RC_ASSERT(registry->issuedCount() == issued.size());
            }

            // Established sessions survive an acceptance and a refusal alike.
            assertEstablishedStillUsable(live);
        } else {
            const auto index = *rc::gen::inRange<std::size_t>(0, live.size());
            const std::string closed = live[index];
            RC_ASSERT(registry->close(closed));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            RC_ASSERT(registry->activeCount() == live.size());
            // A closed identifier is gone but never reusable.
            const Result<McpSessionRecord*> afterClose = registry->touch(closed);
            RC_ASSERT(afterClose.isError());
            RC_ASSERT(afterClose.error().code() == ErrorCode::NotFound);
            RC_ASSERT(registry->wasIssued(closed));
            assertEstablishedStillUsable(live);
        }
    }

    // Coda: saturate, be refused, free exactly one slot, be accepted again — the
    // limit refuses the excess request, not the client, and never permanently.
    while (live.size() < maxSessions) {
        const Result<std::string> created = registry->create("203.0.113.254", "2025-06-18");
        RC_ASSERT(created.isOk());
        RC_ASSERT(issued.insert(created.value()).second);
        live.push_back(created.value());
    }
    RC_ASSERT(registry->activeCount() == maxSessions);

    const Result<std::string> refused = registry->create("203.0.113.254", "2025-06-18");
    RC_ASSERT(refused.isError());
    RC_ASSERT(refused.error().code() == ErrorCode::FailedPrecondition);
    RC_ASSERT(contains(refused.error().message(), "session limit"));
    assertEstablishedStillUsable(live);

    RC_ASSERT(registry->close(live.back()));
    live.pop_back();
    const Result<std::string> accepted = registry->create("203.0.113.254", "2025-06-18");
    RC_ASSERT(accepted.isOk());
    RC_ASSERT(issued.insert(accepted.value()).second);
    live.push_back(accepted.value());
    RC_ASSERT(registry->activeCount() == maxSessions);
    assertEstablishedStillUsable(live);

    // Nothing was minted outside the model, so no refusal ever silently created a
    // session and no acceptance ever created two.
    RC_ASSERT(registry->issuedCount() == issued.size());
    RC_ASSERT(live.size() == maxSessions);
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 57: Idle sessions expire exactly
// at their configured timeout — for any configured idle timeout in 30-3600 seconds
// and any idle interval, an established session is closed with a recorded reason if
// and only if the idle interval exceeds the timeout, and subsequent requests
// carrying that identifier are rejected with an error indicating the session is no
// longer valid.
//
// Requirement 10.11: "WHILE an established MCP session has received no request for
// LONGER than the configured idle timeout, which is configurable in the range 30 to
// 3600 seconds with a default of 300 seconds, THE Remote_Access_Gate SHALL close
// that session, record the closure reason, and reject subsequent requests carrying
// that session identifier with an error indicating the session is no longer valid."
//
// The boundary is STRICT, and that is the whole point of the property
// -----------------------------------------------------------------
// "no request for longer than the configured idle timeout" makes the comparison
// `elapsed > timeout`, not `>=`: a session idle for EXACTLY the timeout has not yet
// gone longer than it, so it is still alive; the first observation strictly past the
// timeout closes it. An off-by-one in either direction is a real defect — closing at
// exactly the timeout evicts a client that is still within its budget, and closing
// only past `timeout + 1` keeps a session past its configured life — so this
// property asserts BOTH sides of the boundary in every generated case, in the
// `exact timeout` / `timeout + 1 second` block below, rather than relying on the
// generator to happen to produce the two adjacent intervals.
//
// The generated interval is then drawn from a distribution deliberately clustered
// on the boundary (0, 1, half, timeout - 1, timeout, timeout + 1, timeout + 2, twice
// the timeout) unioned with a uniform draw across the whole range, and the expected
// outcome is computed from the same strict rule, so aliveness is asserted as an
// if-and-only-if rather than in one direction.
//
// Time is advanced by moving the injected clock, never by sleeping: a case that
// straddles a 3600-second timeout costs no wall clock at all, and the boundary is
// reproducible to the second, which sleeping could never guarantee.
//
// Both closure paths are exercised: the lazy one in `touch()`, whose error is what
// the client is actually told, and the sweep in `expireIdle()`, whose return value is
// the recorded closure count.
//
// **Validates: Requirements 10.11**
// ===========================================================================
RC_GTEST_PROP(McpSessionProperties, IdleSessionsExpireExactlyAtTheirConfiguredTimeout, ()) {
    const std::int64_t         timeoutSeconds = *rc::gen::inRange<std::int64_t>(30, 3601);
    const std::chrono::seconds timeout(timeoutSeconds);

    // -----------------------------------------------------------------------
    // The strict boundary, asserted on both sides in every case.
    //
    // Idle for exactly the timeout: NOT "longer than", so still established, and
    // the sweep closes nothing. One further second: now longer than the timeout, so
    // the session is closed and the client is told it is no longer valid.
    // -----------------------------------------------------------------------
    {
        ManualClock clock;
        auto        registry = makeRegistry(clock, timeout, 8);
        RC_ASSERT(registry->idleTimeout() == timeout);

        const Result<std::string> created = registry->create("203.0.113.7", "2025-06-18");
        RC_ASSERT(created.isOk());
        const std::string id = created.value();

        clock.advance(timeout);  // elapsed == timeout exactly
        RC_ASSERT(registry->expireIdle() == 0);
        RC_ASSERT(registry->activeCount() == 1);

        clock.advance(1s);  // elapsed == timeout + 1: strictly longer
        RC_ASSERT(registry->expireIdle() == 1);  // closed, and the closure is counted
        RC_ASSERT(registry->activeCount() == 0);

        const Result<McpSessionRecord*> afterSweep = registry->touch(id);
        RC_ASSERT(afterSweep.isError());
        RC_ASSERT(afterSweep.error().code() == ErrorCode::NotFound);
        RC_ASSERT(contains(afterSweep.error().message(), "initialize"));
        RC_ASSERT(registry->wasIssued(id));
    }

    // -----------------------------------------------------------------------
    // The same boundary through `touch()`, whose error is what a client presenting
    // the identifier actually receives, over a generated idle interval.
    // -----------------------------------------------------------------------
    const auto idleSeconds = *rc::gen::oneOf(
        rc::gen::element<std::int64_t>(0, 1, timeoutSeconds / 2, timeoutSeconds - 1,
                                       timeoutSeconds, timeoutSeconds + 1, timeoutSeconds + 2,
                                       2 * timeoutSeconds),
        rc::gen::inRange<std::int64_t>(0, 2 * timeoutSeconds + 2));

    // A generated mid-interval request, so the property also pins down what the idle
    // interval is measured FROM: `lastSeen`, refreshed by every accepted request.
    const bool         requestMidway = *rc::gen::element(true, false);
    const std::int64_t firstGap =
        requestMidway ? *rc::gen::inRange<std::int64_t>(0, idleSeconds + 1) : idleSeconds;
    const std::int64_t secondGap = idleSeconds - firstGap;

    ManualClock clock;
    auto        registry = makeRegistry(clock, timeout, 8);

    const Result<std::string> created = registry->create("203.0.113.9", "2025-06-18");
    RC_ASSERT(created.isOk());
    const std::string id = created.value();
    registry->markInitialized(id);
    RC_ASSERT(registry->activeCount() == 1);

    /// Advance `gap` seconds since the session's last accepted request and assert the
    /// outcome of presenting the identifier. Returns true while the session lives.
    const auto observeAfter = [&](std::int64_t gap) {
        clock.advance(std::chrono::seconds(gap));
        const bool expected = gap > timeoutSeconds;  // strictly longer than the timeout

        const Result<McpSessionRecord*> touched = registry->touch(id);
        if (expected) {
            // Closed, with the reason recorded in the error the client is given.
            RC_ASSERT(touched.isError());
            RC_ASSERT(touched.error().code() == ErrorCode::Timeout);
            RC_ASSERT(contains(touched.error().message(), "no longer valid"));
            RC_ASSERT(contains(touched.error().message(), std::to_string(timeoutSeconds)));
            RC_ASSERT(registry->activeCount() == 0);
            // Every subsequent request carrying it is rejected too, and the
            // identifier is never handed out again.
            const Result<McpSessionRecord*> again = registry->touch(id);
            RC_ASSERT(again.isError());
            RC_ASSERT(again.error().code() == ErrorCode::NotFound);
            RC_ASSERT(contains(again.error().message(), "initialize"));
            RC_ASSERT(registry->expireIdle() == 0);  // nothing left to close
            RC_ASSERT(registry->wasIssued(id));
            return false;
        }

        // Within budget: still established, still initialized, and the accepted
        // request has refreshed the idle interval's origin.
        RC_ASSERT(touched.isOk());
        RC_ASSERT(touched.value()->id == id);
        RC_ASSERT(touched.value()->initialized);
        RC_ASSERT(touched.value()->lastSeen == clock.now());
        RC_ASSERT(registry->activeCount() == 1);
        RC_ASSERT(registry->expireIdle() == 0);
        RC_ASSERT(registry->activeCount() == 1);
        return true;
    };

    if (observeAfter(firstGap) && requestMidway) {
        // The mid-interval request reset the clock, so the total elapsed time is
        // irrelevant and only this second gap decides.
        (void)observeAfter(secondGap);
    }
}

}  // namespace
}  // namespace palmier::services
