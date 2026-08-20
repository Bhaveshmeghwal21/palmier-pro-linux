// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaEncoder.hpp — encode composited frames to an output stream,
// preferring hardware and falling back to software transparently.
//
// This is the encode stage of the Media Engine (design.md "Component 3: Media
// Engine (FFmpeg)"). MediaEncoder::create prepares a per-output encode pipeline
// for an EncodeSpec (codec, bitrate, resolution, frame rate, and a hardware
// preference); submit() queues one composited frame — a gpu::RenderedFrame from
// the Compositor (task 7.3) — to the encoder, accepting either a GPU-resident
// (zero-copy) frame or a CPU (host-memory) frame; finish() flushes the encoder
// and finalizes the output.
//
// Routing + fallback (Requirements 10.2 / 10.5) is delegated to the GPU layer's
// CodecBridge exactly as MediaDecoder does, but the encoder's fallback pivots at
// a different moment than the decoder's. The decoder retries an individual frame
// on the CPU; an encoder is a stateful, stream-producing object, so a mid-stream
// CPU switch would corrupt the output. Instead the design's Error Handling table
// ("HW encoder init failure -> retry with software encoder", Requirement 10.5)
// fixes the fallback at *initialization*: create() first tries to build the
// encoder on the routed hardware backend and, if that init fails, logs the
// failure and retries the build exactly once on the FFmpeg software encoder. The
// resulting encoder is then bound to a single backend for the whole stream, so
// every submit() goes to the same (hardware or software) encoder and the output
// is never corrupted by a partial switch.
//
// Audio stream (task 9.3; Requirement 6.5). An EncodeSpec may carry an
// AudioEncodeSpec, in which case the encoder muxes EXACTLY ONE audio stream
// alongside the video stream: submitAudio() queues mixed, interleaved-float
// blocks at the Audio_Engine output format (48 kHz / 2 channels by default), and
// finish() flushes BOTH streams before writing the container trailer. The two
// streams are independent queues with independent ordering guards, which is what
// lets ExportEngine interleave them frame interval by frame interval without the
// video stream's pacing constraining the audio stream's block size. An encoder
// built without an AudioEncodeSpec rejects submitAudio() with FailedPrecondition
// rather than silently discarding audio.
//
// Testability: the actual codec work lives behind the IEncodeBackend seam
// (mirroring MediaDecoder's IDecodeBackend). create() takes an
// EncodeBackendFactory that builds *and initializes* a backend for a chosen
// route; the default factory builds the FFmpeg (libavformat/libavcodec/
// libswscale) backend, compiled only when PALMIER_HAVE_FFMPEG is defined. On a
// machine without FFmpeg the module still builds and the default factory reports
// FailedPrecondition, while the whole create/submit/finish surface — including
// the HW-preferred / SW-fallback-on-init routing, the resolution/order guards,
// and GPU-vs-CPU frame acceptance — stays exercisable with a mock backend (see
// tests/media_encoder_test.cpp).

#ifndef PALMIER_MEDIA_MEDIAENCODER_HPP
#define PALMIER_MEDIA_MEDIAENCODER_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <optional>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/FramePool.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioGraph.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// AudioEncodeSpec — the optional second (audio) stream
// ---------------------------------------------------------------------------

/// The audio stream muxed alongside the video stream when an export includes
/// audio (design.md D7 "Decision — export audio"; Requirement 6.5). The defaults
/// are the Audio_Engine output format — 48 000 Hz, 2 interleaved channels — so a
/// caller that simply asks for audio gets exactly the format `AudioGraph` mixes
/// into and `AudioEngine::renderRange` produces, with no conversion in between.
///
///   * sampleRate / channels — the format submitAudio() accepts. Every submitted
///     AudioBuffer must declare exactly these, so no silent resample happens
///     inside the encoder.
///   * codecName — the FFmpeg audio encoder short name ("aac" by default).
///   * bitrateBitsPerSecond — target average bit rate; 0 means "let the backend
///     choose"; a negative value is rejected.
struct AudioEncodeSpec {
    int          sampleRate{48'000};
    int          channels{2};
    std::string  codecName{"aac"};
    std::int64_t bitrateBitsPerSecond{192'000};

    /// The interleaved-float working format this stream is submitted in.
    [[nodiscard]] AudioFormat format() const noexcept {
        return AudioFormat{sampleRate, channels, SampleFormat::F32};
    }
};

// ---------------------------------------------------------------------------
// EncodeSpec — what to encode and how
// ---------------------------------------------------------------------------

/// Everything MediaEncoder::create needs to build an encode pipeline, plus the
/// GPU context it routes against.
///
///   * codec — the output video codec (gpu::CodecId). CodecId::Unknown is
///     rejected: there is no encoder for it.
///   * bitrateBitsPerSecond — target average bit rate; 0 means "let the backend
///     choose a sensible default"; a negative value is rejected.
///   * resolution — the encoded frame size. Every submitted frame's geometry
///     must match this (the design's submit precondition).
///   * frameRate — the output frame rate; must be valid (positive rational).
///   * preferHardware — when false the encoder forces the software path
///     regardless of device capabilities (the parity/fallback lane of
///     Requirement 10.4/13.3).
///   * caps — capabilities of the selected device; the CodecBridge routes encode
///     to a hardware backend only when these advertise the codec for encode.
///   * availability — which vendor hardware backends are compiled into this
///     build (defaults to the PALMIER_HAVE_* build configuration).
///   * outputPath / containerFormat — where the muxed output is written and the
///     container short-name (e.g. "mp4"); consumed by the FFmpeg backend and
///     ignored by mock backends.
///   * audio — when set, ONE audio stream is muxed alongside the video stream
///     and submitAudio() is accepted; when unset the output is video-only and
///     submitAudio() reports FailedPrecondition (Requirement 6.5).
struct EncodeSpec {
    gpu::CodecId                   codec{gpu::CodecId::H264};
    std::int64_t                   bitrateBitsPerSecond{0};
    Resolution                     resolution{};
    FrameRate                      frameRate{FrameRate::fps30()};
    bool                           preferHardware{true};
    gpu::GpuCaps                   caps{gpu::GpuCaps::software()};
    gpu::BridgeAvailability        availability{gpu::BridgeAvailability::fromBuildConfig()};
    std::filesystem::path          outputPath{};
    std::string                    containerFormat{};
    std::optional<AudioEncodeSpec> audio{};
};

// ---------------------------------------------------------------------------
// EncoderInputFrame — a GPU-resident or CPU frame, as the backend sees it
// ---------------------------------------------------------------------------

/// One frame handed to an encode backend, projected from a gpu::RenderedFrame so
/// the backend never depends on the compositor's types. It carries both a
/// binding handle (for the zero-copy hardware path) and a host-memory pointer
/// (for the software path); `gpuResident` says which the frame primarily is.
struct EncoderInputFrame {
    Duration          presentation{Duration::zero()};
    gpu::FrameDesc    desc{};
    bool              gpuResident{false};   ///< image is a zero-copy GPU import.
    gpu::ImageHandle  image{};              ///< binding handle (hardware path).
    const void*       hostData{nullptr};    ///< host-memory RGBA pixels (software path; may be null).
};

// ---------------------------------------------------------------------------
// EncoderInputAudio — one mixed audio block handed to an encode backend
// ---------------------------------------------------------------------------

/// One block of mixed, interleaved-float audio queued to the audio stream. The
/// buffer is borrowed for the duration of the call: a backend that buffers must
/// copy. `presentation` is the block's timeline position, non-decreasing across
/// consecutive submissions (the same ordering rule the video stream follows).
struct EncoderInputAudio {
    Duration           presentation{Duration::zero()};
    const AudioBuffer* buffer{nullptr};
};

// ---------------------------------------------------------------------------
// Encode backend seam
// ---------------------------------------------------------------------------

/// The pluggable encode implementation. The FFmpeg backend provides the concrete
/// codec/mux work; tests supply mocks. This seam keeps MediaEncoder's routing,
/// fallback, and validation policy free of any FFmpeg dependency and unit-
/// testable without a GPU or FFmpeg.
///
/// A backend is created *already initialized* by the factory: if hardware
/// encoder initialization fails, the factory returns an Error so create() can
/// retry once on the software encoder (Requirement 10.5). Once built, encode()
/// queues frames in submission (presentation) order and finish() flushes and
/// finalizes the stream.
class IEncodeBackend {
public:
    virtual ~IEncodeBackend() = default;

    /// Queue one frame to the encoder. Accepts a GPU-resident frame (zero-copy
    /// via `frame.image`) or a CPU frame (`frame.hostData`). An error must leave
    /// the output stream uncorrupted so the caller can surface it and stop.
    [[nodiscard]] virtual Result<void> encode(const EncoderInputFrame& frame) = 0;

    /// Queue one block of mixed audio to the audio stream (Requirement 6.5).
    ///
    /// This is NOT pure-virtual on purpose: a backend that muxes video only —
    /// which is every backend built from an EncodeSpec whose `audio` is unset,
    /// and every video-only test mock — inherits a definition that reports
    /// Unsupported. MediaEncoder never calls it unless the spec configured an
    /// audio stream, so the default is unreachable for a correctly-built
    /// encoder and exists so adding the audio stream did not break every
    /// existing IEncodeBackend implementation.
    [[nodiscard]] virtual Result<void> encodeAudio(const EncoderInputAudio& audio) {
        (void)audio;
        return unsupported("this encode backend has no audio stream");
    }

    /// Flush any buffered frames — BOTH streams when an audio stream is
    /// configured — and finalize the output (write the trailer/mux).
    [[nodiscard]] virtual Result<void> finish() = 0;
};

/// Builds and initializes an encode backend for `spec` on the chosen `route`.
/// Mirrors MediaDecoder's injectable backend seam so create() is testable
/// without FFmpeg. Returning an Error for a hardware route lets create() retry
/// once on the software route (Requirement 10.5).
using EncodeBackendFactory =
    std::function<Result<std::unique_ptr<IEncodeBackend>>(const EncodeSpec&,
                                                          const gpu::CodecRoute&)>;

/// True iff this build was compiled with FFmpeg encode support
/// (PALMIER_HAVE_FFMPEG). When false the default factory reports
/// FailedPrecondition. (Shares the build flag with the decoder/probe.)
[[nodiscard]] bool isFfmpegEncodeAvailable() noexcept;

/// The default, FFmpeg-backed encode backend factory. When FFmpeg is not
/// compiled in it returns an Error describing that encoding is unavailable.
[[nodiscard]] EncodeBackendFactory ffmpegEncodeBackendFactory();

// ---------------------------------------------------------------------------
// MediaEncoder
// ---------------------------------------------------------------------------

/// Encodes composited frames to an output stream, preferring hardware and
/// falling back to the software encoder when hardware initialization fails
/// (design "Component 3"; Requirements 10.2, 10.5).
class MediaEncoder {
public:
    MediaEncoder(MediaEncoder&&) noexcept = default;
    MediaEncoder& operator=(MediaEncoder&&) noexcept = default;
    MediaEncoder(const MediaEncoder&) = delete;
    MediaEncoder& operator=(const MediaEncoder&) = delete;

    /// Create an encoder for `spec` with the default FFmpeg backend. Errors:
    ///   * InvalidArgument   — degenerate resolution, invalid frame rate, or a
    ///                         negative bit rate.
    ///   * Unsupported       — the codec has no encoder (CodecId::Unknown).
    ///   * FailedPrecondition — this build has no FFmpeg encode support.
    ///   * (backend errors)  — from the backend while initializing the encoder,
    ///                         after the HW->SW init retry has been exhausted.
    [[nodiscard]] static Result<MediaEncoder> create(const EncodeSpec& spec);

    /// Create an encoder with an injected backend factory (the testing seam).
    [[nodiscard]] static Result<MediaEncoder> create(const EncodeSpec& spec,
                                                     const EncodeBackendFactory& factory);

    /// Queue one composited frame to the encoder in presentation order. Accepts a
    /// GPU-resident (zero-copy) frame or a CPU frame. Errors (each of which
    /// leaves the output stream uncorrupted and the encoder state unchanged):
    ///   * FailedPrecondition — the encoder was already finished.
    ///   * InvalidArgument   — the frame has no backing, its resolution does not
    ///                         match the spec, or its presentation time regresses
    ///                         below the previously submitted frame.
    ///   * (backend errors)  — propagated unchanged.
    [[nodiscard]] Result<void> submit(const gpu::RenderedFrame& frame);

    /// Queue one block of mixed timeline audio to the audio stream at timeline
    /// position `presentation` (Requirement 6.5). `buffer` must be interleaved
    /// float at exactly the spec's audio sample rate and channel count — the
    /// Audio_Engine output format — so the encoder never resamples silently.
    /// Errors (each of which leaves the output stream uncorrupted and the
    /// encoder state unchanged):
    ///   * FailedPrecondition — the encoder was already finished, or this
    ///                          encoder has no audio stream (spec.audio unset).
    ///   * InvalidArgument   — the buffer's sample rate or channel count does
    ///                          not match the audio spec, or its presentation
    ///                          time regresses below the previously submitted
    ///                          block.
    ///   * (backend errors)  — propagated unchanged.
    ///
    /// An EMPTY buffer (zero frames) at a non-regressing presentation time is
    /// accepted and forwarded: a frame interval that mixes to no audio frames is
    /// a normal occurrence at high frame rates, not an error.
    [[nodiscard]] Result<void> submitAudio(const AudioBuffer& buffer, Duration presentation);

    /// Flush and finalize the output: both the video stream and, when one is
    /// configured, the audio stream, then the container trailer. Idempotency: a
    /// second call fails with FailedPrecondition. On backend failure the encoder
    /// is still marked finished (no further frames are accepted) and the error is
    /// returned.
    [[nodiscard]] Result<void> finish();

    /// The EncodeSpec the encoder was created with.
    [[nodiscard]] const EncodeSpec& spec() const noexcept { return spec_; }

    /// The route the encoder is bound to (the backend that finally initialized).
    [[nodiscard]] const gpu::CodecRoute& route() const noexcept { return route_; }

    /// True when the encoder runs on a hardware backend.
    [[nodiscard]] bool isHardware() const noexcept { return route_.hardware; }

    /// True when hardware encoder initialization failed and create() fell back to
    /// the software encoder (Requirement 10.5).
    [[nodiscard]] bool usedSoftwareFallback() const noexcept { return usedSoftwareFallback_; }

    /// True once finish() has been called.
    [[nodiscard]] bool isFinished() const noexcept { return finished_; }

    /// Number of frames successfully submitted so far.
    [[nodiscard]] std::size_t submittedFrameCount() const noexcept { return submittedFrames_; }

    /// The presentation time of the most recently submitted frame (zero before
    /// any frame is submitted).
    [[nodiscard]] Duration lastPresentationTime() const noexcept { return lastPresentation_; }

    /// True when this encoder muxes an audio stream alongside the video stream.
    [[nodiscard]] bool hasAudioStream() const noexcept { return spec_.audio.has_value(); }

    /// The audio stream's configuration, or nullopt for a video-only encoder.
    [[nodiscard]] const std::optional<AudioEncodeSpec>& audioSpec() const noexcept {
        return spec_.audio;
    }

    /// Number of audio blocks successfully submitted so far.
    [[nodiscard]] std::size_t submittedAudioBlockCount() const noexcept {
        return submittedAudioBlocks_;
    }

    /// Total audio FRAMES (one sample per channel) successfully submitted. The
    /// export mix duration of Requirement 6.8 is this count at the audio stream's
    /// sample rate.
    [[nodiscard]] std::uint64_t submittedAudioFrameCount() const noexcept {
        return submittedAudioFrames_;
    }

    /// The presentation time of the most recently submitted audio block (zero
    /// before any block is submitted).
    [[nodiscard]] Duration lastAudioPresentationTime() const noexcept {
        return lastAudioPresentation_;
    }

    /// The routing/fallback bridge (observability: route taken, failure log).
    [[nodiscard]] const gpu::CodecBridge& bridge() const noexcept { return bridge_; }

private:
    MediaEncoder(EncodeSpec spec, std::unique_ptr<IEncodeBackend> backend,
                 gpu::CodecBridge bridge, gpu::CodecRoute route, bool usedSoftwareFallback);

    EncodeSpec                      spec_{};
    std::unique_ptr<IEncodeBackend> backend_{};
    gpu::CodecBridge                bridge_;
    gpu::CodecRoute                 route_{};
    bool                            usedSoftwareFallback_{false};
    bool                            finished_{false};
    bool                            hasSubmitted_{false};
    Duration                        lastPresentation_{Duration::zero()};
    std::size_t                     submittedFrames_{0};
    bool                            hasSubmittedAudio_{false};
    Duration                        lastAudioPresentation_{Duration::zero()};
    std::size_t                     submittedAudioBlocks_{0};
    std::uint64_t                   submittedAudioFrames_{0};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_MEDIAENCODER_HPP
