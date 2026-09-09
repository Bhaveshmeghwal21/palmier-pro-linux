# SPDX-License-Identifier: GPL-3.0-or-later
#
# Palmier Pro for Linux — reproducible build and test environment.
#
# Purpose: let someone with nothing but Docker installed configure, compile and run
# the entire test suite, on the same distribution CI uses, without hand-installing
# Qt 6, FFmpeg, Vulkan and the vendor codec SDKs.
#
#     docker build -t palmier-pro:local .
#     docker run --rm palmier-pro:local make test
#
# or simply `make docker-test`.
#
# The package list is NOT duplicated here. It is read from packaging/apt-packages.txt,
# the same file `make deps` and the CI workflow install from, so the image and CI
# cannot drift apart. A test in the suite is not what guards that — the manifest is
# consumed rather than copied, which is stronger.
#
# ubuntu:24.04 is pinned by digest below. The tag alone would let the base move under
# a rebuild; the digest makes "the same Dockerfile" mean the same bytes. Update it
# deliberately (Dependabot watches this file for exactly that reason). The digest is
# the multi-architecture manifest list for the 24.04 tag as published on 2026-08-19,
# so it resolves correctly on both amd64 and arm64.
#
# WHAT THIS IMAGE CANNOT DO. It has no GPU, no sound card and no display. The editor
# is fully functional in that state by design — it reports "no compatible GPU was
# found; using CPU processing", selects the null audio sink, and keeps working — so
# the suite passes, but hardware encode and audible playback are not exercised here.
# Pass `--device /dev/dri` and a PipeWire socket to test those on a capable host.

FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517 AS build

# Non-interactive apt, and keep the layer cache useful by installing before copying source.
ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    CCACHE_DIR=/ccache

# The dependency manifest is copied on its own so editing source does not invalidate
# the (expensive) dependency layer.
COPY packaging/apt-packages.txt /tmp/apt-packages.txt

# Two groups, deliberately separate:
#   * the project's own dependencies, read from the manifest;
#   * tooling this image adds so `make lint`, `make coverage` and `make deps` work
#     inside the container. These are NOT in the manifest because the manifest is a
#     faithful copy of what the product needs to build, and adding developer tooling
#     to it would make CI install things it does not use.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        $(sed -e 's/#.*//' /tmp/apt-packages.txt) \
        ca-certificates \
        git \
        make \
        python3 \
        python3-pip \
        clang-format \
        clang-tidy \
        lcov \
        gcovr \
    && rm -rf /var/lib/apt/lists/* /tmp/apt-packages.txt

# ffnvcodec (nv-codec-headers) is header-only plus a pkg-config file and is
# intermittently missing from the archive snapshot, so it comes from upstream at the
# tag CI uses. Without it the NVENC path reports itself unavailable and the software
# encoder is used; the build and the suite are unaffected.
ARG NV_CODEC_HEADERS_TAG=n13.0.19.0
RUN git clone --depth 1 --branch "${NV_CODEC_HEADERS_TAG}" \
        https://github.com/FFmpeg/nv-codec-headers.git /tmp/nv-codec-headers \
    && make -C /tmp/nv-codec-headers install \
    && rm -rf /tmp/nv-codec-headers \
    && pkg-config --modversion ffnvcodec

# Build and run as an unprivileged user. The editor never needs root, and a container
# that builds as root leaves root-owned artefacts in a bind-mounted working tree.
ARG UID=1001
ARG GID=1001
RUN groupadd --gid "${GID}" palmier \
    && useradd --uid "${UID}" --gid "${GID}" --create-home --shell /bin/bash palmier \
    && mkdir -p /ccache /workspace \
    && chown -R palmier:palmier /ccache /workspace

WORKDIR /workspace
COPY --chown=palmier:palmier . /workspace
USER palmier

# Compile during the build so `docker run ... make test` starts testing immediately.
# Kept as a separate layer from the source copy so a rebuild after a source edit
# reuses every dependency layer above.
RUN make build

# No CMD that starts a GUI: this image's job is building and testing. `make test`
# wraps CTest in xvfb-run, so it needs no display from the host.
CMD ["make", "test"]
