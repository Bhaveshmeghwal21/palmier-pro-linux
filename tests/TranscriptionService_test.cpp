// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/TranscriptionService_test.cpp — unit tests for the transcription policy
// (task 11.1; Requirements 4.1-4.6). The external recognizer is replaced with a
// scriptable mock backend so the service's ordering, validation, association,
// empty-audio, and failure behavior is exercised in isolation, without any
// speech-to-text engine.
//
// Focus areas:
//   * 4.1/4.2 — segments carry start < end and are arranged in non-decreasing
//               start order with no overlap.
//   * 4.3     — a successful transcription is associated with (retrievable via)
//               its source clip.
//   * 4.4     — a clip with no detectable audio yields an empty transcript + the
//               "no audio" indication and leaves stored segments unchanged.
//   * 4.5     — a backend failure leaves the clip's existing segments unchanged
//               and returns the "did not complete" indication.
//   * 4.6     — transcriptionBudget() is 60s per minute of audio (1:1).

#include "services/TranscriptionService.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {
namespace {

// A scriptable recognizer: it either returns a preset segment list (success) or a
// preset Error (failure), and records how many times it was invoked so tests can
// assert the service never calls it on the no-audio path.
class MockBackend : public ITranscriptionBackend {
public:
    Result<std::vector<TextSegment>> transcribe(const TranscriptionAudioSource&) override {
        ++calls;
        if (error.has_value()) {
            return err<std::vector<TextSegment>>(*error);
        }
        return segments;
    }

    std::vector<TextSegment>  segments;              // returned on success
    std::optional<Error>      error;                 // when set, returned instead
    int                       calls = 0;
};

TranscriptionAudioSource withAudio(std::int64_t ms = 60'000) {
    return TranscriptionAudioSource{/*hasAudioTrack=*/true, Duration::fromMilliseconds(ms)};
}

TranscriptionAudioSource withoutAudio() {
    return TranscriptionAudioSource{/*hasAudioTrack=*/false, Duration::zero()};
}

// ---- Requirement 4.1 / 4.2: ordering and validity -------------------------

TEST(TranscriptionOrdering, SortsSegmentsIntoNonDecreasingStartOrder) {
    MockBackend backend;
    // Deliberately out of order.
    backend.segments = {
        {2000, 3000, "third"},
        {0, 1000, "first"},
        {1000, 2000, "second"},
    };
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isOk());
    const auto& segs = result.value().segments;

    ASSERT_EQ(segs.size(), 3u);
    EXPECT_EQ(segs[0].text, "first");
    EXPECT_EQ(segs[1].text, "second");
    EXPECT_EQ(segs[2].text, "third");

    // Non-decreasing start order and each start < end, no overlap.
    for (std::size_t i = 0; i < segs.size(); ++i) {
        EXPECT_LT(segs[i].startMs, segs[i].endMs);
        if (i > 0) {
            EXPECT_LE(segs[i - 1].startMs, segs[i].startMs);
            EXPECT_LE(segs[i - 1].endMs, segs[i].startMs);  // no overlap
        }
    }
}

TEST(TranscriptionOrdering, RejectsSegmentWithStartNotBeforeEnd) {
    MockBackend backend;
    backend.segments = {
        {0, 1000, "ok"},
        {2000, 2000, "zero-length"},  // start == end -> invalid (4.1)
    };
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    // Nothing stored because the transcript was invalid.
    EXPECT_FALSE(svc.hasTranscript(clip));
}

TEST(TranscriptionOrdering, RejectsOverlappingSegments) {
    MockBackend backend;
    backend.segments = {
        {0, 1500, "a"},
        {1000, 2000, "b"},  // overlaps [0,1500)
    };
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
    EXPECT_FALSE(svc.hasTranscript(clip));
}

TEST(TranscriptionOrdering, AbuttingSegmentsAreNotConsideredOverlapping) {
    MockBackend backend;
    backend.segments = {
        {0, 1000, "a"},
        {1000, 2000, "b"},  // starts exactly where "a" ends -> allowed
    };
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().segments.size(), 2u);
}

// ---- Requirement 4.3: association with the source clip --------------------

TEST(TranscriptionAssociation, SegmentsRetrievableViaSourceClip) {
    MockBackend backend;
    backend.segments = {{0, 1000, "hello"}, {1000, 2000, "world"}};
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    ASSERT_FALSE(svc.hasTranscript(clip));

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value().clipId, clip);

    EXPECT_TRUE(svc.hasTranscript(clip));
    const auto stored = svc.transcriptFor(clip);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->clipId, clip);
    EXPECT_TRUE(stored->hasAudio());

    const auto* segs = svc.segmentsFor(clip);
    ASSERT_NE(segs, nullptr);
    ASSERT_EQ(segs->size(), 2u);
    EXPECT_EQ((*segs)[0].text, "hello");
    EXPECT_EQ((*segs)[1].text, "world");
}

TEST(TranscriptionAssociation, DistinctClipsKeepSeparateTranscripts) {
    MockBackend backend;
    TranscriptionService svc(backend);
    const ClipId a = Uuid::generateV4();
    const ClipId b = Uuid::generateV4();

    backend.segments = {{0, 500, "clip-a"}};
    ASSERT_TRUE(svc.transcribe(a, withAudio()).isOk());

    backend.segments = {{0, 500, "clip-b"}};
    ASSERT_TRUE(svc.transcribe(b, withAudio()).isOk());

    ASSERT_NE(svc.segmentsFor(a), nullptr);
    ASSERT_NE(svc.segmentsFor(b), nullptr);
    EXPECT_EQ((*svc.segmentsFor(a))[0].text, "clip-a");
    EXPECT_EQ((*svc.segmentsFor(b))[0].text, "clip-b");
}

// ---- Requirement 4.4: no detectable audio ---------------------------------

TEST(TranscriptionNoAudio, ReturnsEmptyTranscriptWithIndicationAndSkipsBackend) {
    MockBackend backend;
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withoutAudio());
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().noAudioFound());
    EXPECT_FALSE(result.value().hasAudio());
    EXPECT_TRUE(result.value().empty());
    EXPECT_EQ(result.value().clipId, clip);

    // The recognizer is never consulted when there is no audio track.
    EXPECT_EQ(backend.calls, 0);
    // No transcript is stored for a no-audio clip.
    EXPECT_FALSE(svc.hasTranscript(clip));
}

TEST(TranscriptionNoAudio, LeavesExistingSegmentsUnchanged) {
    MockBackend backend;
    backend.segments = {{0, 1000, "original"}};
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    // First transcribe successfully with audio.
    ASSERT_TRUE(svc.transcribe(clip, withAudio()).isOk());
    ASSERT_TRUE(svc.hasTranscript(clip));

    // A subsequent no-audio request must not clear the stored transcript (4.4).
    auto result = svc.transcribe(clip, withoutAudio());
    ASSERT_TRUE(result.isOk());
    EXPECT_TRUE(result.value().noAudioFound());

    const auto* segs = svc.segmentsFor(clip);
    ASSERT_NE(segs, nullptr);
    ASSERT_EQ(segs->size(), 1u);
    EXPECT_EQ((*segs)[0].text, "original");
}

// ---- Requirement 4.5: failure preserves existing segments -----------------

TEST(TranscriptionFailure, ReturnsErrorAndStoresNothingWhenNoPriorTranscript) {
    MockBackend backend;
    backend.error = makeError(ErrorCode::Internal, "recognizer crashed");
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);
    EXPECT_FALSE(svc.hasTranscript(clip));
}

TEST(TranscriptionFailure, LeavesExistingSegmentsUnchangedOnLaterFailure) {
    MockBackend backend;
    backend.segments = {{0, 1000, "kept"}, {1000, 2000, "also-kept"}};
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    // First a successful transcription establishes stored segments.
    ASSERT_TRUE(svc.transcribe(clip, withAudio()).isOk());
    ASSERT_TRUE(svc.hasTranscript(clip));

    // Now the recognizer fails on a re-transcription request.
    backend.error = makeError(ErrorCode::Timeout, "transcription did not complete");
    auto result = svc.transcribe(clip, withAudio());
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);

    // The previously stored segments are left unchanged (4.5).
    const auto* segs = svc.segmentsFor(clip);
    ASSERT_NE(segs, nullptr);
    ASSERT_EQ(segs->size(), 2u);
    EXPECT_EQ((*segs)[0].text, "kept");
    EXPECT_EQ((*segs)[1].text, "also-kept");
}

// ---- Requirement 4.6: time budget -----------------------------------------

TEST(TranscriptionBudget, IsSixtySecondsPerMinuteOfAudio) {
    // One minute of audio -> 60 seconds of budget (1:1).
    EXPECT_EQ(TranscriptionService::transcriptionBudget(Duration::fromSeconds(60)).seconds(),
              60.0);
    // Two minutes -> 120 seconds.
    EXPECT_EQ(TranscriptionService::transcriptionBudget(Duration::fromSeconds(120)).milliseconds(),
              120'000);
}

TEST(TranscriptionBudget, NonPositiveDurationYieldsZeroBudget) {
    EXPECT_TRUE(TranscriptionService::transcriptionBudget(Duration::zero()).isZero());
    EXPECT_TRUE(
        TranscriptionService::transcriptionBudget(Duration::fromMilliseconds(-5)).isZero());
}

// ---- forget() -------------------------------------------------------------

TEST(TranscriptionForget, RemovesStoredTranscript) {
    MockBackend backend;
    backend.segments = {{0, 1000, "x"}};
    TranscriptionService svc(backend);
    const ClipId clip = Uuid::generateV4();

    ASSERT_TRUE(svc.transcribe(clip, withAudio()).isOk());
    ASSERT_TRUE(svc.hasTranscript(clip));

    svc.forget(clip);
    EXPECT_FALSE(svc.hasTranscript(clip));
    EXPECT_EQ(svc.segmentsFor(clip), nullptr);

    // Idempotent: forgetting an unknown clip is a no-op.
    svc.forget(Uuid::generateV4());
}

}  // namespace
}  // namespace palmier::services
