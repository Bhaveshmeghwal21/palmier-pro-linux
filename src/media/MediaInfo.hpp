// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaInfo.hpp — the normalized description of an imported media file.
//
// The Media Engine (design.md "Component 3: Media Engine (FFmpeg)") probes a
// container and produces a MediaInfo: the container format plus the list of
// elementary streams it carries, each described by a codec identity, type, and
// the type-appropriate parameters (resolution/frame rate for video; sample
// rate/channels for audio). MediaDecoder::open returns this so the timeline,
// UI, and export paths can reason about a source without re-opening it.
//
// This header is deliberately FFmpeg-free: the value types, the media-level
// codec identity, the codec-name mapping, the import-support classification,
// and the normalization pass all compile and are testable on machines with no
// FFmpeg (e.g. the CI/sandbox). Only the concrete probe (MediaProbe.cpp) links
// libavformat/libavcodec, and it does so behind the PALMIER_HAVE_FFMPEG guard.
//
// Codec identity note: the GPU layer (gpu/GpuTypes.hpp) defines its own minimal
// CodecId for the hardware bridge. The media module intentionally keeps its own
// richer, self-contained MediaCodecId (covering ProRes, VP8, PCM/AAC/Opus, etc.)
// so it carries no hard dependency on the GPU module. The two are mapped only
// where the decode/encode bridge is wired up (later tasks).

#ifndef PALMIER_MEDIA_MEDIAINFO_HPP
#define PALMIER_MEDIA_MEDIAINFO_HPP

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Resolution.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Stream type
// ---------------------------------------------------------------------------

/// The kind of an elementary stream within a container.
enum class MediaStreamType {
    Unknown = 0,
    Video,
    Audio,
    Subtitle,
    Data,
    Attachment,
};

[[nodiscard]] constexpr std::string_view toStringView(MediaStreamType t) noexcept {
    switch (t) {
        case MediaStreamType::Video:      return "video";
        case MediaStreamType::Audio:      return "audio";
        case MediaStreamType::Subtitle:   return "subtitle";
        case MediaStreamType::Data:       return "data";
        case MediaStreamType::Attachment: return "attachment";
        case MediaStreamType::Unknown:    return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Media-level codec identity
// ---------------------------------------------------------------------------

/// A richer, self-contained codec identity for the media engine. Values cover
/// the formats the design calls out for import normalization
/// (H.264/HEVC/AV1/ProRes-decode/VP9, PCM/AAC/Opus) plus the common companions
/// a heterogeneous library tends to contain. `codecName` on the stream retains
/// the raw decoder name for anything not enumerated here.
enum class MediaCodecId {
    Unknown = 0,

    // --- Video ---
    H264,
    HEVC,
    AV1,
    VP9,
    VP8,
    ProRes,
    Mpeg2Video,
    Mpeg4Part2,
    MJPEG,
    DNxHD,
    Theora,

    // --- Audio ---
    AAC,
    Opus,
    MP3,
    Vorbis,
    FLAC,
    AC3,
    EAC3,
    ALAC,
    Pcm,   ///< Linear PCM family (pcm_s16le, pcm_s24le, pcm_f32le, ...).
};

/// Stable, human-readable codec label.
[[nodiscard]] constexpr std::string_view codecName(MediaCodecId c) noexcept {
    switch (c) {
        case MediaCodecId::H264:       return "H.264";
        case MediaCodecId::HEVC:       return "HEVC";
        case MediaCodecId::AV1:        return "AV1";
        case MediaCodecId::VP9:        return "VP9";
        case MediaCodecId::VP8:        return "VP8";
        case MediaCodecId::ProRes:     return "ProRes";
        case MediaCodecId::Mpeg2Video: return "MPEG-2 Video";
        case MediaCodecId::Mpeg4Part2: return "MPEG-4 Part 2";
        case MediaCodecId::MJPEG:      return "Motion JPEG";
        case MediaCodecId::DNxHD:      return "DNxHD";
        case MediaCodecId::Theora:     return "Theora";
        case MediaCodecId::AAC:        return "AAC";
        case MediaCodecId::Opus:       return "Opus";
        case MediaCodecId::MP3:        return "MP3";
        case MediaCodecId::Vorbis:     return "Vorbis";
        case MediaCodecId::FLAC:       return "FLAC";
        case MediaCodecId::AC3:        return "AC-3";
        case MediaCodecId::EAC3:       return "E-AC-3";
        case MediaCodecId::ALAC:       return "ALAC";
        case MediaCodecId::Pcm:        return "PCM";
        case MediaCodecId::Unknown:    return "unknown";
    }
    return "unknown";
}

namespace detail {

/// ASCII-lowercase a single character (locale-independent).
[[nodiscard]] constexpr char asciiLower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// Case-insensitive equality for ASCII codec names.
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

/// Case-insensitive prefix test for ASCII codec names.
[[nodiscard]] inline bool istartsWith(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (asciiLower(s[i]) != asciiLower(prefix[i])) return false;
    }
    return true;
}

} // namespace detail

/// Map a raw decoder name (as FFmpeg reports via avcodec_get_name, e.g. "h264",
/// "hevc", "aac", "pcm_s16le") to a media-level codec identity. Unrecognized
/// names map to MediaCodecId::Unknown; the caller retains the raw name for
/// diagnostics and for naming an unsupported format to the user.
[[nodiscard]] inline MediaCodecId mediaCodecFromName(std::string_view name) noexcept {
    using detail::iequals;
    using detail::istartsWith;

    // Any linear-PCM variant collapses to the PCM family.
    if (istartsWith(name, "pcm_")) return MediaCodecId::Pcm;

    if (iequals(name, "h264") || iequals(name, "avc") || iequals(name, "avc1"))
        return MediaCodecId::H264;
    if (iequals(name, "hevc") || iequals(name, "h265") || iequals(name, "hvc1"))
        return MediaCodecId::HEVC;
    if (iequals(name, "av1")) return MediaCodecId::AV1;
    if (iequals(name, "vp9")) return MediaCodecId::VP9;
    if (iequals(name, "vp8")) return MediaCodecId::VP8;
    if (iequals(name, "prores")) return MediaCodecId::ProRes;
    if (iequals(name, "mpeg2video")) return MediaCodecId::Mpeg2Video;
    if (iequals(name, "mpeg4")) return MediaCodecId::Mpeg4Part2;
    if (iequals(name, "mjpeg")) return MediaCodecId::MJPEG;
    if (iequals(name, "dnxhd")) return MediaCodecId::DNxHD;
    if (iequals(name, "theora")) return MediaCodecId::Theora;

    if (iequals(name, "aac")) return MediaCodecId::AAC;
    if (iequals(name, "opus")) return MediaCodecId::Opus;
    if (iequals(name, "mp3")) return MediaCodecId::MP3;
    if (iequals(name, "vorbis")) return MediaCodecId::Vorbis;
    if (iequals(name, "flac")) return MediaCodecId::FLAC;
    if (iequals(name, "ac3")) return MediaCodecId::AC3;
    if (iequals(name, "eac3")) return MediaCodecId::EAC3;
    if (iequals(name, "alac")) return MediaCodecId::ALAC;

    return MediaCodecId::Unknown;
}

/// Whether the media engine supports importing (decoding) a codec. This is the
/// design's decode set: H.264, HEVC, AV1, VP9, ProRes (decode), and the PCM /
/// AAC / Opus audio codecs. The rejection UX for unsupported inputs is handled
/// by import validation (task 8.5); this predicate is the shared classification
/// the probe applies while normalizing heterogeneous inputs.
[[nodiscard]] constexpr bool isImportSupported(MediaCodecId c) noexcept {
    switch (c) {
        case MediaCodecId::H264:
        case MediaCodecId::HEVC:
        case MediaCodecId::AV1:
        case MediaCodecId::VP9:
        case MediaCodecId::ProRes:
        case MediaCodecId::AAC:
        case MediaCodecId::Opus:
        case MediaCodecId::Pcm:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Per-stream description
// ---------------------------------------------------------------------------

/// One elementary stream inside a container. Fields not applicable to a stream
/// type keep their zero/default value (e.g. `resolution`/`frameRate` are unset
/// for an audio stream; `sampleRate`/`channels` are 0 for a video stream).
struct MediaStreamInfo {
    int             index = -1;                    ///< Stream index within the container.
    MediaStreamType type = MediaStreamType::Unknown;
    MediaCodecId    codec = MediaCodecId::Unknown;
    std::string     codecName;                     ///< Raw decoder name (e.g. "h264").
    Duration        duration;                      ///< Stream duration (zero if unknown).
    std::int64_t    bitRate = 0;                   ///< Bits/second (0 if unknown).

    // --- Video-only ---
    Resolution      resolution;                    ///< Coded frame size (0x0 if N/A).
    FrameRate       frameRate;                     ///< Average frame rate (invalid if N/A).

    // --- Audio-only ---
    int             sampleRate = 0;                ///< Samples/second (0 if N/A).
    int             channels = 0;                  ///< Channel count (0 if N/A).

    [[nodiscard]] bool isVideo() const noexcept { return type == MediaStreamType::Video; }
    [[nodiscard]] bool isAudio() const noexcept { return type == MediaStreamType::Audio; }

    /// Whether this stream's codec is one the media engine can decode on import.
    [[nodiscard]] bool isSupported() const noexcept { return isImportSupported(codec); }
};

// ---------------------------------------------------------------------------
// Container description
// ---------------------------------------------------------------------------

/// The normalized result of probing a media file: the container format and its
/// elementary streams. Produced by MediaProbe / MediaDecoder::open.
struct MediaInfo {
    std::string  containerFormat;      ///< Demuxer short name (e.g. "mov,mp4,...").
    std::string  containerLongName;    ///< Human-readable format name.
    Duration     duration;             ///< Overall duration (zero if unknown).
    std::int64_t bitRate = 0;          ///< Overall bit rate in bits/second (0 if unknown).
    std::vector<MediaStreamInfo> streams;

    [[nodiscard]] std::size_t streamCount() const noexcept { return streams.size(); }

    [[nodiscard]] bool hasVideo() const noexcept {
        for (const auto& s : streams) {
            if (s.isVideo()) return true;
        }
        return false;
    }

    [[nodiscard]] bool hasAudio() const noexcept {
        for (const auto& s : streams) {
            if (s.isAudio()) return true;
        }
        return false;
    }

    /// The first video stream, or nullptr when the container carries none.
    [[nodiscard]] const MediaStreamInfo* primaryVideoStream() const noexcept {
        for (const auto& s : streams) {
            if (s.isVideo()) return &s;
        }
        return nullptr;
    }

    /// The first audio stream, or nullptr when the container carries none.
    [[nodiscard]] const MediaStreamInfo* primaryAudioStream() const noexcept {
        for (const auto& s : streams) {
            if (s.isAudio()) return &s;
        }
        return nullptr;
    }

    /// True when at least one stream carries a codec the engine can decode. Used
    /// by import validation (task 8.5) to decide acceptance.
    [[nodiscard]] bool hasSupportedStream() const noexcept {
        for (const auto& s : streams) {
            if (s.isSupported()) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

/// Normalize a freshly probed MediaInfo so heterogeneous inputs present a
/// uniform shape to the rest of the engine:
///   * When the container reports no overall duration, derive it from the
///     longest stream (a common case for raw/streamed inputs).
///   * Backfill each stream's media-level codec identity from its raw decoder
///     name when the backend left it Unknown.
/// The pass is idempotent and FFmpeg-independent, so it is unit-testable on any
/// platform and is applied uniformly regardless of which backend produced the
/// MediaInfo.
inline void normalize(MediaInfo& info) {
    for (auto& s : info.streams) {
        if (s.codec == MediaCodecId::Unknown && !s.codecName.empty()) {
            s.codec = mediaCodecFromName(s.codecName);
        }
    }

    if (info.duration.isZero() || info.duration.isNegative()) {
        Duration longest = Duration::zero();
        for (const auto& s : info.streams) {
            if (s.duration > longest) longest = s.duration;
        }
        info.duration = longest;
    }
}

} // namespace palmier::media

#endif // PALMIER_MEDIA_MEDIAINFO_HPP
