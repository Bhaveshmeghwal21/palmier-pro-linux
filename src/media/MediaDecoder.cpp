// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaDecoder.cpp — MediaDecoder routing/fallback + FFmpeg decode backend.
//
// The MediaDecoder logic (open/nextFrame/seek and the HW-preferred-with-SW-
// fallback routing through the GPU CodecBridge) is backend-agnostic and always
// compiled: it drives whatever IDecodeBackend the factory produced. The concrete
// FFmpeg (libavformat/libavcodec/libswscale) backend is compiled only when
// PALMIER_HAVE_FFMPEG is defined; otherwise the default factory returns a
// FailedPrecondition error, mirroring MediaProbe's guard so the module builds and
// its routing/fallback logic stays testable on machines without FFmpeg.

#include "media/MediaDecoder.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "media/MediaProbe.hpp"

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace palmier::media {

// ---------------------------------------------------------------------------
// Codec identity bridge (media -> GPU layer)
// ---------------------------------------------------------------------------

gpu::CodecId toGpuCodec(MediaCodecId codec) noexcept {
    switch (codec) {
        case MediaCodecId::H264:       return gpu::CodecId::H264;
        case MediaCodecId::HEVC:       return gpu::CodecId::HEVC;
        case MediaCodecId::AV1:        return gpu::CodecId::AV1;
        case MediaCodecId::VP9:        return gpu::CodecId::VP9;
        case MediaCodecId::Mpeg2Video: return gpu::CodecId::MPEG2;
        default:                       return gpu::CodecId::Unknown;
    }
}

// ---------------------------------------------------------------------------
// FFmpeg-backed decode backend (compiled only with PALMIER_HAVE_FFMPEG).
// ---------------------------------------------------------------------------
//
// This backend performs software decode (libavcodec) and converts each frame to
// packed RGBA8 (libswscale) into a host-memory buffer, i.e. it produces CPU
// BackendFrames. That is the always-correct fallback path and satisfies the
// decode/parity requirements for any build.
//
// Zero-copy hardware-surface export (adopting a VAAPI DMA-BUF fd or an NVDEC CUDA
// pointer as a BackendFrame with hardware == true, which MediaDecoder then
// imports into the FramePool) is layered on top of this backend where a vendor
// hardware path is compiled in (PALMIER_HAVE_VAAPI / PALMIER_HAVE_NVENC /
// PALMIER_HAVE_QSV); until then this backend reports CPU frames and the
// MediaDecoder routing transparently uses the software path. The HW-preferred /
// SW-fallback routing itself (Requirements 10.2/10.5) lives in MediaDecoder and
// is exercised end-to-end via the mock backend in the unit tests.
#if defined(PALMIER_HAVE_FFMPEG)

namespace {

class FfmpegDecodeBackend final : public IDecodeBackend {
public:
    FfmpegDecodeBackend(MediaInfo info, AVFormatContext* fmt, AVCodecContext* codec,
                        int videoStreamIndex, std::filesystem::path path)
        : info_(std::move(info)),
          fmt_(fmt),
          codec_(codec),
          videoStreamIndex_(videoStreamIndex),
          path_(std::move(path)) {
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
    }

    ~FfmpegDecodeBackend() override {
        closeAudio();
        if (sws_ != nullptr) sws_freeContext(sws_);
        if (frame_ != nullptr) av_frame_free(&frame_);
        if (packet_ != nullptr) av_packet_free(&packet_);
        if (codec_ != nullptr) avcodec_free_context(&codec_);
        if (fmt_ != nullptr) avformat_close_input(&fmt_);
    }

    FfmpegDecodeBackend(const FfmpegDecodeBackend&) = delete;
    FfmpegDecodeBackend& operator=(const FfmpegDecodeBackend&) = delete;

    [[nodiscard]] const MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<BackendFrame> decode(bool /*useHardware*/) override {
        if (codec_ == nullptr || frame_ == nullptr || packet_ == nullptr) {
            return err<BackendFrame>(makeError(ErrorCode::Internal,
                                               "decoder was not initialized"));
        }

        for (;;) {
            const int recv = avcodec_receive_frame(codec_, frame_);
            if (recv == 0) {
                Result<BackendFrame> out = convertCurrentFrame();
                av_frame_unref(frame_);
                return out;
            }
            if (recv == AVERROR_EOF) {
                return BackendFrame::eos();
            }
            if (recv != AVERROR(EAGAIN)) {
                return err<BackendFrame>(makeError(ErrorCode::Io,
                                                   "frame decode failed"));
            }

            // EAGAIN: the decoder needs more input. Feed the next video packet,
            // or signal end-of-input to drain buffered frames.
            const int read = av_read_frame(fmt_, packet_);
            if (read == AVERROR_EOF) {
                avcodec_send_packet(codec_, nullptr); // enter drain mode
                continue;
            }
            if (read < 0) {
                return err<BackendFrame>(makeError(ErrorCode::Io,
                                                   "reading the next packet failed"));
            }

            if (packet_->stream_index == videoStreamIndex_) {
                const int sent = avcodec_send_packet(codec_, packet_);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    av_packet_unref(packet_);
                    return err<BackendFrame>(makeError(ErrorCode::Io,
                                                       "submitting a packet to the decoder failed"));
                }
            }
            av_packet_unref(packet_);
        }
    }

    [[nodiscard]] Result<void> seek(Duration ts) override {
        if (fmt_ == nullptr || codec_ == nullptr) {
            return failedPrecondition("decoder is not open");
        }
        const std::int64_t target = av_rescale_q(
            ts.nanoseconds(), AVRational{1, static_cast<int>(Duration::kTicksPerSecond)},
            fmt_->streams[videoStreamIndex_]->time_base);
        if (av_seek_frame(fmt_, videoStreamIndex_, target, AVSEEK_FLAG_BACKWARD) < 0) {
            return makeError(ErrorCode::Io, "seek failed");
        }
        avcodec_flush_buffers(codec_);
        return ok();
    }

    // --- Audio (task 8.1; Requirement 6.1) ---------------------------------
    //
    // The audio path deliberately demuxes the file through its OWN
    // AVFormatContext rather than sharing the video one. Sharing a demuxer would
    // make the two read loops steal each other's packets (the video loop discards
    // every non-video packet it reads and vice versa), so a caller decoding both
    // streams would silently lose data. A second context costs one file handle
    // and keeps video and audio independently seekable, which is exactly what
    // MediaDecoder::seek / seekAudio promise.
    //
    // Conversion is libswresample into interleaved 32-bit float at the stream's
    // OWN sample rate and channel count — the AudioBuffer AudioGraph consumes.
    // Rate/layout conversion to the engine output format belongs to AudioGraph,
    // so the decoder never resamples twice. Audio decode is always software:
    // no CodecBridge, no hardware route.

    [[nodiscard]] Result<BackendAudioFrame> decodeAudio(int streamIndex) override {
        if (Result<void> ready = ensureAudioOpen(streamIndex); ready.isError()) {
            return err<BackendAudioFrame>(std::move(ready).error());
        }

        for (;;) {
            const int recv = avcodec_receive_frame(audioCodec_, audioFrame_);
            if (recv == 0) {
                Result<BackendAudioFrame> out = convertCurrentAudioFrame();
                av_frame_unref(audioFrame_);
                return out;
            }
            if (recv == AVERROR_EOF) {
                return BackendAudioFrame::eos();
            }
            if (recv != AVERROR(EAGAIN)) {
                return err<BackendAudioFrame>(
                    makeError(ErrorCode::Io, "audio frame decode failed"));
            }

            const int read = av_read_frame(audioFmt_, audioPacket_);
            if (read == AVERROR_EOF) {
                avcodec_send_packet(audioCodec_, nullptr); // drain
                continue;
            }
            if (read < 0) {
                return err<BackendAudioFrame>(
                    makeError(ErrorCode::Io, "reading the next audio packet failed"));
            }

            if (audioPacket_->stream_index == audioStreamIndex_) {
                const int sent = avcodec_send_packet(audioCodec_, audioPacket_);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    av_packet_unref(audioPacket_);
                    return err<BackendAudioFrame>(makeError(
                        ErrorCode::Io, "submitting a packet to the audio decoder failed"));
                }
            }
            av_packet_unref(audioPacket_);
        }
    }

    [[nodiscard]] Result<void> seekAudio(Duration ts, int streamIndex) override {
        if (Result<void> ready = ensureAudioOpen(streamIndex); ready.isError()) {
            return ready;
        }
        const std::int64_t target = av_rescale_q(
            ts.nanoseconds(), AVRational{1, static_cast<int>(Duration::kTicksPerSecond)},
            audioFmt_->streams[audioStreamIndex_]->time_base);
        if (av_seek_frame(audioFmt_, audioStreamIndex_, target, AVSEEK_FLAG_BACKWARD) < 0) {
            return makeError(ErrorCode::Io, "audio seek failed");
        }
        avcodec_flush_buffers(audioCodec_);
        return ok();
    }

private:
    [[nodiscard]] Result<void> ensureAudioOpen(int streamIndex) {
        if (audioCodec_ != nullptr && (streamIndex < 0 || streamIndex == audioStreamIndex_)) {
            return ok();
        }
        if (audioCodec_ != nullptr) closeAudio(); // a different stream was requested

        if (avformat_open_input(&audioFmt_, path_.c_str(), nullptr, nullptr) < 0) {
            audioFmt_ = nullptr;
            return makeError(ErrorCode::Io,
                             "could not open media file for audio decode: " + path_.string());
        }
        if (avformat_find_stream_info(audioFmt_, nullptr) < 0) {
            closeAudio();
            return makeError(ErrorCode::Io,
                             "could not read stream information for audio decode from: " +
                                 path_.string());
        }

        const AVCodec* decoder = nullptr;
        int resolved = streamIndex;
        if (resolved < 0) {
            resolved = av_find_best_stream(audioFmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
            if (resolved < 0) {
                closeAudio();
                return makeError(ErrorCode::Unsupported,
                                 "no decodable audio stream found in: " + path_.string());
            }
        } else {
            if (resolved >= static_cast<int>(audioFmt_->nb_streams) ||
                audioFmt_->streams[resolved]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                closeAudio();
                return invalidArgument("stream " + std::to_string(resolved) +
                                       " is not an audio stream in: " + path_.string());
            }
            decoder = avcodec_find_decoder(audioFmt_->streams[resolved]->codecpar->codec_id);
            if (decoder == nullptr) {
                closeAudio();
                return makeError(ErrorCode::Unsupported,
                                 "no decoder is available for audio stream " +
                                     std::to_string(resolved) + " of: " + path_.string());
            }
        }

        audioCodec_ = avcodec_alloc_context3(decoder);
        if (audioCodec_ == nullptr) {
            closeAudio();
            return makeError(ErrorCode::Internal,
                             "could not allocate an audio decoder context");
        }
        if (avcodec_parameters_to_context(audioCodec_,
                                          audioFmt_->streams[resolved]->codecpar) < 0 ||
            avcodec_open2(audioCodec_, decoder, nullptr) < 0) {
            closeAudio();
            return makeError(ErrorCode::Io, "could not open the audio decoder");
        }

        audioPacket_ = av_packet_alloc();
        audioFrame_ = av_frame_alloc();
        if (audioPacket_ == nullptr || audioFrame_ == nullptr) {
            closeAudio();
            return makeError(ErrorCode::Internal, "could not allocate audio decode buffers");
        }

        audioStreamIndex_ = resolved;
        return ok();
    }

    /// Convert the AVFrame currently held in audioFrame_ into an interleaved
    /// float AudioBuffer at the stream's own rate and channel count.
    [[nodiscard]] Result<BackendAudioFrame> convertCurrentAudioFrame() {
        const int channels = audioFrame_->ch_layout.nb_channels > 0
                                 ? audioFrame_->ch_layout.nb_channels
                                 : audioCodec_->ch_layout.nb_channels;
        const int rate =
            audioFrame_->sample_rate > 0 ? audioFrame_->sample_rate : audioCodec_->sample_rate;
        const int inFrames = audioFrame_->nb_samples;
        if (channels <= 0 || rate <= 0) {
            return err<BackendAudioFrame>(
                makeError(ErrorCode::Io, "decoded audio frame declares no format"));
        }

        if (Result<void> swr = ensureResampler(channels, rate); swr.isError()) {
            return err<BackendAudioFrame>(std::move(swr).error());
        }

        const std::size_t ch = static_cast<std::size_t>(channels);
        std::vector<float> samples;
        std::size_t produced = 0;
        if (inFrames > 0) {
            const int capacity = swr_get_out_samples(swr_, inFrames);
            const std::size_t capacityFrames =
                std::max<std::size_t>(capacity > 0 ? static_cast<std::size_t>(capacity) : 0,
                                      static_cast<std::size_t>(inFrames)) +
                8;
            samples.assign(capacityFrames * ch, 0.0f);

            std::uint8_t* outData[1] = {reinterpret_cast<std::uint8_t*>(samples.data())};
            const std::uint8_t** inData =
                const_cast<const std::uint8_t**>(audioFrame_->extended_data);
            const int got =
                swr_convert(swr_, outData, static_cast<int>(capacityFrames), inData, inFrames);
            if (got < 0) {
                return err<BackendAudioFrame>(
                    makeError(ErrorCode::Internal, "audio sample conversion failed"));
            }
            produced = static_cast<std::size_t>(got);
            samples.resize(produced * ch);
        }

        BackendAudioFrame out;
        out.endOfStream = false;
        const std::int64_t pts = (audioFrame_->best_effort_timestamp != AV_NOPTS_VALUE)
                                     ? audioFrame_->best_effort_timestamp
                                     : 0;
        const AVRational tb = audioFmt_->streams[audioStreamIndex_]->time_base;
        out.timestamp = Duration::fromSeconds(static_cast<double>(pts) * av_q2d(tb));
        out.buffer = AudioBuffer::interleaved(rate, channels, std::move(samples));
        return out;
    }

    /// Build (or rebuild, on a format change mid-stream) the libswresample
    /// context converting the decoder's native layout to interleaved float at the
    /// SAME rate and channel count.
    [[nodiscard]] Result<void> ensureResampler(int channels, int rate) {
        const auto inFormat = static_cast<AVSampleFormat>(audioFrame_->format);
        if (swr_ != nullptr && swrChannels_ == channels && swrRate_ == rate &&
            swrInFormat_ == inFormat) {
            return ok();
        }
        if (swr_ != nullptr) swr_free(&swr_);

        swr_ = swr_alloc();
        if (swr_ == nullptr) {
            return makeError(ErrorCode::Internal,
                             "could not allocate an audio conversion context");
        }

        AVChannelLayout inLayout;
        AVChannelLayout outLayout;
        if (audioFrame_->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inLayout, &audioFrame_->ch_layout);
        } else {
            av_channel_layout_default(&inLayout, channels);
        }
        av_channel_layout_default(&outLayout, channels);

        av_opt_set_chlayout(swr_, "in_chlayout", &inLayout, 0);
        av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr_, "in_sample_rate", rate, 0);
        av_opt_set_int(swr_, "out_sample_rate", rate, 0);
        av_opt_set_sample_fmt(swr_, "in_sample_fmt", inFormat, 0);
        av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        if (swr_init(swr_) < 0) {
            swr_free(&swr_);
            return makeError(ErrorCode::Internal,
                             "could not initialize the audio sample converter");
        }
        swrChannels_ = channels;
        swrRate_ = rate;
        swrInFormat_ = inFormat;
        return ok();
    }

    void closeAudio() {
        if (swr_ != nullptr) swr_free(&swr_);
        if (audioFrame_ != nullptr) av_frame_free(&audioFrame_);
        if (audioPacket_ != nullptr) av_packet_free(&audioPacket_);
        if (audioCodec_ != nullptr) avcodec_free_context(&audioCodec_);
        if (audioFmt_ != nullptr) avformat_close_input(&audioFmt_);
        audioStreamIndex_ = -1;
        swrChannels_ = 0;
        swrRate_ = 0;
        swrInFormat_ = AV_SAMPLE_FMT_NONE;
    }

    [[nodiscard]] Result<BackendFrame> convertCurrentFrame() {
        const int width = frame_->width;
        const int height = frame_->height;
        if (width <= 0 || height <= 0) {
            return err<BackendFrame>(makeError(ErrorCode::Io, "decoded frame has no dimensions"));
        }

        sws_ = sws_getCachedContext(sws_, width, height,
                                    static_cast<AVPixelFormat>(frame_->format), width, height,
                                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) {
            return err<BackendFrame>(makeError(ErrorCode::Internal,
                                               "could not create pixel converter"));
        }

        BackendFrame out;
        out.hardware = false;
        out.desc = gpu::FrameDesc{static_cast<std::uint32_t>(width),
                                  static_cast<std::uint32_t>(height), gpu::FrameFormat::RGBA8};
        out.cpuPixels.resize(out.desc.byteSize());

        std::uint8_t* dstData[4] = {reinterpret_cast<std::uint8_t*>(out.cpuPixels.data()),
                                    nullptr, nullptr, nullptr};
        int dstStride[4] = {width * 4, 0, 0, 0};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, height, dstData, dstStride);

        const std::int64_t pts =
            (frame_->best_effort_timestamp != AV_NOPTS_VALUE) ? frame_->best_effort_timestamp : 0;
        const AVRational tb = fmt_->streams[videoStreamIndex_]->time_base;
        out.timestamp = Duration::fromSeconds(static_cast<double>(pts) * av_q2d(tb));
        return out;
    }

    MediaInfo       info_;
    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_{nullptr};
    AVPacket*        packet_{nullptr};
    AVFrame*         frame_{nullptr};
    SwsContext*      sws_{nullptr};
    int              videoStreamIndex_{-1};

    // Audio: an independent demuxer/decoder/converter opened lazily on the first
    // decodeAudio() or seekAudio() call.
    std::filesystem::path path_{};
    AVFormatContext*      audioFmt_{nullptr};
    AVCodecContext*       audioCodec_{nullptr};
    AVPacket*             audioPacket_{nullptr};
    AVFrame*              audioFrame_{nullptr};
    SwrContext*           swr_{nullptr};
    int                   audioStreamIndex_{-1};
    int                   swrChannels_{0};
    int                   swrRate_{0};
    AVSampleFormat        swrInFormat_{AV_SAMPLE_FMT_NONE};
};

[[nodiscard]] Result<std::unique_ptr<IDecodeBackend>> openFfmpegBackend(
    const std::filesystem::path& path, const DecodePrefs& /*prefs*/) {
    // Reuse the tested probe to build the normalized MediaInfo.
    Result<MediaInfo> probed = probeMediaFile(path);
    if (probed.isError()) return err<std::unique_ptr<IDecodeBackend>>(std::move(probed).error());
    MediaInfo info = std::move(probed).value();

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Io, "could not open media file: " + path.string()));
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Io, "could not read stream information from: " + path.string()));
    }

    const AVCodec* decoder = nullptr;
    const int streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (streamIndex < 0 || decoder == nullptr) {
        avformat_close_input(&fmt);
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Unsupported, "no decodable video stream found in: " + path.string()));
    }

    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    if (codec == nullptr) {
        avformat_close_input(&fmt);
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Internal, "could not allocate a decoder context"));
    }
    if (avcodec_parameters_to_context(codec, fmt->streams[streamIndex]->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
        return err<std::unique_ptr<IDecodeBackend>>(
            makeError(ErrorCode::Io, "could not open the video decoder"));
    }

    return std::unique_ptr<IDecodeBackend>(
        std::make_unique<FfmpegDecodeBackend>(std::move(info), fmt, codec, streamIndex, path));
}

} // namespace

bool isFfmpegDecodeAvailable() noexcept { return true; }

DecodeBackendFactory ffmpegDecodeBackendFactory() {
    return [](const std::filesystem::path& path, const DecodePrefs& prefs) {
        return openFfmpegBackend(path, prefs);
    };
}

#else // !PALMIER_HAVE_FFMPEG

bool isFfmpegDecodeAvailable() noexcept { return false; }

DecodeBackendFactory ffmpegDecodeBackendFactory() {
    return [](const std::filesystem::path&, const DecodePrefs&)
               -> Result<std::unique_ptr<IDecodeBackend>> {
        return err<std::unique_ptr<IDecodeBackend>>(makeError(
            ErrorCode::FailedPrecondition,
            "media decoding requires FFmpeg, which was not compiled into this build"));
    };
}

#endif // PALMIER_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// MediaDecoder — backend-agnostic routing + zero-copy/fallback policy.
// ---------------------------------------------------------------------------

MediaDecoder::MediaDecoder(MediaInfo info, std::unique_ptr<IDecodeBackend> backend, DecodePrefs prefs)
    : info_(std::move(info)),
      backend_(std::move(backend)),
      prefs_(std::move(prefs)),
      // When the caller opts out of hardware, route against the software profile
      // so every frame takes the CPU path (Requirement 10.4 fallback lane).
      bridge_(prefs_.preferHardware ? prefs_.caps : gpu::GpuCaps::software(),
              prefs_.availability) {
    if (const MediaStreamInfo* video = info_.primaryVideoStream(); video != nullptr) {
        videoCodec_ = toGpuCodec(video->codec);
    }
}

Result<MediaDecoder> MediaDecoder::open(const std::filesystem::path& path, DecodePrefs prefs,
                                        const DecodeBackendFactory& factory) {
    if (path.empty()) {
        return err<MediaDecoder>(invalidArgument("media path must not be empty"));
    }
    if (!factory) {
        return err<MediaDecoder>(makeError(ErrorCode::Internal, "no decode backend factory provided"));
    }

    Result<std::unique_ptr<IDecodeBackend>> backend = factory(path, prefs);
    if (backend.isError()) return err<MediaDecoder>(std::move(backend).error());

    std::unique_ptr<IDecodeBackend> impl = std::move(backend).value();
    if (!impl) {
        return err<MediaDecoder>(makeError(ErrorCode::Internal, "decode backend factory returned null"));
    }

    MediaInfo info = impl->info();
    normalize(info);
    return MediaDecoder(std::move(info), std::move(impl), std::move(prefs));
}

Result<MediaDecoder> MediaDecoder::open(const std::filesystem::path& path, DecodePrefs prefs) {
    return open(path, std::move(prefs), ffmpegDecodeBackendFactory());
}

Result<DecodedFrame> MediaDecoder::nextFrame() {
    if (!backend_) {
        return err<DecodedFrame>(failedPrecondition("decoder is not open"));
    }
    lastRetriedOnCpu_ = false;

    // The bridge runs this on the routed backend and, on a hardware failure,
    // retries it exactly once on the CPU path. `pending` is only ever set on a
    // fully-successful attempt, so a failed attempt leaves no partial frame
    // (Requirement 10.5: preserve inputs, lose no data).
    std::optional<DecodedFrame> pending;
    gpu::CodecOpFn op = [&](const gpu::CodecRoute& route) -> Result<void> {
        Result<BackendFrame> decoded = backend_->decode(route.hardware);
        if (decoded.isError()) return decoded.error();
        BackendFrame frame = std::move(decoded).value();

        if (frame.endOfStream) {
            pending = DecodedFrame::endOfStream();
            return ok();
        }

        // Hardware route + a hardware surface -> adopt it into the pool zero-copy.
        if (route.hardware && frame.hardware) {
            if (prefs_.framePool == nullptr) {
                // No pool to adopt the surface into: treat as a hardware failure so
                // the bridge falls back to the CPU path transparently.
                return failedPrecondition(
                    "hardware decode requires a frame pool for zero-copy frames");
            }
            Result<gpu::FrameLease> lease =
                prefs_.framePool->acquireImported(frame.desc, frame.external);
            if (lease.isError()) return lease.error(); // GPU-side failure -> CPU retry
            pending = DecodedFrame::gpu(frame.timestamp, std::move(lease).value());
            return ok();
        }

        // Software frame (either a CPU route or a hardware route the backend
        // satisfied in software).
        pending = DecodedFrame::cpu(frame.timestamp, frame.desc, std::move(frame.cpuPixels));
        return ok();
    };

    gpu::CodecExecution exec = bridge_.execute(videoCodec_, gpu::CodecOperation::Decode, op);
    lastRetriedOnCpu_ = exec.retriedOnCpu;

    if (exec.result.isError()) {
        return err<DecodedFrame>(std::move(exec.result).error());
    }
    if (!pending.has_value()) {
        return err<DecodedFrame>(
            makeError(ErrorCode::Internal, "decode reported success but produced no frame"));
    }
    return Result<DecodedFrame>(std::move(*pending));
}

Result<void> MediaDecoder::seek(Duration ts) {
    if (!backend_) {
        return failedPrecondition("decoder is not open");
    }
    return backend_->seek(ts);
}

// ---------------------------------------------------------------------------
// MediaDecoder — audio surface (task 8.1; Requirement 6.1)
// ---------------------------------------------------------------------------
//
// The decoder is the component that MAKES Requirement 6.1's declared ranges true,
// rather than merely hoping the backend honours them:
//
//   * openAudioStream refuses a stream whose declared sample rate or channel
//     count is outside 8 000-192 000 Hz / 1-8 channels, naming the offending
//     value. Nothing out of range is ever opened, so nothing out of range is ever
//     emitted.
//   * nextAudioFrame re-checks the buffer the backend actually produced (a
//     backend may resample or downmix) and reports an error instead of handing
//     back a non-conforming buffer.
//   * nextAudioFrame raises a regressing backend timestamp to the previous
//     frame's, so presentation timestamps are non-decreasing across consecutive
//     buffers of the stream. Clamping (rather than dropping) keeps every decoded
//     sample, which matters because the audio engine mixes these buffers.
//
// Audio decode is always software: no CodecBridge, no hardware route, no retry.

Result<void> MediaDecoder::openAudioStream(int streamIndex) {
    if (!backend_) {
        return failedPrecondition("decoder is not open");
    }

    const MediaStreamInfo* stream = nullptr;
    if (streamIndex < 0) {
        stream = info_.primaryAudioStream();
        if (stream == nullptr) {
            return failedPrecondition("this source carries no audio stream");
        }
    } else {
        for (const auto& s : info_.streams) {
            if (s.index == streamIndex) {
                stream = &s;
                break;
            }
        }
        if (stream == nullptr) {
            return invalidArgument("no stream with index " + std::to_string(streamIndex) +
                                   " exists in this source");
        }
        if (!stream->isAudio()) {
            return invalidArgument("stream " + std::to_string(streamIndex) +
                                   " is not an audio stream");
        }
    }

    // A stream may legitimately not declare its parameters (0 means "unknown");
    // in that case the per-buffer check in nextAudioFrame is the enforcement
    // point. A stream that DOES declare an out-of-range value is refused here.
    if (stream->sampleRate != 0 && !isDeclaredAudioSampleRate(stream->sampleRate)) {
        return makeError(ErrorCode::Unsupported,
                         "audio stream sample rate " + std::to_string(stream->sampleRate) +
                             " Hz is outside the supported range of " +
                             std::to_string(kMinAudioSampleRate) + " to " +
                             std::to_string(kMaxAudioSampleRate) + " Hz");
    }
    if (stream->channels != 0 && !isDeclaredAudioChannelCount(stream->channels)) {
        return makeError(ErrorCode::Unsupported,
                         "audio stream channel count " + std::to_string(stream->channels) +
                             " is outside the supported range of " +
                             std::to_string(kMinAudioChannels) + " to " +
                             std::to_string(kMaxAudioChannels));
    }

    audioStreamIndex_ = stream->index;
    lastAudioPresentation_.reset();
    return ok();
}

Result<AudioFrame> MediaDecoder::nextAudioFrame() {
    if (!backend_) {
        return err<AudioFrame>(failedPrecondition("decoder is not open"));
    }
    if (audioStreamIndex_ < 0) {
        return err<AudioFrame>(
            failedPrecondition("no audio stream is open; call openAudioStream first"));
    }

    Result<BackendAudioFrame> decoded = backend_->decodeAudio(audioStreamIndex_);
    if (decoded.isError()) return err<AudioFrame>(std::move(decoded).error());
    BackendAudioFrame block = std::move(decoded).value();

    if (block.endOfStream) {
        return AudioFrame::eos();
    }

    const int rate = block.buffer.sampleRate();
    const int channels = block.buffer.channels();
    if (!isDeclaredAudioSampleRate(rate)) {
        return err<AudioFrame>(makeError(
            ErrorCode::Unsupported,
            "decoded audio buffer declares a sample rate of " + std::to_string(rate) +
                " Hz, outside the supported range of " + std::to_string(kMinAudioSampleRate) +
                " to " + std::to_string(kMaxAudioSampleRate) + " Hz"));
    }
    if (!isDeclaredAudioChannelCount(channels)) {
        return err<AudioFrame>(makeError(
            ErrorCode::Unsupported,
            "decoded audio buffer declares " + std::to_string(channels) +
                " channels, outside the supported range of " +
                std::to_string(kMinAudioChannels) + " to " +
                std::to_string(kMaxAudioChannels)));
    }

    Duration presentation = block.timestamp;
    if (lastAudioPresentation_.has_value() && presentation < *lastAudioPresentation_) {
        presentation = *lastAudioPresentation_;
    }
    lastAudioPresentation_ = presentation;

    AudioFrame out;
    out.endOfStream = false;
    out.presentation = presentation;
    out.buffer = std::move(block.buffer);
    return out;
}

Result<void> MediaDecoder::seekAudio(Duration ts) {
    if (!backend_) {
        return failedPrecondition("decoder is not open");
    }
    if (audioStreamIndex_ < 0) {
        return failedPrecondition("no audio stream is open; call openAudioStream first");
    }
    if (Result<void> sought = backend_->seekAudio(ts, audioStreamIndex_); sought.isError()) {
        return sought;
    }
    // A seek begins a new monotonic run, so a backwards seek is not clamped
    // forward to the pre-seek position.
    lastAudioPresentation_.reset();
    return ok();
}

} // namespace palmier::media
