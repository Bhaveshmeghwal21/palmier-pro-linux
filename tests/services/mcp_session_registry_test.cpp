// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_session_registry_test.cpp — unit tests for
// services::McpSessionRegistry (task 5.1; Requirements 9.10, 9.11, 9.14, 9.15,
// 10.9, 10.11).
//
// The registry is the MCP endpoint's identity store: it mints a session on a
// successful `initialize`, validates the identifier presented on every later
// request, enforces the concurrent-session maximum and the idle timeout, and
// guarantees that an identifier is never reused for the whole process lifetime.
// These example-based tests pin each of those behaviours, including the exact
// boundaries; the universally quantified counterparts (Properties 49, 51, 55, 57)
// join this same target from tasks 5.5 and 6.5.
//
// Time is injected, so nothing here sleeps: the tests move a variable and observe
// the registry's decision at, and just past, the timeout.

#include "services/McpSessionRegistry.hpp"

#include <chrono>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace palmier::services {
namespace {

using namespace std::chrono_literals;

/// A manually advanced monotonic clock.
class ManualClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return now_; }
    void advance(std::chrono::seconds delta) { now_ += delta; }

    [[nodiscard]] McpSessionRegistry::Clock source() {
        return [this] { return now_; };
    }

private:
    std::chrono::steady_clock::time_point now_ = std::chrono::steady_clock::time_point{} + 1h;
};

McpSessionRegistry::Options optionsWith(ManualClock& clock, std::size_t maxSessions,
                                        std::chrono::seconds idleTimeout) {
    McpSessionRegistry::Options options;
    options.maxSessions = maxSessions;
    options.idleTimeout = idleTimeout;
    options.clock = clock.source();
    return options;
}

// ---------------------------------------------------------------------------
// Identifier shape and uniqueness (Requirement 9.11)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryIdentity, MintedIdIs64LowercaseHexCharacters) {
    McpSessionRegistry registry;
    const Result<std::string> id = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(id.isOk()) << id.error().toString();

    EXPECT_EQ(id.value().size(), 64u);  // 256 bits, well above the 32-char floor
    EXPECT_TRUE(McpSessionRegistry::isWellFormedId(id.value())) << id.value();
    for (const char c : id.value()) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(hex) << "identifier carries a non-lowercase-hex character: " << c;
    }
}

TEST(McpSessionRegistryIdentity, IdsAreUniqueEvenAfterSessionsAreClosed) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, /*maxSessions=*/2, 300s));

    std::set<std::string> seen;
    for (int round = 0; round < 40; ++round) {
        const Result<std::string> id = registry.create("127.0.0.1", "2025-06-18");
        ASSERT_TRUE(id.isOk()) << id.error().toString();
        EXPECT_TRUE(seen.insert(id.value()).second)
            << "identifier reissued after the session was closed: " << id.value();
        // Free the slot again, so only the retained-issued set can preserve
        // uniqueness across rounds.
        EXPECT_TRUE(registry.close(id.value()));
        EXPECT_TRUE(registry.wasIssued(id.value()));
    }
    EXPECT_EQ(registry.issuedCount(), 40u);
    EXPECT_EQ(registry.activeCount(), 0u);
}

TEST(McpSessionRegistryIdentity, MalformedIdsAreNotWellFormed) {
    EXPECT_FALSE(McpSessionRegistry::isWellFormedId(""));
    EXPECT_FALSE(McpSessionRegistry::isWellFormedId(std::string(63, 'a')));
    EXPECT_FALSE(McpSessionRegistry::isWellFormedId(std::string(65, 'a')));
    EXPECT_FALSE(McpSessionRegistry::isWellFormedId(std::string(64, 'A')));  // upper case
    EXPECT_FALSE(McpSessionRegistry::isWellFormedId(std::string(64, 'g')));  // not hex
    EXPECT_TRUE(McpSessionRegistry::isWellFormedId(std::string(64, 'f')));
}

// ---------------------------------------------------------------------------
// Record contents and the initialized flag (Requirements 9.10, 9.14)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryRecord, CarriesSourceVersionAndTimestamps) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, 8, 300s));

    const Result<std::string> id = registry.create("10.1.2.3", "2025-03-26");
    ASSERT_TRUE(id.isOk());

    const Result<McpSessionRecord*> record = registry.touch(id.value());
    ASSERT_TRUE(record.isOk()) << record.error().toString();
    EXPECT_EQ(record.value()->id, id.value());
    EXPECT_EQ(record.value()->sourceAddress, "10.1.2.3");
    EXPECT_EQ(record.value()->protocolVersion, "2025-03-26");
    EXPECT_EQ(record.value()->createdAt, clock.now());
    EXPECT_EQ(record.value()->lastSeen, clock.now());
    EXPECT_FALSE(record.value()->initialized);  // until notifications/initialized
}

TEST(McpSessionRegistryRecord, MarkInitializedFlipsTheFlagAndIgnoresUnknownIds) {
    McpSessionRegistry registry;
    const Result<std::string> id = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(id.isOk());

    registry.markInitialized(std::string(64, '0'));  // unknown -> no-op, no crash
    const Result<McpSessionRecord*> before = registry.touch(id.value());
    ASSERT_TRUE(before.isOk());
    EXPECT_FALSE(before.value()->initialized);

    registry.markInitialized(id.value());
    const Result<McpSessionRecord*> after = registry.touch(id.value());
    ASSERT_TRUE(after.isOk());
    EXPECT_TRUE(after.value()->initialized);
}

// ---------------------------------------------------------------------------
// touch(): NotFound and expiry (Requirements 9.15, 10.11)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryTouch, UnknownIdentifierIsNotFound) {
    McpSessionRegistry registry;
    const Result<McpSessionRecord*> record = registry.touch(std::string(64, 'a'));
    ASSERT_TRUE(record.isError());
    EXPECT_EQ(record.error().code(), ErrorCode::NotFound);
    EXPECT_NE(record.error().message().find("initialize"), std::string::npos);
}

TEST(McpSessionRegistryTouch, ExpiresExactlyPastTheIdleTimeout) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, 8, /*idleTimeout=*/60s));

    const Result<std::string> id = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(id.isOk());

    // Exactly at the timeout the session is still live (Requirement 10.11 closes a
    // session idle for LONGER than the timeout).
    clock.advance(60s);
    ASSERT_TRUE(registry.touch(id.value()).isOk());

    // The touch above refreshed lastSeen; one second past the timeout now expires.
    clock.advance(61s);
    const Result<McpSessionRecord*> expired = registry.touch(id.value());
    ASSERT_TRUE(expired.isError());
    EXPECT_EQ(expired.error().code(), ErrorCode::Timeout);
    EXPECT_NE(expired.error().message().find("initialize"), std::string::npos);

    // The expired session is gone, and a second attempt reports NotFound.
    EXPECT_EQ(registry.activeCount(), 0u);
    const Result<McpSessionRecord*> again = registry.touch(id.value());
    ASSERT_TRUE(again.isError());
    EXPECT_EQ(again.error().code(), ErrorCode::NotFound);
}

TEST(McpSessionRegistryTouch, ActivityKeepsASessionAliveIndefinitely) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, 8, 60s));
    const Result<std::string> id = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(id.isOk());

    for (int step = 0; step < 20; ++step) {
        clock.advance(59s);
        ASSERT_TRUE(registry.touch(id.value()).isOk()) << "expired at step " << step;
    }
    EXPECT_EQ(registry.activeCount(), 1u);
}

// ---------------------------------------------------------------------------
// expireIdle() (Requirement 10.11)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryExpiry, ExpireIdleClosesOnlyTheIdleSessions) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, 8, 60s));

    const Result<std::string> stale = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(stale.isOk());
    clock.advance(50s);
    const Result<std::string> fresh = registry.create("127.0.0.2", "2025-06-18");
    ASSERT_TRUE(fresh.isOk());
    EXPECT_EQ(registry.activeCount(), 2u);

    clock.advance(11s);  // stale: 61 s idle, fresh: 11 s idle
    EXPECT_EQ(registry.expireIdle(), 1u);
    EXPECT_EQ(registry.activeCount(), 1u);
    EXPECT_TRUE(registry.touch(fresh.value()).isOk());
    EXPECT_TRUE(registry.touch(stale.value()).isError());

    EXPECT_EQ(registry.expireIdle(), 0u);  // nothing left to expire
}

// ---------------------------------------------------------------------------
// The concurrent-session maximum (Requirement 10.9)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryLimit, RejectsOnlyTheExcessRequestAndKeepsSessionsActive) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, /*maxSessions=*/2, 300s));

    const Result<std::string> first = registry.create("127.0.0.1", "2025-06-18");
    const Result<std::string> second = registry.create("127.0.0.2", "2025-06-18");
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(second.isOk());

    const Result<std::string> excess = registry.create("127.0.0.3", "2025-06-18");
    ASSERT_TRUE(excess.isError());
    EXPECT_EQ(excess.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_NE(excess.error().message().find("limit"), std::string::npos);

    // Both established sessions are untouched and still usable.
    EXPECT_EQ(registry.activeCount(), 2u);
    EXPECT_TRUE(registry.touch(first.value()).isOk());
    EXPECT_TRUE(registry.touch(second.value()).isOk());
}

TEST(McpSessionRegistryLimit, AnExpiredSlotIsImmediatelyReusable) {
    ManualClock        clock;
    McpSessionRegistry registry(optionsWith(clock, /*maxSessions=*/1, 60s));

    const Result<std::string> first = registry.create("127.0.0.1", "2025-06-18");
    ASSERT_TRUE(first.isOk());
    ASSERT_TRUE(registry.create("127.0.0.2", "2025-06-18").isError());

    clock.advance(61s);  // the only session goes idle past the timeout
    const Result<std::string> replacement = registry.create("127.0.0.2", "2025-06-18");
    ASSERT_TRUE(replacement.isOk()) << replacement.error().toString();
    EXPECT_NE(replacement.value(), first.value());
    EXPECT_EQ(registry.activeCount(), 1u);
}

// ---------------------------------------------------------------------------
// Option clamping (Requirements 10.9, 10.11)
// ---------------------------------------------------------------------------

TEST(McpSessionRegistryOptions, DefaultsAreEightSessionsAndThreeHundredSeconds) {
    McpSessionRegistry registry;
    EXPECT_EQ(registry.maxSessions(), 8u);
    EXPECT_EQ(registry.idleTimeout(), 300s);
}

TEST(McpSessionRegistryOptions, OutOfRangeValuesAreClampedToThePermittedBounds) {
    McpSessionRegistry::Options tooSmall;
    tooSmall.maxSessions = 0;
    tooSmall.idleTimeout = 1s;
    const McpSessionRegistry low(tooSmall);
    EXPECT_EQ(low.maxSessions(), 1u);
    EXPECT_EQ(low.idleTimeout(), 30s);

    McpSessionRegistry::Options tooLarge;
    tooLarge.maxSessions = 1000;
    tooLarge.idleTimeout = 100000s;
    const McpSessionRegistry high(tooLarge);
    EXPECT_EQ(high.maxSessions(), 32u);
    EXPECT_EQ(high.idleTimeout(), 3600s);
}

}  // namespace
}  // namespace palmier::services
