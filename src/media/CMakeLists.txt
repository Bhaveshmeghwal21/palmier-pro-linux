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
    AudioGraph.cpp
    # Peak envelopes for timeline waveform drawing (monitoring-and-grading
    # Requirement 2). Both are pure standard library — the bucketed min/max
    # reduction and the bounded LRU store — with no decoder, thread or Qt
    # dependency, so they build and are unit-tested wherever palmier_media does.
    PeakEnvelope.cpp
    PeakEnvelopeCache.cpp)
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



# Audio output seam (task 8.3; Requirements 6.2, 6.3, 6.7).
#
# media/AudioSink.{hpp,cpp} declares IAudioSink — the interface the PipeWire and
# ALSA sinks of task 8.6 will implement — and the always-compiled NullAudioSink,
# which consumes and discards buffers while advancing a monotonic sample position
# from an INJECTABLE steady clock. That is what makes "audio output unavailable"
# (Requirement 6.7) a normal code path, keeps the sink-as-master-clock design
# (Requirement 6.3) working with no device present, and lets the whole audio stage
# be tested in CI on a machine with no sound card and without a single sleep.
#
# Standard library plus the existing AudioGraph value types only — no PipeWire,
# ALSA, FFmpeg or GPU dependency. Added via target_sources (append-only) so it
# composes with the sources declared in the add_library() call above without
# editing that list.
target_sources(palmier_media PRIVATE AudioSink.cpp)



# Audio engine (task 8.4; Requirements 6.2, 6.3, 6.4, 6.6, 6.7, 6.8, 6.9, 6.11).
#
# media/AudioEngine.{hpp,cpp} composes what already exists rather than inventing a
# parallel mixer: MediaDecoder's audio surface (task 8.1) supplies source blocks,
# the existing AudioGraph does every bit of the resampling, channel mapping,
# per-clip gain, summing and [-1, 1] clamping, and IAudioSink (task 8.3) receives
# the fixed 48 000 Hz / 2-channel / F32 output and serves as the master clock.
# Retired decoders go to DecoderTeardownQueue (task 7.1), which is why that queue
# had to land first.
#
# Every seam is injectable — project provider, sink, decode backend factory, asset
# path resolver and the steady clock — so the engine is exercisable with no
# FFmpeg, no GPU, no sound card, no media fixture and no sleeping. Added via
# target_sources (append-only) so it composes with the sources declared in the
# add_library() call above without editing that list.
target_sources(palmier_media PRIVATE AudioEngine.cpp)



# Encoder selection policy (task 9.1; Requirements 8.2, 8.3, 8.4, 8.8).
#
# media/EncoderSelector.{hpp,cpp} owns the export-time decision between a vendor
# hardware encoder and the FFmpeg software encoder: the request/compiled-in/
# capability gate, the capability probe run on a detached thread under a 3000 ms
# deadline, the retry-hardware-initialization-exactly-once rule, and the single
# EncoderSelection value that reports the outcome. That value has no public
# constructor — only EncoderSelection::hardware() and EncoderSelection::software()
# — so "hardware used" and "software fallback" cannot both be reported by any
# expressible program (Requirement 8.8 as a type-level invariant).
#
# Standard library plus the existing gpu::CodecBridge / gpu::GpuTypes value types
# only: no FFmpeg, no vendor SDK and no GPU. Device capabilities and the
# compiled-in state arrive as VALUES (gpu::GpuCaps, gpu::BridgeAvailability), and
# the probe, the probe-await strategy and hardware initialization are all
# injectable, so the whole policy is exercisable on a host with neither a GPU nor
# a vendor SDK and without any test sleeping. Added via target_sources
# (append-only) so it composes with the sources declared in the add_library()
# call above without editing that list.
target_sources(palmier_media PRIVATE EncoderSelector.cpp)




# Real audio output sinks + startup selection (task 8.6; Requirements 6.2, 6.7).
#
#   * media/PipeWireAudioSink — the primary sink (libpipewire-0.3, MIT): one
#                               pw_thread_loop carrying one 48 kHz / 2-channel /
#                               F32 playback stream, fed from a bounded FIFO by
#                               the loop's process callback.
#   * media/AlsaAudioSink     — the fallback sink (libasound2,
#                               LGPL-2.1-or-later): the `default` PCM device
#                               opened non-blocking, with xrun recovery and a
#                               bounded write budget so submit() never blocks the
#                               mixing thread.
#   * media/AudioSinkSelector — the single place the design's startup order
#                               PipeWire -> ALSA -> Null lives. A candidate is
#                               selected only if it actually OPENS, so a build
#                               with PipeWire compiled in but no daemon running
#                               still falls through to ALSA and then to the
#                               always-compiled NullAudioSink.
#
# Both sinks are compiled unconditionally, and both keep every third-party type
# behind an opaque Impl, so nothing above this layer needs the backends' include
# paths. Only the BACKEND CALLS are guarded: PALMIER_HAVE_PIPEWIRE /
# PALMIER_HAVE_ALSA are defined, and PkgConfig::LIBPIPEWIRE / PkgConfig::LIBASOUND
# linked, only when the corresponding PALMIER_ENABLE_* option is ON *and* the
# library was actually located by cmake/PalmierDependencies.cmake (which records
# PALMIER_PIPEWIRE_AVAILABLE / PALMIER_ALSA_AVAILABLE). Without a guard the class
# still exists and its start() reports "not compiled in", which the selector
# records as a skipped candidate — so leaving an option ON on a host without the
# library degrades to the null sink instead of breaking configuration
# (Requirements 6.7, 8.9). This is the same ENABLE AND FOUND form used above for
# the vendor codec paths and in src/gpu/CMakeLists.txt; the `<pkg>_FOUND`
# alternative keeps the gate correct with either spelling of the availability
# variable.
#
# Added via target_sources (append-only) so they compose with the sources declared
# in the add_library() call above without editing that list.
target_sources(palmier_media PRIVATE
    PipeWireAudioSink.cpp
    AlsaAudioSink.cpp
    AudioSinkSelector.cpp)

if(PALMIER_ENABLE_PIPEWIRE AND (PALMIER_PIPEWIRE_AVAILABLE OR LIBPIPEWIRE_FOUND))
    if(TARGET PkgConfig::LIBPIPEWIRE)
        target_link_libraries(palmier_media PRIVATE PkgConfig::LIBPIPEWIRE)
    endif()
    # The imported target is PRIVATE — no header exposes a PipeWire type, so no
    # consumer needs its include path (CMake still propagates the link
    # requirement for a static library). The DEFINITION is PUBLIC, because
    # PipeWireAudioSink::compiledIn() is a constexpr predicate in the header:
    # were the definition private, a test translation unit would read "not
    # compiled in" while the library had in fact compiled the real backend.
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_PIPEWIRE=1)
endif()

if(PALMIER_ENABLE_ALSA AND (PALMIER_ALSA_AVAILABLE OR LIBASOUND_FOUND))
    if(TARGET PkgConfig::LIBASOUND)
        target_link_libraries(palmier_media PRIVATE PkgConfig::LIBASOUND)
    endif()
    target_compile_definitions(palmier_media PUBLIC PALMIER_HAVE_ALSA=1)
endif()
