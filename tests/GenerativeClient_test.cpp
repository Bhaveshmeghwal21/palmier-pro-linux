// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/GenerativeClient_test.cpp — unit tests for the client-side generative
// job lifecycle: submit/poll/fetchResult against the hosted backend using the
// auth token, the 300-second timeout+cancel budget, and provider-failure
// handling (Requirements 6.1, 6.3, 6.4, 6.6, 6.8).
//
// The hosted backend is abstracted behind IGenerativeBackend; these tests drive
// the client with a scriptable mock backend and an injectable clock so timeout
// behavior is fully deterministic and no real network is used.

#include "services/GenerativeClient.hpp"

#include <chrono>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "core/Error.hpp"
#include "core/Uuid.hpp"

namespace {

using namespace palmier;
using palmier::services::GenerationJob;
using palmier::services::GenerationMediaType;
using palmier::services::GenerationPhase;
using palmier::services::GenerationRequest;
using palmier::services::GenerationStatus;
using palmier::services::GenerativeClient;
using palmier::services::IGenerativeBackend;
using palmier::services::JobId;
using palmier::services::MediaAsset;

// A backend whose behavior is decided by injected callables, so each test can
// script submit / poll / fetch / cancel outcomes precisely, and observe the
// token each call received and how many times cancel was invoked.
class MockBackend : public IGenerativeBackend {
public:
    std::function<Result<JobId>(const GenerationRequest&, std::string_view)>       onSubmit;
    std::function<Result<GenerationStatus>(const JobId&, std::string_view)>        onPoll;
    std::function<Result<MediaAsset>(const JobId&, std::string_view)>              onFetch;
    std::function<Result<void>(const JobId&, std::string_view)>                    onCancel;

    // Observed state.
    std::string lastSubmitToken;
    std::string lastPollToken;
    std::string lastFetchToken;
    int         submitCalls = 0;
    int         pollCalls   = 0;
    int         fetchCalls  = 0;
    int         cancelCalls = 0;

    Result<JobId> submit(const GenerationRequest& r, std::string_view token) override {
        ++submitCalls;
        lastSubmitToken = std::string(token);
        return onSubmit(r, token);
    }
    Result<GenerationStatus> poll(const JobId& id, std::string_view token) override {
        ++pollCalls;
        lastPollToken = std::string(token);
        return onPoll(id, token);
    }
    Result<MediaAsset> fetchResult(const JobId& id, std::string_view token) override {
        ++fetchCalls;
        lastFetchToken = std::string(token);
        return onFetch(id, token);
    }
    Result<void> cancel(const JobId& id, std::string_view token) override {
        ++cancelCalls;
        return onCancel ? onCancel(id, token) : ok();
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

// Backend defaults that make the "happy path" trivial; individual tests
// override just the handlers they care about.
MockBackend makeBackendAcceptingJob(const std::string& jobId) {
    MockBackend b;
    b.onSubmit = [jobId](const GenerationRequest&, std::string_view) -> Result<JobId> {
        return JobId{jobId};
    };
    b.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Running, 50, {}};
    };
    b.onFetch = [](const JobId&, std::string_view) -> Result<MediaAsset> {
        return MediaAsset{MediaAssetRef{Uuid::generateV4(), "/tmp/out"},
                          GenerationMediaType::Video};
    };
    return b;
}

GenerationRequest videoRequest() {
    GenerationRequest request;
    request.model = "veo";
    request.mediaType = GenerationMediaType::Video;
    request.prompt = "a cat surfing";
    return request;
}

constexpr const char* kToken = "bearer-abc123";

// --- Requirement 6.1: submit with a SOTA model returns a tracked job ---------

TEST(GenerativeClientTest, SubmitForwardsTokenAndReturnsJobHandle) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    GenerativeClient client(backend);

    Result<GenerationJob> job = client.submit(videoRequest(), kToken);

    ASSERT_TRUE(job.isOk());
    EXPECT_EQ(job.value().id.value, "job-1");
    EXPECT_EQ(job.value().model, "veo");
    EXPECT_EQ(job.value().mediaType, GenerationMediaType::Video);
    EXPECT_EQ(backend.submitCalls, 1);
    EXPECT_EQ(backend.lastSubmitToken, kToken);          // sent over TLS as the bearer
    EXPECT_TRUE(client.tracks(job.value().id));
}

TEST(GenerativeClientTest, SubmitWithoutTokenIsRejectedWithoutContactingBackend) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    GenerativeClient client(backend);

    Result<GenerationJob> job = client.submit(videoRequest(), "");

    ASSERT_TRUE(job.isError());
    EXPECT_EQ(job.error().code(), ErrorCode::Unauthenticated);
    EXPECT_EQ(backend.submitCalls, 0);
}

TEST(GenerativeClientTest, SubmitBackendErrorIsForwardedAndNothingIsTracked) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    backend.onSubmit = [](const GenerationRequest&, std::string_view) -> Result<JobId> {
        return err<JobId>(makeError(ErrorCode::Io, "connection reset"));
    };
    GenerativeClient client(backend);

    Result<GenerationJob> job = client.submit(videoRequest(), kToken);

    ASSERT_TRUE(job.isError());
    EXPECT_EQ(job.error().code(), ErrorCode::Io);
    EXPECT_FALSE(client.tracks(JobId{"job-1"}));
}

// --- Requirement 6.1: poll drives the job to completion, reusing the token ---

TEST(GenerativeClientTest, PollReusesCapturedTokenAndReportsStatus) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;

    Result<GenerationStatus> status = client.poll(id);

    ASSERT_TRUE(status.isOk());
    EXPECT_EQ(status.value().phase, GenerationPhase::Running);
    EXPECT_EQ(status.value().progressPercent, 50);
    EXPECT_EQ(backend.lastPollToken, kToken);            // token captured at submit is reused
}

TEST(GenerativeClientTest, PollUnknownJobReturnsNotFound) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    GenerativeClient client(backend);

    Result<GenerationStatus> status = client.poll(JobId{"missing"});

    ASSERT_TRUE(status.isError());
    EXPECT_EQ(status.error().code(), ErrorCode::NotFound);
}

// --- Requirement 6.3: a video model produces a video asset -------------------

TEST(GenerativeClientTest, VideoModelProducesVideoAsset) {
    MockBackend backend = makeBackendAcceptingJob("job-v");
    backend.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Succeeded, 100, {}};
    };
    backend.onFetch = [](const JobId&, std::string_view) -> Result<MediaAsset> {
        return MediaAsset{MediaAssetRef{Uuid::generateV4(), "/tmp/video.mp4"},
                          GenerationMediaType::Video};
    };
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;

    ASSERT_TRUE(client.poll(id).isOk());
    Result<MediaAsset> asset = client.fetchResult(id);

    ASSERT_TRUE(asset.isOk());
    EXPECT_EQ(asset.value().mediaType, GenerationMediaType::Video);
    EXPECT_TRUE(asset.value().ref.isValid());
}

// --- Requirement 6.4: an image model produces an image asset -----------------

TEST(GenerativeClientTest, ImageModelProducesImageAsset) {
    MockBackend backend = makeBackendAcceptingJob("job-i");
    backend.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Succeeded, 100, {}};
    };
    backend.onFetch = [](const JobId&, std::string_view) -> Result<MediaAsset> {
        return MediaAsset{MediaAssetRef{Uuid::generateV4(), "/tmp/image.png"},
                          GenerationMediaType::Image};
    };
    GenerativeClient client(backend);
    GenerationRequest req;
    req.model = "gpt-image";
    req.mediaType = GenerationMediaType::Image;
    req.prompt = "a red door";
    JobId id = client.submit(req, kToken).value().id;

    ASSERT_TRUE(client.poll(id).isOk());
    Result<MediaAsset> asset = client.fetchResult(id);

    ASSERT_TRUE(asset.isOk());
    EXPECT_EQ(asset.value().mediaType, GenerationMediaType::Image);
}

TEST(GenerativeClientTest, MediaTypeMismatchFromProviderIsRejected) {
    MockBackend backend = makeBackendAcceptingJob("job-x");
    backend.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Succeeded, 100, {}};
    };
    // Video was requested but the provider returns an image.
    backend.onFetch = [](const JobId&, std::string_view) -> Result<MediaAsset> {
        return MediaAsset{MediaAssetRef{Uuid::generateV4(), "/tmp/wrong.png"},
                          GenerationMediaType::Image};
    };
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;
    ASSERT_TRUE(client.poll(id).isOk());

    Result<MediaAsset> asset = client.fetchResult(id);

    ASSERT_TRUE(asset.isError());
}

// --- Requirement 6.1: fetching before completion is a precondition failure ---

TEST(GenerativeClientTest, FetchBeforeSuccessReturnsFailedPrecondition) {
    MockBackend backend = makeBackendAcceptingJob("job-1"); // poll -> Running
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;
    ASSERT_TRUE(client.poll(id).isOk());

    Result<MediaAsset> asset = client.fetchResult(id);

    ASSERT_TRUE(asset.isError());
    EXPECT_EQ(asset.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_EQ(backend.fetchCalls, 0);
}

// --- Requirement 6.6: provider failure surfaces a descriptive reason ---------

TEST(GenerativeClientTest, ProviderFailureStatusSurfacesReasonOnFetch) {
    MockBackend backend = makeBackendAcceptingJob("job-f");
    backend.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return GenerationStatus{GenerationPhase::Failed, 0, "content policy violation"};
    };
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;

    Result<GenerationStatus> status = client.poll(id);
    ASSERT_TRUE(status.isOk());
    EXPECT_EQ(status.value().phase, GenerationPhase::Failed);

    Result<MediaAsset> asset = client.fetchResult(id);
    ASSERT_TRUE(asset.isError());
    EXPECT_NE(asset.error().message().find("content policy violation"), std::string::npos);
    // 6.6: no result is fetched from the provider for a failed job.
    EXPECT_EQ(backend.fetchCalls, 0);
}

TEST(GenerativeClientTest, ProviderTransportErrorOnPollIsForwarded) {
    MockBackend backend = makeBackendAcceptingJob("job-t");
    backend.onPoll = [](const JobId&, std::string_view) -> Result<GenerationStatus> {
        return err<GenerationStatus>(makeError(ErrorCode::Io, "gateway 502"));
    };
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;

    Result<GenerationStatus> status = client.poll(id);

    ASSERT_TRUE(status.isError());
    EXPECT_EQ(status.error().code(), ErrorCode::Io);
}

// --- Requirement 6.8: timeout cancels the job and reports a timeout ----------

TEST(GenerativeClientTest, ExceedingBudgetCancelsAndReportsTimeout) {
    MockBackend backend = makeBackendAcceptingJob("job-slow"); // always Running
    FakeClock clock;
    GenerativeClient client(
        backend, std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds),
        clock.fn());
    JobId id = client.submit(videoRequest(), kToken).value().id;

    // Advance just past the 300-second budget.
    clock.advance(std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds) +
                  std::chrono::milliseconds(1));

    Result<GenerationStatus> status = client.poll(id);

    ASSERT_TRUE(status.isError());
    EXPECT_EQ(status.error().code(), ErrorCode::Timeout);
    EXPECT_EQ(backend.cancelCalls, 1);                   // 6.8: the request is cancelled
}

TEST(GenerativeClientTest, TimeoutIsStickyAndDoesNotReCancel) {
    MockBackend backend = makeBackendAcceptingJob("job-slow");
    FakeClock clock;
    GenerativeClient client(
        backend, std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds),
        clock.fn());
    JobId id = client.submit(videoRequest(), kToken).value().id;
    clock.advance(std::chrono::seconds(400));

    ASSERT_TRUE(client.poll(id).isError());
    const int cancelsAfterFirst = backend.cancelCalls;

    // A subsequent poll and fetch keep reporting the timeout without re-cancelling.
    Result<GenerationStatus> again = client.poll(id);
    ASSERT_TRUE(again.isError());
    EXPECT_EQ(again.error().code(), ErrorCode::Timeout);

    Result<MediaAsset> fetch = client.fetchResult(id);
    ASSERT_TRUE(fetch.isError());
    EXPECT_EQ(fetch.error().code(), ErrorCode::Timeout);

    EXPECT_EQ(backend.cancelCalls, cancelsAfterFirst);
    EXPECT_EQ(backend.fetchCalls, 0);
}

TEST(GenerativeClientTest, WithinBudgetDoesNotTimeOut) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    FakeClock clock;
    GenerativeClient client(
        backend, std::chrono::seconds(GenerativeClient::kGenerationBudgetSeconds),
        clock.fn());
    JobId id = client.submit(videoRequest(), kToken).value().id;

    clock.advance(std::chrono::seconds(299)); // still within the 300s budget

    Result<GenerationStatus> status = client.poll(id);
    ASSERT_TRUE(status.isOk());
    EXPECT_EQ(status.value().phase, GenerationPhase::Running);
    EXPECT_EQ(backend.cancelCalls, 0);
}

// --- Explicit cancel ---------------------------------------------------------

TEST(GenerativeClientTest, ExplicitCancelInvokesBackendAndIsIdempotent) {
    MockBackend backend = makeBackendAcceptingJob("job-c");
    GenerativeClient client(backend);
    JobId id = client.submit(videoRequest(), kToken).value().id;

    Result<void> first = client.cancel(id);
    ASSERT_TRUE(first.isOk());
    EXPECT_EQ(backend.cancelCalls, 1);

    // A second cancel is a no-op success (no further backend call).
    Result<void> second = client.cancel(id);
    ASSERT_TRUE(second.isOk());
    EXPECT_EQ(backend.cancelCalls, 1);
}

TEST(GenerativeClientTest, CancelUnknownJobReturnsNotFound) {
    MockBackend backend = makeBackendAcceptingJob("job-1");
    GenerativeClient client(backend);

    Result<void> result = client.cancel(JobId{"nope"});

    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

// --- Static budget helper ----------------------------------------------------

TEST(GenerativeClientTest, GenerationBudgetIsThreeHundredSeconds) {
    EXPECT_EQ(GenerativeClient::kGenerationBudgetSeconds, 300);
    EXPECT_EQ(GenerativeClient::generationBudget().seconds(), 300.0);
}

TEST(GenerativeClientTest, MediaTypeAndPhaseNames) {
    EXPECT_EQ(palmier::services::toStringView(GenerationMediaType::Video), "video");
    EXPECT_EQ(palmier::services::toStringView(GenerationMediaType::Image), "image");
    EXPECT_EQ(palmier::services::toStringView(GenerationPhase::Pending), "pending");
    EXPECT_EQ(palmier::services::toStringView(GenerationPhase::Succeeded), "succeeded");
}

} // namespace
