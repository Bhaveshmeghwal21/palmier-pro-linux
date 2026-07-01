// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaEncoder.cpp — MediaEncoder routing/fallback + FFmpeg encode backend.
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

#include <string>
#include <utility>

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

class FfmpegEncodeBackend final : public IEncodeBackend {
public:
    FfmpegEncodeBackend(AVFormatContext* fmt, AVCodecContext* codec, AVStream* stream)
        : fmt_(fmt), codec_(codec), stream_(stream) {
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

        // Presentation timestamp in the stream time base.
        frame_->pts = av_rescale_q(frame.presentation.nanoseconds(),
                                   AVRational{1, static_cast<int>(Duration::kTicksPerSecond)},
                                   stream_->time_base);

        return sendFrame(frame_);
    }

    [[nodiscard]] Result<void> finish() override {
        if (codec_ == nullptr) {
            return failedPrecondition("encoder is not open");
        }
        // Drain the encoder.
        Result<void> flushed = sendFrame(nullptr);
        if (flushed.isError()) return flushed;
        if (av_write_trailer(fmt_) < 0) {
            return makeError(ErrorCode::Io, "could not finalize the output file");
        }
        return ok();
    }

private:
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

    AVFormatContext* fmt_{nullptr};
    AVCodecContext*  codec_{nullptr};
    AVStream*        stream_{nullptr};
    AVPacket*        packet_{nullptr};
    AVFrame*         frame_{nullptr};
    SwsContext*      sws_{nullptr};
};

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

    if ((fmt->oformat->flags & AVFMT_NOFILE) == 0) {
        if (avio_open(&fmt->pb, spec.outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
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
        avcodec_free_context(&codec);
        avformat_free_context(fmt);
        return err<std::unique_ptr<IEncodeBackend>>(
            makeError(ErrorCode::Io, "could not write the output header"));
    }

    return std::unique_ptr<IEncodeBackend>(
        std::make_unique<FfmpegEncodeBackend>(fmt, codec, stream));
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
