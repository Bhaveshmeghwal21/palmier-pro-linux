// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Logging.hpp — levelled, structured diagnostic logging.
//
// Before this existed the entire product wrote diagnostics with `std::cerr <<`, all of
// it in main.cpp's startup path. That is adequate for a message a human reads once at
// launch and useless for everything else: it carries no level, so nothing can be
// filtered; no category, so nothing can be attributed to a subsystem; no timestamp, so
// two lines cannot be ordered; and no structure, so a field inside a message can only
// be recovered by parsing English prose.
//
// FOUR DECISIONS SHAPE THIS FILE, and each is a decision rather than an obvious
// consequence, so each is written down.
//
// 1. A RECORD IS DATA, AND FORMATTING IS A PURE FUNCTION OF IT.
//
//    `LogRecord` is a plain aggregate. `formatLogRecordAsText` and
//    `formatLogRecordAsJson` are free functions taking one by const reference and
//    returning a string. Nothing about rendering touches a stream, a clock or global
//    state, so a test states a whole expected line in closed form and compares it
//    exactly — no substring matching, no regular expression over log output, no
//    tolerance. The alternative, a logger that formats as it writes, can only be
//    tested by capturing a stream and pattern-matching what comes out, which is how
//    log formatting ends up untested in practice.
//
// 2. THE SINK IS A SEAM, NOT A CLASS HIERARCHY.
//
//    `LogSink` is `std::function<void(const LogRecord&)>`. A test installs a lambda
//    that appends to a vector and then asserts on RECORDS — level, category and fields
//    as typed values — rather than on rendered text. An abstract `Sink` base class with
//    virtual `write` would buy nothing here and would make the common case (capture
//    into a vector) a class definition instead of two lines.
//
//    It also means the destination is a runtime choice: stderr for a desktop launch, a
//    file for a packaged build, a vector for a test, and JSON to stdout when something
//    downstream is going to parse it.
//
// 3. THE CLOCK IS INJECTABLE.
//
//    `setClock` exists for exactly one reason: a timestamp is part of a record, so
//    asserting a formatted line requires knowing what the time was. With a fixed clock
//    a test compares against a literal string. Reading the system clock inside the
//    logger would make every such assertion a pattern match against "some ISO-8601-ish
//    thing", which asserts far less.
//
// 4. A SUPPRESSED RECORD COSTS NOTHING BEYOND THE COMPARISON.
//
//    `log()` checks the threshold and returns before it touches the clock, allocates a
//    string or copies a field. `enabled()` is public so a call site whose ARGUMENTS are
//    expensive can skip building them at all. That matters because the point of having
//    a Trace level is to leave trace calls in the code permanently, and a level that is
//    expensive when disabled gets deleted instead of left in.
//
// `LogLevel::Off` is a level rather than a separate boolean: "log nothing" is the top of
// the same ordered scale, so one comparison implements both filtering and disabling, and
// PALMIER_LOG_LEVEL=off needs no special case.
//
// Thread safety: `Logger` is safe to call from any thread. The export coordinator, the
// peak-envelope service and the audio engine all run off the main thread, and a
// diagnostic that is only safe from one thread is a diagnostic that cannot report a
// threading problem. The mutex covers the sink call as well as the state, so two
// records never interleave inside one line.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace palmier::core {

/// Severity, ordered from most to least verbose. `Off` is the top of the scale and
/// suppresses everything, so disabling logging needs no separate flag.
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off,
};

/// Every level, in ascending severity, including `Off`. Iterating this rather than
/// hardcoding a count is what lets a test assert the set is complete.
constexpr std::array<LogLevel, 7> kLogLevels{
    LogLevel::Trace, LogLevel::Debug,    LogLevel::Info, LogLevel::Warning,
    LogLevel::Error, LogLevel::Critical, LogLevel::Off,
};

/// The lower-case name used in configuration and in rendered output.
[[nodiscard]] std::string_view logLevelName(LogLevel level);

/// Parse a level name, case-insensitively. `std::nullopt` for anything unrecognised,
/// so a mistyped PALMIER_LOG_LEVEL is reported rather than silently treated as a
/// default that hides output the operator asked for.
[[nodiscard]] std::optional<LogLevel> parseLogLevel(std::string_view name);

/// One structured field. Both halves are strings deliberately: a log field's only job
/// is to be read, and a variant would push a visitor into every formatter for no gain.
struct LogField {
    std::string key;
    std::string value;
};

/// A field whose value is rendered from whatever type the call site has, so call sites
/// do not each reimplement number-to-string.
///
/// The test is `is_constructible` rather than `is_convertible` deliberately:
/// `std::string` is constructible from `std::string_view` but NOT convertible from it
/// (the constructor is explicit), so an `is_convertible` test would send a
/// `string_view` down the arithmetic branch and fail to compile inside `std::to_string`.
template <typename T>
[[nodiscard]] LogField field(std::string key, const T& value) {
    if constexpr (std::is_constructible_v<std::string, const T&>) {
        return LogField{std::move(key), std::string(value)};
    } else {
        return LogField{std::move(key), std::to_string(value)};
    }
}

/// Everything one diagnostic knows about itself.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    /// The subsystem the record came from, e.g. "export", "audio", "mcp". Free-form:
    /// an enum would have to be extended in core every time a subsystem was added.
    std::string category;
    std::string message;
    /// In the order the call site wrote them. Not sorted: the author chose an order
    /// that reads well, and re-sorting would discard that for no benefit.
    std::vector<LogField> fields;
    std::chrono::system_clock::time_point time{};
};

/// How a record is rendered.
enum class LogFormat {
    /// One line, human first: `2026-09-09T07:20:00.123Z  info  [export] message k=v`
    Text,
    /// One JSON object per line, for anything that parses logs rather than reads them.
    Json,
};

[[nodiscard]] std::string_view logFormatName(LogFormat format);
[[nodiscard]] std::optional<LogFormat> parseLogFormat(std::string_view name);

/// The record's instant as RFC 3339 with milliseconds, always UTC with a `Z` suffix.
/// UTC rather than local time because logs get compared across machines.
[[nodiscard]] std::string formatLogTimestamp(std::chrono::system_clock::time_point time);

/// Render one record. Pure: same record in, same string out, on any host.
[[nodiscard]] std::string formatLogRecordAsText(const LogRecord& record);

/// Render one record as a single-line JSON object. Control characters, quotes and
/// backslashes in the message and in every field are escaped, because a message
/// routinely carries a filesystem path or an exception's text.
[[nodiscard]] std::string formatLogRecordAsJson(const LogRecord& record);

/// Escape a string for inclusion in a JSON string literal. Exposed because the
/// escaping is the part most likely to be wrong, and a named function can be tested
/// directly rather than only through a whole rendered record.
[[nodiscard]] std::string escapeForJson(std::string_view text);

/// Where records go. Copyable, so a logger can hold one and a test can keep a handle
/// to the state it captures into.
using LogSink = std::function<void(const LogRecord&)>;

/// A sink that writes each record to `stream` in `format`, one record per line. The
/// stream must outlive the sink; in practice it is `std::cerr`.
[[nodiscard]] LogSink makeStreamSink(std::ostream& stream, LogFormat format);

/// A sink that discards everything. The default, so a Logger that has not been
/// configured is silent rather than writing to a stream nobody asked it to touch.
[[nodiscard]] LogSink makeNullSink();

/// Levelled, structured logging to a configurable sink.
class Logger {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    /// Threshold `Info` and a null sink: silent until configured, which is the right
    /// default for a library type that a test might construct incidentally.
    explicit Logger(LogLevel threshold = LogLevel::Info);

    void setThreshold(LogLevel threshold);
    [[nodiscard]] LogLevel threshold() const;

    /// Replace the destination. An empty function is treated as the null sink, so a
    /// caller cannot accidentally install something that is invoked and throws.
    void setSink(LogSink sink);

    /// Replace the clock. Passing an empty function restores the system clock.
    void setClock(Clock clock);

    /// Would a record at this level be emitted? Public so a call site can skip
    /// building expensive arguments.
    [[nodiscard]] bool enabled(LogLevel level) const;

    /// Emit a record, if `level` passes the threshold. Returns whether it was emitted,
    /// which is what lets a test assert suppression without inspecting the sink.
    bool log(LogLevel level, std::string_view category, std::string_view message,
             std::vector<LogField> fields = {});

    bool trace(std::string_view category, std::string_view message,
               std::vector<LogField> fields = {});
    bool debug(std::string_view category, std::string_view message,
               std::vector<LogField> fields = {});
    bool info(std::string_view category, std::string_view message,
              std::vector<LogField> fields = {});
    bool warning(std::string_view category, std::string_view message,
                 std::vector<LogField> fields = {});
    bool error(std::string_view category, std::string_view message,
               std::vector<LogField> fields = {});
    bool critical(std::string_view category, std::string_view message,
                  std::vector<LogField> fields = {});

    /// How many records this logger has emitted, and how many it suppressed. The
    /// logger reporting on itself: a suppressed count that is unexpectedly large is
    /// how a misconfigured threshold gets noticed.
    [[nodiscard]] std::size_t emittedCount() const;
    [[nodiscard]] std::size_t suppressedCount() const;

private:
    mutable std::mutex mutex_;
    LogLevel threshold_;
    LogSink sink_;
    Clock clock_;
    std::size_t emitted_ = 0;
    std::size_t suppressed_ = 0;
};

/// The process-wide logger. A single instance rather than one per subsystem, because
/// the thing an operator configures is "the log", and per-subsystem filtering is what
/// the category field is for.
[[nodiscard]] Logger& sharedLogger();

/// The environment variables the application reads to configure the shared logger.
/// Named here so .env.example and scripts/check_repo_consistency.py have one place to
/// agree with.
constexpr const char* kLogLevelEnvironmentVariable = "PALMIER_LOG_LEVEL";
constexpr const char* kLogFormatEnvironmentVariable = "PALMIER_LOG_FORMAT";

/// Resolve a threshold and a format from two optional configuration strings, falling
/// back to `Info` and `Text`. Returned alongside a diagnostic for each value that was
/// present but unparseable, so the caller can report a typo rather than silently
/// applying a default the operator did not ask for.
struct LoggingConfiguration {
    LogLevel level = LogLevel::Info;
    LogFormat format = LogFormat::Text;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const { return diagnostics.empty(); }
};

[[nodiscard]] LoggingConfiguration resolveLoggingConfiguration(
    std::optional<std::string_view> level, std::optional<std::string_view> format);

}  // namespace palmier::core
