// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpSessionRegistry.hpp — MCP session identity, the concurrent-session
// maximum and the idle timeout (task 5.1; Requirements 9.10, 9.11, 9.14, 9.15,
// 10.9, 10.11).
//
// design.md D3, "Session management": the registry mints a session on a
// successful `initialize` — a 256-bit value drawn from `std::random_device` and
// rendered as 64 lowercase hexadecimal characters, which satisfies Requirement
// 9.11's "opaque string of at least 32 characters" — returns it in the
// `Mcp-Session-Id` response header, and accepts it in the same request header
// thereafter. It tracks `initialized`, `createdAt` and `lastSeen`, enforces the
// configurable concurrent-session maximum (Requirement 10.9) and the
// configurable idle timeout (Requirement 10.11).
//
// Uniqueness for the whole process lifetime (Requirement 9.11: "unique among all
// sessions active since application start") is guaranteed structurally: every id
// the registry has *ever* issued is retained in a set that is never pruned, and
// minting retries until it lands outside that set. Only the *live* records
// expire, so an expired session's identifier can never be handed out again and a
// client presenting it is told to repeat `initialize` (Requirement 9.15) rather
// than silently adopted into someone else's session.
//
// Timing is injected. `Options::clock` supplies the monotonic time source, so the
// expiry tests are deterministic — they advance a variable instead of sleeping —
// and so the idle-timeout property test (task 6.5) can drive the boundary exactly.
//
// Expiry boundary, stated precisely because the property tests depend on it:
// Requirement 10.11 closes a session that "has received no request for LONGER
// than the configured idle timeout", so the comparison is strict. A record whose
// idle interval is exactly the timeout is still live; the first observation
// strictly past it expires the record.
//
// Thread safety. The registry is the one piece of MCP state touched from the
// accept thread (design.md D5 lists it as mutex-guarded), so every member is
// internally synchronised. `touch()` returns a pointer into the registry's own
// storage; records live in a node-based map so the pointer stays valid until that
// session is closed or expires.
//
// Dependency-light: standard library plus core Result/Error. No sockets, no JSON,
// no transport.

#ifndef PALMIER_SERVICES_MCPSESSIONREGISTRY_HPP
#define PALMIER_SERVICES_MCPSESSIONREGISTRY_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/Result.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// McpSessionRecord
// ---------------------------------------------------------------------------

/// One live MCP session. `initialized` becomes true when the client's
/// `notifications/initialized` arrives (Requirement 9.10); until then
/// `tools/list` and `tools/call` on this session are refused (Requirement 9.14).
struct McpSessionRecord {
    std::string                           id;
    bool                                  initialized = false;
    std::string                           protocolVersion;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point lastSeen;
    std::string                           sourceAddress;
};

// ---------------------------------------------------------------------------
// McpSessionRegistry
// ---------------------------------------------------------------------------

/// Mints, validates, expires and counts MCP sessions.
class McpSessionRegistry {
public:
    /// Monotonic time source. Injectable so expiry is testable without sleeping.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// The configurable bounds of Requirements 10.9 and 10.11, and the width of a
    /// minted identifier (64 hex characters == 256 bits, comfortably above
    /// Requirement 9.11's 32-character floor).
    static constexpr std::size_t          kMinSessions = 1;
    static constexpr std::size_t          kMaxSessions = 32;
    static constexpr std::size_t          kDefaultMaxSessions = 8;
    static constexpr std::chrono::seconds kMinIdleTimeout{30};
    static constexpr std::chrono::seconds kMaxIdleTimeout{3600};
    static constexpr std::chrono::seconds kDefaultIdleTimeout{300};
    static constexpr std::size_t          kSessionIdLength = 64;

    /// Tuning knobs. `maxSessions` is clamped into [1, 32] and `idleTimeout` into
    /// [30 s, 3600 s] rather than rejected, so a misconfigured value degrades to
    /// the nearest permitted one instead of failing startup; the effective values
    /// are observable through `maxSessions()` / `idleTimeout()`. An empty `clock`
    /// means `std::chrono::steady_clock::now`.
    struct Options {
        std::size_t          maxSessions = kDefaultMaxSessions;
        std::chrono::seconds idleTimeout = kDefaultIdleTimeout;
        Clock                clock;
    };

    /// Default: 8 concurrent sessions, a 300-second idle timeout, the steady clock.
    McpSessionRegistry();

    explicit McpSessionRegistry(Options options);

    McpSessionRegistry(const McpSessionRegistry&) = delete;
    McpSessionRegistry& operator=(const McpSessionRegistry&) = delete;

    /// Mint a session for a client at `sourceAddress` that negotiated
    /// `protocolVersion` (Requirements 9.11, 10.9).
    ///
    /// Idle sessions are expired first, so a slot freed by a timeout is available
    /// immediately. When the concurrent maximum is still reached the request — and
    /// only that request — is refused with a FailedPrecondition error naming the
    /// limit; every established session stays active (Requirement 10.9).
    [[nodiscard]] Result<std::string> create(std::string sourceAddress,
                                             std::string protocolVersion);

    /// Look up `id`, refresh its `lastSeen`, and return the record.
    ///
    ///   * NotFound — no live session carries that identifier (never issued, or
    ///     already closed): the client must repeat `initialize` (Requirement 9.15).
    ///   * Timeout  — the session existed but had been idle longer than the
    ///     timeout; it is closed by this call and the client must repeat
    ///     `initialize` (Requirements 9.15, 10.11).
    ///
    /// The returned pointer is owned by the registry and remains valid until that
    /// session is closed or expires.
    [[nodiscard]] Result<McpSessionRecord*> touch(std::string_view id);

    /// Mark the session `id` as having completed initialization (Requirement
    /// 9.10). A no-op when no live session carries that identifier.
    void markInitialized(std::string_view id);

    /// Close every session idle for longer than the timeout and return how many
    /// were closed. Called before every admission decision (design.md D3).
    std::size_t expireIdle();

    /// Number of live sessions (does not itself expire anything).
    [[nodiscard]] std::size_t activeCount() const;

    /// Close the session `id`. True when a live session was closed. The
    /// identifier stays retained as issued, so it is never minted again.
    bool close(std::string_view id);

    /// True iff `id` was ever issued by this registry, live or not — the
    /// process-lifetime uniqueness record of Requirement 9.11.
    [[nodiscard]] bool wasIssued(std::string_view id) const;

    /// How many identifiers this registry has issued since construction.
    [[nodiscard]] std::size_t issuedCount() const;

    /// The effective (clamped) configuration.
    [[nodiscard]] std::size_t          maxSessions() const noexcept { return maxSessions_; }
    [[nodiscard]] std::chrono::seconds idleTimeout() const noexcept { return idleTimeout_; }

    /// True iff `id` has the shape this registry mints: exactly
    /// `kSessionIdLength` lowercase hexadecimal characters. Exposed so the
    /// transport and the tests can assert the opacity contract of Requirement
    /// 9.11 without knowing how an identifier is produced.
    [[nodiscard]] static bool isWellFormedId(std::string_view id) noexcept;

private:
    /// Expire idle records. Precondition: `mutex_` held.
    std::size_t expireIdleLocked(std::chrono::steady_clock::time_point now);

    /// `now` from the injected clock (or the steady clock).
    [[nodiscard]] std::chrono::steady_clock::time_point now() const;

    /// A fresh 64-hex-character identifier that has never been issued before.
    /// Precondition: `mutex_` held.
    [[nodiscard]] Result<std::string> mintIdLocked();

    mutable std::mutex                                     mutex_;
    std::unordered_map<std::string, McpSessionRecord>       live_;
    std::unordered_set<std::string>                        issued_;  // never pruned
    std::size_t                                            maxSessions_;
    std::chrono::seconds                                   idleTimeout_;
    Clock                                                  clock_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_MCPSESSIONREGISTRY_HPP
