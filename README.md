# Palmier Pro for Linux

An AI-native, multi-track video editor for Linux with first-class GPU
acceleration (NVIDIA, AMD, Intel) and graceful CPU fallback. This is the
ground-up C++20 Linux port of Palmier Pro, built with Qt 6 (UI), FFmpeg
(media/codecs), and Vulkan (GPU compositing and hardware-codec bridge).

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
