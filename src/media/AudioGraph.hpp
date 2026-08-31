// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioGraph.hpp — the audio graph: resample heterogeneous audio sources
// to a common output format and mix them into a single output buffer.
//
// This is the audio side of the Media Engine (design.md "Component 3: Media
// Engine (FFmpeg)" — "Drive the audio graph (resampling, mixing) via
// libswresample"). During playback (Requirement 2.8) and export the timeline
// carries several audio clips, each decoded at its own sample rate, channel
// layout, and sample format. The audio graph brings every source into one
// canonical output format and sums them into the buffer the playback device or
// the encoder consumes.
//
// Design split (mirrors MediaProbe/MediaDecoder): the buffer math, channel
// up/down-mix, sample-rate conversion, mixing (sum + clamp), and PCM packing are
// plain C++ and always compiled — so the numeric core is unit-testable on any
// machine with no FFmpeg. The concrete high-quality resampler is provided by
// libswresample and is compiled only when PALMIER_HAVE_FFMPEG is defined; it
// sits behind the IResampler seam. When FFmpeg is absent (CI/sandbox, or a
// hardware/GPU-free build) the graph transparently uses the built-in linear
// resampler, so audio still resamples and mixes correctly — the same graceful
// software fallback the rest of the engine follows.

#ifndef PALMIER_MEDIA_AUDIOGRAPH_HPP
#define PALMIER_MEDIA_AUDIOGRAPH_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/Result.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Sample format
// ---------------------------------------------------------------------------

/// Interleaved PCM sample formats the engine reads from decoders and writes to
/// the playback device / encoder. The graph's internal working format is always
/// 32-bit float in [-1, 1]; these describe the external I/O representation.
enum class SampleFormat {
    S16,  ///< signed 16-bit integer, native endian.
    S32,  ///< signed 32-bit integer, native endian.
    F32,  ///< 32-bit float, nominally in [-1, 1].
};

/// Bytes occupied by one sample of the given format.
[[nodiscard]] constexpr int bytesPerSample(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::S16: return 2;
        case SampleFormat::S32: return 4;
        case SampleFormat::F32: return 4;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Audio format
// ---------------------------------------------------------------------------

/// A fully-specified interleaved audio format: sample rate (Hz), channel count,
/// and per-sample representation. Used to describe the graph's output and the
/// external representation a buffer is packed to / unpacked from.
struct AudioFormat {
    int          sampleRate = 0;                       ///< Samples per second (per channel).
    int          channels = 0;                         ///< Interleaved channel count.
    SampleFormat sampleFormat = SampleFormat::F32;     ///< Per-sample representation.

    /// A format is usable only with a positive rate and channel count.
    [[nodiscard]] bool isValid() const noexcept { return sampleRate > 0 && channels > 0; }

    /// Bytes occupied by one interleaved frame (one sample per channel).
    [[nodiscard]] int bytesPerFrame() const noexcept {
        return channels * bytesPerSample(sampleFormat);
    }

    friend bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept {
        return a.sampleRate == b.sampleRate && a.channels == b.channels &&
               a.sampleFormat == b.sampleFormat;
    }
    friend bool operator!=(const AudioFormat& a, const AudioFormat& b) noexcept {
        return !(a == b);
    }
};

// ---------------------------------------------------------------------------
// AudioBuffer — the engine's internal working buffer
// ---------------------------------------------------------------------------

/// A block of interleaved 32-bit-float PCM at a given sample rate and channel
/// count. This is the canonical representation the graph resamples into and
/// mixes: keeping mixing in float avoids intermediate clipping and makes the
/// numeric core exact and testable. Samples are laid out interleaved:
/// `samples[frame * channels + ch]`.
class AudioBuffer {
public:
    AudioBuffer() = default;

    /// A zero-filled buffer of `frameCount` frames.
    AudioBuffer(int sampleRate, int channels, std::size_t frameCount)
        : sampleRate_(sampleRate),
          channels_(channels),
          samples_(frameCount * static_cast<std::size_t>(channels > 0 ? channels : 0), 0.0f) {}

    /// Adopt an existing interleaved sample vector. Its size must be a multiple
    /// of `channels`; any trailing partial frame is ignored by frameCount().
    [[nodiscard]] static AudioBuffer interleaved(int sampleRate, int channels,
                                                 std::vector<float> samples) {
        AudioBuffer b;
        b.sampleRate_ = sampleRate;
        b.channels_ = channels;
        b.samples_ = std::move(samples);
        return b;
    }

    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }

    /// Number of complete interleaved frames.
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channels_ > 0 ? samples_.size() / static_cast<std::size_t>(channels_) : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return frameCount() == 0; }

    [[nodiscard]] const std::vector<float>& samples() const noexcept { return samples_; }
    [[nodiscard]] std::vector<float>& samples() noexcept { return samples_; }

    /// Read one channel of one frame; 0 for out-of-range indices (so a shorter
    /// source contributes silence past its end when mixed).
    [[nodiscard]] float at(std::size_t frame, int ch) const noexcept {
        if (channels_ <= 0 || ch < 0 || ch >= channels_) return 0.0f;
        const std::size_t idx = frame * static_cast<std::size_t>(channels_) +
                                static_cast<std::size_t>(ch);
        return idx < samples_.size() ? samples_[idx] : 0.0f;
    }

private:
    int                sampleRate_ = 0;
    int                channels_ = 0;
    std::vector<float> samples_{};
};

// ---------------------------------------------------------------------------
// Pure buffer math (always compiled, FFmpeg-free, unit-testable)
// ---------------------------------------------------------------------------

/// Number of output frames a resample from `inRate` to `outRate` produces for
/// `inFrames` input frames, rounded to nearest. Zero when either rate is
/// non-positive or the input is empty.
[[nodiscard]] std::size_t resampledFrameCount(std::size_t inFrames, int inRate,
                                              int outRate) noexcept;

/// One participant in a mix: a float buffer and the linear gain applied to it
/// (1.0 = unity; matches Clip.gain in the data model, gain >= 0).
struct MixSource {
    const AudioBuffer* buffer = nullptr;
    double             gain = 1.0;
};

/// Sum `sources` (each interleaved float at `sampleRate`/`channels`) into a
/// single output buffer of `frameCount` frames, applying each source's gain and
/// clamping the result to [-1, 1]. Sources shorter than `frameCount` contribute
/// silence past their end; a source's channel count must match `channels`.
///
/// Errors: InvalidArgument for a non-positive rate/channel count, a null source
/// buffer, a negative gain, or a source whose channel count differs.
[[nodiscard]] Result<AudioBuffer> mix(int sampleRate, int channels, std::size_t frameCount,
                                      const std::vector<MixSource>& sources);

// ---------------------------------------------------------------------------
// Programme levels (monitoring-and-grading Requirement 1)
// ---------------------------------------------------------------------------

/// Per-channel programme levels for one buffer, in normalised units where 1.0 is
/// full scale. Both vectors carry exactly one entry per channel of the measured
/// buffer, so `peak.size() == rms.size() == buffer.channels()` for any buffer
/// with a positive channel count, and both are empty otherwise.
///
/// These are a MEASUREMENT, never a stage: measureLevels() reads a buffer and
/// returns this, changing nothing (Requirement 1.8). Keeping the type here — in
/// the pure, FFmpeg-free, Qt-free buffer-math section — is what lets a meter be
/// tested without a sink, a device or a display (Requirement 1.9).
struct AudioLevels {
    /// Per-channel maximum absolute sample value in the measured buffer.
    std::vector<float> peak{};
    /// Per-channel root mean square over the same samples.
    std::vector<float> rms{};

    /// True when a channel reached or exceeded full scale, which is what a
    /// meter's clip indication latches on (Requirement 1.5).
    [[nodiscard]] bool clippedOn(int channel) const noexcept {
        const auto index = static_cast<std::size_t>(channel);
        return channel >= 0 && index < peak.size() && peak[index] >= 1.0f;
    }

    /// The loudest channel's peak, or 0 when there are no channels.
    [[nodiscard]] float peakAcrossChannels() const noexcept {
        float loudest = 0.0f;
        for (const float value : peak) {
            if (value > loudest) loudest = value;
        }
        return loudest;
    }
};

/// Measure `buffer`'s per-channel peak and RMS (Requirement 1.1, 1.2).
///
/// Peak is the maximum absolute sample value for that channel; RMS is the root
/// mean square over the same samples. A buffer with no frames, or a non-positive
/// channel count, measures as empty rather than as an error: a quantum the engine
/// suppressed because no device is available is zero-filled silence, so it
/// measures zero on both figures without a special case (Requirement 1.3), and
/// "no device" stays distinguishable from "silent timeline" through
/// AudioQuantumReport's own `suppressed` flag rather than through the levels.
///
/// Pure and total: no I/O, no dependency on a sink or a device, and no path that
/// can fail. Its only allocations are the two returned vectors and one
/// channel-sized scratch accumulator, all bounded by the channel count.
[[nodiscard]] AudioLevels measureLevels(const AudioBuffer& buffer) noexcept;

/// Pack an interleaved float buffer into `format`'s interleaved PCM byte layout,
/// clamping to the format's representable range. The buffer's channel count must
/// equal `format.channels`. Errors: InvalidArgument on a channel-count mismatch
/// or an invalid format.
[[nodiscard]] Result<std::vector<std::byte>> pack(const AudioBuffer& buffer,
                                                  SampleFormat format);

/// Unpack interleaved PCM bytes in `format` into a float AudioBuffer. `bytes`
/// size must be a multiple of the format's frame size. Errors: InvalidArgument
/// on an invalid format or a size that is not a whole number of frames.
[[nodiscard]] Result<AudioBuffer> unpack(const std::vector<std::byte>& bytes,
                                         const AudioFormat& format);

// ---------------------------------------------------------------------------
// Resampler seam
// ---------------------------------------------------------------------------

/// Converts a float AudioBuffer from one (sample rate, channel count) to
/// another, staying in the float working domain. The libswresample-backed
/// implementation provides high-quality conversion when FFmpeg is compiled in;
/// the built-in linear implementation is the always-available software
/// fallback. Both satisfy this interface so the AudioGraph is agnostic to which
/// one it holds and is testable without FFmpeg.
class IResampler {
public:
    virtual ~IResampler() = default;

    [[nodiscard]] virtual int inputSampleRate() const noexcept = 0;
    [[nodiscard]] virtual int inputChannels() const noexcept = 0;
    [[nodiscard]] virtual int outputSampleRate() const noexcept = 0;
    [[nodiscard]] virtual int outputChannels() const noexcept = 0;

    /// Resample `input` to the resampler's output rate/channels. `input`'s
    /// sample rate and channel count must match the configured input; an empty
    /// input yields an empty output. Returns a float buffer at the output
    /// rate/channels.
    [[nodiscard]] virtual Result<AudioBuffer> resample(const AudioBuffer& input) = 0;
};

/// True iff this build compiled the libswresample-backed resampler
/// (PALMIER_HAVE_FFMPEG). When false, makeResampler() returns the linear
/// fallback.
[[nodiscard]] bool isFfmpegResamplerAvailable() noexcept;

/// The built-in, dependency-free linear-interpolation resampler with
/// channel up/down-mix. Always available; used as the software fallback and
/// directly unit-testable. Errors: InvalidArgument on a non-positive
/// rate/channel count.
[[nodiscard]] Result<std::unique_ptr<IResampler>> makeLinearResampler(int inRate, int inChannels,
                                                                      int outRate, int outChannels);

/// Build the preferred resampler for the given conversion: the libswresample
/// backend when compiled in, otherwise the linear fallback. Errors:
/// InvalidArgument on a non-positive rate/channel count.
[[nodiscard]] Result<std::unique_ptr<IResampler>> makeResampler(int inRate, int inChannels,
                                                                int outRate, int outChannels);

// ---------------------------------------------------------------------------
// AudioGraph — resample every source to the output format and mix
// ---------------------------------------------------------------------------

/// Mixes several audio sources into one output stream in a common format. Each
/// source declares the format its decoder produces; the graph builds a resampler
/// per source to the shared output format and, per render quantum, resamples
/// each source's decoded block and sums them (with per-source gain, clamped).
///
/// Usage (per playback/export tick):
///   AudioGraph g{{48000, 2, SampleFormat::F32}};
///   auto a = g.addSource(44100, 2).value();   // clip A: 44.1k stereo
///   auto b = g.addSource(48000, 1).value();   // clip B: 48k mono
///   AudioBuffer out = g.render({blockA, blockB}).value(); // mixed 48k stereo
class AudioGraph {
public:
    /// A handle to a registered source (its index in registration order).
    struct SourceId {
        std::size_t value = 0;
    };

    /// Construct a graph producing `output`. `output` must be valid.
    explicit AudioGraph(AudioFormat output);

    [[nodiscard]] const AudioFormat& outputFormat() const noexcept { return output_; }
    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }
    [[nodiscard]] bool isValid() const noexcept { return output_.isValid(); }

    /// Register a source whose decoder emits float frames at `inRate`/`inChannels`,
    /// mixed at linear `gain` (>= 0; 1.0 = unity). Errors: FailedPrecondition if
    /// the graph's output format is invalid; InvalidArgument on a non-positive
    /// rate/channel count or a negative gain.
    [[nodiscard]] Result<SourceId> addSource(int inRate, int inChannels, double gain = 1.0);

    /// Update a registered source's mix gain (>= 0). Errors: NotFound for an
    /// unknown id; InvalidArgument on a negative gain.
    [[nodiscard]] Result<void> setGain(SourceId id, double gain);

    /// Resample each source's decoded block (indexed by registration order) to
    /// the output format and mix them. `sourceInputs` must have exactly
    /// sourceCount() entries; each entry's rate/channels must match that source.
    /// When `frameCount` is 0 the output length is the longest resampled source;
    /// otherwise the output is exactly `frameCount` frames (truncated/silence-
    /// padded). Errors: InvalidArgument on an input-count or format mismatch.
    [[nodiscard]] Result<AudioBuffer> render(const std::vector<AudioBuffer>& sourceInputs,
                                             std::size_t frameCount = 0);

    /// render() followed by pack() into the output format's PCM byte layout —
    /// the buffer the playback device or encoder consumes.
    [[nodiscard]] Result<std::vector<std::byte>> renderPacked(
        const std::vector<AudioBuffer>& sourceInputs, std::size_t frameCount = 0);

private:
    struct Source {
        int                        inRate = 0;
        int                        inChannels = 0;
        double                     gain = 1.0;
        std::unique_ptr<IResampler> resampler;
    };

    AudioFormat         output_;
    std::vector<Source> sources_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_AUDIOGRAPH_HPP
