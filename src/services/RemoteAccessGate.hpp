// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/RemoteAccessGate.hpp — the Remote_Access_Gate: its configuration value
// type, its bind-time decision and its per-request admission control
// (Requirements 10.1-10.5, 10.7-10.10, 10.12, 10.13, 16.3; design.md decision
// D4).
//
// design.md D4 gives the gate two deliberately separated responsibilities, and
// this header declares both:
//
//   * Bind-time — `validate()` turns a `RemoteAccessConfig` into the
//     `BindDecision` the transport binds. Absent configuration yields
//     `127.0.0.1:19789` (Requirement 10.1). With `enabled == true` a non-loopback
//     bind requires ALL of a syntactically valid IPv4/IPv6 literal, a bearer
//     token of 32-512 printable ASCII characters and `acknowledged == true`;
//     plus, when TLS material is configured, a certificate and key that both load
//     and form a matching pair. Any unmet prerequisite still binds loopback and
//     carries the *named* unmet prerequisites for the startup error, never the
//     token or any substring of it (Requirements 10.2, 10.3, 10.12). Enabled
//     without TLS still performs the non-loopback bind and offers exactly one
//     plaintext warning, which `takeStartupWarning()` hands out once
//     (Requirement 10.7).
//   * Per-request — `admit()` answers Allow/Deny for one request. On a
//     loopback-only binding it returns Allow unconditionally, so a request
//     carrying neither `Authorization` nor `Origin` is served exactly as it is
//     today (Requirement 10.10, the compatibility property). On a non-loopback
//     binding it checks, in order: source-address block list, bearer token
//     (constant-time compare, 401), `Origin` against the allow-list extended with
//     the bound address and the loopback hosts (403), and the session-count limit
//     for session-initiating requests (Requirements 10.4, 10.5, 10.9, 10.13).
//
// Every rejection is recorded through the `RejectionLog` sink as a UTC
// millisecond timestamp, the source address and a reason *code*. The presented
// credential is never handed to the logger, so no substring of it can appear in a
// record (Requirement 10.8).
//
// The configuration type lives here — rather than in the app layer — so the gate
// extends one declaration instead of duplicating it. `BindDecision`, the *output*
// of `validate()`, lives in `services/RemoteAccessTypes.hpp` because the MCP
// transport needs it without depending on the gate; this header includes that one
// and extends the struct there in place rather than declaring a second decision
// type.
//
// Secure by default: a default-constructed RemoteAccessConfig describes the
// loopback-only endpoint of Requirement 10.1. Nothing in the settings layer
// turns remote access on; only an explicit `true` in configuration does, and even
// then the gate (task 6.2) still has to find a valid bind address, a 32-512
// printable-ASCII bearer token and `acknowledged == true` before a non-loopback
// address is bound (Requirements 10.2, 10.3).

#ifndef PALMIER_SERVICES_REMOTEACCESSGATE_HPP
#define PALMIER_SERVICES_REMOTEACCESSGATE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "services/McpProtocolHandler.hpp"   // McpRequestContext (the admit() input)
#include "services/RemoteAccessTypes.hpp"    // BindDecision (the validate() output)

namespace palmier::services {

/// Everything the Remote_Access_Gate needs to decide (a) whether a non-loopback
/// bind is permitted at all and (b) whether an individual request is admitted.
/// Populated by `app::AppSettings`; validated by `RemoteAccessGate` (task 6.2).
struct RemoteAccessConfig {
    /// Opt-in switch. False (the default) means the MCP endpoint binds only
    /// 127.0.0.1:19789 and serves loopback clients exactly as it does today
    /// (Requirements 10.1, 10.10).
    bool enabled = false;

    /// The non-loopback address to bind when `enabled`. Must be a syntactically
    /// valid IPv4 or IPv6 literal (Requirement 10.2); empty is an unmet
    /// prerequisite, not an invitation to bind a wildcard address.
    std::string bindAddress;

    /// The port for the endpoint (loopback or not); 19789 is the documented
    /// default (Requirements 10.1, 16.2).
    std::uint16_t port = 19789;

    /// Bearer token required on every request once a non-loopback address is
    /// bound: 32 to 512 printable ASCII characters (Requirement 10.2). Never
    /// logged, never echoed in a diagnostic (Requirements 10.3, 10.8).
    std::string bearerToken;

    /// The operator's explicit acknowledgement that the endpoint is being exposed
    /// beyond loopback (Requirement 10.2).
    bool acknowledged = false;

    /// Hosts accepted in an `Origin` header. An empty list is treated as empty,
    /// i.e. only the bound address and loopback hosts are accepted
    /// (Requirement 10.5).
    std::vector<std::string> originAllowList;

    /// Optional TLS material. Both must be present, loadable and a matching pair,
    /// or the gate falls back to loopback (Requirements 10.6, 10.12).
    std::optional<std::filesystem::path> tlsCertificate;
    std::optional<std::filesystem::path> tlsPrivateKey;

    /// Concurrent MCP session ceiling, 1..32, default 8 (Requirement 10.9).
    int maxSessions = 8;

    /// Idle-session timeout, 30..3600 seconds, default 300 (Requirement 10.11).
    std::chrono::seconds idleTimeout{300};
};

// ---------------------------------------------------------------------------
// Rejection reasons and the rejection log
// ---------------------------------------------------------------------------

/// Why the gate refused a request. This is the *code* recorded in the log: it
/// classifies the refusal without reproducing anything the client presented,
/// which is what makes Requirement 10.8's "exclude the presented token value and
/// every substring of it" structurally true rather than a review obligation.
enum class RejectionReason {
    NoToken,             ///< No `Authorization` header at all (401).
    MalformedToken,      ///< Present but not a `Bearer <token>` credential (401).
    TokenMismatch,       ///< A bearer token that is not the configured one (401).
    OriginNotAllowed,    ///< `Origin` names a host outside the allow-list (403).
    SessionLimitReached, ///< The concurrent-session maximum is already reached.
    SourceBlocked,       ///< The source is inside its 60-second 401 block.
    PlaintextOnTlsPort,  ///< A plaintext request arrived on a TLS listener.
};

/// A stable, machine-readable code for `reason` — the value written to the log
/// and asserted by tests. Never derived from request content.
[[nodiscard]] std::string_view rejectionReasonCode(RejectionReason reason) noexcept;

/// A one-line human-readable explanation of `reason`, safe to return to the
/// client. It names no credential.
[[nodiscard]] std::string_view rejectionReasonMessage(RejectionReason reason) noexcept;

/// One log record. Exactly the three facts Requirement 10.8 names — a UTC
/// timestamp with millisecond precision, the source address and the reason code —
/// and deliberately nothing else: there is no field a credential could occupy.
struct RejectionRecord {
    std::chrono::system_clock::time_point timestamp{};
    std::string                           timestampUtc;   ///< `YYYY-MM-DDThh:mm:ss.sssZ`.
    std::string                           sourceAddress;
    RejectionReason                       reason = RejectionReason::NoToken;
    std::string                           reasonCode;     ///< `rejectionReasonCode(reason)`.
};

/// Format `when` as a UTC instant with millisecond precision
/// (`YYYY-MM-DDThh:mm:ss.sssZ`) — the timestamp shape Requirement 10.8 requires.
[[nodiscard]] std::string formatUtcMilliseconds(std::chrono::system_clock::time_point when);

/// The sink every rejection is written to. Implementations must be safe to call
/// from the transport's accept thread.
class RejectionLog {
public:
    virtual ~RejectionLog() = default;

    /// Record one rejection. Called synchronously from `admit()`, well inside
    /// Requirement 10.8's one-second bound.
    virtual void record(const RejectionRecord& record) = 0;
};

/// A rejection log that keeps its records in memory. Used by the tests, which
/// assert both the completeness of a record and the absence of the presented
/// credential from it.
class RecordingRejectionLog final : public RejectionLog {
public:
    void record(const RejectionRecord& record) override;

    [[nodiscard]] std::vector<RejectionRecord> records() const;
    [[nodiscard]] std::size_t                  size() const;
    void                                       clear();

private:
    mutable std::mutex           mutex_;
    std::vector<RejectionRecord> records_;
};

/// A rejection log that writes one line per rejection to an output stream — the
/// application log of Requirement 10.8. The line carries only the timestamp, the
/// source address and the reason code.
class StreamRejectionLog final : public RejectionLog {
public:
    /// `out` is borrowed and must outlive the log. Defaults to `std::clog`.
    explicit StreamRejectionLog(std::ostream& out);
    StreamRejectionLog();

    void record(const RejectionRecord& record) override;

private:
    std::mutex    mutex_;
    std::ostream* out_;
};

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------

/// The gate's answer for one request. `reason` is meaningful only when `allowed`
/// is false; an allowed admission carries HTTP 200 and no reason.
struct Admission {
    bool            allowed = true;
    int             httpStatus = 200;
    RejectionReason reason = RejectionReason::NoToken;

    [[nodiscard]] static Admission allow() { return Admission{}; }

    [[nodiscard]] static Admission deny(int httpStatus, RejectionReason reason) {
        Admission a;
        a.allowed = false;
        a.httpStatus = httpStatus;
        a.reason = reason;
        return a;
    }
};

// ---------------------------------------------------------------------------
// RemoteAccessGate
// ---------------------------------------------------------------------------

/// Admission control for the MCP endpoint: one bind-time decision, one
/// per-request decision, and a rejection record for every refusal.
///
/// Thread safety. `admit()`, `noteSessionCreated()`, `noteSessionClosed()` and
/// `noteHandshakeFailure()` are called from the transport's accept thread and are
/// internally synchronised. `validate()` returns the decision computed once at
/// construction: a bind decision is a startup fact, and reading the TLS material
/// exactly once keeps `admit()` free of file I/O.
class RemoteAccessGate {
public:
    /// Monotonic-enough time source. `std::chrono::system_clock` is used rather
    /// than the steady clock because the same instant is both the sliding-window
    /// reference and the UTC timestamp Requirement 10.8 records. Injectable so the
    /// rate-limit and block-expiry boundaries are testable without sleeping.
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    /// The bearer-token bounds of Requirement 10.2.
    static constexpr std::size_t kMinTokenLength = 32;
    static constexpr std::size_t kMaxTokenLength = 512;

    /// The rate limiter of Requirement 10.13: five consecutive 401s from one
    /// source inside a 60-second sliding window block that source for 60 seconds.
    static constexpr std::size_t          kAuthFailureThreshold = 5;
    static constexpr std::chrono::seconds kAuthFailureWindow{60};
    static constexpr std::chrono::seconds kSourceBlockDuration{60};

    /// The HTTP statuses the gate answers with (Requirements 10.4, 10.5, 10.9).
    static constexpr int kStatusUnauthorized = 401;
    static constexpr int kStatusForbidden = 403;
    static constexpr int kStatusSessionLimit = 429;
    static constexpr int kStatusBadRequest = 400;

    /// Construct over `config`, recording every rejection into `log` (borrowed;
    /// must outlive the gate). An empty `clock` means
    /// `std::chrono::system_clock::now`.
    RemoteAccessGate(RemoteAccessConfig config, RejectionLog& log);
    RemoteAccessGate(RemoteAccessConfig config, RejectionLog& log, Clock clock);

    RemoteAccessGate(const RemoteAccessGate&) = delete;
    RemoteAccessGate& operator=(const RemoteAccessGate&) = delete;

    /// The bind-time decision (Requirements 10.1-10.3, 10.7, 10.12). Pure: the
    /// same gate always answers the same decision.
    [[nodiscard]] BindDecision validate() const;

    /// Admit or refuse one request (Requirements 10.4, 10.5, 10.9, 10.10, 10.13).
    /// Every refusal is recorded in the rejection log before this returns
    /// (Requirement 10.8).
    [[nodiscard]] Admission admit(const McpRequestContext& context);

    /// Record a TLS handshake that failed because the client spoke plaintext to a
    /// TLS listener (Requirement 10.6). The transport calls this after closing the
    /// connection, so the refusal is logged even though no `HttpRequest` ever
    /// existed.
    void noteHandshakeFailure(const std::string& sourceAddress);

    /// Session accounting for the concurrent-session ceiling (Requirement 10.9).
    void noteSessionCreated();
    void noteSessionClosed();

    /// Live session count as the gate sees it.
    [[nodiscard]] std::size_t activeSessions() const;

    /// The effective concurrent-session ceiling, clamped into [1, 32].
    [[nodiscard]] std::size_t maxSessions() const noexcept { return maxSessions_; }

    /// Hand out the single plaintext startup warning of Requirement 10.7. Returns
    /// the warning the first time and `std::nullopt` on every later call, so the
    /// warning is emitted exactly once no matter how many callers ask.
    [[nodiscard]] std::optional<std::string> takeStartupWarning();

    /// True once `takeStartupWarning()` has handed the warning out.
    [[nodiscard]] bool startupWarningEmitted() const;

    /// The configuration this gate enforces. The bearer token is part of it, so
    /// callers must not log this value.
    [[nodiscard]] const RemoteAccessConfig& config() const noexcept { return config_; }

    /// True iff `text` is a syntactically valid IPv4 or IPv6 literal
    /// (Requirement 10.2). Hostnames are deliberately rejected: the requirement
    /// says literal.
    [[nodiscard]] static bool isIpLiteral(std::string_view text) noexcept;

    /// True iff `text` is an IPv4/IPv6 loopback literal or a loopback host name.
    [[nodiscard]] static bool isLoopbackLiteral(std::string_view text) noexcept;

    /// True iff `token` satisfies Requirement 10.2: 32 to 512 characters, every
    /// one of them printable ASCII (0x20-0x7E).
    [[nodiscard]] static bool isAcceptableBearerToken(std::string_view token) noexcept;

    /// Extract the host of an `Origin` header value: `http://host:port` and
    /// `[::1]:port` forms yield `host` / `::1`, and a bare host yields itself.
    /// Lower-cased, so comparison is case-insensitive as host names require.
    [[nodiscard]] static std::string originHost(std::string_view origin);

    /// Compare two byte strings without an early exit on the first difference, so
    /// the time taken does not reveal how much of a candidate token was correct
    /// (Requirement 10.4's "byte-identical" check, design.md D4's constant-time
    /// compare).
    [[nodiscard]] static bool constantTimeEquals(std::string_view lhs,
                                                 std::string_view rhs) noexcept;

private:
    struct SourceState {
        std::vector<std::chrono::system_clock::time_point> authFailures;
        std::optional<std::chrono::system_clock::time_point> blockedUntil;
    };

    /// The one place a rejection becomes an `Admission` plus a log record. The
    /// presented credential is not a parameter, so it cannot reach the log.
    [[nodiscard]] Admission reject(const std::string& sourceAddress, int httpStatus,
                                   RejectionReason reason);

    /// Compute the bind decision from `config_`. Called once, from the ctor.
    [[nodiscard]] BindDecision computeDecision() const;

    /// Requirement 10.13's sliding window. Preconditions: `mutex_` held.
    void noteAuthFailureLocked(const std::string& sourceAddress,
                               std::chrono::system_clock::time_point now);
    [[nodiscard]] bool isBlockedLocked(const std::string& sourceAddress,
                                       std::chrono::system_clock::time_point now);

    [[nodiscard]] std::chrono::system_clock::time_point now() const;

    /// True iff `origin` names a host the configuration accepts (Requirement 10.5).
    [[nodiscard]] bool originAllowed(std::string_view origin) const;

    RemoteAccessConfig config_;
    RejectionLog*      log_;
    Clock              clock_;
    BindDecision       decision_;
    std::size_t        maxSessions_;

    mutable std::mutex                                mutex_;
    std::unordered_map<std::string, SourceState>      sources_;
    std::size_t                                       activeSessions_ = 0;
    bool                                              warningTaken_ = false;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_REMOTEACCESSGATE_HPP
