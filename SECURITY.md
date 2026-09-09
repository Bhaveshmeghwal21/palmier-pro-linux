# Security policy

## Reporting a vulnerability

Report privately through GitHub's **Report a vulnerability** button under this
repository's *Security* tab, which opens a private advisory visible only to the
maintainers. Please do not open a public issue for anything exploitable.

No email address is published here on purpose: an address in a file is a thing that
rots, and the advisory flow gives you a private, tracked channel with a record.

Include what you would want if you were fixing it: the version or commit, the
configuration (particularly whether remote access was enabled), and the smallest
sequence of steps that shows the problem.

Expect an acknowledgement within a few days. This is a small project and there is no
paid on-call rotation, so please size your expectations accordingly — but a genuine
report will get a genuine answer.

## Supported versions

| Version | Supported |
|---|---|
| `main` | Yes — fixes land here first |
| `v0.1.x` | Yes |
| Older tags | No |

## The security model, and where the boundaries are

Understanding these three points will tell you whether something is a vulnerability or
the documented design.

### 1. The local MCP endpoint is unauthenticated, and that is the loopback bargain

Palmier Pro runs a Model Context Protocol server in-process so external AI agents can
drive the editor. By default it binds `127.0.0.1:19789` and requires no credential.

That is deliberate: on loopback the operating system is the access control, and every
process that could reach the socket could already read your project files directly. A
token there would be security theatre.

It follows that **binding this endpoint to a non-loopback address without a token is a
serious misconfiguration**, because the endpoint can edit and export your work and read
media paths. `PALMIER_MCP_HOST=0.0.0.0` is the wrong way to get remote access. Use the
remote-access feature, which was built for it.

### 2. Remote access is opt-in, token-gated, and requires explicit acknowledgement

Non-loopback access needs, at minimum, `PALMIER_REMOTE_ENABLED`, a
`PALMIER_REMOTE_BEARER_TOKEN`, and `PALMIER_REMOTE_ACKNOWLEDGED`. The separate
acknowledgement exists so exposure is never one accidental setting. TLS material, an
Origin allow-list, a session cap and an idle timeout are all available and documented
in [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md).

Without TLS configured the listener serves plain HTTP, which puts the bearer token on
the wire in clear text. If you enable remote access across a network you do not
control, configure TLS.

### 3. The generative capability is a separate, account-gated service

The editor, its MCP server and its agent chat are GPLv3 and work with no network
connection. The generative-AI backend is a closed hosted service reached by a GPLv3
client. Credentials for it are stored through libsecret, not in the project file. When
it is unreachable or unauthenticated the editor keeps working — see
[`NOTICE`](NOTICE).

## Known considerations that are not bugs

- **Test fixtures contain credential-shaped strings.** `Authorization: Bearer ...`
  literals appear in the transport tests because that is what those tests assert on.
  They are fabricated constants, not live secrets. `tests/docs/repository_hygiene_property_test.cpp`
  scans `src/`, `tests/` and `cmake/` on every CI run for credential-shaped assignments
  and for missing licence headers, and that scan is part of the suite rather than an
  external service.
- **`docker compose up editor` mounts your X11 socket.** Any container with that socket
  can read your keystrokes and screen. This is the standard cost of running a GUI from a
  container; it is why the service is opt-in and annotated in `docker-compose.yml`
  rather than being the default.
- **`.env` is git-ignored; `.env.example` is committed with every value empty.** If you
  find a populated secret in the repository's history, that is a report worth making.

## What is verified, and what is not

Being straight about this is more useful than a claim of completeness:

- The full test suite runs on every push, headlessly, on `ubuntu-24.04`. The remote
  access gate, the Origin allow-list, the token check and the session limits all have
  dedicated property tests.
- There is **no** third-party dependency-audit service wired into CI for the native
  libraries, because Qt, FFmpeg, Vulkan and OpenSSL are consumed as distribution
  packages and tracked at the Ubuntu 24.04 LTS archive's versions. See
  [`packaging/apt-packages.txt`](packaging/apt-packages.txt) for the authoritative list
  and the reasoning.
- No external penetration test has been performed on this project.
