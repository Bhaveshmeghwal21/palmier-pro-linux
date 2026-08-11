// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/SyntheticMedia.hpp — write REAL, decodable media with FFmpeg
// (task 12.10 of the end-to-end-editor-integration spec; Requirements 3.6, 15.1,
// 15.9).
//
// Why this exists
// ---------------------------------------------------------------------------
// Two of task 12.10's obligations need bytes that the product's OWN media entry
// points (`media::probeMediaFile`, `media::MediaDecoder::open`) can read:
//
//   1. The **fixture source**. Requirement 15.1 asks the end-to-end test to
//      import "a fixture media file carrying one video stream and one audio
//      stream of at least 2 seconds duration". Task 12.10 additionally forbids
//      checking a binary in: the fixture is GENERATED, deterministically, at
//      build time. `writeSyntheticAvSource()` is that generator's engine.
//   2. The **export output** of the injected-encode variant. On a host whose
//      libavcodec carries no H.264/HEVC/VP9 encoder, the production encode path
//      cannot produce a decodable file at all, so the end-to-end chain would be
//      unverifiable there. `realBytesEncodeBackendFactory()` supplies a
//      `media::IEncodeBackend` that muxes the frames and audio the export really
//      submitted into a real container with an encoder the host really has, so
//      the chain — and the "probes and decodes" assertion at the end of it — runs
//      on every host.
//
// Why it talks to libav* directly instead of reusing `media::MediaEncoder`
// ---------------------------------------------------------------------------
// `media::MediaEncoder`'s FFmpeg backend hard-codes `AV_PIX_FMT_YUV420P` and
// takes its encoder name from `gpu::softwareEncoderName()`, i.e. exactly one of
// `libx264` / `libx265` / `libsvtav1` / `libvpx-vp9` / `mpeg2video`. That is
// correct for the product — those are the codecs Requirement 8.2 supports — but
// it means the product encoder cannot be pointed at a host-native intra codec
// (ProRes needs a 10-bit 4:2:2 pixel format, which the hard-coded 4:2:0 rejects).
// A fixture writer therefore has to negotiate the pixel format itself, which is
// what this file does and what the product deliberately does not.
//
// Everything else about it mirrors `media/MediaEncoder.cpp`'s backend one to one:
// the same RGBA→encoder-pix_fmt swscale conversion, the same interleaved-float
// FIFO feeding a fixed-frame-size audio encoder through libswresample, the same
// send/receive/rescale/interleaved-write packet loop, and the same
// flush-both-streams-then-trailer finish. So a file it writes is byte-shaped like
// a file the product writes; only the codec differs.
//
// Determinism
// ---------------------------------------------------------------------------
// `writeSyntheticAvSource()` derives every pixel and every sample from the frame
// index alone (an integer ramp for video, a fixed-period integer sawtooth for
// audio) — no clock, no randomness, no locale. Re-running the generator on the
// same libavcodec produces the same bytes, so the fixture is reproducible rather
// than merely regenerated.
//
// Encoder negotiation
// ---------------------------------------------------------------------------
// `videoEncoders` / `audioEncoders` are CANDIDATE LISTS, tried in order, and the
// first one libavcodec both carries and can open at the requested geometry wins.
// The defaults are ordered so that a host with the usual external encoders picks
// H.264 + AAC, and a host with only FFmpeg's native encoders still succeeds on
// ProRes + AAC. Both are codecs `media::isImportSupported()` accepts, so a
// fixture written by either choice is importable by the product. When no
// candidate opens, `open()` reports an Error naming every candidate it tried and
// why each failed — never a silent degradation.

#ifndef PALMIER_TESTS_SUPPORT_SYNTHETICMEDIA_HPP
#define PALMIER_TESTS_SUPPORT_SYNTHETICMEDIA_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Result.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::test_support {

// ---------------------------------------------------------------------------
// SyntheticAvSpec — what to write and with what
// ---------------------------------------------------------------------------

/// The geometry, cadence and candidate encoders of one synthetic output.
struct SyntheticAvSpec {
    std::uint32_t width{320};
    std::uint32_t height{180};
    FrameRate     frameRate{FrameRate::fps30()};

    /// Container short name handed to `avformat_alloc_output_context2`.
    std::string container{"mov"};

    /// Video encoder candidates, most preferred first. Every default is a codec
    /// `media::isImportSupported()` accepts, so whichever one a host has, the
    /// resulting file is importable by the product.
    std::vector<std::string> videoEncoders{"libx264", "prores_ks", "prores_aw", "prores",
                                           "libx265", "libvpx-vp9"};

    /// Audio encoder candidates, most preferred first, or empty for a video-only
    /// output. Again all import-supported codecs (AAC, Opus, linear PCM).
    std::vector<std::string> audioEncoders{"aac", "libopus", "pcm_s16le"};

    int          sampleRate{48'000};
    int          channels{2};
    std::int64_t videoBitrateBitsPerSecond{2'000'000};
    std::int64_t audioBitrateBitsPerSecond{128'000};
};

/// Which candidates actually opened. Reported so a test can state, in its own
/// output, what the file it is asserting on is really encoded with.
struct SyntheticAvChoice {
    std::string videoEncoder{};
    std::string audioEncoder{};  ///< Empty for a video-only output.
    std::string container{};
};

// ---------------------------------------------------------------------------
// SyntheticAvWriter — an open container with one video and (optionally) one
// audio stream
// ---------------------------------------------------------------------------

/// Writes RGBA frames and interleaved-float audio into a real container. Frames
/// must be submitted in non-decreasing presentation order, exactly as
/// `media::MediaEncoder` requires of its own backends.
class SyntheticAvWriter {
public:
    /// Open `path`, negotiating the first candidate encoder that works. Fails —
    /// having created nothing that a caller must clean up beyond `path` itself —
    /// when no candidate opens, naming each candidate and its failure.
    [[nodiscard]] static Result<std::unique_ptr<SyntheticAvWriter>> open(
        const std::filesystem::path& path, const SyntheticAvSpec& spec);

    ~SyntheticAvWriter();

    SyntheticAvWriter(const SyntheticAvWriter&) = delete;
    SyntheticAvWriter& operator=(const SyntheticAvWriter&) = delete;
    SyntheticAvWriter(SyntheticAvWriter&&) = delete;
    SyntheticAvWriter& operator=(SyntheticAvWriter&&) = delete;

    /// Encode one RGBA8 frame of the spec's geometry. `rgba` is
    /// `width * height * 4` bytes, tightly packed; `presentation` is the frame's
    /// timeline position.
    [[nodiscard]] Result<void> writeVideoFrame(const std::uint8_t* rgba, Duration presentation);

    /// Encode `frames` interleaved-float audio frames at the spec's sample rate
    /// and channel count. A zero-length block is accepted and does nothing, which
    /// is the normal outcome for a frame interval that mixed to no audio.
    [[nodiscard]] Result<void> writeAudio(const float* interleaved, std::size_t frames);

    /// Flush both streams and write the container trailer. A second call is a
    /// no-op reported as ok(), so a caller that finishes explicitly and a
    /// destructor that finishes defensively cannot double-finalize.
    [[nodiscard]] Result<void> finish();

    [[nodiscard]] const SyntheticAvChoice& choice() const noexcept { return choice_; }
    [[nodiscard]] std::size_t videoFramesWritten() const noexcept { return videoFrames_; }
    [[nodiscard]] std::uint64_t audioFramesWritten() const noexcept { return audioFrames_; }
    [[nodiscard]] bool hasAudioStream() const noexcept;

private:
    struct Impl;

    explicit SyntheticAvWriter(std::unique_ptr<Impl> impl, SyntheticAvChoice choice);

    std::unique_ptr<Impl> impl_;
    SyntheticAvChoice     choice_{};
    std::size_t           videoFrames_{0};
    std::uint64_t         audioFrames_{0};
};

// ---------------------------------------------------------------------------
// The fixture source
// ---------------------------------------------------------------------------

/// What `writeSyntheticAvSource` produced.
struct SyntheticAvSource {
    std::filesystem::path path{};
    SyntheticAvChoice     choice{};
    std::size_t           videoFrames{0};
    Duration              duration{Duration::zero()};
};

/// Write a deterministic synthetic source of exactly `videoFrames` frames at
/// `spec.frameRate`, carrying one video stream and — unless
/// `spec.audioEncoders` is empty — one audio stream spanning the same range.
///
/// The pixels and the samples are functions of the frame index alone, so two runs
/// against the same libavcodec produce identical bytes.
[[nodiscard]] Result<SyntheticAvSource> writeSyntheticAvSource(const std::filesystem::path& path,
                                                               const SyntheticAvSpec& spec,
                                                               std::size_t videoFrames);

// ---------------------------------------------------------------------------
// The injected encode backend
// ---------------------------------------------------------------------------

/// What a `realBytesEncodeBackendFactory()` backend did. Shared with the export
/// worker thread, so every field is atomic or written only before the export
/// starts.
struct RealBytesEncodeRecord {
    std::atomic<int>           backendsCreated{0};
    std::atomic<std::size_t>   videoFrames{0};
    std::atomic<std::uint64_t> audioFrames{0};
    /// The video/audio encoder the backend negotiated, published once the backend
    /// has opened. Read only after the export has completed.
    std::string                videoEncoder{};
    std::string                audioEncoder{};
    std::mutex                 mutex{};
};

/// An encode-backend factory that writes REAL, decodable bytes for the frames and
/// audio the export submits.
///
/// It honours the `media::EncodeSpec` in every respect that decides what the
/// output looks like — the output path, the container short name, the resolution,
/// the frame rate, the bit rates and whether an audio stream was configured — and
/// substitutes only the CODEC, choosing the first of `hints.videoEncoders` /
/// `hints.audioEncoders` that this host carries. That substitution is the whole
/// point: it is what lets the export leg of an end-to-end chain produce a
/// probeable, decodable file on a host with no H.264, HEVC or VP9 encoder.
///
/// `record` may be null; when supplied it must outlive the export.
[[nodiscard]] media::EncodeBackendFactory realBytesEncodeBackendFactory(
    SyntheticAvSpec hints, RealBytesEncodeRecord* record = nullptr);

// ---------------------------------------------------------------------------
// Availability
// ---------------------------------------------------------------------------

/// Why none of `spec.videoEncoders` (and, when audio is requested, none of
/// `spec.audioEncoders`) can be opened on this host, or an empty string when the
/// spec is writable here. Naming the candidates makes an unrunnable host
/// diagnosable from the test log alone rather than from a bare failure.
[[nodiscard]] std::string syntheticMediaUnavailableReason(const SyntheticAvSpec& spec);

}  // namespace palmier::test_support

#endif  // PALMIER_TESTS_SUPPORT_SYNTHETICMEDIA_HPP
