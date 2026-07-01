// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeClient.cpp — implementation of the client-side generative
// job lifecycle and timeout/cancel policy (Requirement 6.1, 6.3, 6.4, 6.6, 6.8).
// See GenerativeClient.hpp for the contract.

#include "services/GenerativeClient.hpp"

#include <chrono>
#include <string>
#include <utility>

#include "core/Error.hpp"

namespace palmier::services {

std::string_view toStringView(GenerationMediaType type) noexcept {
    switch (type) {
        case GenerationMediaType::Video: return "video";
        case GenerationMediaType::Image: return "image";
    }
    return "video";
}

std::string_view toStringView(GenerationPhase phase) noexcept {
    switch (phase) {
        case GenerationPhase::Pending:   return "pending";
        case GenerationPhase::Running:   return "running";
        case GenerationPhase::Succeeded: return "succeeded";
        case GenerationPhase::Failed:    return "failed";
    }
    return "pending";
}

GenerativeClient::GenerativeClient(IGenerativeBackend& backend,
                                   std::chrono::milliseconds budget,
                                   Clock clock)
    : backend_(backend),
      budget_(budget),
      clock_(clock ? std::move(clock)
                   : Clock{[] { return std::chrono::steady_clock::now(); }}) {}

bool GenerativeClient::budgetExceeded(const JobRecord& record) const {
    return (clock_() - record.submittedAt) >= budget_;
}

Error GenerativeClient::cancelForTimeout(JobRecord& record, const JobId& id) {
    // 6.8: cancel the in-flight request (best-effort — a failed cancel does not
    // change the outcome we report) and mark the job terminally failed so a
    // subsequent poll/fetch does not re-contact the backend.
    if (!record.cancelled) {
        (void)backend_.cancel(id, record.token);
        record.cancelled = true;
        record.lastStatus.phase = GenerationPhase::Failed;
        record.lastStatus.failureReason =
            "generation timed out after " +
            std::to_string(kGenerationBudgetSeconds) + " seconds";
    }
    return makeError(ErrorCode::Timeout,
                     "generation did not complete within " +
                         std::to_string(kGenerationBudgetSeconds) +
                         " seconds; the request was cancelled");
}

Result<GenerationJob> GenerativeClient::submit(const GenerationRequest& request,
                                               std::string_view authToken) {
    // Transport-level guard: a bearer token is required to call the hosted
    // service over TLS. (Subscription/BYOK entitlement gating is task 14.2.)
    if (authToken.empty()) {
        return err<GenerationJob>(makeError(
            ErrorCode::Unauthenticated,
            "a generation request requires an authenticated session token"));
    }

    Result<JobId> submitted = backend_.submit(request, authToken);
    if (submitted.isError()) {
        // 6.6: a submit failure is forwarded and records nothing, so project
        // state is left entirely unchanged.
        return err<GenerationJob>(std::move(submitted).error());
    }

    JobId id = std::move(submitted).value();

    JobRecord record;
    record.token       = std::string(authToken);
    record.submittedAt = clock_();
    record.mediaType   = request.mediaType;
    record.model       = request.model;
    record.lastStatus  = GenerationStatus{GenerationPhase::Pending, 0, {}};
    jobs_[id]          = std::move(record);

    return GenerationJob{id, request.model, request.mediaType};
}

Result<GenerationStatus> GenerativeClient::poll(const JobId& id) {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return err<GenerationStatus>(notFound("unknown generation job id"));
    }
    JobRecord& record = it->second;

    // Once terminal, report the cached status without re-contacting the backend.
    if (record.lastStatus.isTerminal()) {
        if (record.cancelled) {
            // A timed-out/cancelled job keeps reporting the timeout error.
            return err<GenerationStatus>(makeError(
                ErrorCode::Timeout,
                "generation did not complete within " +
                    std::to_string(kGenerationBudgetSeconds) +
                    " seconds; the request was cancelled"));
        }
        return record.lastStatus;
    }

    // 6.8: enforce the budget before polling.
    if (budgetExceeded(record)) {
        return err<GenerationStatus>(cancelForTimeout(record, id));
    }

    Result<GenerationStatus> polled = backend_.poll(id, record.token);
    if (polled.isError()) {
        // 6.6: forward a provider/transport error unchanged; no state mutation.
        return err<GenerationStatus>(std::move(polled).error());
    }

    record.lastStatus = std::move(polled).value();

    // A provider that is still working past the budget is timed out here.
    if (!record.lastStatus.isTerminal() && budgetExceeded(record)) {
        return err<GenerationStatus>(cancelForTimeout(record, id));
    }

    return record.lastStatus;
}

Result<MediaAsset> GenerativeClient::fetchResult(const JobId& id) {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return err<MediaAsset>(notFound("unknown generation job id"));
    }
    JobRecord& record = it->second;

    // 6.8: a non-terminal job that has blown its budget is cancelled + timed out.
    if (!record.lastStatus.isTerminal() && budgetExceeded(record)) {
        return err<MediaAsset>(cancelForTimeout(record, id));
    }

    // A job that was previously timed out/cancelled keeps reporting the timeout.
    if (record.cancelled) {
        return err<MediaAsset>(makeError(
            ErrorCode::Timeout,
            "generation did not complete within " +
                std::to_string(kGenerationBudgetSeconds) +
                " seconds; the request was cancelled"));
    }

    // 6.6: a provider-side failure surfaces its descriptive reason. This client
    // never mutates the project, so nothing needs to be rolled back.
    if (record.lastStatus.phase == GenerationPhase::Failed) {
        const std::string& reason = record.lastStatus.failureReason;
        return err<MediaAsset>(makeError(
            ErrorCode::Internal,
            reason.empty() ? "generation failed at the provider" : reason));
    }

    // The result is only available once the job has succeeded.
    if (record.lastStatus.phase != GenerationPhase::Succeeded) {
        return err<MediaAsset>(failedPrecondition(
            "generation job has not completed; poll until it succeeds before fetching"));
    }

    Result<MediaAsset> fetched = backend_.fetchResult(id, record.token);
    if (fetched.isError()) {
        return err<MediaAsset>(std::move(fetched).error());
    }

    MediaAsset asset = std::move(fetched).value();
    // 6.1/6.3/6.4: the produced media must match the requested media type.
    if (asset.mediaType != record.mediaType) {
        return err<MediaAsset>(makeError(
            ErrorCode::Internal,
            std::string("provider returned ") +
                std::string(toStringView(asset.mediaType)) +
                " media for a " + std::string(toStringView(record.mediaType)) +
                " generation request"));
    }
    return asset;
}

Result<void> GenerativeClient::cancel(const JobId& id) {
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return err(notFound("unknown generation job id"));
    }
    JobRecord& record = it->second;

    // Nothing to cancel for an already-terminal or already-cancelled job.
    if (record.cancelled || record.lastStatus.phase == GenerationPhase::Succeeded) {
        return ok();
    }

    Result<void> result = backend_.cancel(id, record.token);
    record.cancelled = true;
    record.lastStatus.phase = GenerationPhase::Failed;
    record.lastStatus.failureReason = "generation cancelled";
    return result;
}

bool GenerativeClient::tracks(const JobId& id) const {
    return jobs_.find(id) != jobs_.end();
}

} // namespace palmier::services
