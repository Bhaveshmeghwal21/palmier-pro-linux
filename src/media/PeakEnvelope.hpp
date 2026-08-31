// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelope.hpp — peak envelopes for drawing audio waveforms
// (monitoring-and-grading Requirement 2.1, 2.3).
//
// A peak envelope is the drawable summary of an audio stream: a sequence of
// min/max sample pairs over fixed-width source-time buckets. Drawing a waveform
// from raw samples is impossible at timeline zoom levels — a 60-second clip in
// 600 pixels is 80,000 samples per pixel — so what a waveform view actually
// needs is, for each pixel column, the extremes of everything that column covers.
// That is exactly a bucketed min/max hull.
//
// ## Why min and max rather than a single magnitude
//
// Audio is signed and often asymmetric. Collapsing each bucket to one magnitude
// draws a symmetric shape that no longer corresponds to the signal, and loses DC
// offset entirely. Keeping both bounds means the drawn shape is the signal's real
// outer hull: nothing that happened inside a bucket falls outside its drawn
// extent, which is the property that makes cutting on a transient possible.
//
// ## Why this file is pure
//
// Nothing here decodes, opens a file, touches a device, or knows what a clip is.
// The reduction is a fold over AudioBuffers, so every boundary and every value is
// unit-testable against synthesised content with known peaks — no FFmpeg, no
// media file, no GPU, no Qt. Extraction through a real decoder is a separate
// concern layered on top; this is the part that has to be exactly right.

#ifndef PALMIER_MEDIA_PEAKENVELOPE_HPP
#define PALMIER_MEDIA_PEAKENVELOPE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Duration.hpp"
#include "media/AudioGraph.hpp"

namespace palmier::media {

/// The extremes of every sample falling in one source-time bucket, across all
/// channels. `min <= max` always; both are 0 for a bucket no sample reached.
struct EnvelopeBucket {
    float min = 0.0f;
    float max = 0.0f;

    /// The larger absolute bound — the half-height a symmetric renderer would
    /// use, and the figure a "loudest thing in this column" query wants.
    [[nodiscard]] float magnitude() const noexcept {
        const float lo = min < 0.0f ? -min : min;
        const float hi = max < 0.0f ? -max : max;
        return lo > hi ? lo : hi;
    }

    /// True when this bucket spans no amplitude at all (digital silence).
    [[nodiscard]] bool isSilent() const noexcept { return min == 0.0f && max == 0.0f; }
};

/// A whole asset's drawable audio summary.
///
/// Positions are in SOURCE time from the start of the asset's audio stream, never
/// timeline time. That distinction is what lets one envelope serve every clip
/// referencing the asset regardless of where each was placed or how it was
/// trimmed (Requirement 2.5), and it is why the drawing side must map through the
/// clip's own `sourceIn` rather than its width (Requirement 2.3).
struct PeakEnvelope {
    /// The source-time width of one bucket. Zero only for an empty envelope.
    Duration                    bucketDuration{};
    /// The stream's sample rate, kept so a consumer can reason about resolution.
    int                         sampleRate = 0;
    /// Buckets in source order, starting at source time zero.
    std::vector<EnvelopeBucket> buckets{};

    [[nodiscard]] bool empty() const noexcept { return buckets.empty(); }

    /// Total source time this envelope covers.
    [[nodiscard]] Duration duration() const noexcept {
        return bucketDuration * static_cast<std::int64_t>(buckets.size());
    }

    /// The min/max hull of every bucket overlapping the source-time range
    /// [`from`, `to`) — the primitive a waveform renderer calls once per pixel
    /// column, passing the source window that column actually covers.
    ///
    /// A range that is empty, inverted, or entirely outside the envelope yields a
    /// silent bucket rather than an error, so a renderer needs no bounds logic of
    /// its own and a clip trimmed past the end of its audio simply draws flat. A
    /// range narrower than one bucket still reports that bucket, so zooming in
    /// past the envelope's resolution degrades to a held value instead of gaps.
    [[nodiscard]] EnvelopeBucket hullOver(Duration from, Duration to) const noexcept;

    /// The bucket covering `sourceTime`, or a silent bucket when out of range.
    [[nodiscard]] EnvelopeBucket bucketAt(Duration sourceTime) const noexcept;
};

/// Folds decoded audio into a PeakEnvelope, one buffer at a time, in order.
///
/// Sequential by construction: each `add()` continues exactly where the previous
/// one stopped, so the caller streams a decode loop through it without holding
/// the whole asset in memory. Bucket boundaries are derived from the absolute
/// frame counter rather than accumulated per buffer, so a stream delivered in a
/// thousand ragged buffers buckets identically to the same stream delivered in
/// one — asserted directly in the tests, because a renderer that redrew
/// differently depending on decoder block size would be unusable.
class PeakEnvelopeBuilder {
public:
    /// `bucketDuration` must be positive and `sampleRate` must be positive;
    /// otherwise the builder is invalid, ignores every `add()`, and finishes
    /// empty. Constructing invalid is deliberately not an error: an asset that
    /// declares no usable audio format is silent, not broken (Requirement 2.6).
    PeakEnvelopeBuilder(Duration bucketDuration, int sampleRate) noexcept;

    [[nodiscard]] bool isValid() const noexcept { return valid_; }

    /// Fold `buffer`'s frames in, continuing from the current position. Channels
    /// are hulled together (the minimum across channels against the maximum
    /// across channels), so a stereo asset draws one shape that hides neither
    /// side. A buffer with no channels or no frames is a no-op, not an error.
    void add(const AudioBuffer& buffer) noexcept;

    /// Frames folded in so far, across every `add()`.
    [[nodiscard]] std::size_t framesConsumed() const noexcept { return frames_; }

    /// The envelope built so far. Const, so a caller may finish more than once
    /// (e.g. to publish a partial result and then continue).
    [[nodiscard]] PeakEnvelope finish() const;

private:
    /// The bucket a given absolute frame index belongs to.
    [[nodiscard]] std::size_t bucketIndexFor(std::size_t frame) const noexcept;

    Duration                    bucketDuration_{};
    int                         sampleRate_ = 0;
    bool                        valid_ = false;
    std::size_t                 frames_ = 0;
    std::vector<EnvelopeBucket> buckets_{};
    /// Whether each bucket has received a sample, so an untouched bucket stays
    /// {0,0} instead of inheriting a sentinel.
    std::vector<bool>           filled_{};
};

/// Exact source time of an absolute frame index at `sampleRate`.
///
/// Split into whole seconds plus a remainder rather than computed as
/// `frame * kTicksPerSecond / sampleRate`, because that product overflows a
/// signed 64-bit tick count for long assets (at 48 kHz it passes the limit within
/// a day of audio) and would silently wrap into negative source times.
[[nodiscard]] Duration frameToSourceTime(std::int64_t frame, int sampleRate) noexcept;

/// The source-time range one pixel column of a drawn clip covers.
struct ClipSourceWindow {
    Duration from{};
    Duration to{};

    [[nodiscard]] bool isEmpty() const noexcept { return to <= from; }
};

/// Map pixel column `column` of a clip `widthPx` wide onto the source-time range
/// it actually plays, given the clip's trim.
///
/// This is Requirement 2.3 reduced to arithmetic, and it is the reason the rule is
/// stated as "honouring the clip's `sourceIn` and any trim, not merely the clip's
/// width": a clip trimmed to the middle of an asset must draw the MIDDLE of that
/// asset's waveform, and two clips of equal width cut from different points must
/// draw different shapes. Interpolating over the clip's width alone would draw the
/// whole asset squeezed into every clip — which looks plausible and is wrong, so
/// the mapping is a named, separately-tested function rather than three lines
/// inside a paint handler.
///
/// `column` is measured from the clip's left edge. A column outside
/// [0, widthPx) or a non-positive width yields an empty window, so a renderer
/// clipping to the viewport needs no special cases.
[[nodiscard]] ClipSourceWindow sourceWindowForColumn(Duration sourceIn, Duration sourceOut,
                                                     int column, int widthPx) noexcept;

} // namespace palmier::media

#endif // PALMIER_MEDIA_PEAKENVELOPE_HPP
