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

#include <cstdint>
#include <utility>

#include "media/MediaProbe.hpp"

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
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
                        int videoStreamIndex)
        : info_(std::move(info)),
          fmt_(fmt),
          codec_(codec),
          videoStreamIndex_(videoStreamIndex) {
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
    }

    ~FfmpegDecodeBackend() override {
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

private:
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
        std::make_unique<FfmpegDecodeBackend>(std::move(info), fmt, codec, streamIndex));
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

} // namespace palmier::media
