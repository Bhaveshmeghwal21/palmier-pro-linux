// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerativeMediaCoordinator.cpp — implementation of the generative
// media coordinator and its concrete adapters (task 14.2; Requirements 6.2,
// 6.5, 6.7, 9.7). See GenerativeMediaCoordinator.hpp for the design rationale.

#include "services/GenerativeMediaCoordinator.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/EditCommand.hpp"
#include "core/Error.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/TimelineEngine.hpp"
#include "services/AuthenticationService.hpp"
#include "services/GenerativeClient.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// GenerativeMediaCoordinator
// ---------------------------------------------------------------------------

GenerativeMediaCoordinator::GenerativeMediaCoordinator(IGenerationGate& gate,
                                                       IGenerationRunner& runner,
                                                       MediaManager& library,
                                                       ITimelinePlacement& placement)
    : gate_(gate), runner_(runner), library_(library), placement_(placement) {}

Result<void> GenerativeMediaCoordinator::validatePrompt(std::string_view prompt) {
    const std::size_t length = prompt.size();
    if (length < kMinPromptLength || length > kMaxPromptLength) {
        return err(invalidArgument(
            "prompt must be between 1 and 2000 characters"));
    }
    return ok();
}

Result<GeneratedMediaPlacement> GenerativeMediaCoordinator::generateAndPlace(
    const GenerationRequest& request, const GenerationPlacement& where) {
    // 6.7 — prompt length gate. Rejected before anything else runs, so the
    // timeline and library are left untouched.
    if (Result<void> prompt = validatePrompt(request.prompt); prompt.isError()) {
        return err<GeneratedMediaPlacement>(std::move(prompt).error());
    }

    // A malformed source range is a caller error; reject before generating so no
    // provider work happens and the timeline stays unchanged.
    if (where.sourceOut <= where.sourceIn) {
        return err<GeneratedMediaPlacement>(
            invalidArgument("generated clip sourceOut must exceed sourceIn"));
    }

    // 6.5 / 9.7 — entitlement gate. An unauthorized request never contacts the
    // provider and never touches the timeline.
    Result<GenerationAuthorization> auth = gate_.authorize(request);
    if (auth.isError()) {
        return err<GeneratedMediaPlacement>(std::move(auth).error());
    }

    // 6.1/6.3/6.4/6.6/6.8 — run the generation. A provider failure or timeout is
    // forwarded unchanged; nothing has been added to the library or timeline yet.
    Result<MediaAsset> generated = runner_.generate(request, auth.value().authToken);
    if (generated.isError()) {
        return err<GeneratedMediaPlacement>(std::move(generated).error());
    }
    const MediaAsset asset = std::move(generated).value();

    // 6.2 — add the generated media to the project library.
    if (Result<void> imported = library_.importAsset(asset.ref); imported.isError()) {
        return err<GeneratedMediaPlacement>(std::move(imported).error());
    }

    // 6.2 — place it on the timeline at the user-specified frame position. The
    // placement is atomic: a rejected placement (out-of-range position or an
    // overlap) leaves the timeline unchanged.
    TimelinePlacementRequest placementRequest{
        where.trackId, asset.ref, where.framePosition, where.sourceIn, where.sourceOut};
    return placement_.place(placementRequest);
}

// ---------------------------------------------------------------------------
// AuthServiceGenerationGate
// ---------------------------------------------------------------------------

AuthServiceGenerationGate::AuthServiceGenerationGate(const AuthenticationService& auth,
                                                     ProviderResolver providerResolver)
    : auth_(auth), providerResolver_(std::move(providerResolver)) {}

Result<GenerationAuthorization> AuthServiceGenerationGate::authorize(
    const GenerationRequest& request) const {
    const auto& session = auth_.currentSession();

    // An active subscription entitles generation; use the session bearer.
    if (session.has_value() && session->entitlement == EntitlementStatus::Active) {
        return GenerationAuthorization{session->token};
    }

    // Otherwise a valid BYOK credential for the request's provider authorizes it.
    const std::string provider =
        providerResolver_ ? providerResolver_(request) : request.model;
    if (auth_.isByokAuthorized(provider)) {
        // Prefer the session bearer when logged in; otherwise a BYOK-scoped token
        // so the downstream request carries a non-empty authorization.
        std::string token =
            (session.has_value() && !session->token.empty()) ? session->token
                                                             : ("byok:" + provider);
        return GenerationAuthorization{std::move(token)};
    }

    return err<GenerationAuthorization>(makeError(
        ErrorCode::Unauthenticated,
        "an active subscription or BYOK credentials are required to generate media"));
}

// ---------------------------------------------------------------------------
// GenerativeClientRunner
// ---------------------------------------------------------------------------

GenerativeClientRunner::GenerativeClientRunner(GenerativeClient& client,
                                               std::function<void()> pacer,
                                               std::size_t maxPollAttempts)
    : client_(client), pacer_(std::move(pacer)), maxPollAttempts_(maxPollAttempts) {}

Result<MediaAsset> GenerativeClientRunner::generate(const GenerationRequest& request,
                                                    std::string_view authToken) {
    Result<GenerationJob> job = client_.submit(request, authToken);
    if (job.isError()) {
        return err<MediaAsset>(std::move(job).error());
    }
    const JobId id = job.value().id;

    for (std::size_t attempt = 0; attempt < maxPollAttempts_; ++attempt) {
        Result<GenerationStatus> status = client_.poll(id);
        if (status.isError()) {
            // Timeout (6.8) or a transport error is forwarded unchanged.
            return err<MediaAsset>(std::move(status).error());
        }
        if (status.value().isTerminal()) {
            // Succeeded -> fetch the asset; Failed -> fetchResult surfaces the
            // provider reason as an error (6.6).
            return client_.fetchResult(id);
        }
        if (pacer_) {
            pacer_();
        }
    }

    // Safety net: the client's 300-second budget is the real timeout, but bound
    // the loop so a misbehaving backend cannot spin forever.
    (void)client_.cancel(id);
    return err<MediaAsset>(makeError(ErrorCode::Timeout,
                                     "generation did not complete before the poll limit"));
}

// ---------------------------------------------------------------------------
// TimelineEnginePlacer
// ---------------------------------------------------------------------------

namespace {

/// An atomic add-clip command used to drop a generated clip onto a track. It is
/// self-contained (revert removes exactly what apply inserted) so it round-trips
/// through the TimelineEngine's undo/redo stack. Distinct from core's
/// AddClipCommand (task 3.3): this variant also registers the generated asset in
/// the project asset table when it is not already present.
class PlaceGeneratedClipCommand final : public EditCommand {
public:
    PlaceGeneratedClipCommand(ClipId clipId, Uuid trackId, MediaAssetRef asset,
                              Duration timelineStart, Duration sourceIn, Duration sourceOut)
        : clipId_(clipId),
          trackId_(trackId),
          asset_(std::move(asset)),
          timelineStart_(timelineStart),
          sourceIn_(sourceIn),
          sourceOut_(sourceOut) {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return "PlaceGeneratedClip";
    }

    [[nodiscard]] Result<void> apply(Project& project) override {
        auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(),
                                    [this](const Track& t) { return t.id == trackId_; });
        if (trackIt == project.tracks.end()) {
            return err(notFound("target track for generated clip does not exist"));
        }

        // Register the generated asset in the project asset table if absent, so
        // the clip's assetRef resolves (project validation rule).
        assetAdded_ = std::none_of(project.assets.begin(), project.assets.end(),
                                   [this](const MediaAssetRef& a) { return a == asset_; });
        if (assetAdded_) {
            project.assets.push_back(asset_);
        }

        Clip clip;
        clip.id = clipId_;
        clip.assetRef = asset_;
        clip.timelineStart = timelineStart_;
        clip.sourceIn = sourceIn_;
        clip.sourceOut = sourceOut_;

        // Insert keeping the track's clips ordered by timelineStart (the engine
        // enforces ordering + non-overlap and rolls back if this violates them).
        auto& clips = trackIt->clips;
        auto pos = std::upper_bound(clips.begin(), clips.end(), timelineStart_,
                                    [](Duration start, const Clip& c) {
                                        return start < c.timelineStart;
                                    });
        clips.insert(pos, std::move(clip));
        return ok();
    }

    [[nodiscard]] Result<void> revert(Project& project) override {
        auto trackIt = std::find_if(project.tracks.begin(), project.tracks.end(),
                                    [this](const Track& t) { return t.id == trackId_; });
        if (trackIt == project.tracks.end()) {
            return err(notFound("target track for generated clip does not exist"));
        }
        auto& clips = trackIt->clips;
        clips.erase(std::remove_if(clips.begin(), clips.end(),
                                   [this](const Clip& c) { return c.id == clipId_; }),
                    clips.end());

        if (assetAdded_) {
            auto& assets = project.assets;
            assets.erase(std::remove_if(assets.begin(), assets.end(),
                                        [this](const MediaAssetRef& a) { return a == asset_; }),
                         assets.end());
            assetAdded_ = false;
        }
        return ok();
    }

private:
    ClipId        clipId_;
    Uuid          trackId_;
    MediaAssetRef asset_;
    Duration      timelineStart_;
    Duration      sourceIn_;
    Duration      sourceOut_;
    bool          assetAdded_ = false;
};

} // namespace

TimelineEnginePlacer::TimelineEnginePlacer(TimelineEngine& engine) : engine_(engine) {}

Result<GeneratedMediaPlacement> TimelineEnginePlacer::place(
    const TimelinePlacementRequest& request) {
    if (request.sourceOut <= request.sourceIn) {
        return err<GeneratedMediaPlacement>(
            invalidArgument("generated clip sourceOut must exceed sourceIn"));
    }
    if (request.framePosition < 0) {
        return err<GeneratedMediaPlacement>(
            outOfRange("frame position must be at least 0"));
    }

    const Project project = engine_.snapshot();
    if (!project.timelineFps.isValid()) {
        return err<GeneratedMediaPlacement>(
            failedPrecondition("project has no valid timeline frame rate"));
    }

    // Convert the user-specified frame position to a timeline Duration and bound
    // it to [0, current timeline duration] (Requirement 6.2).
    const Duration timelineStart = project.timelineFps.durationForFrames(request.framePosition);
    if (timelineStart > timelineDuration(project)) {
        return err<GeneratedMediaPlacement>(outOfRange(
            "frame position is beyond the current timeline duration"));
    }

    const auto trackExists = std::any_of(project.tracks.begin(), project.tracks.end(),
                                         [&](const Track& t) { return t.id == request.trackId; });
    if (!trackExists) {
        return err<GeneratedMediaPlacement>(notFound("target track does not exist"));
    }

    const ClipId clipId = Uuid::generateV4();
    auto command = std::make_unique<PlaceGeneratedClipCommand>(
        clipId, request.trackId, request.asset, timelineStart, request.sourceIn,
        request.sourceOut);

    CommandResult result = engine_.apply(std::move(command));
    if (result.isError()) {
        // Timeline left unchanged by the engine's atomic apply.
        return err<GeneratedMediaPlacement>(result.error());
    }

    return GeneratedMediaPlacement{request.asset, clipId, timelineStart};
}

} // namespace palmier::services
