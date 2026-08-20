// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/MediaImportService.cpp — probe -> validate -> register (see header).
//
// Two notes on how this composes the existing media layer rather than
// re-implementing it:
//
//   * media::probeMediaFile does the container read through the injected backend
//     and the MediaInfo normalization. Its raw ErrorCode is what lets this service
//     tell "the file does not exist" (NotFound) apart from "the file could not be
//     opened or read" (Io), which Requirement 2.4 requires to be distinguished but
//     media::validateMediaImport deliberately collapses onto one "could not be
//     read" rejection.
//   * media::validateMediaImport is still the acceptance policy: it is applied to
//     the ALREADY probed MediaInfo by handing it a backend that returns that
//     value, so the accept/reject decision lives in exactly one place and the file
//     is never probed twice. Only the rejection MESSAGE is composed here, because
//     Requirement 2.3 asks for the container format and the rejected codecs to be
//     named, and media::describeMediaFormat supplies the container half of that.

#include "services/MediaImportService.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "media/ImportValidation.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services {

namespace {

/// The outcome of the timed section: either a probed, accepted MediaInfo or the
/// classification of why the request was rejected. Built on the worker thread and
/// recorded by the owning thread, so the service's own state is only ever touched
/// by its owner.
struct TimedOutcome {
    ImportFailure    failure = ImportFailure::None;
    std::string      detail;
    media::MediaInfo info;
};

/// The label to show for one stream's codec: the stable media-level name when the
/// codec is recognized, otherwise the raw decoder name the backend reported.
[[nodiscard]] std::string codecLabel(const media::MediaStreamInfo& stream) {
    if (stream.codec != media::MediaCodecId::Unknown) {
        return std::string(media::codecName(stream.codec));
    }
    if (!stream.codecName.empty()) {
        return stream.codecName;
    }
    return "unknown";
}

/// The distinct codecs of `info` that the engine cannot decode, in first-seen
/// order — the codecs Requirement 2.3 requires a rejection to name.
[[nodiscard]] std::string rejectedCodecList(const media::MediaInfo& info) {
    std::vector<std::string> labels;
    for (const auto& stream : info.streams) {
        if (stream.isSupported()) continue;
        std::string label = codecLabel(stream);
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
            labels.push_back(std::move(label));
        }
    }
    if (labels.empty()) return "none";

    std::string out;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i != 0) out += ", ";
        out += labels[i];
    }
    return out;
}

/// The container format name to state in a rejection.
[[nodiscard]] std::string containerName(const media::MediaInfo& info) {
    if (!info.containerFormat.empty()) return info.containerFormat;
    if (!info.containerLongName.empty()) return info.containerLongName;
    return "unknown";
}

/// Run the whole timed section — access check, probe, acceptance policy, decode
/// check — over copies of everything it needs, so it is safe to abandon.
[[nodiscard]] TimedOutcome runTimedSection(const std::filesystem::path& path,
                                          const MediaImportService::PathAccessCheck& access,
                                          const media::MediaProbeBackend& backend,
                                          const MediaImportService::DecodeCheck& decode) {
    TimedOutcome outcome;

    // 2.4 — the two filesystem conditions, kept distinct.
    if (access) {
        if (Result<void> reachable = access(path); reachable.isError()) {
            outcome.failure = reachable.error().code() == ErrorCode::NotFound
                                  ? ImportFailure::FileNotFound
                                  : ImportFailure::FileUnreadable;
            outcome.detail = reachable.error().message();
            return outcome;
        }
    }

    Result<media::MediaInfo> probed = media::probeMediaFile(path, backend);
    if (probed.isError()) {
        const Error& error = probed.error();
        switch (error.code()) {
            case ErrorCode::InvalidArgument:
                outcome.failure = ImportFailure::EmptyPath;
                break;
            case ErrorCode::NotFound:
                outcome.failure = ImportFailure::FileNotFound;
                break;
            case ErrorCode::Io:
                outcome.failure = ImportFailure::FileUnreadable;
                break;
            case ErrorCode::Unsupported:
                // 2.3 — an unrecognised container. The probe's message carries
                // whatever is known about the format; no codec was recognised.
                outcome.failure = ImportFailure::UnsupportedFormat;
                outcome.detail = "unrecognised container format: " +
                                 (error.message().empty() ? std::string("unknown")
                                                          : error.message()) +
                                 "; rejected codecs: unknown";
                return outcome;
            default:
                outcome.failure = ImportFailure::Internal;
                break;
        }
        if (outcome.detail.empty()) outcome.detail = error.message();
        return outcome;
    }

    media::MediaInfo info = std::move(probed).value();

    // 2.3 — the shared acceptance policy, applied to the MediaInfo already in
    // hand so nothing is probed twice.
    const Result<media::MediaInfo> accepted =
        media::validateMediaImport(path, [&info](const std::filesystem::path&) {
            return Result<media::MediaInfo>(info);
        });
    if (accepted.isError()) {
        outcome.failure = ImportFailure::UnsupportedFormat;
        outcome.detail = "container format: " + containerName(info) +
                         "; rejected codecs: " + rejectedCodecList(info);
        return outcome;
    }

    // 2.4 — the decodable stream must actually decode.
    if (decode) {
        if (Result<void> decodable = decode(path, info); decodable.isError()) {
            outcome.failure = ImportFailure::UndecodableContent;
            outcome.detail = decodable.error().message();
            return outcome;
        }
    }

    outcome.info = std::move(info);
    return outcome;
}

} // namespace

// ---------------------------------------------------------------------------
// Failure vocabulary
// ---------------------------------------------------------------------------

std::string_view describeImportFailure(ImportFailure failure) noexcept {
    switch (failure) {
        case ImportFailure::None:               return "the import succeeded";
        case ImportFailure::EmptyPath:          return "the media path is empty";
        case ImportFailure::FileNotFound:       return "the file does not exist";
        case ImportFailure::FileUnreadable:     return "the file could not be opened or read";
        case ImportFailure::UndecodableContent: return "the file's decodable stream failed to decode";
        case ImportFailure::UnsupportedFormat:  return "the media format is not supported";
        case ImportFailure::NoProjectOpen:      return "no project is open";
        case ImportFailure::TimedOut:           return "probing and validation exceeded the time limit";
        case ImportFailure::Internal:           return "the media library refused the registration";
    }
    return "the import failed";
}

ErrorCode importFailureCode(ImportFailure failure) noexcept {
    switch (failure) {
        case ImportFailure::None:               return ErrorCode::Unknown;
        case ImportFailure::EmptyPath:          return ErrorCode::InvalidArgument;
        case ImportFailure::FileNotFound:       return ErrorCode::NotFound;
        case ImportFailure::FileUnreadable:     return ErrorCode::Io;
        case ImportFailure::UndecodableContent: return ErrorCode::Io;
        case ImportFailure::UnsupportedFormat:  return ErrorCode::Unsupported;
        case ImportFailure::NoProjectOpen:      return ErrorCode::FailedPrecondition;
        case ImportFailure::TimedOut:           return ErrorCode::Timeout;
        case ImportFailure::Internal:           return ErrorCode::Internal;
    }
    return ErrorCode::Unknown;
}

// ---------------------------------------------------------------------------
// Pending marker
// ---------------------------------------------------------------------------

MediaImportService::PendingMark::PendingMark(MediaImportService& owner, std::string key)
    : owner_(owner), key_(std::move(key)) {
    const std::lock_guard<std::mutex> lock(owner_.pendingMutex_);
    owner_.pending_.insert(key_);
}

MediaImportService::PendingMark::~PendingMark() {
    const std::lock_guard<std::mutex> lock(owner_.pendingMutex_);
    owner_.pending_.erase(key_);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MediaImportService::PathAccessCheck MediaImportService::filesystemAccessCheck() {
    return [](const std::filesystem::path& path) -> Result<void> {
        if (path.empty()) {
            return err<void>(invalidArgument("media path must not be empty"));
        }

        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::status(path, ec);
        if (ec || !std::filesystem::exists(status)) {
            return err<void>(notFound("no such file: " + path.string()));
        }
        if (std::filesystem::is_directory(status)) {
            return err<void>(makeError(ErrorCode::Io, "path names a directory: " + path.string()));
        }

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return err<void>(makeError(ErrorCode::Io, "cannot open for reading: " + path.string()));
        }
        char probeByte = 0;
        in.read(&probeByte, 1);
        if (in.bad()) {
            return err<void>(makeError(ErrorCode::Io, "cannot read: " + path.string()));
        }
        return ok();
    };
}

MediaImportService::MediaImportService(ProjectSession& session, media::MediaProbeBackend backend,
                                       Options options)
    : MediaImportService(&session, std::move(backend), std::move(options)) {}

MediaImportService::MediaImportService(ProjectSession* session, media::MediaProbeBackend backend,
                                       Options options)
    : session_(session),
      backend_(std::move(backend)),
      timeout_(options.timeout),
      accessCheck_(options.accessCheck ? std::move(options.accessCheck) : filesystemAccessCheck()),
      decodeCheck_(std::move(options.decodeCheck)) {}

// ---------------------------------------------------------------------------
// Path resolution and duplicate lookup
// ---------------------------------------------------------------------------

std::filesystem::path MediaImportService::resolvePath(const std::filesystem::path& path) {
    if (path.empty()) return path;

    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
        ec.clear();
    }

    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (ec || canonical.empty()) return absolute;
    return canonical;
}

std::optional<Uuid> MediaImportService::registeredAsset(
    const std::filesystem::path& resolved) const {
    if (session_ == nullptr) return std::nullopt;

    for (const MediaAssetRef& entry : session_->mediaLibrary().library()) {
        if (entry.sourcePath.empty()) continue;
        if (resolvePath(std::filesystem::path(entry.sourcePath)) == resolved) {
            return entry.assetId;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

Error MediaImportService::makeRejection(ImportFailure failure, const std::filesystem::path& path,
                                       const std::string& detail) {
    lastFailure_ = failure;
    std::string message = "cannot import '" + path.string() + "': " +
                          std::string(describeImportFailure(failure));
    if (!detail.empty()) {
        message += " (" + detail + ")";
    }
    return makeError(importFailureCode(failure), std::move(message));
}

ImportedAsset MediaImportService::describe(const Uuid& assetId,
                                           const std::filesystem::path& resolved,
                                           const media::MediaInfo& info) {
    ImportedAsset asset;
    asset.assetId = assetId;
    asset.sourcePath = resolved;
    asset.containerFormat = containerName(info);
    asset.durationMs = info.duration.milliseconds();

    // 2.2 — resolution and frame rate are reported only for an asset carrying at
    // least one DECODABLE video stream; an audio-only asset, or one whose video
    // stream uses a codec the engine cannot decode, reports both as absent.
    for (const auto& stream : info.streams) {
        if (!stream.isSupported()) continue;
        if (stream.isVideo() && !asset.hasVideo) {
            asset.hasVideo = true;
            if (stream.resolution.isValid()) asset.resolution = stream.resolution;
            if (stream.frameRate.isValid()) asset.frameRate = stream.frameRate;
        } else if (stream.isAudio()) {
            asset.hasAudio = true;
        }
    }
    return asset;
}

Result<MediaImportService::ProbeOutcome> MediaImportService::probeAndValidate(
    const std::filesystem::path& resolved) {
    // Copies only: the worker must be safe to abandon when the deadline expires.
    const PathAccessCheck access = accessCheck_;
    const media::MediaProbeBackend backend = backend_;
    const DecodeCheck decode = decodeCheck_;
    const std::filesystem::path path = resolved;

    auto section = [access, backend, decode, path]() { return runTimedSection(path, access, backend, decode); };

    TimedOutcome outcome;
    if (timeout_.count() <= 0) {
        // No limit configured: run inline rather than spawning a worker.
        outcome = section();
    } else {
        auto task = std::make_shared<std::packaged_task<TimedOutcome()>>(section);
        std::future<TimedOutcome> pending = task->get_future();
        std::thread worker([task]() { (*task)(); });

        if (pending.wait_for(timeout_) != std::future_status::ready) {
            // 2.8 — abandon the import. The worker owns copies of everything it
            // touches (including the packaged task holding the result), so
            // detaching it cannot dangle.
            worker.detach();
            return err<ProbeOutcome>(makeRejection(ImportFailure::TimedOut, resolved,
                                                   "limit " + std::to_string(timeout_.count()) +
                                                       " ms"));
        }
        worker.join();
        outcome = pending.get();
    }

    if (outcome.failure != ImportFailure::None) {
        return err<ProbeOutcome>(makeRejection(outcome.failure, resolved, outcome.detail));
    }
    return ProbeOutcome{std::move(outcome.info)};
}

Result<ImportedAsset> MediaImportService::import(const std::filesystem::path& path) {
    // 2.7 — no project open: register nothing.
    if (session_ == nullptr) {
        return err<ImportedAsset>(makeRejection(ImportFailure::NoProjectOpen, path));
    }
    // 2.4 — an empty path never reaches the probe.
    if (path.empty()) {
        return err<ImportedAsset>(makeRejection(ImportFailure::EmptyPath, path));
    }

    const std::filesystem::path resolved = resolvePath(path);
    const std::string key = resolved.generic_string();

    // 2.5 — the same filesystem location, however it was spelled, yields the
    // asset that is already registered and adds no entry.
    if (const std::optional<Uuid> existing = registeredAsset(resolved)) {
        lastFailure_ = ImportFailure::None;
        if (const auto cached = imported_.find(key); cached != imported_.end()) {
            ImportedAsset asset = cached->second;
            asset.duplicate = true;
            return asset;
        }
        // Registered by something other than this service (for example rebuilt
        // from an opened document), so no probed description is on hand. The
        // identity is what Requirement 2.5 asks for.
        ImportedAsset asset;
        asset.assetId = *existing;
        asset.sourcePath = resolved;
        asset.duplicate = true;
        return asset;
    }

    // 2.9 — pending for exactly the probe + validate + register window.
    const PendingMark mark(*this, key);

    Result<ProbeOutcome> probed = probeAndValidate(resolved);
    if (probed.isError()) {
        return err<ImportedAsset>(std::move(probed).error());
    }
    const media::MediaInfo& info = probed.value().info;

    // 2.1 — exactly one asset is registered, and only now.
    const Uuid assetId = Uuid::generateV4();
    if (Result<void> registered =
            session_->mediaLibrary().importAsset(MediaAssetRef(assetId, resolved.string()));
        registered.isError()) {
        return err<ImportedAsset>(
            makeRejection(ImportFailure::Internal, resolved, registered.error().message()));
    }

    ImportedAsset asset = describe(assetId, resolved, info);
    imported_[key] = asset;

    // The media library changed outside the engine, so the session must be told.
    session_->markModified();

    lastFailure_ = ImportFailure::None;
    return asset;
}

bool MediaImportService::isPending(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    const std::string key = resolvePath(path).generic_string();
    const std::lock_guard<std::mutex> lock(pendingMutex_);
    return pending_.find(key) != pending_.end();
}

std::size_t MediaImportService::pendingCount() const {
    const std::lock_guard<std::mutex> lock(pendingMutex_);
    return pending_.size();
}

} // namespace palmier::services
