// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/HealthMonitor.cpp — implementation of the health and error-tracking surface
// declared in HealthMonitor.hpp.

#include "services/HealthMonitor.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace palmier::services {

std::string_view healthStateName(HealthState state) {
    switch (state) {
        case HealthState::Ok:
            return "ok";
        case HealthState::Degraded:
            return "degraded";
        case HealthState::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

std::optional<HealthState> parseHealthState(std::string_view name) {
    if (name == "ok") {
        return HealthState::Ok;
    }
    if (name == "degraded") {
        return HealthState::Degraded;
    }
    if (name == "unavailable") {
        return HealthState::Unavailable;
    }
    return std::nullopt;
}

HealthState worseOf(HealthState a, HealthState b) {
    // The enumerators are declared in increasing severity, so the larger value is the
    // worse state. Asserted by a test so a future reordering cannot silently invert it.
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

// ---------------------------------------------------------------------------
// LogTally
// ---------------------------------------------------------------------------

void LogTally::observe(const core::LogRecord& record) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.total;
    switch (record.level) {
        case core::LogLevel::Warning:
            ++counters_.warnings;
            break;
        case core::LogLevel::Error:
            ++counters_.errors;
            break;
        case core::LogLevel::Critical:
            ++counters_.critical;
            break;
        default:
            break;
    }
    if (record.level == core::LogLevel::Error || record.level == core::LogLevel::Critical) {
        // Empty categories are counted under an explicit key rather than dropped, so
        // the per-category counts always sum to the failure count.
        ++counters_.errorsByCategory[record.category.empty() ? "uncategorised" : record.category];
        counters_.lastErrorCategory = record.category;
        counters_.lastErrorMessage = record.message;
    }
}

LogCounters LogTally::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

void LogTally::reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    counters_ = LogCounters{};
}

core::LogSink makeCountingLogSink(std::shared_ptr<LogTally> tally, core::LogSink next) {
    return [tally = std::move(tally), next = std::move(next)](const core::LogRecord& record) {
        if (tally != nullptr) {
            tally->observe(record);
        }
        if (next) {
            next(record);
        }
    };
}

// ---------------------------------------------------------------------------
// HealthMonitor
// ---------------------------------------------------------------------------

HealthMonitor::HealthMonitor(std::string version, std::shared_ptr<LogTally> tally, Clock clock)
    : version_(std::move(version)),
      tally_(std::move(tally)),
      clock_(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}),
      started_(clock_()) {}

void HealthMonitor::registerProbe(std::string name, Probe probe) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(probes_.begin(), probes_.end(),
                                    [&](const Entry& e) { return e.name == name; });
    if (found != probes_.end()) {
        found->probe = std::move(probe);
        return;
    }
    probes_.push_back(Entry{std::move(name), std::move(probe)});
}

void HealthMonitor::removeProbe(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    probes_.erase(std::remove_if(probes_.begin(), probes_.end(),
                                 [&](const Entry& e) { return e.name == name; }),
                  probes_.end());
}

std::vector<std::string> HealthMonitor::probeNames() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(probes_.size());
    for (const Entry& e : probes_) {
        names.push_back(e.name);
    }
    return names;
}

HealthReport HealthMonitor::report() const {
    std::vector<Entry> probes;
    std::string version;
    std::shared_ptr<LogTally> tally;
    std::chrono::seconds uptime{0};
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        probes = probes_;
        version = version_;
        tally = tally_;
        uptime = std::chrono::duration_cast<std::chrono::seconds>(clock_() - started_);
    }

    HealthReport out;
    out.version = std::move(version);
    // A clock that went backwards would otherwise report a huge unsigned duration.
    out.uptime = uptime < std::chrono::seconds::zero() ? std::chrono::seconds::zero() : uptime;
    out.state = HealthState::Ok;
    out.checks.reserve(probes.size());
    for (const Entry& entry : probes) {
        HealthCheck check;
        check.name = entry.name;
        if (!entry.probe) {
            check.state = HealthState::Unavailable;
            check.detail = "no probe registered";
        } else {
            // A probe exists to report trouble; it must not be able to cause an outage
            // of the endpoint by throwing.
            try {
                check = entry.probe();
                check.name = entry.name;
            } catch (const std::exception& error) {
                check.state = HealthState::Unavailable;
                check.detail = std::string("probe threw: ") + error.what();
            } catch (...) {
                check.state = HealthState::Unavailable;
                check.detail = "probe threw a non-standard exception";
            }
        }
        out.state = worseOf(out.state, check.state);
        out.checks.push_back(std::move(check));
    }
    if (tally != nullptr) {
        out.logs = tally->snapshot();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Json healthReportToJson(const HealthReport& report) {
    Json checks = Json::array();
    for (const HealthCheck& check : report.checks) {
        Json entry = Json::object();
        entry.set("name", Json(check.name));
        entry.set("state", Json(std::string(healthStateName(check.state))));
        entry.set("detail", Json(check.detail));
        checks.push_back(std::move(entry));
    }

    Json byCategory = Json::object();
    for (const auto& [category, count] : report.logs.errorsByCategory) {
        byCategory.set(category, Json(static_cast<std::int64_t>(count)));
    }

    Json log = Json::object();
    log.set("total", Json(static_cast<std::int64_t>(report.logs.total)));
    log.set("warnings", Json(static_cast<std::int64_t>(report.logs.warnings)));
    log.set("errors", Json(static_cast<std::int64_t>(report.logs.errors)));
    log.set("critical", Json(static_cast<std::int64_t>(report.logs.critical)));
    log.set("errorsByCategory", std::move(byCategory));
    if (report.logs.failures() == 0) {
        log.set("lastError", Json(nullptr));
    } else {
        Json last = Json::object();
        last.set("category", Json(report.logs.lastErrorCategory));
        last.set("message", Json(report.logs.lastErrorMessage));
        log.set("lastError", std::move(last));
    }

    Json out = Json::object();
    out.set("state", Json(std::string(healthStateName(report.state))));
    out.set("version", Json(report.version));
    out.set("uptimeSeconds", Json(static_cast<std::int64_t>(report.uptime.count())));
    out.set("checks", std::move(checks));
    out.set("log", std::move(log));
    return out;
}

int healthHttpStatus(HealthState state) {
    // Degraded is 200 on purpose: no GPU and no sound card are supported ways to run
    // this editor, and a container runtime must not restart the process for either.
    return state == HealthState::Unavailable ? 503 : 200;
}

}  // namespace palmier::services
