// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/support/SyntheticMedia.cpp — implementation (see the header for what this
// is for and why it talks to libav* directly).
//
// The structure deliberately mirrors `media/MediaEncoder.cpp`'s FFmpeg backend:
// one AVFormatContext, one video AVCodecContext plus AVStream, an optional audio
// AVCodecContext/AVStream/SwrContext/AVFrame, an RGBA→pix_fmt SwsContext, and the
// same send-frame / receive-packet / rescale-timestamps / interleaved-write loop.
// The two differences are called out at their sites: the pixel format is
// NEGOTIATED from the encoder's own `pix_fmts` rather than fixed at 4:2:0, and the
// encoder is chosen from a candidate list rather than from
// `gpu::softwareEncoderName()`.

#include "support/SyntheticMedia.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/Error.hpp"

namespace palmier::test_support {

namespace {

// ---------------------------------------------------------------------------
// Pixel-format negotiation — the one thing the product backend does not do
// ---------------------------------------------------------------------------

/// The pixel format to encode in: the encoder's own first choice, unless it also
/// accepts plain 8-bit 4:2:0, which every candidate that has it prefers for size.
/// An encoder that advertises no list at all (rare, and legal) gets 4:2:0, which
/// is what `media::MediaEncoder`'s backend always uses.
[[nodiscard]] AVPixelFormat negotiatePixelFormat(const AVCodec* encoder) {
    if (encoder == nullptr || encoder->pix_fmts == nullptr) {
        return AV_PIX_FMT_YUV420P;
    }
    for (const AVPixelFormat* p = encoder->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    return encoder->pix_fmts[0];
}

/// The sample format to encode audio in: the encoder's first choice, or planar
/// float when it advertises none.
[[nodiscard]] AVSampleFormat negotiateSampleFormat(const AVCodec* encoder) {
    if (encoder == nullptr || encoder->sample_fmts == nullptr) {
        return AV_SAMPLE_FMT_FLTP;
    }
    return encoder->sample_fmts[0];
}

/// True when `encoder` accepts the requested sample rate. An encoder that
/// advertises no rate list accepts anything.
[[nodiscard]] bool acceptsSampleRate(const AVCodec* encoder, int rate) {
    if (encoder == nullptr || encoder->supported_samplerates == nullptr) return true;
    for (const int* r = encoder->supported_samplerates; *r != 0; ++r) {
        if (*r == rate) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// SyntheticAvWriter::Impl — every libav* resource, released in one place
// ---------------------------------------------------------------------------

struct SyntheticAvWriter::Impl {
    AVFormatContext* fmt{nullptr};

    AVCodecContext* video{nullptr};
    AVStream*       videoStream{nullptr};
    AVFrame*        videoFrame{nullptr};
    SwsContext*     sws{nullptr};

    AVCodecContext* audio{nullptr};
    AVStream*       audioStream{nullptr};
    SwrContext*     swr{nullptr};
    AVFrame*        audioFrame{nullptr};
    int             audioFrameSize{0};
    int             audioChannels{0};
    int             audioSampleRate{0};
    /// Interleaved-float samples received but not yet encoded, so a caller may
    /// submit blocks of any length while the encoder still sees whole frames.
    std::vector<float> fifo{};
    std::int64_t       nextAudioPts{0};

    AVPacket* packet{nullptr};
    bool      headerWritten{false};
    bool      finished{false};

    ~Impl() {
        if (sws != nullptr) sws_freeContext(sws);
        if (videoFrame != nullptr) av_frame_free(&videoFrame);
        if (audioFrame != nullptr) av_frame_free(&audioFrame);
        if (swr != nullptr) swr_free(&swr);
        if (audio != nullptr) avcodec_free_context(&audio);
        if (video != nullptr) avcodec_free_context(&video);
        if (packet != nullptr) av_packet_free(&packet);
        if (fmt != nullptr) {
            if ((fmt->oformat->flags & AVFMT_NOFILE) == 0 && fmt->pb != nullptr) {
                avio_closep(&fmt->pb);
            }
            avformat_free_context(fmt);
        }
    }

    [[nodiscard]] Result<void> sendVideo(AVFrame* frame) { return send(video, videoStream, frame); }
    [[nodiscard]] Result<void> sendAudio(AVFrame* frame) { return send(audio, audioStream, frame); }

    /// The product backend's packet loop, shared by both streams.
    [[nodiscard]] Result<void> send(AVCodecContext* codec, AVStream* stream, AVFrame* frame) {
        const int sent = avcodec_send_frame(codec, frame);
        if (sent < 0 && sent != AVERROR_EOF) {
            return err(makeError(ErrorCode::Io, "submitting a frame to the encoder failed"));
        }
        for (;;) {
            const int recv = avcodec_receive_packet(codec, packet);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) {
                return err(makeError(ErrorCode::Io, "receiving an encoded packet failed"));
            }
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int written = av_interleaved_write_frame(fmt, packet);
            av_packet_unref(packet);
            if (written < 0) {
                return err(makeError(ErrorCode::Io, "writing an encoded packet failed"));
            }
        }
        return ok();
    }

    /// Encode whole audio frames out of the interleaved-float FIFO; with `flush`
    /// the trailing partial frame is encoded too, which is legal for the LAST
    /// frame of a fixed-frame-size codec.
    [[nodiscard]] Result<void> drainAudio(bool flush) {
        if (audio == nullptr) return ok();
        const std::size_t channels = static_cast<std::size_t>(audioChannels);
        const std::size_t full = static_cast<std::size_t>(audioFrameSize) * channels;
        if (channels == 0 || full == 0) {
            return err(makeError(ErrorCode::Internal, "the audio stream has no frame geometry"));
        }

        while (fifo.size() >= full || (flush && !fifo.empty())) {
            const std::size_t take = std::min(full, fifo.size());
            const int frames = static_cast<int>(take / channels);
            if (frames <= 0) break;

            if (av_frame_make_writable(audioFrame) < 0) {
                return err(makeError(ErrorCode::Internal, "audio encoder frame is not writable"));
            }
            const std::uint8_t* inData[1] = {reinterpret_cast<const std::uint8_t*>(fifo.data())};
            const int converted = swr_convert(swr, audioFrame->data, frames, inData, frames);
            if (converted < 0) {
                return err(makeError(ErrorCode::Internal, "converting audio samples failed"));
            }
            audioFrame->nb_samples = converted;
            audioFrame->pts = nextAudioPts;
            nextAudioPts += converted;

            Result<void> queued = sendAudio(audioFrame);
            if (queued.isError()) return queued;

            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(take));
        }
        return ok();
    }
};

SyntheticAvWriter::SyntheticAvWriter(std::unique_ptr<Impl> impl, SyntheticAvChoice choice)
    : impl_(std::move(impl)), choice_(std::move(choice)) {}

SyntheticAvWriter::~SyntheticAvWriter() {
    // Defensive: a caller that abandons the writer still leaves a finalized
    // container rather than a truncated one.
    if (impl_ && !impl_->finished) {
        (void)finish();
    }
}

bool SyntheticAvWriter::hasAudioStream() const noexcept {
    return impl_ != nullptr && impl_->audio != nullptr;
}

// ---------------------------------------------------------------------------
// open() — negotiate the encoders, build the container, write the header
// ---------------------------------------------------------------------------

Result<std::unique_ptr<SyntheticAvWriter>> SyntheticAvWriter::open(
    const std::filesystem::path& path, const SyntheticAvSpec& spec) {
    using Out = std::unique_ptr<SyntheticAvWriter>;

    if (path.empty()) {
        return err<Out>(makeError(ErrorCode::InvalidArgument,
                                  "a synthetic media writer needs an output path"));
    }
    if (spec.width == 0 || spec.height == 0 || !spec.frameRate.isValid()) {
        return err<Out>(makeError(ErrorCode::InvalidArgument,
                                  "a synthetic media writer needs a positive geometry and a "
                                  "valid frame rate"));
    }
    if (spec.videoEncoders.empty()) {
        return err<Out>(makeError(ErrorCode::InvalidArgument,
                                  "a synthetic media writer needs at least one candidate video "
                                  "encoder"));
    }

    auto impl = std::make_unique<Impl>();
    impl->packet = av_packet_alloc();
    if (impl->packet == nullptr) {
        return err<Out>(makeError(ErrorCode::Internal, "could not allocate a packet"));
    }

    avformat_alloc_output_context2(&impl->fmt, nullptr,
                                   spec.container.empty() ? nullptr : spec.container.c_str(),
                                   path.c_str());
    if (impl->fmt == nullptr) {
        return err<Out>(makeError(ErrorCode::Io,
                                  "could not allocate an output context for container \"" +
                                      spec.container + "\""));
    }

    SyntheticAvChoice choice;
    choice.container = spec.container;

    // --- Video stream: first candidate that libavcodec carries AND opens ------
    std::ostringstream videoFailures;
    for (const std::string& name : spec.videoEncoders) {
        const AVCodec* encoder = avcodec_find_encoder_by_name(name.c_str());
        if (encoder == nullptr) {
            videoFailures << "\n    * \"" << name << "\": not carried by this libavcodec";
            continue;
        }

        // The encoder context is opened BEFORE a stream is added, so a candidate
        // that fails to open leaves no half-configured stream behind for
        // avformat_write_header to trip over.
        AVCodecContext* codec = avcodec_alloc_context3(encoder);
        if (codec == nullptr) {
            return err<Out>(
                makeError(ErrorCode::Internal, "could not allocate a video encoder context"));
        }

        codec->width = static_cast<int>(spec.width);
        codec->height = static_cast<int>(spec.height);
        codec->pix_fmt = negotiatePixelFormat(encoder);
        codec->time_base = AVRational{static_cast<int>(spec.frameRate.denominator()),
                                      static_cast<int>(spec.frameRate.numerator())};
        codec->framerate = AVRational{static_cast<int>(spec.frameRate.numerator()),
                                      static_cast<int>(spec.frameRate.denominator())};
        if (spec.videoBitrateBitsPerSecond > 0) {
            codec->bit_rate = spec.videoBitrateBitsPerSecond;
        }
        if ((impl->fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        const int opened = avcodec_open2(codec, encoder, nullptr);
        if (opened < 0) {
            videoFailures << "\n    * \"" << name << "\": avcodec_open2 failed at "
                          << spec.width << "x" << spec.height;
            avcodec_free_context(&codec);
            continue;
        }

        AVStream* stream = avformat_new_stream(impl->fmt, nullptr);
        if (stream == nullptr) {
            avcodec_free_context(&codec);
            return err<Out>(
                makeError(ErrorCode::Internal, "could not create the output video stream"));
        }
        stream->time_base = codec->time_base;
        if (avcodec_parameters_from_context(stream->codecpar, codec) < 0) {
            avcodec_free_context(&codec);
            return err<Out>(
                makeError(ErrorCode::Internal, "could not copy video encoder parameters"));
        }

        impl->video = codec;
        impl->videoStream = stream;
        choice.videoEncoder = name;
        break;
    }

    if (impl->video == nullptr) {
        return err<Out>(makeError(
            ErrorCode::Unsupported,
            "no candidate video encoder could be opened on this host:" + videoFailures.str()));
    }

    impl->videoFrame = av_frame_alloc();
    if (impl->videoFrame == nullptr) {
        return err<Out>(makeError(ErrorCode::Internal, "could not allocate a video frame"));
    }
    impl->videoFrame->format = impl->video->pix_fmt;
    impl->videoFrame->width = impl->video->width;
    impl->videoFrame->height = impl->video->height;
    if (av_frame_get_buffer(impl->videoFrame, 0) < 0) {
        return err<Out>(makeError(ErrorCode::Internal, "could not allocate the video frame buffer"));
    }

    // --- Optional audio stream ----------------------------------------------
    if (!spec.audioEncoders.empty()) {
        std::ostringstream audioFailures;
        for (const std::string& name : spec.audioEncoders) {
            const AVCodec* encoder = avcodec_find_encoder_by_name(name.c_str());
            if (encoder == nullptr) {
                audioFailures << "\n    * \"" << name << "\": not carried by this libavcodec";
                continue;
            }
            if (!acceptsSampleRate(encoder, spec.sampleRate)) {
                audioFailures << "\n    * \"" << name << "\": does not accept "
                              << spec.sampleRate << " Hz";
                continue;
            }

            // Same order as the video stream above: open first, add the stream
            // only once the encoder really opened.
            AVCodecContext* codec = avcodec_alloc_context3(encoder);
            if (codec == nullptr) {
                return err<Out>(
                    makeError(ErrorCode::Internal, "could not allocate an audio encoder context"));
            }

            codec->sample_fmt = negotiateSampleFormat(encoder);
            codec->sample_rate = spec.sampleRate;
            codec->time_base = AVRational{1, spec.sampleRate};
            if (spec.audioBitrateBitsPerSecond > 0) {
                codec->bit_rate = spec.audioBitrateBitsPerSecond;
            }
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_default(&codec->ch_layout, spec.channels);
#else
            codec->channels = spec.channels;
            codec->channel_layout =
                static_cast<std::uint64_t>(av_get_default_channel_layout(spec.channels));
#endif
            if ((impl->fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            if (avcodec_open2(codec, encoder, nullptr) < 0) {
                audioFailures << "\n    * \"" << name << "\": avcodec_open2 failed";
                avcodec_free_context(&codec);
                continue;
            }

            AVStream* stream = avformat_new_stream(impl->fmt, nullptr);
            if (stream == nullptr) {
                avcodec_free_context(&codec);
                return err<Out>(
                    makeError(ErrorCode::Internal, "could not create the output audio stream"));
            }
            stream->time_base = codec->time_base;
            if (avcodec_parameters_from_context(stream->codecpar, codec) < 0) {
                avcodec_free_context(&codec);
                return err<Out>(
                    makeError(ErrorCode::Internal, "could not copy audio encoder parameters"));
            }

            impl->audio = codec;
            impl->audioStream = stream;
            impl->audioChannels = spec.channels;
            impl->audioSampleRate = spec.sampleRate;
            // A fixed-frame-size codec (AAC: 1024) dictates the chunk; a
            // variable-frame-size codec (PCM) reports 0, for which any chunk works.
            impl->audioFrameSize = codec->frame_size > 0 ? codec->frame_size : 1024;
            choice.audioEncoder = name;
            break;
        }

        if (impl->audio == nullptr) {
            return err<Out>(makeError(
                ErrorCode::Unsupported,
                "no candidate audio encoder could be opened on this host:" + audioFailures.str()));
        }

        SwrContext* swr = nullptr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inLayout;
        av_channel_layout_default(&inLayout, spec.channels);
        if (swr_alloc_set_opts2(&swr, &impl->audio->ch_layout, impl->audio->sample_fmt,
                                impl->audio->sample_rate, &inLayout, AV_SAMPLE_FMT_FLT,
                                spec.sampleRate, 0, nullptr) < 0) {
            swr = nullptr;
        }
#else
        swr = swr_alloc_set_opts(nullptr, static_cast<std::int64_t>(impl->audio->channel_layout),
                                 impl->audio->sample_fmt, impl->audio->sample_rate,
                                 av_get_default_channel_layout(spec.channels), AV_SAMPLE_FMT_FLT,
                                 spec.sampleRate, 0, nullptr);
#endif
        if (swr == nullptr || swr_init(swr) < 0) {
            if (swr != nullptr) swr_free(&swr);
            return err<Out>(makeError(ErrorCode::Internal, "could not initialize the resampler"));
        }
        impl->swr = swr;

        impl->audioFrame = av_frame_alloc();
        if (impl->audioFrame == nullptr) {
            return err<Out>(makeError(ErrorCode::Internal, "could not allocate an audio frame"));
        }
        impl->audioFrame->format = impl->audio->sample_fmt;
        impl->audioFrame->sample_rate = impl->audio->sample_rate;
        impl->audioFrame->nb_samples = impl->audioFrameSize;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_copy(&impl->audioFrame->ch_layout, &impl->audio->ch_layout);
#else
        impl->audioFrame->channels = impl->audio->channels;
        impl->audioFrame->channel_layout = impl->audio->channel_layout;
#endif
        if (av_frame_get_buffer(impl->audioFrame, 0) < 0) {
            return err<Out>(
                makeError(ErrorCode::Internal, "could not allocate the audio frame buffer"));
        }
    }

    // --- Open the file and write the header ---------------------------------
    if ((impl->fmt->oformat->flags & AVFMT_NOFILE) == 0) {
        if (avio_open(&impl->fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            return err<Out>(
                makeError(ErrorCode::Io, "could not open the output file: " + path.string()));
        }
    }
    if (avformat_write_header(impl->fmt, nullptr) < 0) {
        return err<Out>(makeError(ErrorCode::Io, "could not write the output header"));
    }
    impl->headerWritten = true;

    return Result<Out>(Out(new SyntheticAvWriter(std::move(impl), std::move(choice))));
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

Result<void> SyntheticAvWriter::writeVideoFrame(const std::uint8_t* rgba, Duration presentation) {
    if (!impl_ || impl_->finished) {
        return err(makeError(ErrorCode::FailedPrecondition, "the writer is already finished"));
    }
    if (rgba == nullptr) {
        return err(makeError(ErrorCode::InvalidArgument, "a video frame needs RGBA pixels"));
    }

    const int w = impl_->video->width;
    const int h = impl_->video->height;
    impl_->sws = sws_getCachedContext(impl_->sws, w, h, AV_PIX_FMT_RGBA, w, h,
                                      impl_->video->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                                      nullptr);
    if (impl_->sws == nullptr) {
        return err(makeError(ErrorCode::Internal, "could not create the pixel converter"));
    }
    if (av_frame_make_writable(impl_->videoFrame) < 0) {
        return err(makeError(ErrorCode::Internal, "the encoder frame is not writable"));
    }

    const std::uint8_t* srcData[4] = {rgba, nullptr, nullptr, nullptr};
    const int srcStride[4] = {w * 4, 0, 0, 0};
    sws_scale(impl_->sws, srcData, srcStride, 0, h, impl_->videoFrame->data,
              impl_->videoFrame->linesize);

    // The pts is expressed in the ENCODER's time base, because `send()` below
    // rescales codec -> stream on every packet. Writing it in the stream's time
    // base instead would double-convert, and a muxer is free to rewrite
    // `stream->time_base` inside avformat_write_header (the mov muxer does), so the
    // stream base is not even stable across the header write.
    impl_->videoFrame->pts =
        av_rescale_q(presentation.nanoseconds(),
                     AVRational{1, static_cast<int>(Duration::kTicksPerSecond)},
                     impl_->video->time_base);

    Result<void> queued = impl_->sendVideo(impl_->videoFrame);
    if (queued.isError()) return queued;
    ++videoFrames_;
    return ok();
}

Result<void> SyntheticAvWriter::writeAudio(const float* interleaved, std::size_t frames) {
    if (!impl_ || impl_->finished) {
        return err(makeError(ErrorCode::FailedPrecondition, "the writer is already finished"));
    }
    if (impl_->audio == nullptr) {
        return err(makeError(ErrorCode::FailedPrecondition, "this output has no audio stream"));
    }
    if (frames == 0) return ok();
    if (interleaved == nullptr) {
        return err(makeError(ErrorCode::InvalidArgument, "an audio block needs samples"));
    }

    const std::size_t values = frames * static_cast<std::size_t>(impl_->audioChannels);
    impl_->fifo.insert(impl_->fifo.end(), interleaved, interleaved + values);
    Result<void> drained = impl_->drainAudio(/*flush=*/false);
    if (drained.isError()) return drained;
    audioFrames_ += frames;
    return ok();
}

Result<void> SyntheticAvWriter::finish() {
    if (!impl_) {
        return err(makeError(ErrorCode::FailedPrecondition, "the writer was never opened"));
    }
    if (impl_->finished) return ok();
    impl_->finished = true;

    if (impl_->audio != nullptr) {
        Result<void> tail = impl_->drainAudio(/*flush=*/true);
        if (tail.isError()) return tail;
        Result<void> flushed = impl_->sendAudio(nullptr);
        if (flushed.isError()) return flushed;
    }
    Result<void> flushed = impl_->sendVideo(nullptr);
    if (flushed.isError()) return flushed;

    if (impl_->headerWritten && av_write_trailer(impl_->fmt) < 0) {
        return err(makeError(ErrorCode::Io, "could not finalize the output file"));
    }
    return ok();
}

// ---------------------------------------------------------------------------
// The deterministic fixture source
// ---------------------------------------------------------------------------

namespace {

/// A frame of pixels derived from the frame index ALONE: a horizontal ramp, a
/// vertical ramp and a per-frame blue level, plus a moving 16-pixel-wide bar so
/// consecutive frames differ visibly (and so an inter-frame codec cannot collapse
/// the whole clip into one keyframe plus nothing).
void paintDeterministicFrame(std::vector<std::uint8_t>& rgba, std::uint32_t width,
                             std::uint32_t height, std::size_t index) {
    rgba.assign(static_cast<std::size_t>(width) * height * 4, 0u);
    const std::uint32_t barX = static_cast<std::uint32_t>((index * 7u) % width);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            const bool bar = (x >= barX && x < barX + 16);
            rgba[o + 0] = static_cast<std::uint8_t>(bar ? 255u : (x * 255u) / (width > 1 ? width - 1 : 1));
            rgba[o + 1] = static_cast<std::uint8_t>(bar ? 255u : (y * 255u) / (height > 1 ? height - 1 : 1));
            rgba[o + 2] = static_cast<std::uint8_t>((index * 4u) % 256u);
            rgba[o + 3] = 255u;
        }
    }
}

/// One frame interval of audio derived from the absolute sample index ALONE: an
/// integer sawtooth of a fixed 480-sample period (100 Hz at 48 kHz). Integer
/// arithmetic on purpose — no std::sin, so the samples do not depend on the host
/// libm's rounding.
void fillDeterministicAudio(std::vector<float>& interleaved, std::uint64_t firstSample,
                            std::size_t frames, int channels) {
    interleaved.assign(frames * static_cast<std::size_t>(channels), 0.0f);
    constexpr std::uint64_t kPeriod = 480;
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint64_t phase = (firstSample + f) % kPeriod;
        const float value = static_cast<float>(phase) / static_cast<float>(kPeriod) - 0.5f;
        for (int c = 0; c < channels; ++c) {
            // The right channel is inverted, so a channel mix-up is observable.
            interleaved[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                (c % 2 == 0) ? value * 0.5f : value * -0.5f;
        }
    }
}

}  // namespace

Result<SyntheticAvSource> writeSyntheticAvSource(const std::filesystem::path& path,
                                                 const SyntheticAvSpec& spec,
                                                 std::size_t videoFrames) {
    if (videoFrames == 0) {
        return err<SyntheticAvSource>(makeError(
            ErrorCode::InvalidArgument, "a synthetic source needs at least one video frame"));
    }

    Result<std::unique_ptr<SyntheticAvWriter>> opened = SyntheticAvWriter::open(path, spec);
    if (opened.isError()) return err<SyntheticAvSource>(opened.error());
    std::unique_ptr<SyntheticAvWriter> writer = std::move(opened).value();

    const Duration interval = spec.frameRate.frameDuration();
    std::vector<std::uint8_t> rgba;
    std::vector<float> audio;
    std::uint64_t samplesWritten = 0;

    for (std::size_t i = 0; i < videoFrames; ++i) {
        paintDeterministicFrame(rgba, spec.width, spec.height, i);
        Result<void> video = writer->writeVideoFrame(rgba.data(), interval * static_cast<std::int64_t>(i));
        if (video.isError()) return err<SyntheticAvSource>(video.error());

        if (writer->hasAudioStream()) {
            // Exactly the samples that fall inside frame i, computed from the
            // cumulative total so rounding never leaves a gap or an overlap.
            const std::uint64_t through =
                (static_cast<std::uint64_t>(spec.sampleRate) * (i + 1) *
                 static_cast<std::uint64_t>(spec.frameRate.denominator())) /
                static_cast<std::uint64_t>(spec.frameRate.numerator());
            const std::size_t frames = static_cast<std::size_t>(through - samplesWritten);
            fillDeterministicAudio(audio, samplesWritten, frames, spec.channels);
            Result<void> written = writer->writeAudio(audio.data(), frames);
            if (written.isError()) return err<SyntheticAvSource>(written.error());
            samplesWritten = through;
        }
    }

    Result<void> finished = writer->finish();
    if (finished.isError()) return err<SyntheticAvSource>(finished.error());

    SyntheticAvSource out;
    out.path = path;
    out.choice = writer->choice();
    out.videoFrames = videoFrames;
    out.duration = interval * static_cast<std::int64_t>(videoFrames);
    return out;
}

// ---------------------------------------------------------------------------
// The injected encode backend
// ---------------------------------------------------------------------------

namespace {

/// Bridges `media::IEncodeBackend` onto a `SyntheticAvWriter`, so the frames and
/// audio blocks the export really submitted end up as real coded bytes.
class RealBytesEncodeBackend final : public media::IEncodeBackend {
public:
    RealBytesEncodeBackend(std::unique_ptr<SyntheticAvWriter> writer,
                           RealBytesEncodeRecord* record)
        : writer_(std::move(writer)), record_(record) {
        if (record_ != nullptr) {
            std::lock_guard<std::mutex> lock(record_->mutex);
            record_->videoEncoder = writer_->choice().videoEncoder;
            record_->audioEncoder = writer_->choice().audioEncoder;
        }
    }

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        if (frame.hostData == nullptr) {
            return err(makeError(ErrorCode::FailedPrecondition,
                                 "the submitted frame has no host mapping to encode from"));
        }
        Result<void> written = writer_->writeVideoFrame(
            reinterpret_cast<const std::uint8_t*>(frame.hostData), frame.presentation);
        if (written.isOk() && record_ != nullptr) {
            record_->videoFrames.fetch_add(1);
        }
        return written;
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio& audio) override {
        if (audio.buffer == nullptr) {
            return err(makeError(ErrorCode::InvalidArgument,
                                 "the submitted audio block has no buffer"));
        }
        const std::size_t frames = audio.buffer->frameCount();
        if (frames == 0) return ok();
        Result<void> written = writer_->writeAudio(audio.buffer->samples().data(), frames);
        if (written.isOk() && record_ != nullptr) {
            record_->audioFrames.fetch_add(frames);
        }
        return written;
    }

    [[nodiscard]] Result<void> finish() override { return writer_->finish(); }

private:
    std::unique_ptr<SyntheticAvWriter> writer_;
    RealBytesEncodeRecord*             record_;
};

}  // namespace

media::EncodeBackendFactory realBytesEncodeBackendFactory(SyntheticAvSpec hints,
                                                          RealBytesEncodeRecord* record) {
    return [hints = std::move(hints), record](const media::EncodeSpec& spec,
                                              const gpu::CodecRoute&)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        // Everything that decides what the OUTPUT looks like comes from the
        // export's own spec; only the codec names come from the hints.
        SyntheticAvSpec effective = hints;
        effective.width = spec.resolution.width;
        effective.height = spec.resolution.height;
        effective.frameRate = spec.frameRate;
        if (!spec.containerFormat.empty()) effective.container = spec.containerFormat;
        if (spec.bitrateBitsPerSecond > 0) {
            effective.videoBitrateBitsPerSecond = spec.bitrateBitsPerSecond;
        }
        if (spec.audio.has_value()) {
            effective.sampleRate = spec.audio->sampleRate;
            effective.channels = spec.audio->channels;
            if (spec.audio->bitrateBitsPerSecond > 0) {
                effective.audioBitrateBitsPerSecond = spec.audio->bitrateBitsPerSecond;
            }
        } else {
            effective.audioEncoders.clear();
        }

        Result<std::unique_ptr<SyntheticAvWriter>> opened =
            SyntheticAvWriter::open(spec.outputPath, effective);
        if (opened.isError()) {
            return err<std::unique_ptr<media::IEncodeBackend>>(opened.error());
        }
        if (record != nullptr) record->backendsCreated.fetch_add(1);
        return Result<std::unique_ptr<media::IEncodeBackend>>(
            std::unique_ptr<media::IEncodeBackend>(std::make_unique<RealBytesEncodeBackend>(
                std::move(opened).value(), record)));
    };
}

std::string syntheticMediaUnavailableReason(const SyntheticAvSpec& spec) {
    std::ostringstream why;

    bool anyVideo = false;
    for (const std::string& name : spec.videoEncoders) {
        if (avcodec_find_encoder_by_name(name.c_str()) != nullptr) {
            anyVideo = true;
            break;
        }
    }
    if (!anyVideo) {
        why << "libavcodec on this host carries none of the candidate video encoders (";
        for (std::size_t i = 0; i < spec.videoEncoders.size(); ++i) {
            if (i != 0) why << ", ";
            why << '"' << spec.videoEncoders[i] << '"';
        }
        why << ")";
    }

    if (!spec.audioEncoders.empty()) {
        bool anyAudio = false;
        for (const std::string& name : spec.audioEncoders) {
            const AVCodec* encoder = avcodec_find_encoder_by_name(name.c_str());
            if (encoder != nullptr && acceptsSampleRate(encoder, spec.sampleRate)) {
                anyAudio = true;
                break;
            }
        }
        if (!anyAudio) {
            if (why.tellp() != std::streampos(0)) why << "; ";
            why << "libavcodec on this host carries none of the candidate audio encoders (";
            for (std::size_t i = 0; i < spec.audioEncoders.size(); ++i) {
                if (i != 0) why << ", ";
                why << '"' << spec.audioEncoders[i] << '"';
            }
            why << ") at " << spec.sampleRate << " Hz";
        }
    }

    return why.str();
}

}  // namespace palmier::test_support
