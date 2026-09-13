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

## Health and error tracking

Two questions the editor could not answer about itself until now: *is it up, and is
anything degraded?* and *has anything failed, and how often?*

### `GET /health`

The MCP endpoint serves a second path beside `/mcp`:

```console
$ curl -fsS http://127.0.0.1:19789/health
{"state":"ok","version":"0.4.0","uptimeSeconds":41,
 "checks":[{"name":"gpu","state":"ok","detail":""},
           {"name":"audio","state":"ok","detail":"pipewire"},
           {"name":"startup","state":"ok","detail":""}],
 "log":{"total":12,"warnings":1,"errors":0,"critical":0,
        "errorsByCategory":{},"lastError":null}}
```

A separate path rather than a tool call, so a container runtime, a load balancer or a
person with `curl` can ask without speaking JSON-RPC and without holding a session. It
is `GET` only — anything else is 405, which keeps it from becoming an accidental write
surface.

The document is rendered by `healthReportToJson`, and its five keys are the whole
contract:

| Key | Meaning |
|---|---|
| `state` | `ok`, `degraded` or `unavailable` — the worst of the component states |
| `version` | the server version, the same string `initialize` reports |
| `uptimeSeconds` | seconds since the composition was built |
| `checks` | one entry per registered component: `name`, `state`, `detail` |
| `log` | counts of what the log has carried, and the most recent failure |

### The three states, and why `degraded` is served as 200

`healthHttpStatus` maps `ok` and `degraded` to **200**, and only `unavailable` to **503**.

That is deliberate. No GPU and no audio device are *supported* ways to run this editor —
compositing falls back to the CPU, the audio sink order falls through to `null`, the
status bar says so, and the suite proves it. A container runtime that restarted the
process for either would be destroying a perfectly usable editor. `degraded` means
"working, with something a user would notice missing"; `unavailable` means "cannot do
its job".

The components that report today, all from what the application composition already
knows:

| Check | `degraded` when |
|---|---|
| `gpu` | no Vulkan device, so compositing is on the CPU |
| `audio` | no output device, so audio is suppressed |
| `startup` | one or more startup diagnostics were recorded |

A component answers for itself through a probe registered on `HealthMonitor`, so the
monitor knows names and states rather than internals, and adding a component does not
change it. A probe that throws is reported as `unavailable` with the exception's message
rather than propagating — the endpoint that exists to report brokenness must not be
brought down by a broken probe.

### Error tracking is the log, counted

`makeCountingLogSink` **decorates** the sink installed on the shared logger rather than
adding a parallel reporting path:

```cpp
auto logTally = std::make_shared<services::LogTally>();
sharedLogger().setSink(services::makeCountingLogSink(
    logTally, core::makeStreamSink(std::cerr, format)));
```

So anything already logged is counted by construction, and there is no way to record a
failure that the log does not also carry. The counters are per-severity plus a
per-category breakdown of failures, because knowing there were forty errors is much less
use than knowing all forty were in `media.decode`. The sink is installed **before** any
component is built, since the failures raised during composition are the interesting
ones.

`lastError` is `null` until something actually fails, then names the category and message
of the most recent one.

### What it deliberately does not contain

The document is served over a network socket, so its contents are part of its security
contract: no token, no credential, no filesystem path, no host, nothing identifying a
user. A test asserts the key set is exactly the five above, so a future field cannot be
added without that decision being made again.

When remote access is enabled, `/health` is subject to the **same admission gate** as
`/mcp` — an unauthenticated remote peer gets 401 and the report is never even built. On
the default loopback binding the gate admits unconditionally, so a local probe works with
no configuration. See [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md).

There is no container `HEALTHCHECK` yet: the image ships no HTTP client, and
`packaging/apt-packages.txt` is held byte-identical to what CI installs, so adding one is
a deliberate change rather than a side effect of this feature.

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
