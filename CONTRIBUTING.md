# Contributing to Palmier Pro for Linux

Thank you for your interest in contributing. This document records the
conventions every contribution must follow. It complements the `LICENSE`
(full GNU GPLv3 text) and `NOTICE` (licensing overview) files.

## Licensing of contributions

Palmier Pro for Linux is free software. The open-source components — the
**Timeline Editor**, the **MCP Server**, and the **in-app Agent Chat**, together
with the media engine, GPU abstraction layer, export engine, localization, and
Qt UI — are licensed under the **GNU General Public License, version 3 or (at
your option) any later version** (`GPL-3.0-or-later`).

By submitting a contribution you agree that it is licensed under GPLv3-or-later.

The generative-AI capability is a **closed-source, account-gated hosted
service** and is **not** part of this repository beyond its GPLv3 client stub.
See the `NOTICE` file for the full open-source / closed-service split. Do not
add code to this repository that would place the open-source components under
any license other than GPLv3.

## Mandatory per-file license header

**Every** source file you add or substantially rewrite MUST begin with a GPLv3
license header. The first line MUST be the SPDX short identifier so automated
license scanners recognize the file:

- Hash-comment files (`CMakeLists.txt`, `*.cmake`, shell, YAML, Python):
  `# SPDX-License-Identifier: GPL-3.0-or-later`
- C-style files (`*.cpp`, `*.hpp`, `*.h`, `*.cc`, `*.qml`, GLSL shaders):
  `// SPDX-License-Identifier: GPL-3.0-or-later`

The canonical, ready-to-paste header templates (SPDX line plus the full FSF
"How to Apply These Terms" notice) live in
[`docs/LICENSE-HEADER.txt`](docs/LICENSE-HEADER.txt). Copy the template that
matches your file's comment style, replace `<YEAR>` with the creation year, and
fill in a one-line description of the file's purpose.

### Verifying headers locally

You can check that every tracked source file carries the SPDX identifier before
opening a pull request:

```sh
# Lists source files that are MISSING the SPDX header (should print nothing).
git ls-files '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.qml' \
             'CMakeLists.txt' '*.cmake' '*.sh' '*.py' \
  | while read -r f; do
      grep -qF 'SPDX-License-Identifier: GPL-3.0-or-later' "$f" || echo "MISSING HEADER: $f"
    done
```

Any file printed by that command must be fixed before the change is merged.

## Code layout

Source lives under `src/` split into `core`, `media`, `gpu`, `services`, `ui`,
and `app`, with tests under `tests/`. The domain core is UI-agnostic so the MCP
server and the agent can drive identical editing operations headlessly.
