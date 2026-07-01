// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TranscriptionService.cpp — implementation of the transcription policy
// (Requirement 4.1-4.6). See TranscriptionService.hpp for the contract.

#include "services/TranscriptionService.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace palmier::services {

namespace {

/// Sort `segments` in non-decreasing start-time order (Requirement 4.2) and
/// verify the whole set is well-formed:
///   * every segment satisfies 0 <= startMs < endMs (Requirement 4.1), and
///   * after ordering, no two segments overlap in time (Requirement 4.2), i.e.
///     each segment starts at or after the previous segment's end.
/// On any violation an Error is returned and `segments` is left in an unspecified
/// (but caller-discarded) state; on success `segments` is sorted in place.
[[nodiscard]] Result<void> orderAndValidate(std::vector<TextSegment>& segments) {
    // Non-decreasing start order; ties broken by end so the overlap check below
    // is deterministic for equal-start segments.
    std::stable_sort(segments.begin(), segments.end(),
                     [](const TextSegment& a, const TextSegment& b) {
                         if (a.startMs != b.startMs) return a.startMs < b.startMs;
                         return a.endMs < b.endMs;
                     });

    std::int64_t previousEndMs = 0;
    bool         havePrevious = false;
    for (const TextSegment& seg : segments) {
        if (!seg.isValid()) {
            return err(invalidArgument(
                "transcription produced a segment with start >= end or a negative start"));
        }
        if (havePrevious && seg.startMs < previousEndMs) {
            return err(failedPrecondition(
                "transcription produced overlapping segments"));
        }
        previousEndMs = seg.endMs;
        havePrevious = true;
    }
    return ok();
}

} // namespace

TranscriptionService::TranscriptionService(ITranscriptionBackend& backend)
    : backend_(backend) {}

Result<Transcript> TranscriptionService::transcribe(ClipId clipId,
                                                    const TranscriptionAudioSource& source) {
    // 4.4: no detectable audio track -> empty transcript + "no audio" indication,
    // leaving any previously stored transcript for this clip untouched.
    if (!source.hasAudioTrack) {
        Transcript outcome;
        outcome.clipId = clipId;
        outcome.status = TranscriptionStatus::NoAudioFound;
        // Intentionally does NOT modify transcripts_[clipId].
        return outcome;
    }

    // Audio present: hand off to the external recognizer.
    Result<std::vector<TextSegment>> backendResult = backend_.transcribe(source);

    // 4.5: failure leaves the clip's existing segments unchanged and returns the
    // "did not complete" indication (the error).
    if (backendResult.isError()) {
        return err<Transcript>(std::move(backendResult).error());
    }

    std::vector<TextSegment> segments = std::move(backendResult).value();

    // 4.1 / 4.2: order and validate before we commit anything. A malformed set is
    // treated like a failure: the stored transcript is left unchanged.
    if (Result<void> validation = orderAndValidate(segments); validation.isError()) {
        return err<Transcript>(std::move(validation).error());
    }

    // 4.3: associate the (now ordered, validated) segments with the source clip.
    Transcript transcript;
    transcript.clipId = clipId;
    transcript.status = TranscriptionStatus::Transcribed;
    transcript.segments = std::move(segments);

    transcripts_[clipId] = transcript;
    return transcript;
}

bool TranscriptionService::hasTranscript(ClipId clipId) const {
    return transcripts_.find(clipId) != transcripts_.end();
}

std::optional<Transcript> TranscriptionService::transcriptFor(ClipId clipId) const {
    if (auto it = transcripts_.find(clipId); it != transcripts_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const std::vector<TextSegment>* TranscriptionService::segmentsFor(ClipId clipId) const {
    if (auto it = transcripts_.find(clipId); it != transcripts_.end()) {
        return &it->second.segments;
    }
    return nullptr;
}

void TranscriptionService::forget(ClipId clipId) {
    transcripts_.erase(clipId);
}

Duration TranscriptionService::transcriptionBudget(Duration audioDuration) {
    // 60 wall-clock seconds are allotted per minute of source audio, i.e. a 1:1
    // ratio between budget and audio length. A non-positive duration -> zero.
    if (!audioDuration.isPositive()) {
        return Duration::zero();
    }
    return audioDuration;
}

} // namespace palmier::services
