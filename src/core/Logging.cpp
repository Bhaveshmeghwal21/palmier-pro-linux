// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Logging.cpp — implementation of the levelled, structured logger.
//
// See core/Logging.hpp for the four decisions that shape this file. Two notes that
// belong with the code rather than the interface:
//
//   * The timestamp is built with `gmtime_r` rather than `std::format` or
//     `std::chrono::zoned_time`. GCC 13, which is what Ubuntu 24.04 ships and therefore
//     what CI compiles with, has incomplete `<format>` chrono support; `gmtime_r` is
//     POSIX, thread-safe by contract, and this project is Linux-only.
//
//   * `formatLogRecordAsText` puts the timestamp first and the level second, so a
//     column of log lines is sortable by eye and greppable by level. Fields go last
//     because they are the part that varies in width, and a variable-width column in
//     the middle makes every following column fail to line up.

#include "core/Logging.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace palmier::core {
namespace {

/// Lower-case a name for case-insensitive configuration parsing. ASCII only, which is
/// all a level name can contain.
std::string toLowerAscii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

/// Ordering for threshold comparison. `Off` is highest, so nothing passes a threshold
/// of `Off` — including a record at `Off`, which is why the comparison in `enabled` is
/// written to exclude it explicitly.
int severityRank(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return 0;
        case LogLevel::Debug:
            return 1;
        case LogLevel::Info:
            return 2;
        case LogLevel::Warning:
            return 3;
        case LogLevel::Error:
            return 4;
        case LogLevel::Critical:
            return 5;
        case LogLevel::Off:
            return 6;
    }
    return 6;
}

}  // namespace

// ---------------------------------------------------------------------------
// Level and format names
// ---------------------------------------------------------------------------

std::string_view logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warning:
            return "warning";
        case LogLevel::Error:
            return "error";
        case LogLevel::Critical:
            return "critical";
        case LogLevel::Off:
            return "off";
    }
    return "info";
}

std::optional<LogLevel> parseLogLevel(std::string_view name) {
    const std::string lowered = toLowerAscii(name);
    for (const LogLevel level : kLogLevels) {
        if (lowered == logLevelName(level)) {
            return level;
        }
    }
    // Two spellings that are common enough elsewhere to be worth accepting rather than
    // rejecting on a technicality.
    if (lowered == "warn") {
        return LogLevel::Warning;
    }
    if (lowered == "fatal") {
        return LogLevel::Critical;
    }
    return std::nullopt;
}

std::string_view logFormatName(LogFormat format) {
    return format == LogFormat::Json ? "json" : "text";
}

std::optional<LogFormat> parseLogFormat(std::string_view name) {
    const std::string lowered = toLowerAscii(name);
    if (lowered == "text" || lowered == "plain") {
        return LogFormat::Text;
    }
    if (lowered == "json") {
        return LogFormat::Json;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::string formatLogTimestamp(std::chrono::system_clock::time_point time) {
    const auto sinceEpoch = time.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch - seconds).count();

    std::time_t asTimeT = static_cast<std::time_t>(seconds.count());
    // A time_point before the epoch yields a negative millisecond remainder; carry it
    // into the previous second so the rendered value is still monotonic and valid
    // rather than reading "-1ms".
    if (milliseconds < 0) {
        milliseconds += 1000;
        asTimeT -= 1;
    }

    std::tm utc{};
    if (gmtime_r(&asTimeT, &utc) == nullptr) {
        return "0000-00-00T00:00:00.000Z";
    }

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << milliseconds << 'Z';
    return out.str();
}

std::string escapeForJson(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            default:
                if (byte < 0x20) {
                    // Any other C0 control character has no short escape and must be
                    // written as \u00XX or the line is not valid JSON.
                    static const char* kHex = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += kHex[(byte >> 4) & 0x0F];
                    escaped += kHex[byte & 0x0F];
                } else {
                    // Bytes >= 0x80 pass through unchanged: the input is already UTF-8
                    // and JSON strings carry UTF-8 directly.
                    escaped += raw;
                }
                break;
        }
    }
    return escaped;
}

std::string formatLogRecordAsText(const LogRecord& record) {
    std::ostringstream out;
    out << formatLogTimestamp(record.time) << "  " << logLevelName(record.level);
    if (!record.category.empty()) {
        out << "  [" << record.category << ']';
    }
    if (!record.message.empty()) {
        out << "  " << record.message;
    }
    for (const LogField& one : record.fields) {
        out << ' ' << one.key << '=';
        // Quote a value that contains a space, or the key=value pairs cannot be told
        // apart when read back.
        if (one.value.find(' ') != std::string::npos) {
            out << '"' << one.value << '"';
        } else {
            out << one.value;
        }
    }
    return out.str();
}

std::string formatLogRecordAsJson(const LogRecord& record) {
    std::ostringstream out;
    out << "{\"time\":\"" << formatLogTimestamp(record.time) << "\",\"level\":\""
        << logLevelName(record.level) << "\",\"category\":\"" << escapeForJson(record.category)
        << "\",\"message\":\"" << escapeForJson(record.message) << '"';
    if (!record.fields.empty()) {
        out << ",\"fields\":{";
        bool first = true;
        for (const LogField& one : record.fields) {
            if (!first) {
                out << ',';
            }
            first = false;
            out << '"' << escapeForJson(one.key) << "\":\"" << escapeForJson(one.value) << '"';
        }
        out << '}';
    }
    out << '}';
    return out.str();
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

LogSink makeStreamSink(std::ostream& stream, LogFormat format) {
    return [&stream, format](const LogRecord& record) {
        stream << (format == LogFormat::Json ? formatLogRecordAsJson(record)
                                             : formatLogRecordAsText(record))
               << '\n';
    };
}

LogSink makeNullSink() {
    return [](const LogRecord&) {};
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger::Logger(LogLevel threshold)
    : threshold_(threshold),
      sink_(makeNullSink()),
      clock_([] { return std::chrono::system_clock::now(); }) {}

void Logger::setThreshold(LogLevel threshold) {
    const std::lock_guard<std::mutex> guard(mutex_);
    threshold_ = threshold;
}

LogLevel Logger::threshold() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return threshold_;
}

void Logger::setSink(LogSink sink) {
    const std::lock_guard<std::mutex> guard(mutex_);
    // An empty std::function would throw when invoked. Treating it as "discard" makes
    // `setSink({})` mean the obvious thing instead of arming a crash.
    sink_ = sink ? std::move(sink) : makeNullSink();
}

void Logger::setClock(Clock clock) {
    const std::lock_guard<std::mutex> guard(mutex_);
    clock_ = clock ? std::move(clock) : Clock([] { return std::chrono::system_clock::now(); });
}

bool Logger::enabled(LogLevel level) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    // `Off` never passes, whether it is the threshold or the record's own level.
    if (threshold_ == LogLevel::Off || level == LogLevel::Off) {
        return false;
    }
    return severityRank(level) >= severityRank(threshold_);
}

bool Logger::log(LogLevel level, std::string_view category, std::string_view message,
                 std::vector<LogField> fields) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (threshold_ == LogLevel::Off || level == LogLevel::Off ||
        severityRank(level) < severityRank(threshold_)) {
        ++suppressed_;
        return false;
    }

    LogRecord record;
    record.level = level;
    record.category = std::string(category);
    record.message = std::string(message);
    record.fields = std::move(fields);
    record.time = clock_();

    // Inside the lock on purpose: two records must not interleave within one line, and
    // a sink that writes to a stream is not itself synchronised.
    sink_(record);
    ++emitted_;
    return true;
}

bool Logger::trace(std::string_view category, std::string_view message,
                   std::vector<LogField> fields) {
    return log(LogLevel::Trace, category, message, std::move(fields));
}

bool Logger::debug(std::string_view category, std::string_view message,
                   std::vector<LogField> fields) {
    return log(LogLevel::Debug, category, message, std::move(fields));
}

bool Logger::info(std::string_view category, std::string_view message,
                  std::vector<LogField> fields) {
    return log(LogLevel::Info, category, message, std::move(fields));
}

bool Logger::warning(std::string_view category, std::string_view message,
                     std::vector<LogField> fields) {
    return log(LogLevel::Warning, category, message, std::move(fields));
}

bool Logger::error(std::string_view category, std::string_view message,
                   std::vector<LogField> fields) {
    return log(LogLevel::Error, category, message, std::move(fields));
}

bool Logger::critical(std::string_view category, std::string_view message,
                      std::vector<LogField> fields) {
    return log(LogLevel::Critical, category, message, std::move(fields));
}

std::size_t Logger::emittedCount() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return emitted_;
}

std::size_t Logger::suppressedCount() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return suppressed_;
}

Logger& sharedLogger() {
    // Function-local static: initialised on first use, destroyed after main, and
    // thread-safe to initialise since C++11.
    static Logger logger;
    return logger;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

LoggingConfiguration resolveLoggingConfiguration(std::optional<std::string_view> level,
                                                 std::optional<std::string_view> format) {
    LoggingConfiguration resolved;

    if (level.has_value() && !level->empty()) {
        if (const std::optional<LogLevel> parsed = parseLogLevel(*level)) {
            resolved.level = *parsed;
        } else {
            std::string diagnostic = std::string(kLogLevelEnvironmentVariable) + "=\"" +
                                     std::string(*level) + "\" is not a level; using \"" +
                                     std::string(logLevelName(resolved.level)) +
                                     "\". Valid levels: ";
            bool first = true;
            for (const LogLevel candidate : kLogLevels) {
                if (!first) {
                    diagnostic += ", ";
                }
                first = false;
                diagnostic += std::string(logLevelName(candidate));
            }
            resolved.diagnostics.push_back(std::move(diagnostic));
        }
    }

    if (format.has_value() && !format->empty()) {
        if (const std::optional<LogFormat> parsed = parseLogFormat(*format)) {
            resolved.format = *parsed;
        } else {
            resolved.diagnostics.push_back(std::string(kLogFormatEnvironmentVariable) + "=\"" +
                                           std::string(*format) + "\" is not a format; using \"" +
                                           std::string(logFormatName(resolved.format)) +
                                           "\". Valid formats: text, json");
        }
    }

    return resolved;
}

}  // namespace palmier::core
