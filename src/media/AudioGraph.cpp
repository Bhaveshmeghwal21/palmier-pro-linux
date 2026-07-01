// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioGraph.cpp — audio graph: buffer math, mixing, channel/rate
// conversion, PCM packing, and the resampler seam.
//
// The numeric core (resampledFrameCount, mix, pack/unpack, the linear resampler,
// and the AudioGraph orchestration) is backend-agnostic and always compiled, so
// it is unit-testable on machines without FFmpeg. The high-quality
// libswresample-backed resampler is compiled only when PALMIER_HAVE_FFMPEG is
// defined; otherwise makeResampler() returns the linear fallback, mirroring the
// probe/decoder guards so audio still resamples and mixes in a software-only
// build.

#include "media/AudioGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace palmier::media {

// ---------------------------------------------------------------------------
// Pure buffer math
// ---------------------------------------------------------------------------

std::size_t resampledFrameCount(std::size_t inFrames, int inRate, int outRate) noexcept {
    if (inFrames == 0 || inRate <= 0 || outRate <= 0) return 0;
    if (inRate == outRate) return inFrames;
    // Round to nearest: (inFrames * outRate + inRate/2) / inRate, done in a wide
    // integer so large blocks do not overflow.
    const long double scaled =
        (static_cast<long double>(inFrames) * static_cast<long double>(outRate)) /
        static_cast<long double>(inRate);
    return static_cast<std::size_t>(scaled + 0.5L);
}

namespace {

/// Clamp a float sample into the closed unit interval used by the float domain.
[[nodiscard]] float clampUnit(float v) noexcept {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

} // namespace

Result<AudioBuffer> mix(int sampleRate, int channels, std::size_t frameCount,
                        const std::vector<MixSource>& sources) {
    if (sampleRate <= 0 || channels <= 0) {
        return err<AudioBuffer>(invalidArgument("mix requires a positive sample rate and channel count"));
    }
    for (const MixSource& s : sources) {
        if (s.buffer == nullptr) {
            return err<AudioBuffer>(invalidArgument("mix source buffer must not be null"));
        }
        if (s.gain < 0.0) {
            return err<AudioBuffer>(invalidArgument("mix source gain must be non-negative"));
        }
        if (s.buffer->channels() != channels) {
            return err<AudioBuffer>(
                invalidArgument("mix source channel count does not match the output"));
        }
    }

    AudioBuffer out(sampleRate, channels, frameCount);
    std::vector<float>& acc = out.samples();

    for (const MixSource& s : sources) {
        const AudioBuffer& in = *s.buffer;
        const float gain = static_cast<float>(s.gain);
        const std::size_t frames = std::min(frameCount, in.frameCount());
        const std::vector<float>& src = in.samples();
        const std::size_t count = frames * static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < count; ++i) {
            acc[i] += src[i] * gain;
        }
    }

    for (float& v : acc) v = clampUnit(v);
    return out;
}

Result<std::vector<std::byte>> pack(const AudioBuffer& buffer, SampleFormat format) {
    AudioFormat fmt{buffer.sampleRate(), buffer.channels(), format};
    if (!fmt.isValid()) {
        return err<std::vector<std::byte>>(invalidArgument("pack requires a valid audio format"));
    }

    const std::vector<float>& src = buffer.samples();
    std::vector<std::byte> out(src.size() * static_cast<std::size_t>(bytesPerSample(format)));

    switch (format) {
        case SampleFormat::F32: {
            for (std::size_t i = 0; i < src.size(); ++i) {
                const float v = clampUnit(src[i]);
                std::memcpy(out.data() + i * 4, &v, 4);
            }
            break;
        }
        case SampleFormat::S16: {
            constexpr float kScale = 32767.0f;
            for (std::size_t i = 0; i < src.size(); ++i) {
                const float scaled = clampUnit(src[i]) * kScale;
                const auto q = static_cast<std::int16_t>(std::lround(scaled));
                std::memcpy(out.data() + i * 2, &q, 2);
            }
            break;
        }
        case SampleFormat::S32: {
            constexpr double kScale = 2147483647.0;
            for (std::size_t i = 0; i < src.size(); ++i) {
                const double scaled = static_cast<double>(clampUnit(src[i])) * kScale;
                const auto q = static_cast<std::int32_t>(std::llround(scaled));
                std::memcpy(out.data() + i * 4, &q, 4);
            }
            break;
        }
    }
    return out;
}

Result<AudioBuffer> unpack(const std::vector<std::byte>& bytes, const AudioFormat& format) {
    if (!format.isValid()) {
        return err<AudioBuffer>(invalidArgument("unpack requires a valid audio format"));
    }
    const std::size_t frameBytes = static_cast<std::size_t>(format.bytesPerFrame());
    if (frameBytes == 0 || bytes.size() % frameBytes != 0) {
        return err<AudioBuffer>(
            invalidArgument("unpack byte count is not a whole number of frames"));
    }

    const std::size_t sampleBytes = static_cast<std::size_t>(bytesPerSample(format.sampleFormat));
    const std::size_t sampleCount = bytes.size() / sampleBytes;
    std::vector<float> out(sampleCount, 0.0f);

    switch (format.sampleFormat) {
        case SampleFormat::F32: {
            for (std::size_t i = 0; i < sampleCount; ++i) {
                float v = 0.0f;
                std::memcpy(&v, bytes.data() + i * 4, 4);
                out[i] = v;
            }
            break;
        }
        case SampleFormat::S16: {
            constexpr float kInv = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < sampleCount; ++i) {
                std::int16_t q = 0;
                std::memcpy(&q, bytes.data() + i * 2, 2);
                out[i] = static_cast<float>(q) * kInv;
            }
            break;
        }
        case SampleFormat::S32: {
            constexpr double kInv = 1.0 / 2147483648.0;
            for (std::size_t i = 0; i < sampleCount; ++i) {
                std::int32_t q = 0;
                std::memcpy(&q, bytes.data() + i * 4, 4);
                out[i] = static_cast<float>(static_cast<double>(q) * kInv);
            }
            break;
        }
    }
    return AudioBuffer::interleaved(format.sampleRate, format.channels, std::move(out));
}

// ---------------------------------------------------------------------------
// Channel up/down-mix (shared by the linear resampler)
// ---------------------------------------------------------------------------

namespace {

/// Remap one interleaved float frame from `inCh` to `outCh` channels:
///   * equal counts       -> copy
///   * 1 -> N             -> replicate the mono sample to every output channel
///   * N -> 1             -> average all input channels
///   * otherwise          -> copy the overlapping channels, zero-fill the rest
void remapChannels(const float* in, int inCh, float* out, int outCh) {
    if (inCh == outCh) {
        for (int c = 0; c < outCh; ++c) out[c] = in[c];
        return;
    }
    if (inCh == 1) {
        for (int c = 0; c < outCh; ++c) out[c] = in[0];
        return;
    }
    if (outCh == 1) {
        float sum = 0.0f;
        for (int c = 0; c < inCh; ++c) sum += in[c];
        out[0] = sum / static_cast<float>(inCh);
        return;
    }
    const int common = std::min(inCh, outCh);
    for (int c = 0; c < common; ++c) out[c] = in[c];
    for (int c = common; c < outCh; ++c) out[c] = 0.0f;
}

// ---------------------------------------------------------------------------
// Linear-interpolation resampler (always available software fallback)
// ---------------------------------------------------------------------------

class LinearResampler final : public IResampler {
public:
    LinearResampler(int inRate, int inChannels, int outRate, int outChannels)
        : inRate_(inRate), inChannels_(inChannels), outRate_(outRate), outChannels_(outChannels) {}

    [[nodiscard]] int inputSampleRate() const noexcept override { return inRate_; }
    [[nodiscard]] int inputChannels() const noexcept override { return inChannels_; }
    [[nodiscard]] int outputSampleRate() const noexcept override { return outRate_; }
    [[nodiscard]] int outputChannels() const noexcept override { return outChannels_; }

    [[nodiscard]] Result<AudioBuffer> resample(const AudioBuffer& input) override {
        if (input.channels() != inChannels_ || input.sampleRate() != inRate_) {
            return err<AudioBuffer>(
                invalidArgument("resampler input format does not match its configuration"));
        }
        const std::size_t inFrames = input.frameCount();
        if (inFrames == 0) {
            return AudioBuffer(outRate_, outChannels_, 0);
        }

        // First convert the channel layout in the input rate domain, then
        // resample in time. Doing channels first keeps the time interpolation
        // uniform across the output channels.
        std::vector<float> chConv(inFrames * static_cast<std::size_t>(outChannels_), 0.0f);
        const std::vector<float>& src = input.samples();
        for (std::size_t f = 0; f < inFrames; ++f) {
            remapChannels(src.data() + f * static_cast<std::size_t>(inChannels_), inChannels_,
                          chConv.data() + f * static_cast<std::size_t>(outChannels_), outChannels_);
        }

        if (inRate_ == outRate_) {
            return AudioBuffer::interleaved(outRate_, outChannels_, std::move(chConv));
        }

        const std::size_t outFrames = resampledFrameCount(inFrames, inRate_, outRate_);
        std::vector<float> out(outFrames * static_cast<std::size_t>(outChannels_), 0.0f);
        const double step = static_cast<double>(inRate_) / static_cast<double>(outRate_);

        for (std::size_t of = 0; of < outFrames; ++of) {
            const double srcPos = static_cast<double>(of) * step;
            auto i0 = static_cast<std::size_t>(srcPos);
            const double frac = srcPos - static_cast<double>(i0);
            std::size_t i1 = i0 + 1;
            if (i1 >= inFrames) i1 = inFrames - 1;
            if (i0 >= inFrames) i0 = inFrames - 1;

            for (int c = 0; c < outChannels_; ++c) {
                const float a = chConv[i0 * static_cast<std::size_t>(outChannels_) +
                                       static_cast<std::size_t>(c)];
                const float b = chConv[i1 * static_cast<std::size_t>(outChannels_) +
                                       static_cast<std::size_t>(c)];
                out[of * static_cast<std::size_t>(outChannels_) + static_cast<std::size_t>(c)] =
                    a + static_cast<float>(frac) * (b - a);
            }
        }
        return AudioBuffer::interleaved(outRate_, outChannels_, std::move(out));
    }

private:
    int inRate_;
    int inChannels_;
    int outRate_;
    int outChannels_;
};

[[nodiscard]] Result<void> validateConversion(int inRate, int inChannels, int outRate,
                                               int outChannels) {
    if (inRate <= 0 || outRate <= 0 || inChannels <= 0 || outChannels <= 0) {
        return err<void>(invalidArgument(
            "resampler requires positive input/output sample rates and channel counts"));
    }
    return ok();
}

} // namespace

Result<std::unique_ptr<IResampler>> makeLinearResampler(int inRate, int inChannels, int outRate,
                                                        int outChannels) {
    if (Result<void> v = validateConversion(inRate, inChannels, outRate, outChannels); v.isError()) {
        return err<std::unique_ptr<IResampler>>(std::move(v).error());
    }
    return std::unique_ptr<IResampler>(
        std::make_unique<LinearResampler>(inRate, inChannels, outRate, outChannels));
}

// ---------------------------------------------------------------------------
// libswresample-backed resampler (compiled only with PALMIER_HAVE_FFMPEG)
// ---------------------------------------------------------------------------
#if defined(PALMIER_HAVE_FFMPEG)

namespace {

class SwrResampler final : public IResampler {
public:
    static Result<std::unique_ptr<IResampler>> create(int inRate, int inChannels, int outRate,
                                                       int outChannels) {
        SwrContext* swr = swr_alloc();
        if (swr == nullptr) {
            return err<std::unique_ptr<IResampler>>(
                makeError(ErrorCode::Internal, "could not allocate a resampler context"));
        }

        AVChannelLayout inLayout;
        AVChannelLayout outLayout;
        av_channel_layout_default(&inLayout, inChannels);
        av_channel_layout_default(&outLayout, outChannels);

        av_opt_set_chlayout(swr, "in_chlayout", &inLayout, 0);
        av_opt_set_chlayout(swr, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr, "in_sample_rate", inRate, 0);
        av_opt_set_int(swr, "out_sample_rate", outRate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        if (swr_init(swr) < 0) {
            swr_free(&swr);
            return err<std::unique_ptr<IResampler>>(
                makeError(ErrorCode::Internal, "could not initialize the resampler"));
        }
        return std::unique_ptr<IResampler>(
            std::unique_ptr<SwrResampler>(new SwrResampler(swr, inRate, inChannels, outRate, outChannels)));
    }

    ~SwrResampler() override {
        if (swr_ != nullptr) swr_free(&swr_);
    }

    SwrResampler(const SwrResampler&) = delete;
    SwrResampler& operator=(const SwrResampler&) = delete;

    [[nodiscard]] int inputSampleRate() const noexcept override { return inRate_; }
    [[nodiscard]] int inputChannels() const noexcept override { return inChannels_; }
    [[nodiscard]] int outputSampleRate() const noexcept override { return outRate_; }
    [[nodiscard]] int outputChannels() const noexcept override { return outChannels_; }

    [[nodiscard]] Result<AudioBuffer> resample(const AudioBuffer& input) override {
        if (input.channels() != inChannels_ || input.sampleRate() != inRate_) {
            return err<AudioBuffer>(
                invalidArgument("resampler input format does not match its configuration"));
        }
        const std::size_t inFrames = input.frameCount();
        if (inFrames == 0) {
            return AudioBuffer(outRate_, outChannels_, 0);
        }

        const int maxOut = swr_get_out_samples(swr_, static_cast<int>(inFrames));
        if (maxOut < 0) {
            return err<AudioBuffer>(makeError(ErrorCode::Internal, "resampler size query failed"));
        }

        std::vector<float> out(static_cast<std::size_t>(maxOut) *
                                   static_cast<std::size_t>(outChannels_),
                               0.0f);
        const std::uint8_t* inData[1] = {
            reinterpret_cast<const std::uint8_t*>(input.samples().data())};
        std::uint8_t* outData[1] = {reinterpret_cast<std::uint8_t*>(out.data())};

        const int produced = swr_convert(swr_, outData, maxOut, inData, static_cast<int>(inFrames));
        if (produced < 0) {
            return err<AudioBuffer>(makeError(ErrorCode::Internal, "resampling failed"));
        }
        out.resize(static_cast<std::size_t>(produced) * static_cast<std::size_t>(outChannels_));
        return AudioBuffer::interleaved(outRate_, outChannels_, std::move(out));
    }

private:
    SwrResampler(SwrContext* swr, int inRate, int inChannels, int outRate, int outChannels)
        : swr_(swr), inRate_(inRate), inChannels_(inChannels), outRate_(outRate),
          outChannels_(outChannels) {}

    SwrContext* swr_{nullptr};
    int         inRate_;
    int         inChannels_;
    int         outRate_;
    int         outChannels_;
};

} // namespace

bool isFfmpegResamplerAvailable() noexcept { return true; }

Result<std::unique_ptr<IResampler>> makeResampler(int inRate, int inChannels, int outRate,
                                                  int outChannels) {
    if (Result<void> v = validateConversion(inRate, inChannels, outRate, outChannels); v.isError()) {
        return err<std::unique_ptr<IResampler>>(std::move(v).error());
    }
    return SwrResampler::create(inRate, inChannels, outRate, outChannels);
}

#else // !PALMIER_HAVE_FFMPEG

bool isFfmpegResamplerAvailable() noexcept { return false; }

Result<std::unique_ptr<IResampler>> makeResampler(int inRate, int inChannels, int outRate,
                                                  int outChannels) {
    // No FFmpeg in this build: fall back to the built-in linear resampler so the
    // audio graph still resamples and mixes correctly (software-only lane).
    return makeLinearResampler(inRate, inChannels, outRate, outChannels);
}

#endif // PALMIER_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// AudioGraph
// ---------------------------------------------------------------------------

AudioGraph::AudioGraph(AudioFormat output) : output_(output) {}

Result<AudioGraph::SourceId> AudioGraph::addSource(int inRate, int inChannels, double gain) {
    if (!output_.isValid()) {
        return err<SourceId>(failedPrecondition("audio graph output format is invalid"));
    }
    if (gain < 0.0) {
        return err<SourceId>(invalidArgument("source gain must be non-negative"));
    }
    Result<std::unique_ptr<IResampler>> resampler =
        makeResampler(inRate, inChannels, output_.sampleRate, output_.channels);
    if (resampler.isError()) {
        return err<SourceId>(std::move(resampler).error());
    }

    Source s;
    s.inRate = inRate;
    s.inChannels = inChannels;
    s.gain = gain;
    s.resampler = std::move(resampler).value();
    sources_.push_back(std::move(s));
    return SourceId{sources_.size() - 1};
}

Result<void> AudioGraph::setGain(SourceId id, double gain) {
    if (id.value >= sources_.size()) {
        return err<void>(notFound("unknown audio source id"));
    }
    if (gain < 0.0) {
        return err<void>(invalidArgument("source gain must be non-negative"));
    }
    sources_[id.value].gain = gain;
    return ok();
}

Result<AudioBuffer> AudioGraph::render(const std::vector<AudioBuffer>& sourceInputs,
                                       std::size_t frameCount) {
    if (!output_.isValid()) {
        return err<AudioBuffer>(failedPrecondition("audio graph output format is invalid"));
    }
    if (sourceInputs.size() != sources_.size()) {
        return err<AudioBuffer>(
            invalidArgument("render input count does not match the registered source count"));
    }

    // Resample every source into the output rate/channels, keeping the results
    // alive for the mix step.
    std::vector<AudioBuffer> resampled;
    resampled.reserve(sources_.size());
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        const AudioBuffer& in = sourceInputs[i];
        if (in.channels() != sources_[i].inChannels || in.sampleRate() != sources_[i].inRate) {
            return err<AudioBuffer>(
                invalidArgument("render input format does not match its source configuration"));
        }
        Result<AudioBuffer> r = sources_[i].resampler->resample(in);
        if (r.isError()) return err<AudioBuffer>(std::move(r).error());
        resampled.push_back(std::move(r).value());
    }

    // Default the output length to the longest resampled source.
    std::size_t frames = frameCount;
    if (frames == 0) {
        for (const AudioBuffer& b : resampled) frames = std::max(frames, b.frameCount());
    }

    std::vector<MixSource> mixSources;
    mixSources.reserve(resampled.size());
    for (std::size_t i = 0; i < resampled.size(); ++i) {
        mixSources.push_back(MixSource{&resampled[i], sources_[i].gain});
    }

    return mix(output_.sampleRate, output_.channels, frames, mixSources);
}

Result<std::vector<std::byte>> AudioGraph::renderPacked(
    const std::vector<AudioBuffer>& sourceInputs, std::size_t frameCount) {
    Result<AudioBuffer> mixed = render(sourceInputs, frameCount);
    if (mixed.isError()) return err<std::vector<std::byte>>(std::move(mixed).error());
    return pack(mixed.value(), output_.sampleFormat);
}

} // namespace palmier::media
