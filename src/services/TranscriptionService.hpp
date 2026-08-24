// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/TranscriptionService.hpp — audio-to-text transcription producing
// time-aligned segments (Requirement 4: 4.1-4.6).
//
// The Transcription_Service (design.md Glossary / component list) "converts audio
// in media clips into time-aligned text". The actual speech-to-text engine is an
// external concern (a hosted model or a bundled recognizer); this service owns the
// *editor-side* policy that the requirements pin down and keeps it independent of
// any particular recognizer:
//
//   * 4.1 — each produced segment carries a start/end time in MILLISECONDS
//           relative to the start of the source clip, with start strictly < end.
//   * 4.2 — segments are arranged in non-decreasing start-time order and no two
//           segments overlap in time.
//   * 4.3 — a successful transcription is associated with the source clip so the
//           segments are retrievable via that clip (segmentsFor / transcriptFor).
//   * 4.4 — a clip with no detectable audio track yields an empty transcript, the
//           source clip's stored segments are left unchanged, and the outcome
//           carries a "no audio found" indication (TranscriptionStatus::NoAudioFound).
//   * 4.5 — if transcription fails for a clip that *does* have audio, the clip's
//           existing stored segments are left unchanged and an error is returned
//           (the "did not complete" indication).
//   * 4.6 — the transcription budget is 60 seconds of wall-clock per minute of
//           source audio; transcriptionBudget() computes that deadline for callers.
//
// The recognizer is abstracted behind ITranscriptionBackend so the ordering,
// validation, association, empty-audio, and failure logic is fully unit-testable
// with a mock backend and no external dependency. This header depends only on the
// domain core (Result/Error, Duration, Uuid/ClipId), so it compiles and tests on
// any platform.

#ifndef PALMIER_SERVICES_TRANSCRIPTIONSERVICE_HPP
#define PALMIER_SERVICES_TRANSCRIPTIONSERVICE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Clip.hpp"      // ClipId
#include "core/Duration.hpp"
#include "core/Result.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// TextSegment
// ---------------------------------------------------------------------------

/// A single time-aligned span of transcribed text. `startMs`/`endMs` are
/// expressed in milliseconds relative to the start of the source clip
/// (Requirement 4.1). A well-formed segment has `0 <= startMs < endMs`.
struct TextSegment {
    std::int64_t startMs = 0;   ///< Inclusive start, ms from clip start.
    std::int64_t endMs = 0;     ///< Exclusive end, ms from clip start.
    std::string  text;          ///< Recognized text for this span.

    /// True iff the span is well-formed: non-negative start strictly before end.
    [[nodiscard]] bool isValid() const noexcept { return startMs >= 0 && startMs < endMs; }

    [[nodiscard]] std::int64_t durationMs() const noexcept { return endMs - startMs; }

    friend bool operator==(const TextSegment& a, const TextSegment& b) {
        return a.startMs == b.startMs && a.endMs == b.endMs && a.text == b.text;
    }
    friend bool operator!=(const TextSegment& a, const TextSegment& b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// Transcription outcome
// ---------------------------------------------------------------------------

/// The kind of outcome a completed transcription request produced.
enum class TranscriptionStatus {
    Transcribed,   ///< Audio was present and segments were produced (possibly empty for silence).
    NoAudioFound,  ///< The clip had no detectable audio track (Requirement 4.4).
};

/// The result of a successful transcribe() call, associated with its source clip
/// (Requirement 4.3). For NoAudioFound the segment list is always empty.
struct Transcript {
    ClipId                   clipId;                                   ///< The source clip these segments belong to.
    TranscriptionStatus      status = TranscriptionStatus::Transcribed;
    std::vector<TextSegment> segments;                                 ///< Ordered, non-overlapping (Requirement 4.2).

    /// True when the clip carried a detectable audio track.
    [[nodiscard]] bool hasAudio() const noexcept {
        return status == TranscriptionStatus::Transcribed;
    }
    /// True when no detectable audio was found (the 4.4 indication).
    [[nodiscard]] bool noAudioFound() const noexcept {
        return status == TranscriptionStatus::NoAudioFound;
    }
    [[nodiscard]] bool empty() const noexcept { return segments.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return segments.size(); }
};

// ---------------------------------------------------------------------------
// Audio source description
// ---------------------------------------------------------------------------

/// Describes the audio to transcribe for a clip. The caller (media engine / UI)
/// determines whether the clip has a detectable audio track and how long it is;
/// the service uses `hasAudioTrack` to decide the empty-transcript path (4.4) and
/// `duration` to compute the time budget (4.6).
struct TranscriptionAudioSource {
    bool     hasAudioTrack = false;  ///< Whether the clip carries a detectable audio track.
    Duration duration;               ///< Length of the audio (relative to clip start).
};

// ---------------------------------------------------------------------------
// Recognizer seam
// ---------------------------------------------------------------------------

/// The external speech-to-text engine, abstracted so the service's ordering /
/// validation / association / failure policy is testable with a mock. The service
/// only invokes the backend when a detectable audio track is present, so
/// implementations may assume `source.hasAudioTrack == true`.
///
/// Returning an Error models a transcription failure (Requirement 4.5). Returning
/// a segment vector models success; the service is responsible for ordering and
/// validating those segments — the backend need not pre-sort them.
class ITranscriptionBackend {
public:
    virtual ~ITranscriptionBackend() = default;

    /// Transcribe the supplied audio into raw (possibly unordered) segments.
    [[nodiscard]] virtual Result<std::vector<TextSegment>> transcribe(
        const TranscriptionAudioSource& source) = 0;
};

// ---------------------------------------------------------------------------
// UnavailableTranscriptionBackend
// ---------------------------------------------------------------------------

/// The backend installed when no real recognizer is bundled — mirrors
/// `UnavailableGenerativeHttpTransport` (services/GenerativeHttpTransport.cpp)
/// exactly: it always fails with `Unsupported`, naming what is missing, rather
/// than the Composition_Root simply never constructing a `TranscriptionService`
/// at all. This is what lets Requirement 10.4's "the Composition_Root SHALL
/// construct services::TranscriptionService" hold literally even in a build
/// with no recognizer configured, while Requirement 10.5's "report that
/// precondition by name... and SHALL NOT prevent captions from being authored
/// by hand" is exactly the failure this class reports (`transcribe()`'s own
/// EditCommand path — SetCaptionTextCommand and friends — never touches this
/// class at all, so hand-authoring is never affected by whether a real
/// backend is installed).
class UnavailableTranscriptionBackend final : public ITranscriptionBackend {
public:
    [[nodiscard]] Result<std::vector<TextSegment>> transcribe(
        const TranscriptionAudioSource&) override {
        return err<std::vector<TextSegment>>(makeError(
            ErrorCode::Unsupported,
            "no recognizer backend is available in this build; no transcription was "
            "attempted"));
    }
};

// ---------------------------------------------------------------------------
// TranscriptionService
// ---------------------------------------------------------------------------

/// Owns the editor-side transcription policy for the current project. The
/// `backend` reference must outlive the service.
class TranscriptionService {
public:
    /// Number of wall-clock seconds allotted per minute of source audio
    /// (Requirement 4.6).
    static constexpr std::int64_t kBudgetSecondsPerAudioMinute = 60;

    explicit TranscriptionService(ITranscriptionBackend& backend);

    /// Transcribe `source` for the clip identified by `clipId`.
    ///
    /// Behavior by case:
    ///   * No detectable audio track (source.hasAudioTrack == false): returns a
    ///     Transcript with status NoAudioFound and no segments, and leaves any
    ///     previously stored transcript for `clipId` unchanged (Requirement 4.4).
    ///   * Audio present, backend succeeds: the returned segments are sorted into
    ///     non-decreasing start order and validated (each start < end, no overlap;
    ///     Requirements 4.1, 4.2). On success the transcript is stored and
    ///     associated with `clipId` (Requirement 4.3) and returned with status
    ///     Transcribed.
    ///   * Audio present, backend fails OR returns malformed/overlapping segments:
    ///     an Error is returned and the clip's existing stored segments are left
    ///     unchanged (Requirement 4.5).
    [[nodiscard]] Result<Transcript> transcribe(ClipId clipId,
                                                const TranscriptionAudioSource& source);

    /// True iff a transcript has been stored for `clipId` via a successful
    /// transcription (Requirement 4.3).
    [[nodiscard]] bool hasTranscript(ClipId clipId) const;

    /// The stored transcript for `clipId`, or std::nullopt when none exists.
    [[nodiscard]] std::optional<Transcript> transcriptFor(ClipId clipId) const;

    /// The stored segments for `clipId`, or nullptr when no transcript exists.
    /// The pointer is invalidated by any subsequent transcribe()/forget() call.
    [[nodiscard]] const std::vector<TextSegment>* segmentsFor(ClipId clipId) const;

    /// Forget any stored transcript for `clipId`. Idempotent.
    void forget(ClipId clipId);

    /// The wall-clock budget for transcribing `audioDuration` of source audio:
    /// 60 seconds per minute of audio, i.e. a 1:1 ratio (Requirement 4.6). A
    /// non-positive duration yields a zero budget.
    [[nodiscard]] static Duration transcriptionBudget(Duration audioDuration);

private:
    ITranscriptionBackend&                    backend_;
    std::unordered_map<ClipId, Transcript>    transcripts_;
};

} // namespace palmier::services

#endif // PALMIER_SERVICES_TRANSCRIPTIONSERVICE_HPP
