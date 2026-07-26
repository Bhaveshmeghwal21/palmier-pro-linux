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
#
# A path is compiled in only when the corresponding PALMIER_ENABLE_* option is ON
# *and* its SDK was actually located during dependency discovery (see
# cmake/PalmierDependencies.cmake, which records PALMIER_<PATH>_AVAILABLE); the
# vendor imported target is linked only inside that same branch. Leaving an
# option ON on a host without the SDK therefore degrades to the software path
# instead of breaking configuration (Requirements 8.1, 8.9). This mirrors the
# ENABLE AND FOUND form used by src/gpu/CMakeLists.txt. The `<pkg>_FOUND`
# alternative keeps the gate correct with either spelling of the availability
# variable.
if(PALMIER_ENABLE_VAAPI AND (PALMIER_VAAPI_AVAILABLE OR LIBVA_FOUND))
    if(TARGET PkgConfig::LIBVA)
        target_link_libraries(palmier_media PUBLIC PkgConfig::LIBVA)
    endif()
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_VAAPI=1)
endif()

if(PALMIER_ENABLE_QSV AND (PALMIER_QSV_AVAILABLE OR LIBVPL_FOUND))
    if(TARGET PkgConfig::LIBVPL)
        target_link_libraries(palmier_media PUBLIC PkgConfig::LIBVPL)
    endif()
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_QSV=1)
endif()

if(PALMIER_ENABLE_NVENC AND (PALMIER_NVENC_AVAILABLE OR FFNVCODEC_FOUND))
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


# Decoder teardown queue (task 7.1; Requirement 14.8). Destroys retired
# MediaDecoder objects (and any other resource with a slow destructor) on one
# dedicated thread, so closing an FFmpeg decode context never blocks the
# playback thread, the audio callback thread or the Qt main thread. Standard
# library only — no FFmpeg, GPU or vendor SDK dependency — so it builds and is
# testable everywhere. Reused by the audio decoder in stage 8 (upstream PR 405).
# Added via target_sources (append-only) so it composes with the sources declared
# in the add_library() call above without editing that list.
target_sources(palmier_media PRIVATE DecoderTeardownQueue.cpp)


# Playback decode stage (task 7.3; Requirements 5.1, 5.5, 14.8).
#
#   * DecodeWorkerPool          — N = 2 worker threads, one MediaDecoder per
#                                 active asset, decoded frames pushed to a
#                                 bounded lock-protected per-clip queue, and
#                                 retired decoders handed to
#                                 DecoderTeardownQueue so an eviction never
#                                 blocks the playback thread.
#   * DecoderClipFrameProvider  — the gpu::ClipFrameProvider the Compositor
#                                 consumes: maps (Clip, timelinePosition) to
#                                 sourceIn + (position - timelineStart), keeps
#                                 decoders in an LRU cache, seeks when a request
#                                 is not the next sequential frame, converts
#                                 DecodedFrame to gpu::SourceFrame, and returns a
#                                 decode failure as an error so
#                                 Compositor::renderAt emits no partial frame.
#
# Both are standard-library plus Palmier::gpu only: decoders are opened through
# MediaDecoder's existing DecodeBackendFactory seam, so they build and are
# testable with no FFmpeg, GPU or media fixture. Added via target_sources
# (append-only) so they compose with the sources declared in the add_library()
# call above without editing that list.
target_sources(palmier_media PRIVATE
    DecodeWorkerPool.cpp
    DecoderClipFrameProvider.cpp)
