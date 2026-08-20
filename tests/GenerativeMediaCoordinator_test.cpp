// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/GenerativeMediaCoordinator_test.cpp — unit tests for the generative
// media coordinator (task 14.2; Requirements 6.2, 6.5, 6.7, 9.7).
//
// The coordinator ties GenerativeClient + AuthenticationService + MediaManager +
// TimelineEngine together. These tests drive it through its narrow seams:
//   * prompt-length validation (6.7) and its "timeline unchanged" guarantee;
//   * entitlement gating (6.5 / 9.7) with a mock gate and with the real
//     AuthServiceGenerationGate over an AuthenticationService (subscription and
//     BYOK paths);
//   * successful generate-and-place through the real TimelineEnginePlacer and a
//     real MediaManager library (6.2).

#include "services/GenerativeMediaCoordinator.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AuthenticationService.hpp"
#include "services/ByokCredentialManager.hpp"
#include "services/ByokCredentials.hpp"
#include "services/GenerativeClient.hpp"
#include "services/SecretStore.hpp"

namespace {

using namespace palmier;
using palmier::services::AuthBackend;
using palmier::services::AuthenticationService;
using palmier::services::AuthServiceGenerationGate;
using palmier::services::BackendSession;
using palmier::services::ByokCredential;
using palmier::services::ByokCredentialManager;
using palmier::services::ByokProviderValidator;
using palmier::services::EntitlementStatus;
using palmier::services::GenerationAuthorization;
using palmier::services::GenerationMediaType;
using palmier::services::GenerationPlacement;
using palmier::services::GenerationRequest;
using palmier::services::GeneratedMediaPlacement;
using palmier::services::GenerativeMediaCoordinator;
using palmier::services::IGenerationGate;
using palmier::services::IGenerationRunner;
using palmier::services::ITimelinePlacement;
using palmier::services::InMemorySecretStore;
using palmier::services::LoginCredentials;
using palmier::services::MediaAsset;
using palmier::services::TimelineEnginePlacer;
using palmier::services::TimelinePlacementRequest;

// --- Test doubles ------------------------------------------------------------

// A gate whose decision is scripted per test.
class MockGate : public IGenerationGate {
public:
    std::function<Result<GenerationAuthorization>(const GenerationRequest&)> onAuthorize;
    mutable int calls = 0;

    Result<GenerationAuthorization> authorize(const GenerationRequest& r) const override {
        ++calls;
        return onAuthorize(r);
    }
};

// A runner whose result is scripted per test; records whether it ran.
class MockRunner : public IGenerationRunner {
public:
    std::function<Result<MediaAsset>(const GenerationRequest&, std::string_view)> onGenerate;
    int         calls = 0;
    std::string lastToken;

    Result<MediaAsset> generate(const GenerationRequest& r, std::string_view token) override {
        ++calls;
        lastToken = std::string(token);
        return onGenerate(r, token);
    }
};

// A placement seam that records calls; used to assert the timeline is never
// touched when an earlier gate rejects.
class RecordingPlacement : public ITimelinePlacement {
public:
    int                      calls = 0;
    TimelinePlacementRequest lastRequest;
    std::function<Result<GeneratedMediaPlacement>(const TimelinePlacementRequest&)> onPlace;
    bool                     knownTrack = true;

    Result<GeneratedMediaPlacement> place(const TimelinePlacementRequest& r) override {
        ++calls;
        lastRequest = r;
        if (onPlace) return onPlace(r);
        return GeneratedMediaPlacement{r.asset, Uuid::generateV4(),
                                       Duration::zero()};
    }

    bool trackExists(const Uuid&) const override { return knownTrack; }
};

// Scriptable auth backend for exercising the real AuthServiceGenerationGate.
class MockAuthBackend : public AuthBackend {
public:
    std::function<Result<BackendSession>(const LoginCredentials&)> onAuth;
    Result<BackendSession> authenticate(const LoginCredentials& c) override {
        return onAuth(c);
    }
};

// A BYOK provider validator that always accepts.
class AcceptingValidator : public ByokProviderValidator {
public:
    Result<void> validate(const ByokCredential&) override { return ok(); }
};

GenerationRequest videoRequest(std::string prompt = "a cat surfing a wave") {
    return GenerationRequest{"veo", GenerationMediaType::Video, std::move(prompt), {}};
}

MediaAsset videoAsset(const std::string& path = "/tmp/generated.mp4") {
    return MediaAsset{MediaAssetRef{Uuid::generateV4(), path}, GenerationMediaType::Video};
}

// A project with one video track and a valid 24fps timeline, wrapped in an
// engine. Returns the engine and the track id.
struct EngineFixture {
    std::unique_ptr<TimelineEngine> engine;
    Uuid                            trackId;
};

EngineFixture makeEngine() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.timelineFps = FrameRate::fps24();
    project.canvas = Resolution{1920, 1080};

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    project.tracks.push_back(track);

    EngineFixture fx;
    fx.trackId = track.id;
    fx.engine = std::make_unique<TimelineEngine>(std::move(project));
    return fx;
}

GenerationPlacement placementAt(Uuid trackId, std::int64_t frame) {
    GenerationPlacement p;
    p.trackId = trackId;
    p.framePosition = frame;
    p.sourceIn = Duration::zero();
    p.sourceOut = Duration::fromSeconds(2.0);
    return p;
}

// ===========================================================================
// Prompt validation (Requirement 6.7)
// ===========================================================================

TEST(GenerativeMediaCoordinatorTest, ValidatePromptAcceptsInRange) {
    EXPECT_TRUE(GenerativeMediaCoordinator::validatePrompt("x").isOk());
    EXPECT_TRUE(GenerativeMediaCoordinator::validatePrompt(std::string(2000, 'a')).isOk());
}

TEST(GenerativeMediaCoordinatorTest, ValidatePromptRejectsEmpty) {
    Result<void> r = GenerativeMediaCoordinator::validatePrompt("");
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message().find("2000"), std::string::npos); // indicates allowed length
}

TEST(GenerativeMediaCoordinatorTest, ValidatePromptRejectsOverLength) {
    Result<void> r = GenerativeMediaCoordinator::validatePrompt(std::string(2001, 'a'));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(GenerativeMediaCoordinatorTest, EmptyPromptRejectedLeavesTimelineUntouched) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return GenerationAuthorization{"bearer"};
    };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(""), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    // Nothing downstream ran: no auth, no generation, no library add, no placement.
    EXPECT_EQ(gate.calls, 0);
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(placement.calls, 0);
    EXPECT_EQ(library.assetCount(), 0u);
}

TEST(GenerativeMediaCoordinatorTest, OverLengthPromptRejectedBeforeGenerating) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return GenerationAuthorization{"bearer"};
    };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    Result<GeneratedMediaPlacement> result = coordinator.generateAndPlace(
        videoRequest(std::string(2001, 'a')), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(placement.calls, 0);
}

// ===========================================================================
// Entitlement gating (Requirements 6.5, 9.7)
// ===========================================================================

TEST(GenerativeMediaCoordinatorTest, UnauthorizedRequestRejectedTimelineUnchanged) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return err<GenerationAuthorization>(makeError(
            ErrorCode::Unauthenticated, "an active subscription or BYOK credentials are required"));
    };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unauthenticated);
    // Generation never ran and the timeline/library were never touched.
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(placement.calls, 0);
    EXPECT_EQ(library.assetCount(), 0u);
}

TEST(GenerativeMediaCoordinatorTest, AuthorizedTokenIsForwardedToRunner) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return GenerationAuthorization{"bearer-xyz"};
    };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(runner.calls, 1);
    EXPECT_EQ(runner.lastToken, "bearer-xyz");
}

// --- Real AuthServiceGenerationGate over AuthenticationService ---------------

TEST(GenerativeMediaCoordinatorTest, GateAuthorizesActiveSubscription) {
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"session-token", EntitlementStatus::Active};
    };
    AuthenticationService auth(backend);
    ASSERT_TRUE(auth.login(LoginCredentials{"user", "pw"}).isOk());

    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isOk());
    EXPECT_EQ(decision.value().authToken, "session-token");
}

TEST(GenerativeMediaCoordinatorTest, GateRejectsWhenNotAuthenticated) {
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::Active};
    };
    AuthenticationService auth(backend); // never logged in

    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isError());
    EXPECT_EQ(decision.error().code(), ErrorCode::Unauthenticated);
}

TEST(GenerativeMediaCoordinatorTest, GateRejectsExpiredSubscriptionWithoutByok) {
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::Expired};
    };
    AuthenticationService auth(backend);
    ASSERT_TRUE(auth.login(LoginCredentials{"user", "pw"}).isOk());

    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isError());
    EXPECT_EQ(decision.error().code(), ErrorCode::Unauthenticated);
}

TEST(GenerativeMediaCoordinatorTest, GateAuthorizesValidByokWithoutSubscription) {
    // No login: entitlement path is unavailable, so BYOK must carry the request.
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::None};
    };
    AuthenticationService auth(backend);

    AcceptingValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "user");
    ASSERT_TRUE(byok.saveCredential(ByokCredential{"veo", "sk-live-123"}).isOk());
    auth.setByokManager(byok);
    ASSERT_TRUE(auth.isByokAuthorized("veo"));

    // Provider is derived from the model id ("veo") by default.
    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isOk());
    EXPECT_FALSE(decision.value().authToken.empty());
}

// ===========================================================================
// Successful generate-and-place through the real TimelineEnginePlacer (6.2)
// ===========================================================================

TEST(GenerativeMediaCoordinatorTest, SuccessAddsToLibraryAndPlacesOnTimeline) {
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return GenerationAuthorization{"bearer"};
    };
    MockRunner runner;
    const MediaAsset asset = videoAsset("/tmp/gen.mp4");
    runner.onGenerate = [asset](const GenerationRequest&, std::string_view) { return asset; };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    // Place at frame 24 == 1.0s on a 24fps timeline (empty timeline -> valid).
    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isOk());
    // 6.2 — library received the generated asset.
    EXPECT_EQ(library.assetCount(), 1u);
    EXPECT_TRUE(library.hasAsset(asset.ref.assetId));
    // 6.2 — the clip is on the timeline at the resolved position.
    const Project after = fx.engine->snapshot();
    ASSERT_EQ(after.tracks.size(), 1u);
    ASSERT_EQ(after.tracks[0].clips.size(), 1u);
    EXPECT_EQ(after.tracks[0].clips[0].id, result.value().clipId);
    EXPECT_EQ(after.tracks[0].clips[0].assetRef.assetId, asset.ref.assetId);
    EXPECT_EQ(after.tracks[0].clips[0].timelineStart, Duration::zero());
}

TEST(GenerativeMediaCoordinatorTest, PlacementResolvesFramePositionViaFrameRate) {
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    // Seed a clip [0s,2s) so the timeline has a 2s duration, allowing placement
    // at frame 48 (== 2.0s at 24fps), the timeline end boundary.
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return videoAsset("/tmp/first.mp4");
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);
    ASSERT_TRUE(coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0)).isOk());

    // Second placement at frame 48 == 2.0s (right after the first clip ends).
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return videoAsset("/tmp/second.mp4");
    };
    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 48));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().timelineStart, Duration::fromSeconds(2.0));
    const Project after = fx.engine->snapshot();
    EXPECT_EQ(after.tracks[0].clips.size(), 2u);
}

TEST(GenerativeMediaCoordinatorTest, PlacementBeyondTimelineDurationRejected) {
    EngineFixture fx = makeEngine(); // empty timeline -> duration 0
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    // Frame 24 == 1.0s is beyond an empty (zero-duration) timeline.
    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 24));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
    // Timeline is unchanged.
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 0u);
}

TEST(GenerativeMediaCoordinatorTest, PlacementOnUnknownTrackRejected) {
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 0u);
}

// Requirement 12.9: a syntactically valid but unknown track must be refused
// BEFORE any generation runs — not merely before the clip lands on the
// timeline. This is the gap task 10.6 documented and left open: generation used
// to run (and the asset used to reach the media library) before
// TimelineEnginePlacer::place ever looked the track up.
TEST(GenerativeMediaCoordinatorTest, UnknownTrackRejectedBeforeGeneratingOrTouchingLibrary) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) {
        return GenerationAuthorization{"bearer"};
    };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    placement.knownTrack = false;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(Uuid::generateV4(), 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    // Nothing downstream ran: no entitlement check, no generation, no library
    // add, and place() itself was never called.
    EXPECT_EQ(gate.calls, 0);
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(placement.calls, 0);
    EXPECT_EQ(library.assetCount(), 0u);
}

TEST(GenerativeMediaCoordinatorTest, GenerationFailureLeavesLibraryAndTimelineUnchanged) {
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return err<MediaAsset>(makeError(ErrorCode::Unknown, "provider rejected the prompt"));
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isError());
    // 6.6 — a provider failure leaves the timeline and library unchanged.
    EXPECT_EQ(library.assetCount(), 0u);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 0u);
}

TEST(GenerativeMediaCoordinatorTest, InvalidSourceRangeRejectedBeforeGenerating) {
    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    RecordingPlacement placement;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placement);

    GenerationPlacement where = placementAt(Uuid::generateV4(), 0);
    where.sourceOut = where.sourceIn; // zero-length source range
    Result<GeneratedMediaPlacement> result = coordinator.generateAndPlace(videoRequest(), where);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(placement.calls, 0);
}

} // namespace
