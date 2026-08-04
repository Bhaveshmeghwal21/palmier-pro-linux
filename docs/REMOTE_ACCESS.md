<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Remote MCP access

This is the Requirement 16.3 document: every configuration input the Remote_Access_Gate requires,
and the warning that remote access without TLS transmits traffic unencrypted.

The default is loopback-only. `127.0.0.1:19789` refuses every non-loopback source, needs no token
and serves requests carrying neither `Authorization` nor `Origin` exactly as it always has. Nothing
here happens as a side effect: remote access turns on only when configuration says `true` in so
many words, and even then the gate still has to find all of its prerequisites.

> **Status: the settings layer is not yet wired into the shipped binary.** Every key, variable and
> flag below is implemented and enforced by `app::AppSettings` and `services::RemoteAccessGate`, and
> `app::ApplicationComposition` acts on the resulting `AppConfig`. But `src/app/main.cpp` currently
> constructs `ApplicationComposition` with a **default-constructed** `AppConfig` and never calls
> `AppSettings::load()` or `AppSettings::fromArgv()`. Until that one call is added, `palmier-pro`
> ignores the config file, the environment and the command line, and the endpoint always comes up on
> loopback. Read this document as the configuration contract — accurate for embedders and for the
> composition root — and not yet as something the shipped executable will honour.

## Where configuration comes from

Four layers, each overriding the one below it:

1. built-in defaults;
2. a `key=value` file at `$XDG_CONFIG_HOME/palmier-pro/config`, or
   `$HOME/.config/palmier-pro/config` when `XDG_CONFIG_HOME` is unset. Override the path with
   `--config <path>` or `PALMIER_CONFIG_FILE`;
3. environment variables — the `PALMIER_*` names below;
4. command-line flags — the `--kebab-case` names below.

Anything unusable — a malformed line, an unknown key, a value outside its bounds — is recorded as a
diagnostic and **ignored**, leaving the safer lower-precedence value in place. An absent config file
is not an error.

Booleans accept `true`/`false`, `1`/`0`, `yes`/`no`, `on`/`off`, case-insensitively. The two boolean
flags may also be passed bare (`--remote-enabled` means `true`); a following argument is only
consumed when it looks like a boolean, so `--remote-enabled project.palmier` is the flag plus a
positional path. Lists are comma-separated, trimmed, with blank entries dropped.

## Every configuration input

| Config-file key | Environment variable | Command-line flag | Default | Accepted values |
|---|---|---|---|---|
| `remote.enabled` | `PALMIER_REMOTE_ENABLED` | `--remote-enabled` | `false` | boolean. The opt-in switch |
| `remote.bind_address` | `PALMIER_REMOTE_BIND_ADDRESS` | `--remote-bind-address` | *(empty)* | an IPv4 or IPv6 **literal**. Host names are rejected; an empty value is an unmet prerequisite, not an invitation to bind a wildcard |
| `remote.port` | `PALMIER_REMOTE_PORT` | `--remote-port` | `19789` | 1..65535. Used for the **non-loopback** bind |
| `remote.bearer_token` | `PALMIER_REMOTE_BEARER_TOKEN` | `--remote-bearer-token` | *(empty)* | 32 to 512 printable ASCII characters (0x20–0x7E). Taken verbatim, since admission compares byte-for-byte |
| `remote.acknowledged` | `PALMIER_REMOTE_ACKNOWLEDGED` | `--remote-acknowledged` | `false` | boolean. Must be explicitly true |
| `remote.origin_allow_list` | `PALMIER_REMOTE_ORIGIN_ALLOW_LIST` | `--remote-origin-allow-list` | *(empty)* | comma-separated host names |
| `remote.tls_certificate` | `PALMIER_REMOTE_TLS_CERTIFICATE` | `--remote-tls-certificate` | *(unset)* | path to a PEM certificate |
| `remote.tls_private_key` | `PALMIER_REMOTE_TLS_PRIVATE_KEY` | `--remote-tls-private-key` | *(unset)* | path to the matching PEM private key |
| `remote.max_sessions` | `PALMIER_REMOTE_MAX_SESSIONS` | `--remote-max-sessions` | `8` | 1..32 |
| `remote.idle_timeout_seconds` | `PALMIER_REMOTE_IDLE_TIMEOUT_SECONDS` | `--remote-idle-timeout-seconds` | `300` | 30..3600 seconds |
| `mcp.host` | `PALMIER_MCP_HOST` | `--mcp-host` | `127.0.0.1` | the host bound when the decision is loopback |
| `mcp.port` | `PALMIER_MCP_PORT` | `--mcp-port` | `19789` | 0..65535; `0` binds an ephemeral port |

`mcp.host` / `mcp.port` describe the ordinary (loopback) endpoint; `remote.bind_address` /
`remote.port` describe the non-loopback one. They are separate keys on purpose: enabling remote
access does not move the local endpoint, and a rejected remote configuration does not either.

## The three prerequisites, and what happens when one is missing

A non-loopback bind requires **all three** of:

1. a syntactically valid IPv4 or IPv6 literal in `remote.bind_address`;
2. a bearer token of 32–512 printable ASCII characters in `remote.bearer_token`;
3. `remote.acknowledged` explicitly `true`.

Plus, when TLS material is configured, a certificate and private key that both load and form a
matching pair.

If any prerequisite is unmet, the gate **refuses the non-loopback bind, binds `127.0.0.1:19789`
instead, and emits a startup error naming every unmet prerequisite** — never the token and never any
substring of it. The error reads:

```
Remote MCP access is enabled but was not started: unmet prerequisites — <each one>.
The endpoint is bound to loopback instead.
```

Two details worth knowing:

- The fallback port is **19789, not the configured `remote.port`**. The configured port was chosen
  for a non-loopback endpoint the gate has just refused to bind, and carrying it over would move a
  local endpoint that existing loopback clients rely on, as a side effect of a rejected remote
  configuration. (Note: the composition root's loopback bind actually uses `mcp.host`/`mcp.port`, so
  with those at their defaults the fallback is `127.0.0.1:19789` as documented — but an operator who
  changed `mcp.port` will see that port instead.)
- Enabling remote access while naming a **loopback** literal yields a loopback binding, and
  per-request admission follows the binding rather than the switch: token and Origin enforcement
  apply to non-loopback bindings.

## Generating the bearer token

At least 32 characters, at most 512, all printable ASCII. Any of these produces a token comfortably
inside the bounds:

```sh
openssl rand -base64 48        # 64 characters
head -c 32 /dev/urandom | base64
python3 -c 'import secrets; print(secrets.token_urlsafe(48))'
```

Put it somewhere the process can read without it landing in shell history or a world-readable file.
The config file is the usual place; `chmod 600` it. The token is read but never reproduced: no
diagnostic, no log record and no startup message contains it or any substring of it.

Clients send it as an ordinary bearer credential:

```
Authorization: Bearer <token>
```

## TLS, or an SSH tunnel instead

**Warning: remote access without TLS transmits every request — including the bearer token — over
the network unencrypted.** The gate still completes the configured non-loopback bind, and it emits
exactly one startup warning before the first request is accepted:

> Remote MCP access is enabled on `<host>`:`<port>` without TLS, so all traffic including bearer
> tokens is transmitted unencrypted. Configure a TLS certificate and private key, or reach the
> endpoint through an SSH tunnel instead.

### Option A — TLS

Requires a build with `PALMIER_ENABLE_OPENSSL=ON` **and** OpenSSL 3.x present at configure time. On
a build without the TLS transport compiled in, configuring TLS material is itself an unmet
prerequisite and the endpoint falls back to loopback rather than silently serving plaintext.

```sh
openssl req -x509 -newkey rsa:4096 -sha256 -days 365 -nodes \
  -keyout /etc/palmier-pro/tls.key -out /etc/palmier-pro/tls.crt \
  -subj "/CN=editor.example.internal" \
  -addext "subjectAltName=DNS:editor.example.internal,IP:192.0.2.10"
chmod 600 /etc/palmier-pro/tls.key
```

A certificate or key that cannot be read, cannot be parsed, or does not form a matching pair is
refused: the gate binds loopback and the startup error says which of those three happened. Once TLS
is serving, a plaintext HTTP request on that port is rejected without ever reaching the tool surface.

### Option B — SSH tunnel (no TLS material needed)

Leave the endpoint on its loopback default and forward it. This is the lower-risk option and needs
no remote-access configuration at all:

```sh
ssh -N -L 19789:127.0.0.1:19789 user@editor-host
```

Clients then use `http://127.0.0.1:19789/mcp` on the local machine, exactly as in
[`MCP_CLIENTS.md`](MCP_CLIENTS.md).

## Origin allow-list

`remote.origin_allow_list` names the hosts accepted in an `Origin` header. A request whose `Origin`
is present and names a host outside the list is rejected with **403** and is not dispatched. The
allow-list is extended implicitly with the bound address and the loopback hosts, and an unconfigured
list is treated as empty — meaning only those implicit hosts are accepted, which is the safe reading.
Comparison is on the host component only (`http://host:port` and `[::1]:port` forms both reduce to
the host) and is case-insensitive.

## Sessions

- **Concurrent sessions**: `remote.max_sessions`, 1..32, default 8. A session-initiating request
  arriving at the ceiling is rejected on its own — every established session stays active — with
  HTTP 429 and JSON-RPC `-32003`.
- **Idle timeout**: `remote.idle_timeout_seconds`, 30..3600, default 300. A session that receives no
  request for longer than the timeout is closed, the closure reason is recorded, and later requests
  carrying that session identifier are refused with `-32001`. Clients recover by re-running
  `initialize`.

## Admission, in the order it happens

On a non-loopback binding each request is checked in this order, and the first failure ends it:

| Check | Failure |
|---|---|
| Source-address block list | 401 (`source_blocked`) |
| Bearer token — absent, malformed, or not byte-identical (constant-time compare) | 401, answered within 500 ms, never dispatched |
| `Origin` against the allow-list | 403 |
| Session ceiling, for session-initiating requests | 429 / `-32003` |

**The 401 rate limiter.** Five 401 rejections from the same source address inside any 60-second
sliding window block that source: every further request from it is rejected with 401 for the next
60 seconds. Each blocked rejection is logged like any other, and requests from all other source
addresses continue to be served normally.

**What gets logged.** Every rejection is recorded as exactly three facts — a UTC timestamp with
millisecond precision (`YYYY-MM-DDThh:mm:ss.sssZ`), the source address, and a reason **code** — and
nothing else. The presented credential is never passed to the logger, so no substring of it can
appear in a record. The reason codes correspond to: no token, malformed token, token mismatch,
origin not allowed, session limit reached, source blocked, and plaintext on a TLS port.

## A complete worked example

`~/.config/palmier-pro/config`, `chmod 600`:

```ini
# The loopback endpoint stays where clients expect it.
mcp.host = 127.0.0.1
mcp.port = 19789

# Remote access, TLS-protected, on the LAN interface.
remote.enabled = true
remote.bind_address = 192.0.2.10
remote.port = 19789
remote.bearer_token = <output of `openssl rand -base64 48`>
remote.acknowledged = true
remote.origin_allow_list = laptop.example.internal, editor.example.internal
remote.tls_certificate = /etc/palmier-pro/tls.crt
remote.tls_private_key = /etc/palmier-pro/tls.key
remote.max_sessions = 8
remote.idle_timeout_seconds = 300
```

The equivalent as flags — note that a token on a command line is visible in the process table, so
prefer the file or the environment:

```sh
palmier-pro --remote-enabled --remote-bind-address 192.0.2.10 \
  --remote-acknowledged --remote-max-sessions 8 \
  --remote-idle-timeout-seconds 300 \
  --remote-origin-allow-list laptop.example.internal \
  --remote-tls-certificate /etc/palmier-pro/tls.crt \
  --remote-tls-private-key /etc/palmier-pro/tls.key
```

Expected startup output: no unmet-prerequisite error, and no plaintext warning (because TLS is
configured). If you see either, the message names exactly what to fix.
