// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PeakEnvelope.cpp — implementation of the peak-envelope reduction.

#include "media/PeakEnvelope.hpp"

#include <algorithm>
#include <utility>

namespace palmier::media {

Duration frameToSourceTime(std::int64_t frame, int sampleRate) noexcept {
    if (sampleRate <= 0) return Duration::zero();
    const auto rate = static_cast<std::int64_t>(sampleRate);
    // whole seconds exactly, then the sub-second remainder — never frame * 1e9.
    const std::int64_t whole = frame / rate;
    const std::int64_t remainder = frame % rate;
    return Duration::fromNanoseconds(whole * Duration::kTicksPerSecond +
                                     remainder * Duration::kTicksPerSecond / rate);
}

// ---------------------------------------------------------------------------
// PeakEnvelope
// ---------------------------------------------------------------------------

EnvelopeBucket PeakEnvelope::hullOver(Duration from, Duration to) const noexcept {
    if (buckets.empty() || bucketDuration.ticks() <= 0) return EnvelopeBucket{};

    // Worked in raw ticks throughout, and clamped into range BEFORE any division,
    // so no intermediate can be negative when it is converted to an index. (An
    // earlier form clamped `from` before normalising the order, which let an
    // inverted negative range survive as a negative `begin` and wrap to a huge
    // index.)
    const std::int64_t width = bucketDuration.ticks();
    const std::int64_t total = width * static_cast<std::int64_t>(buckets.size());

    std::int64_t begin = from.ticks();
    std::int64_t end = to.ticks();
    if (end < begin) std::swap(begin, end);
    const bool degenerate = (end == begin);

    if (end < 0) return EnvelopeBucket{};   // entirely before the stream
    if (begin < 0) begin = 0;
    if (begin >= total) return EnvelopeBucket{};  // entirely past the end
    if (end > total) end = total;

    const auto  first = static_cast<std::size_t>(begin / width);
    std::size_t last = degenerate ? first : static_cast<std::size_t>((end - 1) / width);
    last = std::min(last, buckets.size() - 1);

    EnvelopeBucket hull = buckets[first];
    for (std::size_t i = first + 1; i <= last; ++i) {
        hull.min = std::min(hull.min, buckets[i].min);
        hull.max = std::max(hull.max, buckets[i].max);
    }
    return hull;
}

EnvelopeBucket PeakEnvelope::bucketAt(Duration sourceTime) const noexcept {
    if (buckets.empty() || bucketDuration.ticks() <= 0) return EnvelopeBucket{};
    if (sourceTime < Duration::zero()) return EnvelopeBucket{};
    const auto index = static_cast<std::size_t>(sourceTime.ticks() / bucketDuration.ticks());
    if (index >= buckets.size()) return EnvelopeBucket{};
    return buckets[index];
}

// ---------------------------------------------------------------------------
// PeakEnvelopeBuilder
// ---------------------------------------------------------------------------

PeakEnvelopeBuilder::PeakEnvelopeBuilder(Duration bucketDuration, int sampleRate) noexcept
    : bucketDuration_(bucketDuration),
      sampleRate_(sampleRate),
      valid_(bucketDuration.ticks() > 0 && sampleRate > 0) {}

std::size_t PeakEnvelopeBuilder::bucketIndexFor(std::size_t frame) const noexcept {
    const Duration at = frameToSourceTime(static_cast<std::int64_t>(frame), sampleRate_);
    return static_cast<std::size_t>(at.ticks() / bucketDuration_.ticks());
}

void PeakEnvelopeBuilder::add(const AudioBuffer& buffer) noexcept {
    if (!valid_) return;
    const int channels = buffer.channels();
    if (channels <= 0) return;

    const std::vector<float>& samples = buffer.samples();
    const std::size_t         frames = buffer.frameCount();
    const auto                chan = static_cast<std::size_t>(channels);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = frame * chan;
        if (base + chan > samples.size()) break;  // ragged tail: stop, do not invent

        // Hull the channels together so neither side of a stereo pair is hidden.
        float lo = samples[base];
        float hi = lo;
        for (std::size_t channel = 1; channel < chan; ++channel) {
            const float value = samples[base + channel];
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }

        // Derived from the ABSOLUTE frame counter, so buffer boundaries cannot
        // shift bucket boundaries.
        const std::size_t index = bucketIndexFor(frames_);
        if (index >= buckets_.size()) {
            buckets_.resize(index + 1, EnvelopeBucket{});
            filled_.resize(index + 1, false);
        }
        if (filled_[index]) {
            buckets_[index].min = std::min(buckets_[index].min, lo);
            buckets_[index].max = std::max(buckets_[index].max, hi);
        } else {
            buckets_[index].min = lo;
            buckets_[index].max = hi;
            filled_[index] = true;
        }
        ++frames_;
    }
}

PeakEnvelope PeakEnvelopeBuilder::finish() const {
    PeakEnvelope envelope;
    if (!valid_) return envelope;  // stays empty: silent, not failed
    envelope.bucketDuration = bucketDuration_;
    envelope.sampleRate = sampleRate_;
    envelope.buckets = buckets_;
    return envelope;
}

} // namespace palmier::media
