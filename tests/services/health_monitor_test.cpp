// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/health_monitor_test.cpp — the health and error-tracking surface.
//
// A health endpoint that lies is worse than no endpoint at all, because it is trusted.
// So the cases that matter here are the ones where the monitor could plausibly lie: a
// probe that throws, a clock that goes backwards, a component that is degraded rather
// than broken, and failures that get counted twice or not at all.

#include "services/HealthMonitor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace palmier::services {
namespace {

core::LogRecord recordAt(core::LogLevel level, std::string category, std::string message) {
    core::LogRecord record;
    record.level = level;
    record.category = std::move(category);
    record.message = std::move(message);
    return record;
}

// A controllable steady clock, so uptime is asserted exactly rather than slept for.
struct ManualClock {
    std::chrono::steady_clock::time_point now{};

    std::chrono::steady_clock::time_point operator()() const { return now; }
};

}  // namespace

// ---------------------------------------------------------------------------
// State vocabulary
// ---------------------------------------------------------------------------

TEST(HealthState, EveryStateRoundTripsThroughItsWireName) {
    for (const HealthState state :
         {HealthState::Ok, HealthState::Degraded, HealthState::Unavailable}) {
        const std::string_view name = healthStateName(state);
        EXPECT_FALSE(name.empty());
        const std::optional<HealthState> parsed = parseHealthState(name);
        ASSERT_TRUE(parsed.has_value()) << "state name did not parse: " << name;
        EXPECT_EQ(*parsed, state);
    }
}

TEST(HealthState, AnUnknownNameDoesNotParse) {
    EXPECT_FALSE(parseHealthState("").has_value());
    EXPECT_FALSE(parseHealthState("OK").has_value());  // the wire form is lower case
    EXPECT_FALSE(parseHealthState("healthy").has_value());
}

TEST(HealthState, WorseOfPrefersTheMoreSevereStateInEveryPairing) {
    EXPECT_EQ(worseOf(HealthState::Ok, HealthState::Ok), HealthState::Ok);
    EXPECT_EQ(worseOf(HealthState::Ok, HealthState::Degraded), HealthState::Degraded);
    EXPECT_EQ(worseOf(HealthState::Degraded, HealthState::Ok), HealthState::Degraded);
    EXPECT_EQ(worseOf(HealthState::Degraded, HealthState::Unavailable), HealthState::Unavailable);
    EXPECT_EQ(worseOf(HealthState::Unavailable, HealthState::Ok), HealthState::Unavailable);
}

TEST(HealthState, DegradedIsServedAsSuccessAndOnlyUnavailableIsAFailure) {
    // Deliberate: no GPU and no sound card are supported ways to run this editor, so a
    // container runtime must not restart the process for either.
    EXPECT_EQ(healthHttpStatus(HealthState::Ok), 200);
    EXPECT_EQ(healthHttpStatus(HealthState::Degraded), 200);
    EXPECT_EQ(healthHttpStatus(HealthState::Unavailable), 503);
}

// ---------------------------------------------------------------------------
// LogTally — the error tracking
// ---------------------------------------------------------------------------

TEST(LogTally, CountsEachSeverityUnderItsOwnHeadingAndTotalsThemAll) {
    LogTally tally;
    tally.observe(recordAt(core::LogLevel::Info, "export", "started"));
    tally.observe(recordAt(core::LogLevel::Warning, "media", "slow read"));
    tally.observe(recordAt(core::LogLevel::Error, "media", "decode failed"));
    tally.observe(recordAt(core::LogLevel::Error, "media", "decode failed again"));
    tally.observe(recordAt(core::LogLevel::Critical, "gpu", "device lost"));

    const LogCounters counters = tally.snapshot();
    EXPECT_EQ(counters.total, 5u);
    EXPECT_EQ(counters.warnings, 1u);
    EXPECT_EQ(counters.errors, 2u);
    EXPECT_EQ(counters.critical, 1u);
    EXPECT_EQ(counters.failures(), 3u);
}

TEST(LogTally, AttributesFailuresToTheirCategorySoFortyErrorsInOnePlaceIsVisible) {
    LogTally tally;
    for (int i = 0; i < 40; ++i) {
        tally.observe(recordAt(core::LogLevel::Error, "media.decode", "frame dropped"));
    }
    tally.observe(recordAt(core::LogLevel::Error, "mcp", "bad request"));

    const LogCounters counters = tally.snapshot();
    ASSERT_EQ(counters.errorsByCategory.size(), 2u);
    EXPECT_EQ(counters.errorsByCategory.at("media.decode"), 40u);
    EXPECT_EQ(counters.errorsByCategory.at("mcp"), 1u);

    // The per-category counts must account for every failure, or the breakdown is
    // misleading in exactly the situation it exists for.
    std::size_t summed = 0;
    for (const auto& [category, count] : counters.errorsByCategory) {
        summed += count;
    }
    EXPECT_EQ(summed, counters.failures());
}

TEST(LogTally, AnUncategorisedFailureIsBucketedRatherThanDroppedFromTheBreakdown) {
    LogTally tally;
    tally.observe(recordAt(core::LogLevel::Error, "", "no category"));

    const LogCounters counters = tally.snapshot();
    EXPECT_EQ(counters.failures(), 1u);
    ASSERT_EQ(counters.errorsByCategory.size(), 1u);
    EXPECT_EQ(counters.errorsByCategory.at("uncategorised"), 1u);
}

TEST(LogTally, RemembersTheMostRecentFailureAndIgnoresLaterNonFailures) {
    LogTally tally;
    tally.observe(recordAt(core::LogLevel::Error, "export", "first"));
    tally.observe(recordAt(core::LogLevel::Critical, "gpu", "second"));
    tally.observe(recordAt(core::LogLevel::Info, "export", "recovered"));

    const LogCounters counters = tally.snapshot();
    EXPECT_EQ(counters.lastErrorCategory, "gpu");
    EXPECT_EQ(counters.lastErrorMessage, "second");
}

TEST(LogTally, NothingObservedMeansNoLastError) {
    const LogCounters counters = LogTally{}.snapshot();
    EXPECT_EQ(counters.failures(), 0u);
    EXPECT_TRUE(counters.lastErrorMessage.empty());
    EXPECT_TRUE(counters.errorsByCategory.empty());
}

TEST(LogTally, ResetForgetsEverythingIncludingTheBreakdown) {
    LogTally tally;
    tally.observe(recordAt(core::LogLevel::Error, "media", "boom"));
    tally.reset();

    const LogCounters counters = tally.snapshot();
    EXPECT_EQ(counters.total, 0u);
    EXPECT_EQ(counters.failures(), 0u);
    EXPECT_TRUE(counters.errorsByCategory.empty());
    EXPECT_TRUE(counters.lastErrorMessage.empty());
}

TEST(LogTally, ConcurrentObservationsAreAllCounted) {
    // Logging happens on whichever thread failed, so the tally is written from many
    // threads while health is read from the server's thread.
    auto tally = std::make_shared<LogTally>();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([tally] {
            for (int i = 0; i < kPerThread; ++i) {
                tally->observe(recordAt(core::LogLevel::Error, "concurrent", "boom"));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    const LogCounters counters = tally->snapshot();
    EXPECT_EQ(counters.total, static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(counters.errors, static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(counters.errorsByCategory.at("concurrent"),
              static_cast<std::size_t>(kThreads * kPerThread));
}

// ---------------------------------------------------------------------------
// The counting sink decorator
// ---------------------------------------------------------------------------

TEST(CountingLogSink, CountsAndStillForwardsEveryRecord) {
    auto tally = std::make_shared<LogTally>();
    std::vector<core::LogRecord> seen;
    const core::LogSink sink = makeCountingLogSink(
        tally, [&seen](const core::LogRecord& record) { seen.push_back(record); });

    sink(recordAt(core::LogLevel::Warning, "mcp", "retrying"));
    sink(recordAt(core::LogLevel::Error, "mcp", "gave up"));

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1].message, "gave up");
    EXPECT_EQ(tally->snapshot().total, 2u);
    EXPECT_EQ(tally->snapshot().failures(), 1u);
}

TEST(CountingLogSink, CountsWithNoDownstreamSinkWired) {
    auto tally = std::make_shared<LogTally>();
    const core::LogSink sink = makeCountingLogSink(tally, nullptr);
    sink(recordAt(core::LogLevel::Error, "gpu", "device lost"));
    EXPECT_EQ(tally->snapshot().failures(), 1u);
}

TEST(CountingLogSink, ANullTallyIsToleratedAndStillForwards) {
    int forwarded = 0;
    const core::LogSink sink =
        makeCountingLogSink(nullptr, [&forwarded](const core::LogRecord&) { ++forwarded; });
    sink(recordAt(core::LogLevel::Info, "app", "hello"));
    EXPECT_EQ(forwarded, 1);
}

// ---------------------------------------------------------------------------
// HealthMonitor
// ---------------------------------------------------------------------------

TEST(HealthMonitor, ReportsTheVersionItWasGivenAndNoChecksWhenNoneAreRegistered) {
    const HealthMonitor monitor("0.3.0");
    const HealthReport report = monitor.report();
    EXPECT_EQ(report.version, "0.3.0");
    EXPECT_EQ(report.state, HealthState::Ok);
    EXPECT_TRUE(report.checks.empty());
}

TEST(HealthMonitor, UptimeComesFromTheInjectedClock) {
    auto clock = std::make_shared<ManualClock>();
    clock->now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{100};
    const HealthMonitor monitor("0.3.0", nullptr, [clock] { return clock->now; });

    EXPECT_EQ(monitor.report().uptime, std::chrono::seconds{0});
    clock->now += std::chrono::seconds{41};
    EXPECT_EQ(monitor.report().uptime, std::chrono::seconds{41});
}

TEST(HealthMonitor, AClockThatGoesBackwardsReportsZeroRatherThanAnAbsurdUptime) {
    auto clock = std::make_shared<ManualClock>();
    clock->now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{100};
    const HealthMonitor monitor("0.3.0", nullptr, [clock] { return clock->now; });

    clock->now -= std::chrono::seconds{30};
    EXPECT_EQ(monitor.report().uptime, std::chrono::seconds{0});
}

TEST(HealthMonitor, RegisteringTheSameNameTwiceReplacesRatherThanDuplicates) {
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("gpu", [] { return HealthCheck{"gpu", HealthState::Ok, "first"}; });
    monitor.registerProbe("gpu",
                          [] { return HealthCheck{"gpu", HealthState::Degraded, "second"}; });

    const std::vector<std::string> names = monitor.probeNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "gpu");

    const HealthReport report = monitor.report();
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].detail, "second");
}

TEST(HealthMonitor, ProbesAreReportedInRegistrationOrder) {
    HealthMonitor monitor("0.3.0");
    for (const char* name : {"gpu", "audio", "mcp"}) {
        monitor.registerProbe(name, [name] { return HealthCheck{name, HealthState::Ok, ""}; });
    }
    const HealthReport report = monitor.report();
    ASSERT_EQ(report.checks.size(), 3u);
    EXPECT_EQ(report.checks[0].name, "gpu");
    EXPECT_EQ(report.checks[1].name, "audio");
    EXPECT_EQ(report.checks[2].name, "mcp");
}

TEST(HealthMonitor, RemovingAProbeDropsItAndRemovingAnUnknownNameIsHarmless) {
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("gpu", [] { return HealthCheck{"gpu", HealthState::Ok, ""}; });
    monitor.removeProbe("never registered");
    EXPECT_EQ(monitor.probeNames().size(), 1u);
    monitor.removeProbe("gpu");
    EXPECT_TRUE(monitor.probeNames().empty());
}

TEST(HealthMonitor, TheOverallStateIsTheWorstOfTheComponents) {
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("a", [] { return HealthCheck{"a", HealthState::Ok, ""}; });
    monitor.registerProbe(
        "b", [] { return HealthCheck{"b", HealthState::Degraded, "no Vulkan device"}; });
    EXPECT_EQ(monitor.report().state, HealthState::Degraded);

    monitor.registerProbe(
        "c", [] { return HealthCheck{"c", HealthState::Unavailable, "project store unreadable"}; });
    EXPECT_EQ(monitor.report().state, HealthState::Unavailable);
}

TEST(HealthMonitor, AProbeThatThrowsIsReportedUnavailableAndDoesNotBreakTheReport) {
    // The endpoint exists to report brokenness; a broken probe must not be able to take
    // it down.
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("healthy", [] { return HealthCheck{"healthy", HealthState::Ok, ""}; });
    monitor.registerProbe("throws",
                          []() -> HealthCheck { throw std::runtime_error("probe exploded"); });

    HealthReport report;
    ASSERT_NO_THROW(report = monitor.report());
    ASSERT_EQ(report.checks.size(), 2u);
    EXPECT_EQ(report.checks[0].state, HealthState::Ok);
    EXPECT_EQ(report.checks[1].state, HealthState::Unavailable);
    EXPECT_NE(report.checks[1].detail.find("probe exploded"), std::string::npos);
    EXPECT_EQ(report.state, HealthState::Unavailable);
}

TEST(HealthMonitor, AProbeThatThrowsANonStandardExceptionIsStillContained) {
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("rude", []() -> HealthCheck { throw 42; });

    HealthReport report;
    ASSERT_NO_THROW(report = monitor.report());
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].state, HealthState::Unavailable);
}

TEST(HealthMonitor, AnEmptyProbeIsUnavailableRatherThanSilentlyOk) {
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe("empty", nullptr);
    const HealthReport report = monitor.report();
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].state, HealthState::Unavailable);
    EXPECT_EQ(report.state, HealthState::Unavailable);
}

TEST(HealthMonitor, TheReportedCheckNameIsAlwaysTheRegisteredOne) {
    // A probe that fills in a different name would let the document disagree with the
    // registration, so the monitor overwrites it.
    HealthMonitor monitor("0.3.0");
    monitor.registerProbe(
        "gpu", [] { return HealthCheck{"something else entirely", HealthState::Ok, ""}; });
    const HealthReport report = monitor.report();
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].name, "gpu");
}

TEST(HealthMonitor, CarriesTheLogCountersWhenATallyIsWired) {
    auto tally = std::make_shared<LogTally>();
    tally->observe(recordAt(core::LogLevel::Error, "media", "decode failed"));
    const HealthMonitor monitor("0.3.0", tally);

    const HealthReport report = monitor.report();
    EXPECT_EQ(report.logs.failures(), 1u);
    EXPECT_EQ(report.logs.lastErrorCategory, "media");
}

TEST(HealthMonitor, WithNoTallyTheCountersAreZeroRatherThanAbsent) {
    const HealthReport report = HealthMonitor("0.3.0").report();
    EXPECT_EQ(report.logs.total, 0u);
    EXPECT_EQ(report.logs.failures(), 0u);
}

// ---------------------------------------------------------------------------
// The served document
// ---------------------------------------------------------------------------

TEST(HealthReportToJson, CarriesStateVersionUptimeChecksAndLogCounters) {
    HealthReport report;
    report.state = HealthState::Degraded;
    report.version = "0.3.0";
    report.uptime = std::chrono::seconds{41};
    report.checks.push_back(HealthCheck{"gpu", HealthState::Degraded, "no Vulkan device"});
    report.logs.total = 12;
    report.logs.warnings = 1;

    const Json document = healthReportToJson(report);
    ASSERT_TRUE(document.isObject());
    EXPECT_EQ(document.stringOr("state"), "degraded");
    EXPECT_EQ(document.stringOr("version"), "0.3.0");
    EXPECT_EQ(document.intOr("uptimeSeconds"), 41);

    const Json* checks = document.find("checks");
    ASSERT_NE(checks, nullptr);
    ASSERT_TRUE(checks->isArray());
    ASSERT_EQ(checks->asArray().size(), 1u);
    EXPECT_EQ(checks->asArray()[0].stringOr("name"), "gpu");
    EXPECT_EQ(checks->asArray()[0].stringOr("state"), "degraded");
    EXPECT_EQ(checks->asArray()[0].stringOr("detail"), "no Vulkan device");

    const Json* log = document.find("log");
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->intOr("total"), 12);
    EXPECT_EQ(log->intOr("warnings"), 1);
    EXPECT_EQ(log->intOr("errors"), 0);
}

TEST(HealthReportToJson, LastErrorIsNullUntilSomethingHasActuallyFailed) {
    const Json document = healthReportToJson(HealthReport{});
    const Json* log = document.find("log");
    ASSERT_NE(log, nullptr);
    const Json* lastError = log->find("lastError");
    ASSERT_NE(lastError, nullptr);
    EXPECT_TRUE(lastError->isNull());
}

TEST(HealthReportToJson, LastErrorNamesTheCategoryAndMessageOnceSomethingHasFailed) {
    HealthReport report;
    report.logs.errors = 1;
    report.logs.lastErrorCategory = "media.decode";
    report.logs.lastErrorMessage = "unsupported pixel format";
    report.logs.errorsByCategory["media.decode"] = 1;

    const Json document = healthReportToJson(report);
    const Json* log = document.find("log");
    ASSERT_NE(log, nullptr);
    const Json* lastError = log->find("lastError");
    ASSERT_NE(lastError, nullptr);
    ASSERT_TRUE(lastError->isObject());
    EXPECT_EQ(lastError->stringOr("category"), "media.decode");
    EXPECT_EQ(lastError->stringOr("message"), "unsupported pixel format");

    const Json* byCategory = log->find("errorsByCategory");
    ASSERT_NE(byCategory, nullptr);
    EXPECT_EQ(byCategory->intOr("media.decode"), 1);
}

TEST(HealthReportToJson, CarriesNoCredentialHostOrFilesystemPath) {
    // The document is served over the MCP endpoint, so what it does NOT contain is part
    // of its contract. This asserts the shape is exactly the five documented keys.
    HealthReport report;
    report.version = "0.3.0";
    report.checks.push_back(HealthCheck{"gpu", HealthState::Ok, ""});

    const Json document = healthReportToJson(report);
    std::vector<std::string> keys;
    for (const auto& [key, value] : document.asObject()) {
        keys.push_back(key);
    }
    const std::vector<std::string> expected{"state", "version", "uptimeSeconds", "checks", "log"};
    EXPECT_EQ(keys, expected);
}

}  // namespace palmier::services
