# Palmier Pro for Linux

An AI-native, multi-track video editor for Linux with first-class GPU
acceleration (NVIDIA, AMD, Intel) and graceful CPU fallback. This is the
ground-up C++20 Linux port of Palmier Pro, built with Qt 6 (UI), FFmpeg
(media/codecs), and Vulkan (GPU compositing and hardware-codec bridge).

## Documentation

Everything operational lives in [`docs/`](docs/), so each option, tool and
procedure is documented in exactly one place.

| Document | What it covers |
|---|---|
| [`docs/QUICKSTART.md`](docs/QUICKSTART.md) | Build, launch and drive the editor end to end, with remediation for a missing GPU, missing audio output and a refused MCP connection |
| [`docs/BUILD.md`](docs/BUILD.md) | Configure → build → test → launch, native packages per distribution, every `PALMIER_*` option, the minimum host specification and the environment traps |
| [`docs/MCP_CLIENTS.md`](docs/MCP_CLIENTS.md) | Connecting Claude Code, Codex and Cursor to the loopback endpoint at `127.0.0.1:19789` |
| [`docs/TOOLS.md`](docs/TOOLS.md) | Every tool the agent surface publishes, with each argument and result field |
| [`docs/HARDWARE_ENCODE.md`](docs/HARDWARE_ENCODE.md) | NVENC, Quick Sync and VAAPI prerequisites, the software fallback, host verification and the L4 validation procedure |
| [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md) | Opt-in non-loopback access: token, TLS, Origin allow-list, session and timeout limits |
| [`docs/UPSTREAM_PARITY.md`](docs/UPSTREAM_PARITY.md) · [`docs/PORT_BACKLOG.md`](docs/PORT_BACKLOG.md) | Upstream parity report and the deferred-port backlog |

## Licensing

Palmier Pro for Linux uses a deliberate open-source / closed-service split.
Read the [`LICENSE`](LICENSE) (full, unmodified GNU GPLv3 text) and
[`NOTICE`](NOTICE) files for the authoritative details.

### Open source — GPLv3-or-later

The editor and its tooling are free software under the **GNU General Public
License, version 3 or later**. This covers:

- **Timeline Editor** — the multi-track timeline editor, domain core, media
  engine, GPU abstraction layer, export engine, localization, and Qt 6 UI.
- **MCP Server** — the local Model Context Protocol server exposing editor
  tools to external AI agents over loopback HTTP.
- **Agent Chat** — the in-app assistant that drives the editor through the same
  tool surface as the MCP server.

These components are fully functional with **no network connection** to the
generative service.

### Closed service — account-gated

The **generative-AI capability** is a proprietary, hosted service that requires
a valid authenticated account (active subscription or Bring-Your-Own-Key
credentials). Its backend and models are **not** distributed under GPLv3 and are
**not** part of this repository beyond the GPLv3 client that contacts it. When
the service is unreachable or unauthenticated, the open-source editor continues
to work without degradation.

## Contributing

All source files must carry a GPLv3 per-file header. See
[`CONTRIBUTING.md`](CONTRIBUTING.md) and the header templates in
[`docs/LICENSE-HEADER.txt`](docs/LICENSE-HEADER.txt).
