// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaDecoder.hpp — open a media file and decode video frames, preferring
// hardware and falling back to software transparently.
//
// This is the decode stage of the Media Engine (design.md "Component 3: Media
// Engine (FFmpeg)"). MediaDecoder::open probes a container and prepares a
// per-source decode pipeline; nextFrame() yields the next video frame and
// seek() repositions the source. Following the design's zero-copy frame
// lifecycle, a decoded frame is exposed either as a GPU-resident frame adopted
// into the GPU FramePool without a copy (the hardware path) or as a CPU pixel
// buffer (the software path).
//
// Routing (Requirements 10.2 / 10.5) is delegated to the GPU layer's CodecBridge:
// for each frame the decoder asks the bridge to run the decode on the backend it
// selects for the detected codec and device capabilities. When a hardware decode
// fails, the bridge retries the SAME frame exactly once on the CPU path, logs the
// failure, and preserves the caller's inputs — so an unsupported codec or a GPU
// failure degrades to software decode transparently, losing no data.
//
// Testability: the actual codec work lives behind the IDecodeBackend seam (a
// pluggable interface, mirroring MediaProbe's backend seam). open() takes a
// DecodeBackendFactory; the default factory builds the FFmpeg
// (libavformat/libavcodec/libswscale) backend, which is compiled only when
// PALMIER_HAVE_FFMPEG is defined. On a machine without FFmpeg the module still
// builds and the default factory reports FailedPrecondition, while the whole
// open/nextFrame/seek + routing/fallback surface stays exercisable with a mock
// backend (see tests/media_decoder_test.cpp).
//
// Audio (task 8.1 of end-to-end-editor-integration; Requirement 6.1): the audio
// surface — openAudioStream / nextAudioFrame / seekAudio / hasAudio — mirrors the
// video surface and yields interleaved-float AudioBuffers (media/AudioGraph.hpp)
// that the existing AudioGraph consumes directly. Audio decode is ALWAYS
// software, so it does not go through the CodecBridge and needs no
// hardware-fallback logic. IDecodeBackend::decodeAudio / seekAudio are
// default-implemented as "this backend decodes no audio", so every backend and
// test double written before the audio surface existed keeps compiling and simply
// presents as an audio-less source. Encode lives in MediaEncoder.

#ifndef PALMIER_MEDIA_MEDIADECODER_HPP
#define PALMIER_MEDIA_MEDIADECODER_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/Result.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/FramePool.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/AudioGraph.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Codec identity bridge (media -> GPU layer)
// ---------------------------------------------------------------------------

/// Map the media engine's richer codec identity onto the GPU layer's minimal
/// CodecId used by the hardware bridge. Codecs the bridge does not enumerate
/// (ProRes, VP8, MJPEG, all audio, ...) map to gpu::CodecId::Unknown, which the
/// bridge routes to the software path.
[[nodiscard]] gpu::CodecId toGpuCodec(MediaCodecId codec) noexcept;

// ---------------------------------------------------------------------------
// Decode preferences + injected GPU context
// ---------------------------------------------------------------------------

/// Caller preferences and the GPU context the decoder routes against.
///
///   * preferHardware — when false the decoder forces the software path
///     regardless of device capabilities (mirrors GpuSelectionMode::ForceSoftware
///     for a single source; the parity/fallback lane of Requirement 10.4/13.3).
///   * caps — capabilities of the selected device; the CodecBridge routes decode
///     to a hardware backend only when these advertise the codec.
///   * framePool — pool used to adopt hardware-decoded surfaces zero-copy
///     (Requirement 10.2). When null, hardware frames cannot be imported and the
///     decoder falls back to CPU frames transparently.
///   * availability — which vendor hardware backends are compiled into this
///     build (defaults to the PALMIER_HAVE_* build configuration).
struct DecodePrefs {
    bool                     preferHardware{true};
    gpu::GpuCaps             caps{gpu::GpuCaps::software()};
    gpu::FramePool*          framePool{nullptr};
    gpu::BridgeAvailability  availability{gpu::BridgeAvailability::fromBuildConfig()};
};

// ---------------------------------------------------------------------------
// DecodedFrame — a GPU-resident frame or a CPU pixel buffer
// ---------------------------------------------------------------------------

/// One decoded video frame. It is either:
///   * GPU-resident — a FrameLease on a FramePool frame that adopted the
///     hardware decoder's surface without a copy (isGpuResident()), or
///   * CPU — a host-memory pixel buffer produced by software decode (isCpu()), or
///   * an end-of-stream marker (isEndOfStream()) with no pixels.
/// Move-only, because a GPU-resident frame owns a move-only FrameLease that
/// returns the frame to its pool on destruction.
class DecodedFrame {
public:
    DecodedFrame() = default;
    DecodedFrame(DecodedFrame&&) noexcept = default;
    DecodedFrame& operator=(DecodedFrame&&) noexcept = default;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    /// End-of-stream marker: no more frames are available from the source.
    [[nodiscard]] static DecodedFrame endOfStream() {
        DecodedFrame f;
        f.eos_ = true;
        return f;
    }

    /// A GPU-resident frame backed by a zero-copy / pooled FramePool lease.
    [[nodiscard]] static DecodedFrame gpu(Duration timestamp, gpu::FrameLease lease) {
        DecodedFrame f;
        f.timestamp_ = timestamp;
        f.gpuLease_.emplace(std::move(lease));
        return f;
    }

    /// A CPU frame backed by a host-memory pixel buffer.
    [[nodiscard]] static DecodedFrame cpu(Duration timestamp, gpu::FrameDesc desc,
                                          std::vector<std::byte> pixels) {
        DecodedFrame f;
        f.timestamp_ = timestamp;
        f.cpuDesc_ = desc;
        f.cpuPixels_ = std::move(pixels);
        f.hasCpu_ = true;
        return f;
    }

    [[nodiscard]] bool isEndOfStream() const noexcept { return eos_; }

    [[nodiscard]] bool isGpuResident() const noexcept {
        return gpuLease_.has_value() && gpuLease_->valid();
    }

    [[nodiscard]] bool isCpu() const noexcept { return hasCpu_; }

    /// True when the GPU-resident frame adopted external memory without a copy.
    [[nodiscard]] bool isZeroCopy() const noexcept {
        return isGpuResident() && gpuLease_->frame().isZeroCopy();
    }

    [[nodiscard]] Duration timestamp() const noexcept { return timestamp_; }

    /// The frame geometry/format, whether GPU-resident or CPU.
    [[nodiscard]] gpu::FrameDesc desc() const noexcept {
        if (isGpuResident()) return gpuLease_->frame().desc();
        return cpuDesc_;
    }

    // --- GPU access (precondition: isGpuResident()) ---
    [[nodiscard]] const gpu::GpuFrame& gpuFrame() const { return gpuLease_->frame(); }
    [[nodiscard]] gpu::FrameLease& lease() noexcept { return *gpuLease_; }

    // --- CPU access (precondition: isCpu()) ---
    [[nodiscard]] const std::vector<std::byte>& cpuPixels() const noexcept { return cpuPixels_; }

private:
    Duration                       timestamp_{};
    bool                           eos_{false};
    std::optional<gpu::FrameLease> gpuLease_{};
    bool                           hasCpu_{false};
    gpu::FrameDesc                 cpuDesc_{};
    std::vector<std::byte>         cpuPixels_{};
};

// ---------------------------------------------------------------------------
// Decode backend seam
// ---------------------------------------------------------------------------

/// One frame as produced by a decode backend, before it is wrapped in a
/// DecodedFrame. A backend either exports a hardware surface (`hardware == true`,
/// `external` valid) for the caller to adopt zero-copy into the FramePool, or a
/// host-memory buffer (`cpuPixels`) for a software frame.
struct BackendFrame {
    bool                       endOfStream{false};
    Duration                   timestamp{};
    gpu::FrameDesc             desc{};
    bool                       hardware{false};
    gpu::ExternalImageSource   external{};
    std::vector<std::byte>     cpuPixels{};

    [[nodiscard]] static BackendFrame eos() {
        BackendFrame f;
        f.endOfStream = true;
        return f;
    }
};

// ---------------------------------------------------------------------------
// Audio decode surface (task 8.1; Requirement 6.1)
// ---------------------------------------------------------------------------

/// Declared bounds every audio buffer the decoder yields conforms to
/// (Requirement 6.1). A source whose audio stream falls outside them is rejected
/// at openAudioStream() rather than emitted out of range, so the invariant holds
/// for every buffer nextAudioFrame() ever returns.
constexpr int kMinAudioSampleRate = 8'000;
constexpr int kMaxAudioSampleRate = 192'000;
constexpr int kMinAudioChannels = 1;
constexpr int kMaxAudioChannels = 8;

/// True iff `rate` is inside the declared sample-rate range.
[[nodiscard]] constexpr bool isDeclaredAudioSampleRate(int rate) noexcept {
    return rate >= kMinAudioSampleRate && rate <= kMaxAudioSampleRate;
}

/// True iff `channels` is inside the declared channel-count range.
[[nodiscard]] constexpr bool isDeclaredAudioChannelCount(int channels) noexcept {
    return channels >= kMinAudioChannels && channels <= kMaxAudioChannels;
}

/// One decoded block of audio: interleaved 32-bit float samples in the
/// `AudioBuffer` the existing AudioGraph already consumes, plus the presentation
/// timestamp of its first sample. `endOfStream` marks the exhausted stream and
/// carries an empty buffer.
///
/// Unlike DecodedFrame there is no GPU variant: audio decode is always software
/// (there is no hardware audio decode path to route), so the CodecBridge is not
/// involved and no hardware/CPU fallback logic exists on this path.
struct AudioFrame {
    bool        endOfStream = false;
    Duration    presentation{Duration::zero()};
    AudioBuffer buffer{};

    [[nodiscard]] static AudioFrame eos() {
        AudioFrame f;
        f.endOfStream = true;
        return f;
    }

    /// Sample rate declared by this buffer (0 for an end-of-stream marker).
    [[nodiscard]] int sampleRate() const noexcept { return buffer.sampleRate(); }

    /// Interleaved channel count declared by this buffer (0 at end of stream).
    [[nodiscard]] int channels() const noexcept { return buffer.channels(); }

    /// Complete interleaved frames carried by this buffer.
    [[nodiscard]] std::size_t frameCount() const noexcept { return buffer.frameCount(); }
};

/// One block of audio as produced by a decode backend, before MediaDecoder
/// validates it against the declared ranges and enforces timestamp monotonicity.
/// The buffer is already interleaved float at the source's own sample rate and
/// channel count — resampling to the engine's output format is the AudioGraph's
/// job, not the decoder's.
struct BackendAudioFrame {
    bool        endOfStream = false;
    Duration    timestamp{Duration::zero()};
    AudioBuffer buffer{};

    [[nodiscard]] static BackendAudioFrame eos() {
        BackendAudioFrame f;
        f.endOfStream = true;
        return f;
    }
};

/// The pluggable decode implementation. The FFmpeg backend provides the concrete
/// codec work; tests supply mocks. This seam keeps MediaDecoder's routing and
/// zero-copy/fallback policy free of any FFmpeg dependency and unit-testable
/// without a GPU or FFmpeg.
class IDecodeBackend {
public:
    virtual ~IDecodeBackend() = default;

    /// Normalized description of the opened source.
    [[nodiscard]] virtual const MediaInfo& info() const = 0;

    /// Decode the next video frame.
    ///
    /// When `useHardware` is true the backend attempts a hardware decode: on
    /// success it returns a hardware BackendFrame; on hardware failure it returns
    /// an Error so the CodecBridge can retry the SAME frame once on the CPU by
    /// calling this again with `useHardware == false`. Backends MUST therefore be
    /// able to reprocess the current frame after a hardware failure (i.e. retain
    /// the pending packet) so no data is lost (Requirement 10.5). When
    /// `useHardware` is false the backend performs a software decode and returns a
    /// CPU BackendFrame. End of stream is signalled by BackendFrame::eos().
    [[nodiscard]] virtual Result<BackendFrame> decode(bool useHardware) = 0;

    /// Reposition the source so the next decode resumes at (or near) `ts`.
    [[nodiscard]] virtual Result<void> seek(Duration ts) = 0;

    // --- Audio (task 8.1; Requirement 6.1) ---------------------------------
    //
    // Both audio entry points are NON-pure and default to "this backend decodes
    // no audio". That is deliberate: every backend that predates the audio
    // surface — the FFmpeg video backend before it grew audio support, and every
    // test double across the suite — keeps compiling and behaves as an
    // audio-less source, which is exactly what Requirement 6.6 wants an
    // audio-less asset to look like. A backend opts in by overriding them.

    /// Decode the next block of audio from `streamIndex` (an index into
    /// info().streams naming an audio stream), converting it to interleaved
    /// 32-bit float at the stream's own sample rate and channel count. End of
    /// stream is signalled by BackendAudioFrame::eos(). Audio decode is always
    /// software, so there is no hardware/CPU route parameter.
    ///
    /// The default implementation reports that this backend provides no audio.
    [[nodiscard]] virtual Result<BackendAudioFrame> decodeAudio(int streamIndex) {
        (void)streamIndex;
        return err<BackendAudioFrame>(makeError(
            ErrorCode::Unsupported, "this decode backend provides no audio decode support"));
    }

    /// Reposition `streamIndex` so the next decodeAudio() resumes at (or near)
    /// `ts`. Independent of seek(), which repositions the video stream.
    ///
    /// The default implementation reports that this backend provides no audio.
    [[nodiscard]] virtual Result<void> seekAudio(Duration ts, int streamIndex) {
        (void)ts;
        (void)streamIndex;
        return makeError(ErrorCode::Unsupported,
                         "this decode backend provides no audio decode support");
    }
};

/// Builds a decode backend for `path` under `prefs`. Mirrors MediaProbe's
/// injectable backend seam so open() is testable without FFmpeg.
using DecodeBackendFactory =
    std::function<Result<std::unique_ptr<IDecodeBackend>>(const std::filesystem::path&,
                                                          const DecodePrefs&)>;

/// True iff this build was compiled with FFmpeg decode support
/// (PALMIER_HAVE_FFMPEG). When false the default factory reports
/// FailedPrecondition. (Shares the build flag with MediaProbe::isFfmpegAvailable.)
[[nodiscard]] bool isFfmpegDecodeAvailable() noexcept;

/// The default, FFmpeg-backed decode backend factory. When FFmpeg is not
/// compiled in it returns an Error describing that decoding is unavailable.
[[nodiscard]] DecodeBackendFactory ffmpegDecodeBackendFactory();

// ---------------------------------------------------------------------------
// MediaDecoder
// ---------------------------------------------------------------------------

/// Opens a media source and decodes its video frames, preferring hardware and
/// falling back to software transparently (design "Component 3"; Requirements
/// 3.1, 10.2, 10.5).
class MediaDecoder {
public:
    MediaDecoder(MediaDecoder&&) noexcept = default;
    MediaDecoder& operator=(MediaDecoder&&) noexcept = default;
    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;

    /// Open `path` with the default FFmpeg backend. Errors:
    ///   * InvalidArgument   — the path is empty.
    ///   * FailedPrecondition — this build has no FFmpeg decode support.
    ///   * NotFound / Io / Unsupported — from the backend while opening the file.
    [[nodiscard]] static Result<MediaDecoder> open(const std::filesystem::path& path,
                                                   DecodePrefs prefs);

    /// Open `path` with an injected backend factory (the testing seam).
    [[nodiscard]] static Result<MediaDecoder> open(const std::filesystem::path& path,
                                                   DecodePrefs prefs,
                                                   const DecodeBackendFactory& factory);

    /// The normalized description of the opened source.
    [[nodiscard]] const MediaInfo& info() const noexcept { return info_; }

    /// Decode the next video frame. Returns a GPU-resident frame when the source
    /// was hardware-decoded and adopted into the FramePool, a CPU frame when the
    /// software path produced it (including after a transparent HW->CPU
    /// fallback), or an end-of-stream DecodedFrame when the source is exhausted.
    [[nodiscard]] Result<DecodedFrame> nextFrame();

    /// Reposition the source so the next nextFrame() resumes at (or near) `ts`.
    [[nodiscard]] Result<void> seek(Duration ts);

    /// The GPU codec identity the decoder routes against (from the primary video
    /// stream); gpu::CodecId::Unknown when the source carries no known video codec.
    [[nodiscard]] gpu::CodecId videoCodec() const noexcept { return videoCodec_; }

    /// The routing/fallback bridge (observability: routes taken, failure log).
    [[nodiscard]] const gpu::CodecBridge& bridge() const noexcept { return bridge_; }

    /// True when the most recent nextFrame() fell back from hardware to the CPU
    /// path (Requirement 10.5).
    [[nodiscard]] bool lastFrameRetriedOnCpu() const noexcept { return lastRetriedOnCpu_; }

    // --- Audio surface (task 8.1; Requirement 6.1) --------------------------

    /// True when the opened source carries at least one audio stream. An
    /// audio-less asset is not an error anywhere: the Audio_Engine contributes
    /// silence for its timeline range (Requirement 6.6), and this predicate is
    /// how it decides that without opening anything.
    [[nodiscard]] bool hasAudio() const noexcept { return info_.hasAudio(); }

    /// Select the audio stream to decode. `streamIndex == -1` selects the
    /// source's primary (first) audio stream; any other value must name an audio
    /// stream in info().streams — an audio stream is NOT required to be stream 0.
    /// Calling it again re-selects, resetting the timestamp baseline.
    ///
    /// Errors:
    ///   * FailedPrecondition — the decoder is not open, or the source carries no
    ///     audio stream at all.
    ///   * InvalidArgument    — `streamIndex` is out of range or names a stream
    ///     that is not audio.
    ///   * Unsupported        — the stream declares a sample rate outside
    ///     8 000–192 000 Hz or a channel count outside 1–8, naming the value. The
    ///     stream is refused rather than decoded out of range, so every buffer
    ///     nextAudioFrame() yields conforms to the declared ranges
    ///     (Requirement 6.1).
    [[nodiscard]] Result<void> openAudioStream(int streamIndex = -1);

    /// The stream index openAudioStream() selected, or -1 when no audio stream is
    /// open.
    [[nodiscard]] int audioStreamIndex() const noexcept { return audioStreamIndex_; }

    /// Decode the next block of audio from the open audio stream as interleaved
    /// 32-bit float samples, or an end-of-stream AudioFrame once exhausted.
    ///
    /// Guarantees for every returned non-end-of-stream frame (Requirement 6.1):
    /// the buffer declares a sample rate in 8 000–192 000 Hz, a channel count in
    /// 1–8, and a presentation timestamp no earlier than that of the previous
    /// frame of this stream. A backend block that violates the range guarantee is
    /// reported as an error and no frame is emitted; a backend timestamp that
    /// regresses is raised to the previous frame's timestamp, which keeps the
    /// non-decreasing invariant true without discarding audible samples.
    ///
    /// Errors: FailedPrecondition when no audio stream is open; Unsupported when
    /// the backend decodes no audio; whatever the backend reports otherwise.
    [[nodiscard]] Result<AudioFrame> nextAudioFrame();

    /// Reposition the open audio stream so the next nextAudioFrame() resumes at
    /// (or near) `ts`. A seek starts a new monotonic run: the timestamp baseline
    /// is cleared, so a legitimate backwards seek is not clamped forward.
    ///
    /// Errors: FailedPrecondition when no audio stream is open; whatever the
    /// backend reports otherwise.
    [[nodiscard]] Result<void> seekAudio(Duration ts);

private:
    MediaDecoder(MediaInfo info, std::unique_ptr<IDecodeBackend> backend, DecodePrefs prefs);

    MediaInfo                       info_{};
    std::unique_ptr<IDecodeBackend> backend_{};
    DecodePrefs                     prefs_{};
    gpu::CodecBridge                bridge_;
    gpu::CodecId                    videoCodec_{gpu::CodecId::Unknown};
    bool                            lastRetriedOnCpu_{false};

    // Audio state: the selected stream and the monotonicity baseline. The
    // baseline is std::nullopt before the first frame of a run and after a seek.
    int                             audioStreamIndex_{-1};
    std::optional<Duration>         lastAudioPresentation_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_MEDIADECODER_HPP
