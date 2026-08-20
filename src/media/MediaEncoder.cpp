// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaEncoder.cpp — MediaEncoder routing/fallback + FFmpeg encode backend.
//
// Task 9.3 adds the second (audio) stream (Requirement 6.5). The backend-agnostic
// half — EncodeSpec::audio validation, submitAudio()'s format and ordering guards,
// and the "flush both streams then write the trailer" contract of finish() — is
// always compiled and fully exercised through the IEncodeBackend seam. The
// concrete libav* audio work (one extra stream added to the muxer before the
// header is written, an interleaved-float FIFO cut into the codec's fixed frame
// size, libswresample bridging float -> the codec's sample format) lives beside
// the video backend under PALMIER_HAVE_FFMPEG.
//
// The MediaEncoder logic (create/submit/finish, the HW-preferred-with-SW-
// fallback-on-init routing through the GPU CodecBridge, and the resolution /
// presentation-order guards) is backend-agnostic and always compiled: it drives
// whatever IEncodeBackend the factory produced. The concrete FFmpeg
// (libavformat/libavcodec/libswscale) backend is compiled only when
// PALMIER_HAVE_FFMPEG is defined; otherwise the default factory returns a
// FailedPrecondition error, mirroring MediaDecoder/MediaProbe so the module
// builds and its routing/fallback logic stays testable on machines without
// FFmpeg.

#include "media/MediaEncoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace palmier::media {

// ---------------------------------------------------------------------------
// FFmpeg-backed encode backend (compiled only with PALMIER_HAVE_FFMPEG).
// ---------------------------------------------------------------------------
//
// This backend encodes host-memory RGBA8 frames (converting to the encoder's
// pixel format with libswscale) through a chosen encoder and muxes them into the
// output container. The route's `softwareEncoder` names the FFmpeg software
// encoder library ("libx264" | "libx265" | "libsvtav1" | "libvpx-vp9") for a
// software route; a hardware route selects the vendor encoder by name. Backend
// *initialization* (finding/opening the encoder, opening the muxer, writing the
// header) happens in openFfmpegEncoder(); when a hardware encoder cannot be
// initialized it returns an Error so MediaEncoder::create retries once on the
// software encoder (Requirement 10.5). Zero-copy hardware-surface submission is
// layered on where a vendor path is compiled in; until then GPU frames whose
// host mapping is available are encoded from that mapping, and the HW-init/SW-
// fallback routing itself is exercised end-to-end via the mock backend in the
// unit tests.
#if defined(PALMIER_HAVE_FFMPEG)

namespace {

[[nodiscard]] const char* ffmpegEncoderName(const gpu::CodecRoute& route) {
    if (!route.hardware) {
        // Software route: the bridge already picked the library name.
        return route.softwareEncoder.empty() ? "libx264" : route.softwareEncoder.c_str();
    }
    switch (route.backend) {
        case gpu::CodecBackend::Nvenc:
            return route.codec == gpu::CodecId::HEVC ? "hevc_nvenc" : "h264_nvenc";
        case gpu::CodecBackend::Vaapi:
            return route.codec == gpu::CodecId::HEVC ? "hevc_vaapi" : "h264_vaapi";
        case gpu::CodecBackend::QuickSync:
            return route.codec == gpu::CodecId::HEVC ? "hevc_qsv" : "h264_qsv";
        default:
            return "libx264";
    }
}

/// The muxer-side state of the optional audio stream (Requirement 6.5). Built by
/// openFfmpegEncoder BEFORE the container header is written — a stream cannot be
/// added afterwards — and then owned by the backend.
struct FfmpegAudioStream {
    AVCodecContext* ctx{nullptr};
    AVStream*       stream{nullptr};
    SwrContext*     swr{nullptr};
    AVFrame*        frame{nullptr};
    /// Samples per encoded frame. Fixed-frame-size codecs (AAC: 1024) require
    /// exactly this count for every frame but the last.
    int             frameSize{0};
    int             channels{0};
    int             sampleRate{0};
    /// Interleaved-float samples received but not yet encoded, so a caller may
    /// submit blocks of any length while the encoder still sees whole frames.
    std::vector<float> fifo{};
    /// Next frame's pts in samples (the audio stream's time base is 1/rate).
    std::int64_t    nextPts{0};

    [[nodiscard]] bool isOpen() const noexcept { return ctx != nullptr; }
};

class FfmpegEncodeBackend final : public IEncodeBackend {
public:
    FfmpegEncodeBackend(AVFormatContext* fmt, AVCodecContext* codec, AVStream* stream,
                        FfmpegAudioStream audio = {})
        : fmt_(fmt), codec_(codec), stream_(stream), audio_(std::move(audio)) {
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (frame_ != nullptr) {
            frame_->format = codec_->pix_fmt;
            frame_->width = codec_->width;
            frame_->height = codec_->height;
            av_frame_get_buffer(frame_, 0);
        }
    }

    ~FfmpegEncodeBackend() override {
        if (sws_ != nullptr) sws_freeContext(sws_);
        if (frame_ != nullptr) av_frame_free(&frame_);
        if (audio_.frame != nullptr) av_frame_free(&audio_.frame);
        if (audio_.swr != nullptr) swr_free(&audio_.swr);
        if (audio_.ctx != nullptr) avcodec_free_context(&audio_.ctx);
        if (packet_ != nullptr) av_packet_free(&packet_);
        if (codec_ != nullptr) avcodec_free_context(&codec_);
        if (fmt_ != nullptr) {
            if ((fmt_->oformat->flags & AVFMT_NOFILE) == 0 && fmt_->pb != nullptr) {
                avio_closep(&fmt_->pb);
            }
            avformat_free_context(fmt_);
        }
    }

    FfmpegEncodeBackend(const FfmpegEncodeBackend&) = delete;
    FfmpegEncodeBackend& operator=(const FfmpegEncodeBackend&) = delete;

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        if (codec_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
            return makeError(ErrorCode::Internal, "encoder was not initialized");
        }
        if (frame.hostData == nullptr) {
            // A pure-GPU surface with no host mapping needs the vendor zero-copy
            // path (compiled in with PALMIER_HAVE_VAAPI/NVENC/QSV); without it we
            // cannot read the pixels.
            return failedPrecondition(
                "hardware frame has no host mapping for software submission");
        }

        if (av_frame_make_writable(frame_) < 0) {
            return makeError(ErrorCode::Internal, "encoder frame is not writable");
        }

        const int w = static_cast<int>(frame.desc.width);
        const int h = static_cast<int>(frame.desc.height);
        sws_ = sws_getCachedContext(sws_, w, h, AV_PIX_FMT_RGBA, w, h, codec_->pix_fmt,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) {
            return makeError(ErrorCode::Internal, "could not create pixel converter");
        }

        const std::uint8_t* srcData[4] = {
            reinterpret_cast<const std::uint8_t*>(frame.hostData), nullptr, nullptr, nullptr};
        const int srcStride[4] = {w * 4, 0, 0, 0};
        sws_scale(sws_, srcData, srcStride, 0, h, frame_->data, frame_->linesize);

        // Presentation timestamp in the ENCODER's time base, not the stream's:
        // avcodec_send_frame() interprets AVFrame::pts against codec_->time_base.
        // avformat_write_header() rewrites AVStream::time_base for many muxers
        // (MOV/MP4 included) to whatever timescale the container format prefers,
        // which is why the packet-level rescale below (sendFrame) goes through
        // av_packet_rescale_ts(codec_->time_base -> stream_->time_base) instead of
        // assuming the two ever stay equal. Computing the frame's own pts against
        // stream_->time_base here fed the encoder timestamps scaled for the wrong
        // clock, so every encoded frame reported a presentation time far later
        // than intended without changing the frame COUNT — this is what produced
        // a probed/muxed duration wildly larger than the timeline duration while
        // still encoding exactly the right number of frames.
        frame_->pts = av_rescale_q(frame.presentation.nanoseconds(),
                                   AVRational{1, static_cast<int>(Duration::kTicksPerSecond)},
                                   codec_->time_base);

        return sendFrame(frame_);
    }

    [[nodiscard]] Result<void> encodeAudio(const EncoderInputAudio& audio) override {
        if (!audio_.isOpen()) {
            return failedPrecondition("this output has no audio stream");
        }
        if (audio.buffer == nullptr) {
            return invalidArgument("submitted audio block has no buffer");
        }
        // Buffer the block's interleaved float samples and emit whole encoder
        // frames; a fixed-frame-size codec (AAC) cannot be fed arbitrary lengths.
        const std::vector<float>& samples = audio.buffer->samples();
        audio_.fifo.insert(audio_.fifo.end(), samples.begin(), samples.end());
        return drainAudio(/*flush=*/false);
    }

    [[nodiscard]] Result<void> finish() override {
        if (codec_ == nullptr) {
            return failedPrecondition("encoder is not open");
        }
        // Flush BOTH streams before the trailer (Requirement 6.5): the buffered
        // audio tail, then each encoder's internal delay, then finalize the mux.
        if (audio_.isOpen()) {
            Result<void> tail = drainAudio(/*flush=*/true);
            if (tail.isError()) return tail;
            Result<void> audioFlushed = sendAudioFrame(nullptr);
            if (audioFlushed.isError()) return audioFlushed;
        }
        Result<void> flushed = sendFrame(nullptr);
        if (flushed.isError()) return flushed;
        if (av_write_trailer(fmt_) < 0) {
            return makeError(ErrorCode::Io, "could not finalize the output file");
        }
        return ok();
    }

private:
    /// Encode whole frames out of the interleaved-float FIFO. With `flush` set the
    /// trailing partial frame is encoded too — permitted for the LAST frame of a
    /// fixed-frame-size codec, which is exactly what this is.
    [[nodiscard]] Result<void> drainAudio(bool flush) {
        const std::size_t channels = static_cast<std::size_t>(audio_.channels);
        const std::size_t full = static_cast<std::size_t>(audio_.frameSize) * channels;
        if (channels == 0 || full == 0) {
            return makeError(ErrorCode::Internal, "the audio stream has no frame geometry");
        }

        while (audio_.fifo.size() >= full || (flush && !audio_.fifo.empty())) {
            const std::size_t take = std::min(full, audio_.fifo.size());
            const int frames = static_cast<int>(take / channels);
            if (frames <= 0) break;

            if (av_frame_make_writable(audio_.frame) < 0) {
                return makeError(ErrorCode::Internal, "audio encoder frame is not writable");
            }
            const std::uint8_t* inData[1] = {
                reinterpret_cast<const std::uint8_t*>(audio_.fifo.data())};
            const int converted =
                swr_convert(audio_.swr, audio_.frame->data, frames, inData, frames);
            if (converted < 0) {
                return makeError(ErrorCode::Internal, "converting audio samples failed");
            }
            audio_.frame->nb_samples = converted;
            audio_.frame->pts = audio_.nextPts;
            audio_.nextPts += converted;

            Result<void> sent = sendAudioFrame(audio_.frame);
            if (sent.isError()) return sent;

            audio_.fifo.erase(audio_.fifo.begin(),
                              audio_.fifo.begin() + static_cast<std::ptrdiff_t>(take));
        }
        return ok();
    }

    [[nodiscard]] Result<void> sendAudioFrame(AVFrame* f) {
        const int sent = avcodec_send_frame(audio_.ctx, f);
        if (sent < 0 && sent != AVERROR_EOF) {
            return makeError(ErrorCode::Io, "submitting a block to the audio encoder failed");
        }
        for (;;) {
            const int recv = avcodec_receive_packet(audio_.ctx, packet_);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) {
                return makeError(ErrorCode::Io, "receiving an encoded audio packet failed");
            }
            av_packet_rescale_ts(packet_, audio_.ctx->time_base, audio_.stream->time_base);
            packet_->stream_index = audio_.stream->index;
            const int written = av_interleaved_write_frame(fmt_, packet_);
            av_packet_unref(packet_);
            if (written < 0) {
                return makeError(ErrorCode::Io, "writing an encoded audio packet failed");
            }
        }
        return ok();
    }

    [[nodiscard]] Result<void> sendFrame(AVFrame* f) {
        const int sent = avcodec_send_frame(codec_, f);
        if (sent < 0 && sent != AVERROR_EOF) {
            return makeError(ErrorCode::Io, "submitting a frame to the encoder failed");
        }
        for (;;) {
            const int recv = avcodec_receive_packet(codec_, packet_);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) {
                return makeError(ErrorCode::Io, "receiving an encoded packet failed");
            }
            av_packet_rescale_ts(packet_, codec_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            const int written = av_interleaved_write_frame(fmt_, packet_);
            av_packet_unref(packet_);
            if (written < 0) {
                return makeError(ErrorCode::Io, "writing an encoded packet failed");
            }
        }
        return ok();
    }

    AVFormatContext*  fmt_{nullptr};
    AVCodecContext*   codec_{nullptr};
    AVStream*         stream_{nullptr};
    AVPacket*         packet_{nullptr};
    AVFrame*          frame_{nullptr};
    SwsContext*       sws_{nullptr};
    FfmpegAudioStream audio_{};
};

/// Release an audio stream's encoder/resampler/frame. Used on the open paths that
/// fail AFTER the audio stream was built (the backend's destructor owns it once
/// construction succeeds).
void freeFfmpegAudioStream(FfmpegAudioStream& audio) noexcept {
    if (audio.frame != nullptr) av_frame_free(&audio.frame);
    if (audio.swr != nullptr) swr_free(&audio.swr);
    if (audio.ctx != nullptr) avcodec_free_context(&audio.ctx);
}

/// Add the audio stream to `fmt` and initialize its encoder and resampler. Called
/// before the container header is written. On failure nothing is added and the
/// error names the audio stage, so MediaEncoder reports audio encoding as the
/// failing stage (Requirement 6.10).
[[nodiscard]] Result<FfmpegAudioStream> openFfmpegAudioStream(AVFormatContext* fmt,
                                                              const AudioEncodeSpec& spec) {
    FfmpegAudioStream audio;
    audio.channels = spec.channels;
    audio.sampleRate = spec.sampleRate;

    const AVCodec* encoder = avcodec_find_encoder_by_name(spec.codecName.c_str());
    if (encoder == nullptr) {
        return err<FfmpegAudioStream>(makeError(
            ErrorCode::Unsupported, "audio encoder not found: " + spec.codecName));
    }

    AVStream* stream = avformat_new_stream(fmt, nullptr);
    if (stream == nullptr) {
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not create the output audio stream"));
    }
    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    if (ctx == nullptr) {
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not allocate an audio encoder context"));
    }

    // The encoder's sample format is whatever the codec prefers; the graph always
    // hands us interleaved float, so libswresample bridges the two.
    ctx->sample_fmt = encoder->sample_fmts != nullptr ? encoder->sample_fmts[0]
                                                      : AV_SAMPLE_FMT_FLTP;
    ctx->sample_rate = spec.sampleRate;
    ctx->time_base = AVRational{1, spec.sampleRate};
    if (spec.bitrateBitsPerSecond > 0) {
        ctx->bit_rate = spec.bitrateBitsPerSecond;
    }
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&ctx->ch_layout, spec.channels);
#else
    ctx->channels = spec.channels;
    ctx->channel_layout = static_cast<std::uint64_t>(
        av_get_default_channel_layout(spec.channels));
#endif
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ctx, encoder, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return err<FfmpegAudioStream>(makeError(
            ErrorCode::Internal, "could not initialize the " + spec.codecName + " audio encoder"));
    }
    if (avcodec_parameters_from_context(stream->codecpar, ctx) < 0) {
        avcodec_free_context(&ctx);
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not copy audio encoder parameters"));
    }
    stream->time_base = ctx->time_base;

    // A fixed-frame-size codec (AAC: 1024) dictates the encode chunk; a
    // variable-frame-size codec (PCM) reports 0, for which any chunk is legal.
    audio.frameSize = ctx->frame_size > 0 ? ctx->frame_size : 1024;

    SwrContext* swr = nullptr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, spec.channels);
    if (swr_alloc_set_opts2(&swr, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, &inLayout,
                            AV_SAMPLE_FMT_FLT, spec.sampleRate, 0, nullptr) < 0) {
        swr = nullptr;
    }
#else
    swr = swr_alloc_set_opts(nullptr,
                             static_cast<std::int64_t>(ctx->channel_layout), ctx->sample_fmt,
                             ctx->sample_rate,
                             av_get_default_channel_layout(spec.channels), AV_SAMPLE_FMT_FLT,
                             spec.sampleRate, 0, nullptr);
#endif
    if (swr == nullptr || swr_init(swr) < 0) {
        if (swr != nullptr) swr_free(&swr);
        avcodec_free_context(&ctx);
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not initialize the audio resampler"));
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        swr_free(&swr);
        avcodec_free_context(&ctx);
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not allocate an audio encoder frame"));
    }
    frame->format = ctx->sample_fmt;
    frame->sample_rate = ctx->sample_rate;
    frame->nb_samples = audio.frameSize;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
#else
    frame->channels = ctx->channels;
    frame->channel_layout = ctx->channel_layout;
#endif
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&ctx);
        return err<FfmpegAudioStream>(
            makeError(ErrorCode::Internal, "could not allocate the audio encoder frame buffer"));
    }

    audio.ctx = ctx;
    audio.stream = stream;
    audio.swr = swr;
    audio.frame = frame;
    return audio;
}

[[nodiscard]] Result<std::unique_ptr<IEncodeBackend>> openFfmpegEncoder(
    const EncodeSpec& spec, const gpu::CodecRoute& route) {
    if (spec.outputPath.empty()) {
        return err<std::unique_ptr<IEncodeBackend>>(
            invalidArgument("encode requires an output path"));
    }

    const std::string container =
        spec.containerFormat.empty() ? std::string{} : spec.containerFormat;
    AVFormatContext* fmt = nullptr;
    avformat_alloc_output_context2(&fmt, nullptr,
                                   container.empty() ? nullptr : container.c_str(),
                                   spec.outputPath.c_str());
    if (fmt == nullptr) {
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Io, "could not allocate an output context"));
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name(ffmpegEncoderName(route));
    if (encoder == nullptr) {
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(makeError(
            ErrorCode::Unsupported, std::string("encoder not found: ") + ffmpegEncoderName(route)));
    }

    AVStream* stream = avformat_new_stream(fmt, nullptr);
    if (stream == nullptr) {
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Internal, "could not create an output stream"));
    }

    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    if (codec == nullptr) {
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Internal, "could not allocate an encoder context"));
    }

    codec->width = static_cast<int>(spec.resolution.width);
    codec->height = static_cast<int>(spec.resolution.height);
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational{static_cast<int>(spec.frameRate.denominator()),
                                  static_cast<int>(spec.frameRate.numerator())};
    codec->framerate = AVRational{static_cast<int>(spec.frameRate.numerator()),
                                  static_cast<int>(spec.frameRate.denominator())};
    stream->time_base = codec->time_base;
    if (spec.bitrateBitsPerSecond > 0) {
        codec->bit_rate = spec.bitrateBitsPerSecond;
    }
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Opening the encoder is the step that fails when a hardware encoder is
    // unavailable/uninitializable — the trigger for the HW->SW init retry.
    if (avcodec_open2(codec, encoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(makeError(
            ErrorCode::Internal,
            std::string("could not initialize the ") + ffmpegEncoderName(route) + " encoder"));
    }
    if (avcodec_parameters_from_context(stream->codecpar, codec) < 0) {
        avcodec_free_context(&codec);
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Internal, "could not copy encoder parameters"));
    }

    // The optional audio stream must join the container BEFORE the header is
    // written — a muxer accepts no new stream afterwards (Requirement 6.5).
    FfmpegAudioStream audio;
    if (spec.audio.has_value()) {
        Result<FfmpegAudioStream> opened = openFfmpegAudioStream(fmt, *spec.audio);
        if (opened.isError()) {
            avcodec_free_context(&codec);
            avformat_free_context(fmt);
            return err<std::unique_ptr<IEncodeBackend>>(std::move(opened).error());
        }
        audio = std::move(opened).value();
    }

    if ((fmt->oformat->flags & AVFMT_NOFILE) == 0) {
        if (avio_open(&fmt->pb, spec.outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            freeFfmpegAudioStream(audio);
            avcodec_free_context(&codec);
            avformat_free_context(fmt);
            return err<std::unique_ptr<IEncodeBackend>>(makeError(
                ErrorCode::Io, "could not open the output file: " + spec.outputPath.string()));
        }
    }
    if (avformat_write_header(fmt, nullptr) < 0) {
        if ((fmt->oformat->flags & AVFMT_NOFILE) == 0 && fmt->pb != nullptr) {
            avio_closep(&fmt->pb);
        }
        freeFfmpegAudioStream(audio);
        avcodec_free_context(&codec);
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Io, "could not write the output header"));
    }

    return std::unique_ptr<IEncodeBackend>(
        std::make_unique<FfmpegEncodeBackend>(fmt, codec, stream, std::move(audio)));
}

} // namespace

bool isFfmpegEncodeAvailable() noexcept { return true; }

EncodeBackendFactory ffmpegEncodeBackendFactory() {
    return [](const EncodeSpec& spec, const gpu::CodecRoute& route) {
        return openFfmpegEncoder(spec, route);
    };
}

#else // !PALMIER_HAVE_FFMPEG

bool isFfmpegEncodeAvailable() noexcept { return false; }

EncodeBackendFactory ffmpegEncodeBackendFactory() {
    return [](const EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        return err<std::unique_ptr<IEncodeBackend>>(makeError(
            ErrorCode::FailedPrecondition,
            "media encoding requires FFmpeg, which was not compiled into this build"));
    };
}

#endif // PALMIER_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// MediaEncoder — backend-agnostic routing + validation.
// ---------------------------------------------------------------------------

MediaEncoder::MediaEncoder(EncodeSpec spec, std::unique_ptr<IEncodeBackend> backend,
                           gpu::CodecBridge bridge, gpu::CodecRoute route,
                           bool usedSoftwareFallback)
    : spec_(std::move(spec)),
      backend_(std::move(backend)),
      bridge_(std::move(bridge)),
      route_(std::move(route)),
      usedSoftwareFallback_(usedSoftwareFallback) {}

Result<MediaEncoder> MediaEncoder::create(const EncodeSpec& spec,
                                          const EncodeBackendFactory& factory) {
    if (!spec.resolution.isValid()) {
        return err<MediaEncoder>(invalidArgument("encode resolution must be positive"));
    }
    if (!spec.frameRate.isValid()) {
        return err<MediaEncoder>(invalidArgument("encode frame rate must be positive"));
    }
    if (spec.bitrateBitsPerSecond < 0) {
        return err<MediaEncoder>(invalidArgument("encode bit rate must not be negative"));
    }
    if (spec.codec == gpu::CodecId::Unknown) {
        return err<MediaEncoder>(unsupported("no encoder exists for an unknown codec"));
    }
    // The optional audio stream is validated with the same "reject before the
    // backend is built" discipline as the video parameters, so a malformed audio
    // request never opens the output file (Requirement 6.5).
    if (spec.audio.has_value()) {
        const AudioEncodeSpec& audio = *spec.audio;
        if (audio.sampleRate <= 0) {
            return err<MediaEncoder>(
                invalidArgument("audio encode sample rate must be positive"));
        }
        if (audio.channels <= 0) {
            return err<MediaEncoder>(
                invalidArgument("audio encode channel count must be positive"));
        }
        if (audio.bitrateBitsPerSecond < 0) {
            return err<MediaEncoder>(
                invalidArgument("audio encode bit rate must not be negative"));
        }
        if (audio.codecName.empty()) {
            return err<MediaEncoder>(invalidArgument("audio encode requires a codec name"));
        }
    }
    if (!factory) {
        return err<MediaEncoder>(makeError(ErrorCode::Internal, "no encode backend factory provided"));
    }

    // Route against the requested device (or the software profile when hardware
    // is opted out), so the bridge falls back to the software encoder on a
    // hardware init failure (Requirement 10.5).
    gpu::CodecBridge bridge(spec.preferHardware ? spec.caps : gpu::GpuCaps::software(),
                            spec.availability);

    std::unique_ptr<IEncodeBackend> backend;
    gpu::CodecOpFn initOp = [&](const gpu::CodecRoute& route) -> Result<void> {
        Result<std::unique_ptr<IEncodeBackend>> made = factory(spec, route);
        if (made.isError()) return made.error();
        std::unique_ptr<IEncodeBackend> impl = std::move(made).value();
        if (!impl) {
            return makeError(ErrorCode::Internal, "encode backend factory returned null");
        }
        backend = std::move(impl);
        return ok();
    };

    // execute() runs initOp on the routed backend and, on a hardware failure,
    // retries it exactly once on the software encoder — logging the failure.
    gpu::CodecExecution exec = bridge.execute(spec.codec, gpu::CodecOperation::Encode, initOp);
    if (exec.result.isError()) {
        return err<MediaEncoder>(std::move(exec.result).error());
    }
    if (!backend) {
        return err<MediaEncoder>(
            makeError(ErrorCode::Internal, "encoder init reported success but produced no backend"));
    }

    return MediaEncoder(spec, std::move(backend), std::move(bridge), exec.route,
                        exec.retriedOnCpu);
}

Result<MediaEncoder> MediaEncoder::create(const EncodeSpec& spec) {
    return create(spec, ffmpegEncodeBackendFactory());
}

Result<void> MediaEncoder::submit(const gpu::RenderedFrame& frame) {
    if (finished_) {
        return failedPrecondition("cannot submit to an encoder that was already finished");
    }
    if (!backend_) {
        return failedPrecondition("encoder is not open");
    }
    if (!frame.valid()) {
        return invalidArgument("submitted frame has no backing image");
    }

    // The design's submit precondition: frame resolution matches the spec. A
    // mismatch is rejected without touching the encoder, so the stream is not
    // corrupted.
    if (frame.width() != spec_.resolution.width || frame.height() != spec_.resolution.height) {
        return invalidArgument("submitted frame resolution does not match the encoder spec");
    }

    // Frames must be queued in non-decreasing presentation order (design
    // postcondition; property P6). A regression is rejected without advancing
    // state, leaving the stream uncorrupted.
    const Duration pts = frame.presentationTime();
    if (hasSubmitted_ && pts < lastPresentation_) {
        return invalidArgument("submitted frame presentation time regresses below the previous frame");
    }

    EncoderInputFrame input;
    input.presentation = pts;
    input.desc = frame.desc();
    input.image = frame.image();
    input.gpuResident = input.image.isZeroCopy();
    input.hostData = frame.hostData();

    Result<void> encoded = backend_->encode(input);
    if (encoded.isError()) {
        // Do not advance ordering/count state on failure: the caller sees the
        // error and the stream is left uncorrupted.
        return encoded;
    }

    lastPresentation_ = pts;
    hasSubmitted_ = true;
    ++submittedFrames_;
    return ok();
}

Result<void> MediaEncoder::submitAudio(const AudioBuffer& buffer, Duration presentation) {
    if (finished_) {
        return failedPrecondition("cannot submit to an encoder that was already finished");
    }
    if (!backend_) {
        return failedPrecondition("encoder is not open");
    }
    // A video-only encoder rejects audio rather than dropping it silently: an
    // export that believed it was writing audio must find out here, not by
    // probing the finished file (Requirement 6.5).
    if (!spec_.audio.has_value()) {
        return failedPrecondition(
            "this encoder has no audio stream: EncodeSpec::audio was not set");
    }
    const AudioEncodeSpec& audioSpec = *spec_.audio;

    // The submitted format must be exactly the configured stream format. Silent
    // resampling here would put a second, hidden mixer in the pipeline; the
    // caller mixes through AudioGraph, which already produces this format.
    if (buffer.sampleRate() != audioSpec.sampleRate) {
        return invalidArgument(
            "submitted audio sample rate does not match the encoder audio spec");
    }
    if (buffer.channels() != audioSpec.channels) {
        return invalidArgument(
            "submitted audio channel count does not match the encoder audio spec");
    }

    // Blocks must be queued in non-decreasing presentation order, the same rule
    // the video stream follows. A regression is rejected without advancing state,
    // leaving the stream uncorrupted.
    if (hasSubmittedAudio_ && presentation < lastAudioPresentation_) {
        return invalidArgument(
            "submitted audio presentation time regresses below the previous block");
    }

    EncoderInputAudio input;
    input.presentation = presentation;
    input.buffer = &buffer;

    Result<void> encoded = backend_->encodeAudio(input);
    if (encoded.isError()) {
        // Do not advance ordering/count state on failure, exactly as submit()
        // does: the caller sees the error and the stream is left uncorrupted.
        return encoded;
    }

    lastAudioPresentation_ = presentation;
    hasSubmittedAudio_ = true;
    ++submittedAudioBlocks_;
    submittedAudioFrames_ += static_cast<std::uint64_t>(buffer.frameCount());
    return ok();
}

Result<void> MediaEncoder::finish() {
    if (finished_) {
        return failedPrecondition("encoder was already finished");
    }
    if (!backend_) {
        return failedPrecondition("encoder is not open");
    }
    // Mark finished first so no further frames are accepted regardless of the
    // finalize outcome.
    finished_ = true;
    return backend_->finish();
}

} // namespace palmier::media
