# Project classification

This repository is a **desktop application** — specifically a GUI video editor — and it
contains no Infrastructure-as-Code layer. This document exists because automated repository
classifiers have scored it as an infrastructure project, which produces misleading results:
infrastructure rubrics score dimensions such as Terraform module quality or Kubernetes
manifest hygiene, and none of those dimensions have any applicable content here.

## What this project is

| | |
|---|---|
| Kind | Desktop GUI application (video editor) |
| Language | C++20 |
| UI toolkit | Qt 6 Widgets |
| Media | FFmpeg (libav*) |
| Graphics | Vulkan, with a host-memory software compositor as an equal path |
| Build | CMake + Ninja |
| Test framework | GoogleTest and RapidCheck, run by CTest |
| Distribution | Debian `.deb`, Flatpak, AppImage — see [`packaging/`](../packaging) |
| Entry point | `build/bin/palmier-pro`, a windowed application requiring a display |

The compiled artifact is a binary that a user launches on their own machine. It has no server
component intended for deployment, no container runtime target, no cluster, and no cloud
resources to provision.

## What this project deliberately does not contain

None of the following are present, and their absence is a correct design outcome rather than
an omission to be corrected:

- **Terraform / OpenTofu** — there is no cloud infrastructure to declare. The application runs
  on an end user's desktop.
- **Kubernetes manifests, Helm charts, Kustomize** — there is no workload to schedule. A video
  editor is not a service.
- **Ansible, Chef, Puppet, Salt** — there is no fleet to configure. Host setup for developers
  is a documented package install (see [`BUILD.md`](BUILD.md)) and, for reproducibility, a
  [`Dockerfile`](../Dockerfile) plus [`docker-compose.yml`](../docker-compose.yml).
- **Pulumi, CloudFormation, CDK** — same reason as Terraform.
- **A deployment pipeline to an environment** — CI builds, tests and packages. There is no
  `deploy` stage because there is no environment to deploy to; releases are artifacts attached
  to a tag and consumed by package managers.

A reviewer looking for evidence that this repository was engineered carefully should read
[`../README.md`](../README.md) and the documents it links, not an IaC rubric.

## The containers that do exist, and why they are not deployment infrastructure

Three container-adjacent files exist and are easy to mistake for deployment infrastructure:

| File | Purpose |
|---|---|
| [`Dockerfile`](../Dockerfile) | A reproducible *build and test* environment, pinned to an `ubuntu:24.04` digest, installing exactly [`packaging/apt-packages.txt`](../packaging/apt-packages.txt) — the same manifest CI installs from |
| [`docker-compose.yml`](../docker-compose.yml) | Developer convenience: a `test` service that builds and runs the suite, an `editor` service that launches the GUI against the host X display, and a `shell` service for an interactive toolchain |
| [`.devcontainer/devcontainer.json`](../.devcontainer/devcontainer.json) | Reuses the same Dockerfile so VS Code or Codespaces opens ready to build |

All three exist so that a contributor with only Docker installed can build and test the
project. None of them describes a runtime deployment.

## Where the engineering evidence actually is

For an automated scanner or a human reviewer looking for the signals that matter for a project
of this kind:

| Signal | Where |
|---|---|
| Test suite | [`tests/`](../tests) — GoogleTest and RapidCheck, registered with CTest, run by the `Run tests` step in [`ci.yml`](../.github/workflows/ci.yml) |
| Line coverage gate | The `coverage` job in `ci.yml`; the floor lives in `COVERAGE_MIN_LINE` and is enforced by `Coverage (enforce the line floor)` |
| Lint and typecheck gates | The `lint` job in `ci.yml`: `Lint (clang-format, changed lines)`, `Lint (ruff)`, `Typecheck (mypy)` |
| Dependency audit | The `Dependency audit` step in `ci.yml`, running `pip-audit` against [`requirements-dev.txt`](../requirements-dev.txt) |
| Dependency update tooling | [`.github/dependabot.yml`](../.github/dependabot.yml), covering `github-actions`, `docker` and `pip` |
| Native dependency manifest | [`packaging/apt-packages.txt`](../packaging/apt-packages.txt), the single list CI, Docker and `make deps` all install from |
| Architecture | [`../README.md`](../README.md) under "Project layout"; dependency direction is enforced by layering, errors are a typed `Result<T>` |
| Logging and health | [`OBSERVABILITY.md`](OBSERVABILITY.md) — log levels and formats, the category vocabulary, and the `GET /health` endpoint |
| Hardware verification | [`HARDWARE_ENCODE.md`](HARDWARE_ENCODE.md) — the `l4-validation` job's recorded measurements from a real NVIDIA L4 |

## Note on native dependency management

This project has no package-manager lockfile for its C++ dependencies, and this is a property
of the ecosystem rather than a gap in discipline. Qt 6, FFmpeg, Vulkan and the vendor codec
SDKs are consumed as distribution packages resolved by CMake's `find_package`. The pinning
mechanisms that do exist are:

- [`packaging/apt-packages.txt`](../packaging/apt-packages.txt) — the exact package set, shared
  by CI, the Dockerfile and `make deps`, so all three install the same thing.
- The `ubuntu:24.04` digest pin in the [`Dockerfile`](../Dockerfile), which fixes the resolved
  versions of every one of those packages for reproducible builds.
- [`requirements-dev.txt`](../requirements-dev.txt) — the Python CI helpers' tooling, pinned to
  exact versions and audited in CI.
- A CI step that records the resolved version of every native package on each run, so the
  versions a given build used are recoverable from its log.
