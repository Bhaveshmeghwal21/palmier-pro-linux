// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/ImportValidation.cpp — import acceptance/rejection policy (see header).
//
// The logic here is pure and FFmpeg-independent: it composes MediaProbe (via an
// injectable backend) and the classification helpers on MediaInfo
// (hasSupportedStream / codec identity), then maps the outcome onto the two
// distinct rejections Requirements 3.2 and 3.3 require. No filesystem or FFmpeg
// access happens except through the supplied probe backend.

#include "media/ImportValidation.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Error.hpp"

namespace palmier::media {

namespace {

/// The label the user should see for one stream's codec: the stable media-level
/// name when recognized, otherwise the raw decoder name the backend reported,
/// otherwise "unknown".
[[nodiscard]] std::string codecLabel(const MediaStreamInfo& s) {
    if (s.codec != MediaCodecId::Unknown) {
        return std::string(codecName(s.codec));
    }
    if (!s.codecName.empty()) {
        return s.codecName;
    }
    return "unknown";
}

} // namespace

std::string describeMediaFormat(const MediaInfo& info) {
    // Prefer the human-readable container name; fall back to the demuxer short
    // name; leave empty when the container is unknown.
    std::string container;
    if (!info.containerLongName.empty()) {
        container = info.containerLongName;
    } else if (!info.containerFormat.empty()) {
        container = info.containerFormat;
    }

    // Distinct codec labels, in first-seen order.
    std::vector<std::string> codecs;
    for (const auto& s : info.streams) {
        std::string label = codecLabel(s);
        if (std::find(codecs.begin(), codecs.end(), label) == codecs.end()) {
            codecs.push_back(std::move(label));
        }
    }

    std::string codecList;
    for (std::size_t i = 0; i < codecs.size(); ++i) {
        if (i != 0) codecList += ", ";
        codecList += codecs[i];
    }

    if (!container.empty() && !codecList.empty()) {
        return container + " (" + codecList + ")";
    }
    if (!container.empty()) {
        return container;
    }
    if (!codecList.empty()) {
        return codecList;
    }
    return "unknown format";
}

Result<MediaInfo> validateMediaImport(const std::filesystem::path& path,
                                      const MediaProbeBackend& backend) {
    Result<MediaInfo> probed = probeMediaFile(path, backend);

    if (probed.isError()) {
        const Error& e = probed.error();
        switch (e.code()) {
            // The container/format itself could not be recognized: this is an
            // unsupported-format rejection (Requirement 3.2). We surface the
            // probe's message, which names what little is known about the format.
            case ErrorCode::Unsupported: {
                std::string named = e.message().empty() ? std::string("unknown format")
                                                         : e.message();
                return err<MediaInfo>(makeError(
                    ErrorCode::Unsupported,
                    "unsupported media format: " + named));
            }
            // The file could not be opened, found, or read: its contents are
            // unreadable/undecodable (Requirement 3.3). Normalize NotFound and
            // Io onto a single "could not be read" rejection.
            case ErrorCode::NotFound:
            case ErrorCode::Io:
                return err<MediaInfo>(makeError(
                    ErrorCode::Io,
                    "media file could not be read: " + path.string()));
            // Empty path (InvalidArgument), missing FFmpeg (FailedPrecondition),
            // or an internal probe error: propagate unchanged. These are caller/
            // build errors, not import classifications.
            default:
                return probed;
        }
    }

    MediaInfo info = std::move(probed).value();

    // The container was read successfully. If nothing in it is decodable, this
    // is an unsupported-format rejection whose message names the format so the
    // UI can tell the user exactly what was rejected (Requirement 3.2).
    if (!info.hasSupportedStream()) {
        return err<MediaInfo>(makeError(
            ErrorCode::Unsupported,
            "unsupported media format: " + describeMediaFormat(info)));
    }

    // Accepted: at least one stream is decodable (Requirement 3.1).
    return info;
}

Result<MediaInfo> validateMediaImport(const std::filesystem::path& path) {
    return validateMediaImport(path, ffmpegProbeBackend());
}

} // namespace palmier::media
