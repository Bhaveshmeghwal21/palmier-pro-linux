// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaProbe.cpp — FFmpeg-backed container probing (see MediaProbe.hpp).
//
// The libavformat/libavcodec calls are compiled only when PALMIER_HAVE_FFMPEG
// is defined; otherwise the default backend reports that probing is
// unavailable in this build. This mirrors the GPU layer's PALMIER_HAVE_VULKAN
// pattern so the media module builds and its normalization/classification logic
// stays testable on machines without FFmpeg (e.g. CI/sandbox).

#include "media/MediaProbe.hpp"

#include <cstdint>
#include <system_error>
#include <utility>

#if defined(PALMIER_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}
#endif

namespace palmier::media {

// ---------------------------------------------------------------------------
// FFmpeg-backed extraction (compiled only when the libav* headers are present).
// ---------------------------------------------------------------------------
#if defined(PALMIER_HAVE_FFMPEG)

namespace {

/// Map an FFmpeg media type to the engine's stream type.
[[nodiscard]] MediaStreamType mapStreamType(AVMediaType t) noexcept {
    switch (t) {
        case AVMEDIA_TYPE_VIDEO:      return MediaStreamType::Video;
        case AVMEDIA_TYPE_AUDIO:      return MediaStreamType::Audio;
        case AVMEDIA_TYPE_SUBTITLE:   return MediaStreamType::Subtitle;
        case AVMEDIA_TYPE_ATTACHMENT: return MediaStreamType::Attachment;
        case AVMEDIA_TYPE_DATA:       return MediaStreamType::Data;
        default:                      return MediaStreamType::Unknown;
    }
}

/// Convert an FFmpeg time-base-scaled stream duration to a Duration.
[[nodiscard]] Duration streamDuration(const AVStream* stream) noexcept {
    if (stream == nullptr || stream->duration == AV_NOPTS_VALUE) return Duration::zero();
    const double seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    if (seconds <= 0.0) return Duration::zero();
    return Duration::fromSeconds(seconds);
}

/// Choose the most reliable frame rate FFmpeg exposes for a video stream.
[[nodiscard]] FrameRate videoFrameRate(const AVStream* stream) noexcept {
    if (stream == nullptr) return FrameRate{};
    AVRational r = stream->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = stream->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) return FrameRate{};
    return FrameRate{r.num, r.den};
}

/// Number of audio channels, tolerant of the FFmpeg channel-layout API change
/// (ch_layout arrived in libavcodec 59; older releases expose `channels`).
[[nodiscard]] int audioChannels(const AVCodecParameters* par) noexcept {
    if (par == nullptr) return 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
    return par->ch_layout.nb_channels;
#else
    return par->channels;
#endif
}

/// Populate a MediaStreamInfo from one AVStream.
[[nodiscard]] MediaStreamInfo describeStream(const AVStream* stream) {
    MediaStreamInfo info;
    info.index = stream->index;

    const AVCodecParameters* par = stream->codecpar;
    if (par != nullptr) {
        info.type = mapStreamType(par->codec_type);

        if (const char* name = avcodec_get_name(par->codec_id); name != nullptr) {
            info.codecName = name;
            info.codec = mediaCodecFromName(info.codecName);
        }

        if (par->bit_rate > 0) info.bitRate = par->bit_rate;

        if (info.type == MediaStreamType::Video) {
            if (par->width > 0 && par->height > 0) {
                info.resolution = Resolution{static_cast<std::uint32_t>(par->width),
                                             static_cast<std::uint32_t>(par->height)};
            }
            info.frameRate = videoFrameRate(stream);
        } else if (info.type == MediaStreamType::Audio) {
            info.sampleRate = par->sample_rate;
            info.channels = audioChannels(par);
        }
    }

    info.duration = streamDuration(stream);
    return info;
}

/// Open the container, read stream info, and build a MediaInfo.
[[nodiscard]] Result<MediaInfo> probeWithFfmpeg(const std::filesystem::path& path) {
    AVFormatContext* ctx = nullptr;
    const int openRc = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
    if (openRc < 0) {
        // Could not open/read the container. Distinguishing "unsupported format"
        // from "unreadable contents" is import-validation's job (task 8.5); here
        // we surface a single clear read failure.
        return err<MediaInfo>(makeError(
            ErrorCode::Io, "could not open media file: " + path.string()));
    }

    // Ensure the context is always freed, on every return path below.
    struct Closer {
        AVFormatContext* c;
        ~Closer() { if (c) avformat_close_input(&c); }
    } closer{ctx};

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        return err<MediaInfo>(makeError(
            ErrorCode::Io, "could not read stream information from: " + path.string()));
    }

    MediaInfo media;
    if (ctx->iformat != nullptr) {
        if (ctx->iformat->name != nullptr) media.containerFormat = ctx->iformat->name;
        if (ctx->iformat->long_name != nullptr) media.containerLongName = ctx->iformat->long_name;
    }
    if (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0) {
        media.duration = Duration::fromMicroseconds(ctx->duration);
    }
    if (ctx->bit_rate > 0) media.bitRate = ctx->bit_rate;

    media.streams.reserve(ctx->nb_streams);
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        media.streams.push_back(describeStream(ctx->streams[i]));
    }

    return media;
}

} // namespace

bool isFfmpegAvailable() noexcept { return true; }

MediaProbeBackend ffmpegProbeBackend() {
    return [](const std::filesystem::path& path) { return probeWithFfmpeg(path); };
}

#else // !PALMIER_HAVE_FFMPEG

bool isFfmpegAvailable() noexcept { return false; }

MediaProbeBackend ffmpegProbeBackend() {
    return [](const std::filesystem::path&) -> Result<MediaInfo> {
        return err<MediaInfo>(makeError(
            ErrorCode::FailedPrecondition,
            "media probing requires FFmpeg, which was not compiled into this build"));
    };
}

#endif // PALMIER_HAVE_FFMPEG

// ---------------------------------------------------------------------------
// Public entry points (backend-agnostic; normalization applies uniformly).
// ---------------------------------------------------------------------------

Result<MediaInfo> probeMediaFile(const std::filesystem::path& path,
                                 const MediaProbeBackend& backend) {
    if (path.empty()) {
        return err<MediaInfo>(invalidArgument("media path must not be empty"));
    }
    if (!backend) {
        return err<MediaInfo>(makeError(ErrorCode::Internal, "no media probe backend provided"));
    }

    Result<MediaInfo> probed = backend(path);
    if (probed.isError()) return probed;

    MediaInfo info = std::move(probed).value();
    normalize(info);
    return info;
}

Result<MediaInfo> probeMediaFile(const std::filesystem::path& path) {
    return probeMediaFile(path, ffmpegProbeBackend());
}

} // namespace palmier::media
