# SPDX-License-Identifier: GPL-3.0-or-later
#
# src/media — FFmpeg-based media engine.
#
# Demux/probe, decode (HW-preferred), encode (HW-preferred), and the audio graph.
# Links the FFmpeg libav* libraries and, when enabled, the vendor hardware-codec
# support libraries.
#
# Task 8.1 introduces the first concrete sources: container probing and MediaInfo
# extraction (streams, codecs, resolution, fps). Task 8.2 adds MediaDecoder:
# open/nextFrame/seek with HW-preferred, SW-fallback routing through the GPU
# CodecBridge and zero-copy frames via the FramePool. The FFmpeg-specific probe
# and decode backend are compiled only when the libav* libraries are present
# (PALMIER_HAVE_FFMPEG); otherwise the module still builds and those paths report
# that FFmpeg support is unavailable, so the value types, normalization, and the
# decoder's routing/fallback logic build and are testable without FFmpeg.
# MediaEncoder/audio graph/import validation arrive in tasks 8.3-8.5.

add_library(palmier_media STATIC
    MediaProbe.cpp
    MediaDecoder.cpp
    MediaEncoder.cpp
    AudioGraph.cpp)
add_library(Palmier::media ALIAS palmier_media)

# Public headers are included as "media/<Header>.hpp" / "core/<Header>.hpp".
target_include_directories(palmier_media PUBLIC
    "${PROJECT_SOURCE_DIR}/src")

target_compile_features(palmier_media PUBLIC cxx_std_20)

target_link_libraries(palmier_media PUBLIC
    Palmier::core
    Palmier::gpu)

# FFmpeg (libav*) is a required project dependency (see cmake/PalmierDependencies.cmake).
# When its imported target is present we link it and enable the FFmpeg-backed
# probe/decode/encode paths; if it is somehow absent the module still builds and
# the probe degrades to a clear "FFmpeg unavailable" error at runtime.
if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(palmier_media PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_FFMPEG=1)
endif()

# Vendor hardware-codec paths are conditionally compiled in. Each define lets
# the media engine select the appropriate FFmpeg hwaccel/encoder at runtime.
if(PALMIER_ENABLE_VAAPI)
    target_link_libraries(palmier_media PUBLIC PkgConfig::LIBVA)
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_VAAPI=1)
endif()

if(PALMIER_ENABLE_QSV)
    target_link_libraries(palmier_media PUBLIC PkgConfig::LIBVPL)
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_QSV=1)
endif()

if(PALMIER_ENABLE_NVENC)
    if(TARGET PkgConfig::FFNVCODEC)
        target_link_libraries(palmier_media PUBLIC PkgConfig::FFNVCODEC)
    endif()
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_NVENC=1)
endif()

if(PALMIER_WERROR)
    target_compile_options(palmier_media PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>)
endif()


# Import validation and error handling (task 8.5; Requirements 3.2, 3.3).
# Classifies a probed file into accept / unsupported-format / unreadable. Added
# via target_sources (append-only) so it composes with the sources declared in
# the add_library() call above without editing that list.
target_sources(palmier_media PRIVATE ImportValidation.cpp)



# Export Engine render loop + progress reporting (task 10.1; Requirements 11.1,
# 11.2, 10.3, 10.8). Renders the timeline via the Compositor and encodes it via
# MediaEncoder into a single output file, reporting monotonic 0-100% progress.
# Backend-agnostic (no direct FFmpeg dependency): it composes MediaEncoder behind
# the existing EncodeBackendFactory seam, so it builds and is testable without
# FFmpeg/GPU. Added via target_sources (append-only) so it composes with the
# sources declared in the add_library() call above without editing that list.
target_sources(palmier_media PRIVATE ExportEngine.cpp)
