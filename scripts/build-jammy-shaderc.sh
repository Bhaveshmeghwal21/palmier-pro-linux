#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Ubuntu 22.04 has no shaderc development package. Build the static library
# with the exact glslang and SPIRV revisions listed in shaderc v2024.4's DEPS.
set -euo pipefail

cd "$(dirname -- "$0")/.."
prefix="$PWD/build-deps/shaderc"
source_dir="$prefix/src"
mkdir -p "$source_dir"

checkout() {
    local dir=$1 url=$2 revision=$3
    if [[ ! -d "$dir/.git" ]]; then
        git init -q "$dir"
    fi
    if [[ $(git -C "$dir" rev-parse --verify HEAD 2>/dev/null || true) != "$revision" ]]; then
        git -C "$dir" fetch --depth 1 "$url" "$revision"
        git -C "$dir" checkout --detach FETCH_HEAD
    fi
}

checkout "$source_dir" https://github.com/google/shaderc.git \
    caa54d9779d5605aca4e1a0c0c962a3d8f4aeb31
checkout "$source_dir/third_party/glslang" https://github.com/KhronosGroup/glslang.git \
    a0995c49ebcaca2c6d3b03efbabf74f3843decdb
checkout "$source_dir/third_party/spirv-headers" https://github.com/KhronosGroup/SPIRV-Headers.git \
    3f17b2af6784bfa2c5aa5dbb8e0e74a607dd8b3b
checkout "$source_dir/third_party/spirv-tools" https://github.com/KhronosGroup/SPIRV-Tools.git \
    4d2f0b40bfe290dea6c6904dafdf7fd8328ba346

cmake -S "$source_dir" -B "$prefix/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSHADERC_SKIP_TESTS=ON \
    -DSHADERC_SKIP_EXAMPLES=ON \
    -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSHADERC_ENABLE_WERROR_COMPILE=OFF \
    -DSPIRV_SKIP_EXECUTABLES=ON \
    -DENABLE_GLSLANG_BINARIES=OFF
cmake --build "$prefix/build" --target shaderc_combined --parallel "${JOBS:-$(nproc)}"

licenses="$prefix/licenses"
mkdir -p "$prefix/lib" "$prefix/include" "$licenses"
cp -R "$source_dir/libshaderc/include/shaderc" "$prefix/include/"
install -m 644 "$source_dir/LICENSE" "$licenses/shaderc.txt"
install -m 644 "$source_dir/third_party/glslang/LICENSE.txt" "$licenses/glslang.txt"
install -m 644 "$source_dir/third_party/spirv-headers/LICENSE" "$licenses/spirv-headers.txt"
install -m 644 "$source_dir/third_party/spirv-tools/LICENSE" "$licenses/spirv-tools.txt"
archive="$prefix/lib/libshaderc_combined.a"
install -m 644 "$prefix/build/libshaderc/libshaderc_combined.a" "$archive.tmp"
mv -f -- "$archive.tmp" "$archive"
