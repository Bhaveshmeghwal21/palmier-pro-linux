# Observability

How Palmier Pro reports what it is doing, and how to change what it reports.

Everything here is implemented in `src/core/Logging.hpp` and tested in
`tests/core/logging_test.cpp`.

---

## Configuring the log

Two environment variables, both listed in [`.env.example`](../.env.example) with the
rest of the configuration surface:

| Variable | Values | Default | Effect |
|---|---|---|---|
| `PALMIER_LOG_LEVEL` | `trace`, `debug`, `info`, `warning`, `error`, `critical`, `off` | `info` | Suppresses every record below the named severity |
| `PALMIER_LOG_FORMAT` | `text`, `json` | `text` | How each record is rendered |

`warn` is accepted as a synonym for `warning`, `fatal` for `critical`, and `plain` for
`text`. Anything else is **reported and then ignored** rather than silently treated as
the default — a mistyped level that quietly became `info` would hide output you
explicitly asked for.

```bash
PALMIER_LOG_LEVEL=debug ./build/bin/palmier-pro          # more detail
PALMIER_LOG_LEVEL=off ./build/bin/palmier-pro            # silence
PALMIER_LOG_FORMAT=json ./build/bin/palmier-pro 2> app.jsonl
```

Records go to standard error, so redirecting `2>` captures the log without touching
anything the editor writes to standard output.

---

## The two formats

### `text` — one line, human first

```
2026-09-09T07:20:00.123Z  warning  [export]  encoder fell back from=h264_nvenc to=libx264
```

Timestamp, then level, then category in brackets, then the message, then the structured
fields. The timestamp is RFC 3339 with milliseconds and is **always UTC** with a `Z`
suffix, because logs get compared across machines. A field value containing a space is
quoted, so `key=value` pairs can still be told apart when read back.

### `json` — one object per line

```json
{"time":"2026-09-09T07:20:00.123Z","level":"warning","category":"export","message":"encoder fell back","fields":{"from":"h264_nvenc","to":"libx264"}}
```

Newline-delimited JSON, so `jq` works line by line:

```bash
PALMIER_LOG_FORMAT=json ./build/bin/palmier-pro 2>&1 \
  | jq -c 'select(.level == "error" or .level == "critical")'
```

The `fields` object is **omitted entirely** when a record carries no fields, rather than
emitted empty. Quotes, backslashes, newlines and C0 control characters are escaped in
the message, the category and both halves of every field, because a message routinely
carries a filesystem path or an exception's text. Bytes above `0x7f` pass through
unchanged: the input is already UTF-8 and JSON carries UTF-8 directly.

---

## Categories

The category names the subsystem a record came from, and is what to filter on when you
know roughly where a problem is. It is a free-form string rather than an enum, so a new
subsystem does not require a change in `core`. The ones in use:

| Category | Covers |
|---|---|
| `startup` | Platform compatibility, settings resolution, composition |
| `mcp` | The Model Context Protocol endpoint and its sessions |
| `remote` | Opt-in non-loopback access and its prerequisites |
| `gpu` | Vulkan device selection and the CPU fallback |
| `audio` | Output sink selection and the audio graph |
| `export` | Encoder selection, hardware fallback, export progress |
| `media` | Import, probing and decode |

---

## The design, for anyone extending it

Four decisions, each recorded at length in `src/core/Logging.hpp`:

1. **A `LogRecord` is data, and formatting is a pure function of it.**
   `formatLogRecordAsText` and `formatLogRecordAsJson` each take one record by const
   reference and return a string. Nothing about rendering touches a stream, a clock or
   global state, so a test states a whole expected line in closed form and compares it
   exactly. A logger that formats as it writes can only be tested by capturing a stream
   and pattern-matching, which is how log formatting ends up untested in practice.

2. **The sink is a seam, not a class hierarchy.** `LogSink` is
   `std::function<void(const LogRecord&)>`. A test installs a lambda that appends to a
   vector and asserts on records — level, category and fields as typed values — rather
   than on rendered text. It also makes the destination a runtime choice: stderr for a
   desktop launch, a vector for a test, JSON for something downstream that parses.

3. **The clock is injectable.** `Logger::setClock` exists so that asserting a formatted
   line is possible at all: with a fixed clock a test compares against a literal
   string instead of a pattern.

4. **A suppressed record costs nothing beyond the comparison.** `Logger::log` returns
   before it reads the clock, allocates a string or copies a field, and
   `Logger::enabled` is public so a call site whose arguments are expensive can skip
   building them. That is what makes it reasonable to leave `trace` calls in the code
   permanently.

`LogLevel::Off` is a level rather than a separate boolean, so one comparison implements
both filtering and disabling. `LogField` holds two strings; the `field()` helper renders
numbers so call sites do not each reimplement number-to-string.

`Logger` is safe to call from any thread — the export coordinator, the peak-envelope
service and the audio engine all run off the main thread, and a diagnostic that is only
safe from one thread cannot report a threading problem. The lock covers the sink call as
well as the state, so two records never interleave within one line.

`sharedLogger()` is the process-wide instance. There is one rather than one per
subsystem because the thing an operator configures is "the log"; per-subsystem
filtering is what the category field is for.

---

## What CI records about a run

Separately from the application's own log, every CI run publishes:

| Artifact | Contents |
|---|---|
| `ctest-results` | The complete test log, CTest's own logs, and a generated summary listing every test with its outcome and each skipped test's recorded reason |
| `launch-smoke-test` | Two screenshots of the running editor, its stderr, and the Xvfb log |
| `coverage-report` | Line coverage as Cobertura XML, an lcov `coverage.info`, and drill-down HTML |

The lint job additionally publishes formatting conformance and the resolved version of
every native dependency to the run summary. See
[`docs/BUILD.md`](BUILD.md) for the build options themselves.
