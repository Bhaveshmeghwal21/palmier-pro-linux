// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/HealthMonitor.hpp — whether this process is well, and what has gone wrong.
//
// Two things the editor could not answer about itself before:
//
//   1. "Is it up, and is anything degraded?" — asked by a container runtime, an
//      orchestrator, or a person before they trust a long export to it.
//   2. "Has anything failed, and how often?" — asked after the fact. The structured
//      logger records failures one at a time, but nothing counted them, so a hundred
//      decode errors and one looked the same from outside.
//
// The design keeps both answers cheap and honest:
//
//   * A component reports its own readiness through a registered callback rather than
//     the monitor reaching into it. The monitor knows names and states, not internals,
//     so adding a component does not change this file.
//   * Error tracking is a DECORATOR over the existing log sink
//     (`makeCountingLogSink`), not a parallel reporting path. Anything already logged
//     is counted by construction, and there is no way to record an error that the log
//     does not also carry.
//   * The clock is injected, so uptime is testable without sleeping.
//   * Everything here is thread-safe: logging happens on whatever thread failed, and
//     health is read from the MCP server's thread.
//
// What is deliberately NOT here: nothing in the report identifies a user, a file path,
// a token or a host. The document is served over the MCP endpoint, so it is written to
// be safe to read — see docs/OBSERVABILITY.md.

#ifndef PALMIER_SERVICES_HEALTHMONITOR_HPP
#define PALMIER_SERVICES_HEALTHMONITOR_HPP

#include "core/Logging.hpp"
#include "services/Json.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palmier::services {

/// How well a component, or the process as a whole, is doing.
///
/// `Degraded` is the interesting one: it means "working, with something missing that a
/// user would notice" — no GPU so compositing is on the CPU, or no audio device. Those
/// are supported states of this editor rather than failures, so they must be
/// distinguishable from `Unavailable`, which means the thing cannot do its job at all.
enum class HealthState {
    Ok,
    Degraded,
    Unavailable,
};

/// The lower-case wire name of `state`, as it appears in the served document.
[[nodiscard]] std::string_view healthStateName(HealthState state);

/// Parse a wire name back to a state; nullopt when it is not one of the three.
[[nodiscard]] std::optional<HealthState> parseHealthState(std::string_view name);

/// The worse of two states, with Unavailable worst. Used to roll component states up
/// into the overall one.
[[nodiscard]] HealthState worseOf(HealthState a, HealthState b);

/// One component's answer about itself. `detail` is a short human-readable phrase and
/// may be empty when the state is `Ok`.
struct HealthCheck {
    std::string name;
    HealthState state = HealthState::Ok;
    std::string detail;
};

/// What the log has carried since the process started.
///
/// `errorsByCategory` is what makes this useful rather than decorative: knowing there
/// were forty errors is much less use than knowing all forty were in `media.decode`.
struct LogCounters {
    std::size_t total = 0;
    std::size_t warnings = 0;
    std::size_t errors = 0;
    std::size_t critical = 0;
    std::map<std::string, std::size_t> errorsByCategory;
    std::string lastErrorCategory;
    std::string lastErrorMessage;

    /// Errors and criticals together — the number most callers actually want.
    [[nodiscard]] std::size_t failures() const { return errors + critical; }
};

/// Counts log records as they pass. Thread-safe: records arrive on whichever thread
/// logged them.
class LogTally {
public:
    /// Count one record. Cheap and non-throwing in the common case.
    void observe(const core::LogRecord& record);

    /// A consistent copy of the counters.
    [[nodiscard]] LogCounters snapshot() const;

    /// Forget everything counted so far. For tests, and for a caller that wants
    /// "errors since I last looked".
    void reset();

private:
    mutable std::mutex mutex_;
    LogCounters counters_;
};

/// Wrap `next` so every record it carries is also counted into `tally`.
///
/// A decorator rather than a second sink because ordering matters: the record is
/// counted and then forwarded, so a crash between the two cannot lose the count of a
/// failure that was already written. A null `next` is allowed and simply counts.
[[nodiscard]] core::LogSink makeCountingLogSink(std::shared_ptr<LogTally> tally,
                                                core::LogSink next);

/// The whole answer, at one instant.
struct HealthReport {
    HealthState state = HealthState::Ok;
    std::string version;
    std::chrono::seconds uptime{0};
    std::vector<HealthCheck> checks;
    LogCounters logs;
};

/// Aggregates component readiness and log counters into a report.
///
/// The monitor owns no component and starts nothing. It holds callbacks that each
/// answer for one component, which is why it can live in `services` without dragging
/// in the GPU, media or UI layers.
class HealthMonitor {
public:
    /// Injected so uptime can be asserted exactly rather than slept for.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// A component's own answer about itself, asked on every report.
    using Probe = std::function<HealthCheck()>;

    /// `version` is reported verbatim; `tally` may be null, in which case the counters
    /// are reported as zero. `clock` defaults to the steady clock.
    explicit HealthMonitor(std::string version, std::shared_ptr<LogTally> tally = nullptr,
                           Clock clock = nullptr);

    /// Register a component's probe. Registering the same name twice replaces it, so a
    /// component that is rebuilt does not appear twice.
    void registerProbe(std::string name, Probe probe);

    /// Remove a probe. Safe when the name was never registered.
    void removeProbe(std::string_view name);

    /// The names of the registered probes, in registration order.
    [[nodiscard]] std::vector<std::string> probeNames() const;

    /// Ask every probe and roll the answers up.
    ///
    /// A probe that throws is reported as `Unavailable` with the exception's message
    /// rather than propagating: a broken probe must not be able to take down the
    /// endpoint that exists to report brokenness.
    [[nodiscard]] HealthReport report() const;

private:
    struct Entry {
        std::string name;
        Probe probe;
    };

    mutable std::mutex mutex_;
    std::string version_;
    std::shared_ptr<LogTally> tally_;
    Clock clock_;
    std::chrono::steady_clock::time_point started_;
    std::vector<Entry> probes_;
};

/// Render a report as the served document.
///
/// Shape (stable, documented in docs/OBSERVABILITY.md):
///
///     {"state":"ok","version":"0.3.0","uptimeSeconds":41,
///      "checks":[{"name":"gpu","state":"degraded","detail":"no Vulkan device"}],
///      "log":{"total":12,"warnings":1,"errors":0,"critical":0,
///             "errorsByCategory":{},"lastError":null}}
[[nodiscard]] Json healthReportToJson(const HealthReport& report);

/// The HTTP status a state should be served with.
///
/// `Degraded` is deliberately 200: a container must not be restarted because the host
/// has no GPU, which is a supported way to run this editor. Only `Unavailable` is 503.
[[nodiscard]] int healthHttpStatus(HealthState state);

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_HEALTHMONITOR_HPP
