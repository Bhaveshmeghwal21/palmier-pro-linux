// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportValidation.cpp - what makes an export request acceptable before any
// work starts: the supported container and codec sets, the request-level checks and
// the timeline-level ones.
//
// Kept apart from the worker deliberately: these are pure predicates over a request
// and a project, so they are testable without starting an export.
//
// Split out of services/ExportCoordinator.cpp; the shared contract is
// services/ExportCoordinatorInternal.hpp. Every definition was moved, not rewritten.

#include "services/ExportCoordinatorInternal.hpp"

namespace palmier::services {

using namespace palmier::services::detail;  // NOLINT(google-build-using-namespace)

bool isSupportedExportContainer(const std::string& container) {
    const std::string c = toLowerAscii(container);
    return c == "mp4" || c == "mov" || c == "mkv" || c == "webm";
}

bool isSupportedExportCodec(gpu::CodecId codec) noexcept {
    // The three codecs the Encoder_Selector supports (Requirement 8.2).
    return codec == gpu::CodecId::H264 || codec == gpu::CodecId::HEVC || codec == gpu::CodecId::VP9;
}

Result<void> ExportCoordinator::validate(const ExportRequest2& request) {
    // --- Output path -------------------------------------------------------
    if (request.outputPath.empty()) {
        return err(invalidArgument("export requires an output path"));
    }
    const std::string pathText = request.outputPath.string();
    if (pathText.size() > kMaxExportPathLength) {
        return err(outOfRange("export output path is " + std::to_string(pathText.size()) +
                              " characters; the accepted range is 1 to " +
                              std::to_string(kMaxExportPathLength)));
    }

    std::filesystem::path parent = request.outputPath.parent_path();
    if (parent.empty()) parent = std::filesystem::path{"."};

    std::error_code ec;
    if (!std::filesystem::exists(parent, ec) || ec) {
        return err(notFound("the export output directory does not exist: " + parent.string()));
    }
    if (!std::filesystem::is_directory(parent, ec) || ec) {
        return err(invalidArgument("the export output path's parent is not a directory: " +
                                   parent.string()));
    }
    // Writability is asked of the operating system rather than inferred from the
    // permission bits, so the answer accounts for ownership, ACLs and read-only
    // mounts. It creates nothing.
    if (::access(parent.c_str(), W_OK) != 0) {
        return err(makeError(ErrorCode::PermissionDenied,
                             "the export output directory is not writable: " + parent.string()));
    }

    // --- Format ------------------------------------------------------------
    if (!isSupportedExportContainer(request.container)) {
        return err(unsupported("export container \"" + request.container +
                               "\" is not supported; the supported containers are "
                               "mp4, mov, mkv and webm"));
    }
    if (!isSupportedExportCodec(request.codec)) {
        return err(unsupported("export video codec " + codecLabel(request.codec) +
                               " is not supported; the supported codecs are H.264, HEVC and VP9"));
    }

    // --- Geometry, cadence, bit rate ---------------------------------------
    if (!request.resolution.isValid() || request.resolution.width < kMinExportWidth ||
        request.resolution.width > kMaxExportWidth ||
        request.resolution.height < kMinExportHeight ||
        request.resolution.height > kMaxExportHeight) {
        return err(outOfRange(
            "export resolution " + std::to_string(request.resolution.width) + "x" +
            std::to_string(request.resolution.height) + " is out of range; the accepted range is " +
            std::to_string(kMinExportWidth) + "x" + std::to_string(kMinExportHeight) + " to " +
            std::to_string(kMaxExportWidth) + "x" + std::to_string(kMaxExportHeight)));
    }
    if (!request.frameRate.isValid()) {
        return err(invalidArgument("export frame rate must be a positive rational; the accepted "
                                   "range is 1 to 120 frames per second"));
    }
    const double fps = request.frameRate.toDouble();
    if (fps < kMinExportFps || fps > kMaxExportFps) {
        return err(
            outOfRange("export frame rate " + std::to_string(fps) +
                       " frames per second is out of range; the accepted range is 1 to 120"));
    }
    if (request.bitrateKbps < kMinExportBitrateKbps ||
        request.bitrateKbps > kMaxExportBitrateKbps) {
        return err(outOfRange("export video bit rate " + std::to_string(request.bitrateKbps) +
                              " kbps is out of range; the accepted range is " +
                              std::to_string(kMinExportBitrateKbps) + " to " +
                              std::to_string(kMaxExportBitrateKbps)));
    }

    // --- Overwrite protection (Requirement 7.11) ----------------------------
    // Checked LAST and, like every check above, without touching the file: an
    // existing destination is left byte-for-byte intact by a rejection.
    if (std::filesystem::exists(request.outputPath, ec) && !ec && !request.overwrite) {
        return err(makeError(ErrorCode::AlreadyExists,
                             "the export destination already exists: " + pathText +
                                 "; re-request with an explicit overwrite acknowledgement to "
                                 "replace it"));
    }

    return ok();
}

Result<void> ExportCoordinator::validateTimeline(const Project& project) {
    std::size_t segments = 0;
    for (const Track& track : project.tracks) {
        segments += track.clips.size();
    }
    if (segments == 0) {
        return err(failedPrecondition(
            "the timeline is empty: an export requires at least one media segment"));
    }
    return ok();
}

}  // namespace palmier::services
