// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ExportCoordinatorInternal.hpp - the internal contract shared by the units
// that implement export coordination.
//
// Carries the guards the export worker runs behind - the backend wrapper that turns a
// stage failure into a typed error, the observation record the encode reports through,
// and the output guard that removes a partial file when an export does not finish -
// together with the small codec and container spellings both validation and the worker
// need.
//
// These were an anonymous namespace inside a single 1080-line
// services/ExportCoordinator.cpp. Splitting that file by subject means they must cross
// translation units, and internal linkage cannot, so they now live in
// palmier::services::detail. The public surface remains
// services/ExportCoordinator.hpp, unchanged.

#ifndef PALMIER_SERVICES_EXPORTCOORDINATORINTERNAL_HPP
#define PALMIER_SERVICES_EXPORTCOORDINATORINTERNAL_HPP

#include "services/ExportCoordinator.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <unistd.h>                 // access(), W_OK — the parent-directory writability check
#include "core/TimelineEngine.hpp"  // timelineDuration()
#include "media/AudioGraph.hpp"
#include "media/DecoderClipFrameProvider.hpp"
#include "services/CaptionExport.hpp"

namespace palmier::services::detail {

[[nodiscard]] std::string toLowerAscii(std::string s);

[[nodiscard]] std::string codecLabel(gpu::CodecId codec);

enum class FailingStage {
    None,
    Video,   ///< video encode / mux.
    Audio,   ///< audio encode / mux (Requirement 6.10).
    Finish,  ///< flushing the streams or writing the trailer.
};

struct EncodeObservation {
    std::mutex mutex{};
    media::IEncodeBackend* live{nullptr};  ///< non-null while the backend exists.
    bool finished{false};
    std::size_t videoFrames{0};
    std::size_t audioBlocks{0};
    FailingStage failingStage{FailingStage::None};

    /// Best-effort finish, used by the scope guard to release the output file
    /// handle before the file is removed (Requirement 7.5). A no-op once the
    /// backend has been finished or destroyed — which is the normal case, because
    /// ExportEngine::run finishes or destroys it before returning.
    void finishBestEffort() {
        std::lock_guard<std::mutex> lock(mutex);
        if (live == nullptr || finished) return;
        finished = true;
        (void)live->finish();  // deliberately ignored: cleanup must not mask the cause.
    }
};

class GuardedBackend final : public media::IEncodeBackend {
public:
    GuardedBackend(std::unique_ptr<media::IEncodeBackend> inner,
                   std::shared_ptr<EncodeObservation> observation)
        : inner_(std::move(inner)), observation_(std::move(observation)) {
        std::lock_guard<std::mutex> lock(observation_->mutex);
        observation_->live = inner_.get();
    }

    ~GuardedBackend() override {
        std::lock_guard<std::mutex> lock(observation_->mutex);
        observation_->live = nullptr;
    }

    GuardedBackend(const GuardedBackend&) = delete;
    GuardedBackend& operator=(const GuardedBackend&) = delete;

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        Result<void> r = inner_->encode(frame);
        std::lock_guard<std::mutex> lock(observation_->mutex);
        if (r.isError()) {
            observation_->failingStage = FailingStage::Video;
        } else {
            ++observation_->videoFrames;
        }
        return r;
    }

    [[nodiscard]] Result<void> encodeAudio(const media::EncoderInputAudio& audio) override {
        Result<void> r = inner_->encodeAudio(audio);
        std::lock_guard<std::mutex> lock(observation_->mutex);
        if (r.isError()) {
            // Requirement 6.10: the reported stage must identify audio encoding
            // or muxing, not "the export".
            observation_->failingStage = FailingStage::Audio;
        } else {
            ++observation_->audioBlocks;
        }
        return r;
    }

    [[nodiscard]] Result<void> finish() override {
        {
            std::lock_guard<std::mutex> lock(observation_->mutex);
            if (observation_->finished) {
                // The scope guard already finished this backend best-effort.
                return ok();
            }
            observation_->finished = true;
        }
        Result<void> r = inner_->finish();
        if (r.isError()) {
            std::lock_guard<std::mutex> lock(observation_->mutex);
            observation_->failingStage = FailingStage::Finish;
        }
        return r;
    }

private:
    std::unique_ptr<media::IEncodeBackend> inner_;
    std::shared_ptr<EncodeObservation> observation_;
};

class OutputGuard {
public:
    OutputGuard(std::filesystem::path path, std::shared_ptr<EncodeObservation> observation)
        : path_(std::move(path)), observation_(std::move(observation)) {}

    ~OutputGuard() {
        if (committed_) return;
        if (observation_) observation_->finishBestEffort();
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);  // best-effort; must not mask the cause.
    }

    OutputGuard(const OutputGuard&) = delete;
    OutputGuard& operator=(const OutputGuard&) = delete;

    /// The export succeeded: the output file is the deliverable and must survive.
    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    std::shared_ptr<EncodeObservation> observation_;
    bool committed_{false};
};

}  // namespace palmier::services::detail

#endif  // PALMIER_SERVICES_EXPORTCOORDINATORINTERNAL_HPP
