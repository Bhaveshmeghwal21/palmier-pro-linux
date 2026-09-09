// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core/logging_test.cpp — the levelled, structured Logger and its formatters.
//
// The point of core/Logging.hpp's design is that almost none of it needs a stream to
// test. A LogRecord is data; formatLogRecordAsText and formatLogRecordAsJson are pure
// functions of one; and the Logger's clock and sink are both injectable. So every
// assertion here states a whole expected value in closed form and compares it exactly.
// There is no substring matching against log output and no regular expression over a
// timestamp anywhere in this file — that is the property the design was chosen for, and
// asserting exact strings is how it gets checked rather than assumed.
//
// The clock is fixed to a known instant in the tests that render one, which is the only
// reason an exact comparison against a timestamp is possible at all.

#include "core/Logging.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using palmier::core::escapeForJson;
using palmier::core::field;
using palmier::core::formatLogRecordAsJson;
using palmier::core::formatLogRecordAsText;
using palmier::core::formatLogTimestamp;
using palmier::core::kLogFormatEnvironmentVariable;
using palmier::core::kLogLevelEnvironmentVariable;
using palmier::core::kLogLevels;
using palmier::core::LogField;
using palmier::core::LogFormat;
using palmier::core::logFormatName;
using palmier::core::Logger;
using palmier::core::LogLevel;
using palmier::core::logLevelName;
using palmier::core::LogRecord;
using palmier::core::makeNullSink;
using palmier::core::makeStreamSink;
using palmier::core::parseLogFormat;
using palmier::core::parseLogLevel;
using palmier::core::resolveLoggingConfiguration;
using palmier::core::sharedLogger;

/// A fixed instant, so a rendered timestamp can be compared to a literal.
/// 2026-09-09T07:20:00.123Z.
std::chrono::system_clock::time_point fixedInstant() {
    // Seconds since the Unix epoch for 2026-09-09T07:20:00Z, plus 123 ms.
    return std::chrono::system_clock::time_point{std::chrono::seconds{1789197600} +
                                                 std::chrono::milliseconds{123}};
}

/// A sink that records everything it is given, so assertions can be made about typed
/// records rather than about rendered text.
struct Capture {
    std::vector<LogRecord> records;

    palmier::core::LogSink sink() {
        return [this](const LogRecord& record) { records.push_back(record); };
    }
};

LogRecord recordFor(LogLevel level, std::string category, std::string message,
                    std::vector<LogField> fields = {}) {
    LogRecord record;
    record.level = level;
    record.category = std::move(category);
    record.message = std::move(message);
    record.fields = std::move(fields);
    record.time = fixedInstant();
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

TEST(LogLevelNames, EveryLevelHasADistinctNameThatParsesBackToItself) {
    std::set<std::string> names;
    for (const LogLevel level : kLogLevels) {
        const std::string name(logLevelName(level));
        EXPECT_FALSE(name.empty());
        EXPECT_TRUE(names.insert(name).second) << "duplicate level name: " << name;
        const std::optional<LogLevel> parsed = parseLogLevel(name);
        ASSERT_TRUE(parsed.has_value()) << name;
        EXPECT_EQ(*parsed, level) << name;
    }
    EXPECT_EQ(names.size(), kLogLevels.size());
}

TEST(LogLevelNames, TheLevelSetIsTheSevenThisModuleDocuments) {
    // A size assertion so adding a level without updating kLogLevels fails here rather
    // than silently leaving the new level out of every loop that iterates it.
    EXPECT_EQ(kLogLevels.size(), 7u);
    EXPECT_EQ(logLevelName(LogLevel::Trace), "trace");
    EXPECT_EQ(logLevelName(LogLevel::Debug), "debug");
    EXPECT_EQ(logLevelName(LogLevel::Info), "info");
    EXPECT_EQ(logLevelName(LogLevel::Warning), "warning");
    EXPECT_EQ(logLevelName(LogLevel::Error), "error");
    EXPECT_EQ(logLevelName(LogLevel::Critical), "critical");
    EXPECT_EQ(logLevelName(LogLevel::Off), "off");
}

TEST(ParseLogLevel, IsCaseInsensitive) {
    EXPECT_EQ(parseLogLevel("INFO"), LogLevel::Info);
    EXPECT_EQ(parseLogLevel("Warning"), LogLevel::Warning);
    EXPECT_EQ(parseLogLevel("cRiTiCaL"), LogLevel::Critical);
}

TEST(ParseLogLevel, AcceptsTheTwoCommonAliases) {
    EXPECT_EQ(parseLogLevel("warn"), LogLevel::Warning);
    EXPECT_EQ(parseLogLevel("fatal"), LogLevel::Critical);
}

TEST(ParseLogLevel, RejectsAnythingElseRatherThanDefaulting) {
    // Reported rather than silently defaulted: a typo that quietly became "info" would
    // hide output the operator explicitly asked for.
    EXPECT_FALSE(parseLogLevel("verbose").has_value());
    EXPECT_FALSE(parseLogLevel("").has_value());
    EXPECT_FALSE(parseLogLevel("inf").has_value());
    EXPECT_FALSE(parseLogLevel("2").has_value());
}

TEST(LogFormatNames, RoundTripAndAcceptPlainAsAnAliasForText) {
    EXPECT_EQ(logFormatName(LogFormat::Text), "text");
    EXPECT_EQ(logFormatName(LogFormat::Json), "json");
    EXPECT_EQ(parseLogFormat("text"), LogFormat::Text);
    EXPECT_EQ(parseLogFormat("JSON"), LogFormat::Json);
    EXPECT_EQ(parseLogFormat("plain"), LogFormat::Text);
    EXPECT_FALSE(parseLogFormat("xml").has_value());
    EXPECT_FALSE(parseLogFormat("").has_value());
}

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------

TEST(FormatLogTimestamp, RendersTheEpochExactly) {
    EXPECT_EQ(formatLogTimestamp(std::chrono::system_clock::time_point{}),
              "1970-01-01T00:00:00.000Z");
}

TEST(FormatLogTimestamp, RendersAKnownInstantExactly) {
    EXPECT_EQ(formatLogTimestamp(fixedInstant()), "2026-09-09T07:20:00.123Z");
}

TEST(FormatLogTimestamp, PadsMillisecondsToThreeDigits) {
    const auto base = std::chrono::system_clock::time_point{} + std::chrono::seconds{1};
    EXPECT_EQ(formatLogTimestamp(base + std::chrono::milliseconds{5}), "1970-01-01T00:00:01.005Z");
    EXPECT_EQ(formatLogTimestamp(base + std::chrono::milliseconds{50}), "1970-01-01T00:00:01.050Z");
    EXPECT_EQ(formatLogTimestamp(base + std::chrono::milliseconds{500}),
              "1970-01-01T00:00:01.500Z");
}

TEST(FormatLogTimestamp, AnInstantBeforeTheEpochDoesNotRenderANegativeRemainder) {
    // The millisecond remainder of a negative duration is negative; carrying it into
    // the previous second is what keeps the output a valid timestamp.
    const auto before = std::chrono::system_clock::time_point{} - std::chrono::milliseconds{1};
    EXPECT_EQ(formatLogTimestamp(before), "1969-12-31T23:59:59.999Z");
}

// ---------------------------------------------------------------------------
// JSON escaping — the part most likely to be wrong
// ---------------------------------------------------------------------------

TEST(EscapeForJson, EscapesTheCharactersThatWouldBreakAStringLiteral) {
    EXPECT_EQ(escapeForJson("plain"), "plain");
    EXPECT_EQ(escapeForJson("say \"hi\""), "say \\\"hi\\\"");
    EXPECT_EQ(escapeForJson("a\\b"), "a\\\\b");
    EXPECT_EQ(escapeForJson("line\nbreak"), "line\\nbreak");
    EXPECT_EQ(escapeForJson("tab\there"), "tab\\there");
    EXPECT_EQ(escapeForJson("cr\rhere"), "cr\\rhere");
    EXPECT_EQ(escapeForJson("bs\bhere"), "bs\\bhere");
    EXPECT_EQ(escapeForJson("ff\fhere"), "ff\\fhere");
}

TEST(EscapeForJson, EscapesOtherControlCharactersAsAUnicodeSequence) {
    // \x01 has no short escape; emitted raw it would make the line invalid JSON.
    EXPECT_EQ(escapeForJson(std::string("a\x01"
                                        "b")),
              "a\\u0001b");
    EXPECT_EQ(escapeForJson(std::string("\x1f")), "\\u001f");
}

TEST(EscapeForJson, PassesUtf8ThroughUnchanged) {
    // A JSON string carries UTF-8 directly, and a file path in a message may hold any
    // of it. Escaping these would corrupt the text for no gain.
    const std::string utf8 = "caf\xc3\xa9 \xe2\x80\x94 dash";
    EXPECT_EQ(escapeForJson(utf8), utf8);
}

TEST(EscapeForJson, AnEmptyStringEscapesToAnEmptyString) { EXPECT_EQ(escapeForJson(""), ""); }

// ---------------------------------------------------------------------------
// Text rendering
// ---------------------------------------------------------------------------

TEST(FormatLogRecordAsText, RendersEveryPartInADocumentedOrder) {
    const LogRecord record = recordFor(LogLevel::Warning, "export", "encoder fell back",
                                       {LogField{"from", "h264_nvenc"}, LogField{"to", "libx264"}});
    EXPECT_EQ(formatLogRecordAsText(record),
              "2026-09-09T07:20:00.123Z  warning  [export]  encoder fell back "
              "from=h264_nvenc to=libx264");
}

TEST(FormatLogRecordAsText, OmitsAnEmptyCategoryAndAnEmptyMessage) {
    LogRecord record = recordFor(LogLevel::Info, "", "");
    EXPECT_EQ(formatLogRecordAsText(record), "2026-09-09T07:20:00.123Z  info");

    record.category = "audio";
    EXPECT_EQ(formatLogRecordAsText(record), "2026-09-09T07:20:00.123Z  info  [audio]");
}

TEST(FormatLogRecordAsText, QuotesAFieldValueContainingASpace) {
    // Without the quoting, two key=value pairs cannot be told apart when read back.
    const LogRecord record =
        recordFor(LogLevel::Error, "mcp", "refused",
                  {LogField{"reason", "no bearer token"}, LogField{"port", "19789"}});
    EXPECT_EQ(formatLogRecordAsText(record), "2026-09-09T07:20:00.123Z  error  [mcp]  refused "
                                             "reason=\"no bearer token\" port=19789");
}

TEST(FormatLogRecordAsText, KeepsFieldsInTheOrderTheCallSiteWroteThem) {
    const LogRecord forward =
        recordFor(LogLevel::Info, "c", "m", {LogField{"zebra", "1"}, LogField{"alpha", "2"}});
    EXPECT_EQ(formatLogRecordAsText(forward),
              "2026-09-09T07:20:00.123Z  info  [c]  m zebra=1 alpha=2");
}

// ---------------------------------------------------------------------------
// JSON rendering
// ---------------------------------------------------------------------------

TEST(FormatLogRecordAsJson, RendersOneObjectOnOneLine) {
    const LogRecord record =
        recordFor(LogLevel::Critical, "startup", "cannot continue", {LogField{"code", "7"}});
    EXPECT_EQ(formatLogRecordAsJson(record),
              "{\"time\":\"2026-09-09T07:20:00.123Z\",\"level\":\"critical\","
              "\"category\":\"startup\",\"message\":\"cannot continue\","
              "\"fields\":{\"code\":\"7\"}}");
}

TEST(FormatLogRecordAsJson, OmitsTheFieldsObjectEntirelyWhenThereAreNoFields) {
    const LogRecord record = recordFor(LogLevel::Info, "audio", "sink selected");
    EXPECT_EQ(formatLogRecordAsJson(record),
              "{\"time\":\"2026-09-09T07:20:00.123Z\",\"level\":\"info\","
              "\"category\":\"audio\",\"message\":\"sink selected\"}");
}

TEST(FormatLogRecordAsJson, EscapesTheMessageTheCategoryAndBothHalvesOfEveryField) {
    const LogRecord record =
        recordFor(LogLevel::Error, "a\"b", "path C:\\x\ny", {LogField{"k\"1", "v\\2"}});
    EXPECT_EQ(formatLogRecordAsJson(record),
              "{\"time\":\"2026-09-09T07:20:00.123Z\",\"level\":\"error\","
              "\"category\":\"a\\\"b\",\"message\":\"path C:\\\\x\\ny\","
              "\"fields\":{\"k\\\"1\":\"v\\\\2\"}}");
}

TEST(FormatLogRecordAsJson, RendersEveryLevelWithItsOwnName) {
    for (const LogLevel level : kLogLevels) {
        const LogRecord record = recordFor(level, "c", "m");
        const std::string rendered = formatLogRecordAsJson(record);
        EXPECT_NE(rendered.find("\"level\":\"" + std::string(logLevelName(level)) + "\""),
                  std::string::npos)
            << rendered;
    }
}

// ---------------------------------------------------------------------------
// Thresholds
// ---------------------------------------------------------------------------

TEST(LoggerThreshold, DefaultsToInfoAndSuppressesEverythingBelowIt) {
    Logger logger;
    EXPECT_EQ(logger.threshold(), LogLevel::Info);
    EXPECT_FALSE(logger.enabled(LogLevel::Trace));
    EXPECT_FALSE(logger.enabled(LogLevel::Debug));
    EXPECT_TRUE(logger.enabled(LogLevel::Info));
    EXPECT_TRUE(logger.enabled(LogLevel::Warning));
    EXPECT_TRUE(logger.enabled(LogLevel::Error));
    EXPECT_TRUE(logger.enabled(LogLevel::Critical));
}

TEST(LoggerThreshold, EnabledAgreesWithWhetherLogActuallyEmits) {
    Capture capture;
    Logger logger(LogLevel::Warning);
    logger.setSink(capture.sink());
    for (const LogLevel level : kLogLevels) {
        const bool predicted = logger.enabled(level);
        const bool emitted = logger.log(level, "c", "m");
        EXPECT_EQ(predicted, emitted) << logLevelName(level);
    }
}

TEST(LoggerThreshold, OffSuppressesEverythingIncludingCritical) {
    Capture capture;
    Logger logger(LogLevel::Off);
    logger.setSink(capture.sink());
    for (const LogLevel level : kLogLevels) {
        EXPECT_FALSE(logger.log(level, "c", "m")) << logLevelName(level);
    }
    EXPECT_TRUE(capture.records.empty());
    EXPECT_EQ(logger.emittedCount(), 0u);
    EXPECT_EQ(logger.suppressedCount(), kLogLevels.size());
}

TEST(LoggerThreshold, ARecordAtOffIsNeverEmittedEvenAtTheMostVerboseThreshold) {
    Capture capture;
    Logger logger(LogLevel::Trace);
    logger.setSink(capture.sink());
    EXPECT_FALSE(logger.log(LogLevel::Off, "c", "m"));
    EXPECT_TRUE(capture.records.empty());
}

TEST(LoggerThreshold, RaisingTheThresholdSuppressesWhatWasPreviouslyEmitted) {
    Capture capture;
    Logger logger(LogLevel::Trace);
    logger.setSink(capture.sink());
    EXPECT_TRUE(logger.debug("c", "first"));
    logger.setThreshold(LogLevel::Error);
    EXPECT_FALSE(logger.debug("c", "second"));
    EXPECT_TRUE(logger.error("c", "third"));
    ASSERT_EQ(capture.records.size(), 2u);
    EXPECT_EQ(capture.records[0].message, "first");
    EXPECT_EQ(capture.records[1].message, "third");
}

TEST(LoggerThreshold, CountsBothWhatItEmittedAndWhatItSuppressed) {
    Logger logger(LogLevel::Info);
    logger.setSink(makeNullSink());
    EXPECT_TRUE(logger.info("c", "1"));
    EXPECT_TRUE(logger.error("c", "2"));
    EXPECT_FALSE(logger.debug("c", "3"));
    EXPECT_FALSE(logger.trace("c", "4"));
    EXPECT_EQ(logger.emittedCount(), 2u);
    EXPECT_EQ(logger.suppressedCount(), 2u);
}

// ---------------------------------------------------------------------------
// Records reach the sink intact
// ---------------------------------------------------------------------------

TEST(LoggerSink, DeliversLevelCategoryMessageAndFieldsAsTypedValues) {
    Capture capture;
    Logger logger(LogLevel::Trace);
    logger.setSink(capture.sink());
    logger.setClock([] { return fixedInstant(); });

    ASSERT_TRUE(
        logger.warning("gpu", "no device", {LogField{"fallback", "cpu"}, field("devices", 0)}));

    ASSERT_EQ(capture.records.size(), 1u);
    const LogRecord& record = capture.records.front();
    EXPECT_EQ(record.level, LogLevel::Warning);
    EXPECT_EQ(record.category, "gpu");
    EXPECT_EQ(record.message, "no device");
    EXPECT_EQ(record.time, fixedInstant());
    ASSERT_EQ(record.fields.size(), 2u);
    EXPECT_EQ(record.fields[0].key, "fallback");
    EXPECT_EQ(record.fields[0].value, "cpu");
    EXPECT_EQ(record.fields[1].key, "devices");
    EXPECT_EQ(record.fields[1].value, "0");
}

TEST(LoggerSink, EveryConvenienceMethodUsesItsOwnLevel) {
    Capture capture;
    Logger logger(LogLevel::Trace);
    logger.setSink(capture.sink());

    logger.trace("c", "t");
    logger.debug("c", "d");
    logger.info("c", "i");
    logger.warning("c", "w");
    logger.error("c", "e");
    logger.critical("c", "x");

    ASSERT_EQ(capture.records.size(), 6u);
    EXPECT_EQ(capture.records[0].level, LogLevel::Trace);
    EXPECT_EQ(capture.records[1].level, LogLevel::Debug);
    EXPECT_EQ(capture.records[2].level, LogLevel::Info);
    EXPECT_EQ(capture.records[3].level, LogLevel::Warning);
    EXPECT_EQ(capture.records[4].level, LogLevel::Error);
    EXPECT_EQ(capture.records[5].level, LogLevel::Critical);
}

TEST(LoggerSink, AnEmptySinkIsTreatedAsDiscardRatherThanArmingACrash) {
    Logger logger(LogLevel::Trace);
    logger.setSink({});
    // An empty std::function would throw std::bad_function_call when invoked.
    EXPECT_TRUE(logger.info("c", "m"));
    EXPECT_EQ(logger.emittedCount(), 1u);
}

TEST(LoggerSink, ANewlyConstructedLoggerIsSilentUntilGivenASink) {
    // The default is the null sink, so a Logger constructed incidentally by a test does
    // not write to a stream nobody asked it to touch.
    Logger logger(LogLevel::Trace);
    EXPECT_TRUE(logger.info("c", "m"));
    EXPECT_EQ(logger.emittedCount(), 1u);
}

TEST(LoggerClock, AnEmptyClockRestoresTheSystemClock) {
    Capture capture;
    Logger logger(LogLevel::Trace);
    logger.setSink(capture.sink());
    logger.setClock([] { return fixedInstant(); });
    ASSERT_TRUE(logger.info("c", "fixed"));
    logger.setClock({});
    ASSERT_TRUE(logger.info("c", "real"));

    ASSERT_EQ(capture.records.size(), 2u);
    EXPECT_EQ(capture.records[0].time, fixedInstant());
    EXPECT_NE(capture.records[1].time, fixedInstant());
}

TEST(LoggerThreadSafety, ConcurrentCallersEachLandExactlyOneRecord) {
    // The export coordinator, the envelope service and the audio engine all log from
    // off the main thread; a logger that is only safe from one thread cannot report a
    // threading problem.
    Capture capture;
    std::mutex captureGuard;
    Logger logger(LogLevel::Trace);
    logger.setSink([&capture, &captureGuard](const LogRecord& record) {
        const std::lock_guard<std::mutex> hold(captureGuard);
        capture.records.push_back(record);
    });

    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&logger] {
            for (int i = 0; i < kPerThread; ++i) {
                logger.info("concurrent", "record");
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(logger.emittedCount(), static_cast<std::size_t>(kThreads * kPerThread));
    const std::lock_guard<std::mutex> hold(captureGuard);
    EXPECT_EQ(capture.records.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

// ---------------------------------------------------------------------------
// Stream sinks
// ---------------------------------------------------------------------------

TEST(StreamSink, WritesOneLinePerRecordInTheChosenFormat) {
    std::ostringstream text;
    Logger logger(LogLevel::Trace);
    logger.setSink(makeStreamSink(text, LogFormat::Text));
    logger.setClock([] { return fixedInstant(); });
    logger.info("audio", "sink selected", {LogField{"sink", "null"}});
    logger.error("audio", "device lost");

    EXPECT_EQ(text.str(), "2026-09-09T07:20:00.123Z  info  [audio]  sink selected sink=null\n"
                          "2026-09-09T07:20:00.123Z  error  [audio]  device lost\n");
}

TEST(StreamSink, TheJsonFormatWritesOneParseableObjectPerLine) {
    std::ostringstream json;
    Logger logger(LogLevel::Trace);
    logger.setSink(makeStreamSink(json, LogFormat::Json));
    logger.setClock([] { return fixedInstant(); });
    logger.info("mcp", "listening", {LogField{"port", "19789"}});

    EXPECT_EQ(json.str(), "{\"time\":\"2026-09-09T07:20:00.123Z\",\"level\":\"info\","
                          "\"category\":\"mcp\",\"message\":\"listening\","
                          "\"fields\":{\"port\":\"19789\"}}\n");
}

TEST(NullSink, DiscardsWithoutWritingAnything) {
    std::ostringstream stream;
    Logger logger(LogLevel::Trace);
    logger.setSink(makeNullSink());
    logger.info("c", "m");
    EXPECT_TRUE(stream.str().empty());
    EXPECT_EQ(logger.emittedCount(), 1u);
}

// ---------------------------------------------------------------------------
// The field() helper
// ---------------------------------------------------------------------------

TEST(FieldHelper, RendersStringsStringViewsAndNumbers) {
    // string_view is the interesting case: std::string is constructible from it but not
    // convertible from it, so an is_convertible test would send it to std::to_string.
    EXPECT_EQ(field("k", std::string("v")).value, "v");
    EXPECT_EQ(field("k", std::string_view("v")).value, "v");
    EXPECT_EQ(field("k", "v").value, "v");
    EXPECT_EQ(field("k", 42).value, "42");
    EXPECT_EQ(field("k", -1).value, "-1");
    EXPECT_EQ(field("k", static_cast<std::size_t>(7)).value, "7");
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(ResolveLoggingConfiguration, DefaultsToInfoAndTextWhenNothingIsSet) {
    const auto resolved = resolveLoggingConfiguration(std::nullopt, std::nullopt);
    EXPECT_EQ(resolved.level, LogLevel::Info);
    EXPECT_EQ(resolved.format, LogFormat::Text);
    EXPECT_TRUE(resolved.ok());
    EXPECT_TRUE(resolved.diagnostics.empty());
}

TEST(ResolveLoggingConfiguration, AppliesBothValuesWhenBothParse) {
    const auto resolved = resolveLoggingConfiguration("debug", "json");
    EXPECT_EQ(resolved.level, LogLevel::Debug);
    EXPECT_EQ(resolved.format, LogFormat::Json);
    EXPECT_TRUE(resolved.ok());
}

TEST(ResolveLoggingConfiguration, TreatsAnEmptyStringAsUnset) {
    const auto resolved = resolveLoggingConfiguration("", "");
    EXPECT_EQ(resolved.level, LogLevel::Info);
    EXPECT_EQ(resolved.format, LogFormat::Text);
    EXPECT_TRUE(resolved.ok());
}

TEST(ResolveLoggingConfiguration, ReportsAnUnparseableLevelAndKeepsTheDefault) {
    const auto resolved = resolveLoggingConfiguration("verbose", std::nullopt);
    EXPECT_EQ(resolved.level, LogLevel::Info);
    EXPECT_FALSE(resolved.ok());
    ASSERT_EQ(resolved.diagnostics.size(), 1u);
    // The diagnostic must name the variable, the rejected value and the alternatives,
    // because its whole job is to let an operator fix a typo without reading the source.
    EXPECT_NE(resolved.diagnostics[0].find(kLogLevelEnvironmentVariable), std::string::npos);
    EXPECT_NE(resolved.diagnostics[0].find("verbose"), std::string::npos);
    EXPECT_NE(resolved.diagnostics[0].find("critical"), std::string::npos);
}

TEST(ResolveLoggingConfiguration, ReportsAnUnparseableFormatAndKeepsTheDefault) {
    const auto resolved = resolveLoggingConfiguration(std::nullopt, "yaml");
    EXPECT_EQ(resolved.format, LogFormat::Text);
    EXPECT_FALSE(resolved.ok());
    ASSERT_EQ(resolved.diagnostics.size(), 1u);
    EXPECT_NE(resolved.diagnostics[0].find(kLogFormatEnvironmentVariable), std::string::npos);
    EXPECT_NE(resolved.diagnostics[0].find("yaml"), std::string::npos);
}

TEST(ResolveLoggingConfiguration, ReportsBothWhenBothAreWrong) {
    const auto resolved = resolveLoggingConfiguration("nope", "nope");
    EXPECT_EQ(resolved.diagnostics.size(), 2u);
    EXPECT_EQ(resolved.level, LogLevel::Info);
    EXPECT_EQ(resolved.format, LogFormat::Text);
}

TEST(ResolveLoggingConfiguration, AGoodValueBesideABadOneIsStillApplied) {
    const auto resolved = resolveLoggingConfiguration("error", "nope");
    EXPECT_EQ(resolved.level, LogLevel::Error);
    EXPECT_EQ(resolved.format, LogFormat::Text);
    EXPECT_EQ(resolved.diagnostics.size(), 1u);
}

TEST(SharedLogger, IsOneInstanceForTheWholeProcess) { EXPECT_EQ(&sharedLogger(), &sharedLogger()); }

TEST(SharedLogger, NamesTheTwoEnvironmentVariablesTheApplicationReads) {
    // These constants are what .env.example and scripts/check_repo_consistency.py
    // agree with, so they are asserted rather than left as prose.
    EXPECT_STREQ(kLogLevelEnvironmentVariable, "PALMIER_LOG_LEVEL");
    EXPECT_STREQ(kLogFormatEnvironmentVariable, "PALMIER_LOG_FORMAT");
}
