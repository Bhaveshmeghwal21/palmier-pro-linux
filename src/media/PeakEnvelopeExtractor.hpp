// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelopeExtractor.hpp — build an asset's peak envelope by reading it
// through the decoder playback uses (monitoring-and-grading Requirement 2.1,
// 2.6, 2.7, 2.8).
//
// Requirement 2.8 is the reason this is a thin function over MediaDecoder rather
// than a second reader: what is DRAWN and what is HEARD must not be able to
// disagree about an asset's content. Both go through MediaDecoder's audio surface
// (openAudioStream / nextAudioFrame), so a decoder quirk — a channel layout, a
// timestamp discontinuity, a codec's priming samples — shows up identically in
// the waveform and in the mix instead of only in one of them.
//
// ## The three outcomes, and why only one of them is an error
//
//   * **An envelope.** The asset carries audio and it decoded.
//   * **An EMPTY envelope, reported as success.** The asset carries no audio
//     stream, or its audio stream yields nothing at all. Requirement 2.6: draw
//     nothing, report nothing. This mirrors the Audio_Engine, which already
//     contributes silence for an audio-less asset rather than failing
//     (`MediaDecoder::hasAudio()` exists precisely so that decision costs nothing).
//   * **An error.** The file would not open, the audio stream was refused, or a
//     decode failed. Requirement 2.7: reported once by the caller, never retried
//     per repaint, and the clip still draws and stays editable.
//
// Conflating the middle case with the last is the mistake this signature exists to
// prevent: a silent asset is not a broken one, and reporting it as a failure would
// put an error in front of the user for a video with no soundtrack.
//
// ## Testability
//
// The decoder is opened through MediaDecoder's existing DecodeBackendFactory seam,
// so every case above is reachable with a synthetic backend — no FFmpeg, no media
// file, no device. This function is deliberately synchronous and knows nothing
// about threads; running it off the UI thread is the caller's concern
// (Requirement 2.2), which keeps the reading logic testable without a scheduler.

#ifndef PALMIER_MEDIA_PEAKENVELOPEEXTRACTOR_HPP
#define PALMIER_MEDIA_PEAKENVELOPEEXTRACTOR_HPP

#include <cstddef>
#include <filesystem>

#include "core/Duration.hpp"
#include "core/Result.hpp"
#include "media/MediaDecoder.hpp"
#include "media/PeakEnvelope.hpp"

namespace palmier::media {

/// How much audio an extraction will read before giving up.
///
/// A decode loop that trusts the backend to report end-of-stream hangs forever if
/// it never does. Bounding the read converts that from a stuck worker — which in a
/// test suite is an expired job and in an editor is a leaked thread — into an
/// ordinary reported failure. The default is far past any realistic edit source.
struct EnvelopeExtractionLimits {
    /// Maximum frames folded in. 48 kHz x 3600 s x 12 = twelve hours at 48 kHz.
    std::size_t maxFrames{48'000ULL * 3600ULL * 12ULL};
    /// Maximum decode calls, bounding a backend that returns endless empty blocks
    /// without ever advancing the frame count.
    std::size_t maxBlocks{4'000'000};
};

/// Read `path`'s primary audio stream and reduce it to a peak envelope with
/// `bucketDuration`-wide buckets.
///
/// Returns an EMPTY envelope — successfully — when the asset carries no audio or
/// its stream yields nothing (Requirement 2.6). Returns an error only when the
/// file would not open, the audio stream was refused, a decode failed, or the
/// limits above were exhausted.
///
/// Errors: whatever `MediaDecoder::open` or the audio surface reports, unchanged,
/// plus `ErrorCode::OutOfRange` when a limit is hit (naming which one).
[[nodiscard]] Result<PeakEnvelope> extractPeakEnvelope(
    const std::filesystem::path& path, Duration bucketDuration, const DecodePrefs& prefs,
    const DecodeBackendFactory& factory, EnvelopeExtractionLimits limits = {});

} // namespace palmier::media

#endif // PALMIER_MEDIA_PEAKENVELOPEEXTRACTOR_HPP
