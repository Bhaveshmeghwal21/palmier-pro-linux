<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Connecting an MCP client

This is the Requirement 16.2 document: the complete client configuration entry for Claude Code,
Codex and Cursor against the default loopback endpoint, and the observable confirmation that the
connection succeeded.

The editor serves the MCP endpoint at **`http://127.0.0.1:19789/mcp`** whenever the application is
running. Nothing needs enabling: that bind is the default and it is loopback-only, so no token, no
TLS and no acknowledgement is involved. Exposing the endpoint to another machine is a separate,
opt-in operation — see [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md).

## What the endpoint is, in one paragraph

`POST /mcp` speaking **JSON-RPC 2.0**, one JSON object per request, `Content-Type:
application/json`, bodies capped at 1 MiB (a larger body is refused with JSON-RPC `-32700` and never
reaches the tool surface). Any path other than `/mcp` answers 404 and any method other than `POST`
answers 405. Protocol revisions spoken, newest first: `2025-06-18` and `2025-03-26`; `initialize`
negotiates the client's requested version when it is one of those and otherwise answers the newest.

The method sequence a client must follow, and the one a misconfigured client usually gets wrong:

1. **`initialize`** — the response carries `protocolVersion`, `capabilities.tools` and `serverInfo`,
   and the endpoint mints a session identifier returned in the **`Mcp-Session-Id`** response header.
2. **`notifications/initialized`** — sent with the `Mcp-Session-Id` request header. Being a
   notification it has no `id`, and the endpoint answers **HTTP 202 with a zero-byte body** and no
   `Content-Type`. A client that treats an empty 202 as a protocol error will appear to fail here
   even though the handshake succeeded.
3. **`tools/list`** — returns `result.tools`, each entry carrying `name`, `description` and
   `inputSchema`.
4. **`tools/call`** — returns `result.content` (a one-element text block) plus `result.isError`.

Every request after `initialize` must carry the `Mcp-Session-Id` header it issued. An unknown
session identifier is `-32001`; a session that has not completed step 2 is `-32002`; a
session-initiating request that exceeds the concurrent-session ceiling is `-32003`.

**One transport caveat to know before you configure a client.** The endpoint implements the
request/response half of streamable HTTP only: there is no `GET /mcp` event-stream channel and no
server-initiated notification. Clients that require the SSE channel to consider a connection
established will not connect directly; use the stdio bridge shown below, which speaks stdio to the
client and plain HTTP POST to the editor.

## Claude Code

Either register it from the command line:

```sh
claude mcp add --transport http palmier-pro http://127.0.0.1:19789/mcp
```

or check the entry into the project as `.mcp.json` (or add it to `~/.claude.json` for a user-wide
registration):

```json
{
  "mcpServers": {
    "palmier-pro": {
      "type": "http",
      "url": "http://127.0.0.1:19789/mcp"
    }
  }
}
```

Confirm with `claude mcp list`, which should show `palmier-pro` as connected, and then with `/mcp`
inside a session to list its tools.

## Codex

Add the server to `~/.codex/config.toml`:

```toml
[mcp_servers.palmier-pro]
url = "http://127.0.0.1:19789/mcp"
```

Confirm with `codex mcp list`. If your Codex build does not accept a `url` server — HTTP transport
has been gated behind an experimental flag in some releases — use the stdio bridge form instead,
which works on every version because it is an ordinary `command`/`args` server:

```toml
[mcp_servers.palmier-pro]
command = "npx"
args = ["-y", "mcp-remote", "http://127.0.0.1:19789/mcp", "--allow-http"]
```

## Cursor

Add the server to `~/.cursor/mcp.json` for every project, or to `.cursor/mcp.json` for one project:

```json
{
  "mcpServers": {
    "palmier-pro": {
      "url": "http://127.0.0.1:19789/mcp"
    }
  }
}
```

Confirm in Settings → MCP: the `palmier-pro` entry should be enabled and list its tools.

## Confirming the connection (Requirement 16.2)

The connection succeeded when the client's tool list contains **at least** these nine names, which
Requirement 3.1 enumerates:

```
project.create   project.open   project.save   project.info
media.import     media.list
timeline.add_track   timeline.remove_track   timeline.export
```

A healthy endpoint lists more than nine — the timeline editing tools (`timeline.add_clip`,
`timeline.delete_clip`, `timeline.move_clip`, `timeline.trim_clip`, `timeline.split_clip`,
`timeline.reorder_clips`, `timeline.read`, `timeline.add_effect`, `timeline.add_transition`,
`timeline.set_track_muted`) and `edit.undo` / `edit.redo` are registered on the same registry. Those
nine are the floor, not the whole surface; the complete list with each argument and result field is
`docs/TOOLS.md`.

To confirm without any client at all, drive the handshake with `curl`. This is also the fastest way
to decide whether a failure is in the editor or in the client:

```sh
# 1. initialize — note the Mcp-Session-Id response header
curl -si http://127.0.0.1:19789/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
       "params":{"protocolVersion":"2025-06-18","capabilities":{},
                 "clientInfo":{"name":"curl","version":"1"}}}'

# 2. notifications/initialized — expect HTTP 202 and an empty body
SID=<the Mcp-Session-Id value from step 1>
curl -si http://127.0.0.1:19789/mcp \
  -H 'Content-Type: application/json' -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

# 3. tools/list — expect the nine names above among result.tools[].name
curl -s http://127.0.0.1:19789/mcp \
  -H 'Content-Type: application/json' -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

## When it does not connect

| Symptom | Cause and remedy |
|---|---|
| Connection refused | The editor is not running, or its endpoint never bound. Check the console: a port conflict on 19789 produces a warning at startup and the editor continues **without** the endpoint. Free the port, or run only one instance. |
| 404 on every request | Wrong path. The only served path is `/mcp`; `http://127.0.0.1:19789/` is a 404. |
| 405 | The client is issuing `GET` (usually to open an event stream). There is no `GET /mcp`; use the stdio bridge form. |
| Handshake appears to stall after `initialize` | The client rejected the zero-byte HTTP 202 answer to `notifications/initialized`, or it is not echoing `Mcp-Session-Id`. Both are visible in `curl -si` output. |
| `-32002` on `tools/list` | `notifications/initialized` was never sent for that session. |
| `-32001` on every call | The `Mcp-Session-Id` is stale — the session was closed, most often by the idle timeout. Re-run `initialize`. |
| 401 or 403 | You are talking to a **non-loopback** binding, which enforces the bearer token and the Origin allow-list. See [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md). |
