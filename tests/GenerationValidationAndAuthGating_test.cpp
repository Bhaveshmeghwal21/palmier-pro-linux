// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/GenerationValidationAndAuthGating_test.cpp — targeted unit tests for
// generation validation and auth gating (task 14.4; Requirements 6.5, 6.7, 6.8).
//
// The submit/poll/fetch lifecycle (GenerativeClient) and the coordinator's
// prompt validation / entitlement gating / placement (GenerativeMediaCoordinator)
// already have broad coverage in GenerativeClient_test.cpp and
// GenerativeMediaCoordinator_test.cpp. This file deliberately does NOT re-test
// what those cover; it fills the specific gaps task 14.4 calls out, exercising
// the branches those suites leave untouched:
//
//   * 6.7 prompt-length bounds — the boundary lengths (exactly kMinPromptLength
//         and exactly kMaxPromptLength) accepted through the *full*
//         generate-and-place pipeline (existing tests only check the boundaries
//         via the static validatePrompt() and reject-paths through the flow),
//         plus the length-constant contract.
//   * 6.8 timeout cancellation — fetchResult() itself detecting the elapsed
//         budget with no preceding poll(), the GenerativeClientRunner adapter
//         forwarding a client Timeout, and a Timeout surfacing through
//         GenerativeMediaCoordinator::generateAndPlace leaving state unchanged.
//   * 6.5 subscription/BYOK gating — the AuthServiceGenerationGate branches not
//         covered elsewhere (logged-in-but-expired subscription rescued by a
//         valid BYOK key using the session bearer; a custom provider resolver;
//         BYOK scoped to a different provider is not honored) and the real gate
//         driving the coordinator end-to-end (unauthenticated rejected /
//         active-subscription authorized).
//
// Everything runs through the narrow seams with mocks, an in-memory secret
// store, and an injectable clock — no real network, GPU, auth backend, or
// libsecret.

#include "services/GenerativeClient.hpp"
#include "services/GenerativeMediaCoordinator.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

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
using palmier::services::GeneratedMediaPlacement;
using palmier::services::GenerationAuthorization;
using palmier::services::GenerationMediaType;
using palmier::services::GenerationPhase;
using palmier::services::GenerationPlacement;
using palmier::services::GenerationRequest;
using palmier::services::GenerationStatus;
using palmier::services::GenerativeClient;
using palmier::services::GenerativeClientRunner;
using palmier::services::GenerativeMediaCoordinator;
using palmier::services::IGenerationGate;
using palmier::services::IGenerationRunner;
using palmier::services::IGenerativeBackend;
using palmier::services::InMemorySecretStore;
using palmier::services::ITimelinePlacement;
using palmier::services::JobId;
using palmier::services::LoginCredentials;
using palmier::services::MediaAsset;
using palmier::services::TimelineEnginePlacer;
using palmier::services::TimelinePlacementRequest;
using palmier::services::kMaxPromptLength;
using palmier::services::kMinPromptLength;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// A GenerativeClient backend whose per-call behavior is scripted, recording the
// number of cancel invocations so the timeout path can be asserted.
class MockBackend : public IGenerativeBackend {
public:
    std::function<Result<JobId>(const GenerationRequest&, std::string_view)>  onSubmit;
    std::function<Result<GenerationStatus>(const JobId&, std::string_view)>   onPoll;
    std::function<Result<MediaAsset>(const JobId&, std::string_view)>         onFetch;
    int cancelCalls = 0;
    int fetchCalls  = 0;

    Result<JobId> submit(const GenerationRequest& r, std::string_view t) override {
        return onSubmit(r, t);
    }
    Result<GenerationStatus> poll(const JobId& id, std::string_view t) override {
        return onPoll(id, t);
    }
    Result<MediaAsset> fetchResult(const JobId& id, std::string_view t) override {
        ++fetchCalls;
        return onFetch(id, t);
    }
    Result<void> cancel(const JobId&, std::string_view) override {
        ++cancelCalls;
        return ok();
    }
};

// A manually advanced steady clock for deterministic timeout tests.
class FakeClock {
public:
    GenerativeClient::TimePoint now() const { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }
    GenerativeClient::Clock fn() { return [this] { return now(); }; }

private:
    GenerativeClient::TimePoint now_{};
};

// A gate scripted per test; records whether it was consulted.
class MockGate : public IGenerationGate {
public:
    std::function<Result<GenerationAuthorization>(const GenerationRequest&)> onAuthorize;
    mutable int calls = 0;
    Result<GenerationAuthorization> authorize(const GenerationRequest& r) const override {
        ++calls;
        return onAuthorize(r);
    }
};

// A runner scripted per test; records whether it ran.
class MockRunner : public IGenerationRunner {
public:
    std::function<Result<MediaAsset>(const GenerationRequest&, std::string_view)> onGenerate;
    int calls = 0;
    Result<MediaAsset> generate(const GenerationRequest& r, std::string_view t) override {
        ++calls;
        return onGenerate(r, t);
    }
};

// A placement seam recording whether the timeline was ever touched.
class RecordingPlacement : public ITimelinePlacement {
public:
    int calls = 0;
    Result<GeneratedMediaPlacement> place(const TimelinePlacementRequest& r) override {
        ++calls;
        return GeneratedMediaPlacement{r.asset, Uuid::generateV4(), Duration::zero()};
    }
};

// Scriptable auth backend for the real AuthServiceGenerationGate.
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

// ---------------------------------------------------------------------------
// Fixtures / helpers
// ---------------------------------------------------------------------------

GenerationRequest videoRequest(std::string prompt = "a cat surfing a wave") {
    return GenerationRequest{"veo", GenerationMediaType::Video, std::move(prompt), {}};
}

MediaAsset videoAsset(const std::string& path = "/tmp/generated.mp4") {
    return MediaAsset{MediaAssetRef{Uuid::generateV4(), path}, GenerationMediaType::Video};
}

MockBackend makeRunningBackend(const std::string& jobId) {
    MockBackend b;
    b.onSubmit = [jobId](const GenerationRequest&, std::string_view) -> Result<JobId> {
        return JobId{jobId};
    };
    b.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Running, 10, {}};
    };
    b.onFetch = [](const JobId&, std::string_view) -> Result<MediaAsset> {
        return videoAsset();
    };
    return b;
}

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

constexpr const char* kToken = "bearer-abc123";

// ===========================================================================
// 6.7 — prompt-length bounds (boundary + contract cases)
// ===========================================================================

TEST(GenerationValidationTest, PromptLengthConstantsMatchRequirement) {
    // Requirement 6.7 pins the allowed prompt length to 1..2000 characters.
    EXPECT_EQ(kMinPromptLength, 1u);
    EXPECT_EQ(kMaxPromptLength, 2000u);
}

TEST(GenerationValidationTest, MinLengthPromptAcceptedThroughGenerateAndPlace) {
    // A single-character prompt is the lower boundary of the allowed range and
    // must flow all the way through generate-and-place (existing tests only
    // check the lower boundary via the static validatePrompt()).
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return videoAsset("/tmp/min.mp4");
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest("x"), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(runner.calls, 1);
    EXPECT_EQ(library.assetCount(), 1u);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 1u);
}

TEST(GenerationValidationTest, MaxLengthPromptAcceptedThroughGenerateAndPlace) {
    // Exactly kMaxPromptLength (2000) characters is the inclusive upper boundary
    // and must be accepted end-to-end, not merely by validatePrompt().
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return videoAsset("/tmp/max.mp4");
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    const std::string maxPrompt(kMaxPromptLength, 'a');
    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(maxPrompt), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(runner.calls, 1);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 1u);
}

// ===========================================================================
// 6.8 — timeout cancellation (branches not covered elsewhere)
// ===========================================================================

TEST(GenerationTimeoutTest, FetchResultDetectsElapsedBudgetAndCancels) {
    // fetchResult() must enforce the 300-second budget itself, even when the
    // caller never poll()ed after the budget elapsed: a still-non-terminal job
    // is cancelled at the backend and reported as a Timeout, and no result is
    // fetched (leaving project state unchanged, Requirement 6.8).
    MockBackend backend = makeRunningBackend("job-slow");
    FakeClock clock;
    GenerativeClient client(
        backend, std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds), clock.fn());
    JobId id = client.submit(videoRequest(), kToken).value().id;

    // Advance past the budget WITHOUT an intervening poll(), then fetch.
    clock.advance(std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds) +
                  std::chrono::milliseconds(1));

    Result<MediaAsset> asset = client.fetchResult(id);

    ASSERT_TRUE(asset.isError());
    EXPECT_EQ(asset.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(backend.cancelCalls, 1);   // the in-flight request was cancelled
    EXPECT_EQ(backend.fetchCalls, 0);    // no result fetched for a timed-out job
}

TEST(GenerationTimeoutTest, RunnerForwardsClientTimeout) {
    // The GenerativeClientRunner adapter drives submit -> poll -> fetch; when the
    // underlying client trips its timeout budget between polls, the runner must
    // forward the Timeout error unchanged (Requirement 6.8).
    MockBackend backend = makeRunningBackend("job-slow"); // always Running
    FakeClock clock;
    GenerativeClient client(
        backend, std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds), clock.fn());

    // The pacer runs between non-terminal polls; advancing the clock past the
    // budget there makes the next poll trip the timeout deterministically.
    GenerativeClientRunner runner(client, [&clock] {
        clock.advance(std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds + 1));
    });

    Result<MediaAsset> result = runner.generate(videoRequest(), kToken);

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(backend.cancelCalls, 1);
    EXPECT_EQ(backend.fetchCalls, 0);
}

TEST(GenerationTimeoutTest, CoordinatorForwardsTimeoutLeavingTimelineUnchanged) {
    // A generation timeout surfaced by the runner must propagate through the
    // coordinator as a Timeout, adding nothing to the library and leaving the
    // timeline byte-for-byte unchanged (Requirement 6.8).
    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);

    MockGate gate;
    gate.onAuthorize = [](const GenerationRequest&) { return GenerationAuthorization{"b"}; };
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) {
        return err<MediaAsset>(makeError(ErrorCode::Timeout,
                                         "generation did not complete within 300 seconds"));
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(library.assetCount(), 0u);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 0u);
}

// ===========================================================================
// 6.5 — subscription / BYOK gating (branches not covered elsewhere)
// ===========================================================================

TEST(GenerationAuthGatingTest, ExpiredSubscriptionRescuedByByokUsesSessionToken) {
    // Logged in but the subscription has lapsed: a valid BYOK key for the
    // request's provider still authorizes generation, and because the user is
    // logged in the session bearer (not a byok: token) is used.
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"session-token", EntitlementStatus::Expired};
    };
    AuthenticationService auth(backend);
    ASSERT_TRUE(auth.login(LoginCredentials{"user", "pw"}).isOk());

    AcceptingValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "user");
    ASSERT_TRUE(byok.saveCredential(ByokCredential{"veo", "sk-live-123"}).isOk());
    auth.setByokManager(byok);

    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isOk());
    EXPECT_EQ(decision.value().authToken, "session-token");
}

TEST(GenerationAuthGatingTest, GateUsesCustomProviderResolverForByok) {
    // The provider a request maps to need not equal its model id: a custom
    // resolver decides. Here the "veo" model routes to the "studio" provider,
    // and only the "studio" BYOK key is present, so the request is authorized.
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::None};
    };
    AuthenticationService auth(backend); // never logged in

    AcceptingValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "user");
    ASSERT_TRUE(byok.saveCredential(ByokCredential{"studio", "sk-studio"}).isOk());
    auth.setByokManager(byok);

    AuthServiceGenerationGate gate(auth, [](const GenerationRequest&) -> std::string {
        return "studio";
    });
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isOk());
    EXPECT_FALSE(decision.value().authToken.empty());
}

TEST(GenerationAuthGatingTest, ByokForDifferentProviderDoesNotAuthorize) {
    // A BYOK key authorizes only its own provider: with a key for "anthropic"
    // but a request whose (default) provider is the model "veo", and no active
    // subscription, the request is rejected as Unauthenticated.
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::None};
    };
    AuthenticationService auth(backend);

    AcceptingValidator validator;
    InMemorySecretStore store;
    ByokCredentialManager byok(validator, store, "user");
    ASSERT_TRUE(byok.saveCredential(ByokCredential{"anthropic", "sk-ant"}).isOk());
    auth.setByokManager(byok);

    AuthServiceGenerationGate gate(auth);
    Result<GenerationAuthorization> decision = gate.authorize(videoRequest());

    ASSERT_TRUE(decision.isError());
    EXPECT_EQ(decision.error().code(), ErrorCode::Unauthenticated);
}

TEST(GenerationAuthGatingTest, RealGateRejectsUnauthenticatedThroughCoordinator) {
    // End-to-end: the real AuthServiceGenerationGate (never logged in, no BYOK)
    // wired into the coordinator rejects the request as Unauthenticated before
    // any generation runs, leaving the library and timeline untouched (6.5).
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"tok", EntitlementStatus::Active};
    };
    AuthenticationService auth(backend); // never logged in
    AuthServiceGenerationGate gate(auth);

    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);
    MockRunner runner;
    runner.onGenerate = [](const GenerationRequest&, std::string_view) { return videoAsset(); };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Unauthenticated);
    EXPECT_EQ(runner.calls, 0);
    EXPECT_EQ(library.assetCount(), 0u);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 0u);
}

TEST(GenerationAuthGatingTest, RealGateAuthorizesActiveSubscriptionThroughCoordinator) {
    // End-to-end: an active subscription authorizes generation through the real
    // gate, the session bearer is forwarded to the runner, and the generated
    // media lands on the timeline (6.5 authorize path).
    MockAuthBackend backend;
    backend.onAuth = [](const LoginCredentials&) {
        return BackendSession{"session-token", EntitlementStatus::Active};
    };
    AuthenticationService auth(backend);
    ASSERT_TRUE(auth.login(LoginCredentials{"user", "pw"}).isOk());
    AuthServiceGenerationGate gate(auth);

    EngineFixture fx = makeEngine();
    TimelineEnginePlacer placer(*fx.engine);
    MockRunner runner;
    std::string forwardedToken;
    runner.onGenerate = [&forwardedToken](const GenerationRequest&, std::string_view t) {
        forwardedToken = std::string(t);
        return videoAsset("/tmp/ok.mp4");
    };
    MediaManager library;
    GenerativeMediaCoordinator coordinator(gate, runner, library, placer);

    Result<GeneratedMediaPlacement> result =
        coordinator.generateAndPlace(videoRequest(), placementAt(fx.trackId, 0));

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(runner.calls, 1);
    EXPECT_EQ(forwardedToken, "session-token");
    EXPECT_EQ(library.assetCount(), 1u);
    EXPECT_EQ(fx.engine->snapshot().tracks[0].clips.size(), 1u);
}

} // namespace
