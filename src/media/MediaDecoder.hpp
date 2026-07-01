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
// backend (see tests/media_decoder_test.cpp). Audio decode/resampling (task 8.4)
// and encode (task 8.3) are out of scope here.

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

private:
    MediaDecoder(MediaInfo info, std::unique_ptr<IDecodeBackend> backend, DecodePrefs prefs);

    MediaInfo                       info_{};
    std::unique_ptr<IDecodeBackend> backend_{};
    DecodePrefs                     prefs_{};
    gpu::CodecBridge                bridge_;
    gpu::CodecId                    videoCodec_{gpu::CodecId::Unknown};
    bool                            lastRetriedOnCpu_{false};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_MEDIADECODER_HPP
