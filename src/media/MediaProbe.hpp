// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/MediaProbe.hpp — open a media file and extract its MediaInfo.
//
// This is the demux/probe entry point of the Media Engine (design.md
// "Component 3: Media Engine (FFmpeg)"): it opens a container with FFmpeg
// (libavformat/libavcodec), reads the stream layout, and returns a normalized
// MediaInfo (container format, streams, codecs, resolution, fps, durations,
// sample rate/channels). It performs no frame decoding — that is MediaDecoder
// (task 8.2); this covers only probing and import metadata.
//
// FFmpeg usage is compiled only when PALMIER_HAVE_FFMPEG is defined (set by the
// build when the libav* libraries are present). Where FFmpeg is absent the
// module still builds; the default backend then returns a FailedPrecondition
// error rather than a MediaInfo. A backend injection seam lets the probe and
// its normalization be exercised without FFmpeg, so the logic is testable on
// any platform.

#ifndef PALMIER_MEDIA_MEDIAPROBE_HPP
#define PALMIER_MEDIA_MEDIAPROBE_HPP

#include <filesystem>
#include <functional>

#include "core/Result.hpp"
#include "media/MediaInfo.hpp"

namespace palmier::media {

/// A pluggable probe implementation: given a path, produce a (possibly
/// un-normalized) MediaInfo or an Error. The public probe normalizes whatever a
/// backend returns. Injectable so tests can supply synthetic containers without
/// touching the filesystem or FFmpeg (mirrors the GPU layer's enumerator seam).
using MediaProbeBackend = std::function<Result<MediaInfo>(const std::filesystem::path&)>;

/// True iff this build was compiled against FFmpeg (PALMIER_HAVE_FFMPEG). When
/// false, the default backend cannot read files and probeMediaFile reports a
/// FailedPrecondition error.
[[nodiscard]] bool isFfmpegAvailable() noexcept;

/// The default, FFmpeg-backed probe backend. When FFmpeg is not compiled in it
/// returns an Error describing that probing is unavailable in this build.
[[nodiscard]] MediaProbeBackend ffmpegProbeBackend();

/// Probe `path` with the default (FFmpeg) backend and return a normalized
/// MediaInfo. Errors:
///   * InvalidArgument — the path is empty.
///   * NotFound / Io   — the file is missing or cannot be opened/read.
///   * Unsupported     — the container/format cannot be recognized.
///   * FailedPrecondition — this build has no FFmpeg support.
[[nodiscard]] Result<MediaInfo> probeMediaFile(const std::filesystem::path& path);

/// Probe `path` with an injected backend, then normalize the result. This is
/// the composition point the default overload builds on and the testing seam.
[[nodiscard]] Result<MediaInfo> probeMediaFile(const std::filesystem::path& path,
                                               const MediaProbeBackend& backend);

} // namespace palmier::media

#endif // PALMIER_MEDIA_MEDIAPROBE_HPP
