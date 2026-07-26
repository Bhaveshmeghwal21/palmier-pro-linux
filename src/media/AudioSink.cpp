// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioSink.cpp — the steady-clock-driven NullAudioSink and the shared
// frame/duration conversions (task 8.3; Requirements 6.2, 6.3, 6.7).

#include "media/AudioSink.hpp"

#include <algorithm>

#include "core/Error.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Clock and conversions
// ---------------------------------------------------------------------------

SteadyClock systemSteadyClock() {
    return [] { return std::chrono::steady_clock::now(); };
}

std::size_t preferredQuantumFrames(double projectFrameRateFps) noexcept {
    return projectFrameRateFps > kHighFrameRateThresholdFps ? kHighFrameRateQuantumFrames
                                                            : kDefaultQuantumFrames;
}

Duration framesToDuration(std::uint64_t frames, int sampleRate) noexcept {
    if (sampleRate <= 0 || frames == 0) return Duration::zero();
    // ticks = frames * 1e9 / rate, computed so the multiplication cannot overflow
    // for any playback run of a plausible length: split frames into whole seconds
    // and a remainder.
    const auto rate = static_cast<std::uint64_t>(sampleRate);
    const std::uint64_t wholeSeconds = frames / rate;
    const std::uint64_t remainder = frames % rate;
    const std::int64_t ticks =
        static_cast<std::int64_t>(wholeSeconds) * Duration::kTicksPerSecond +
        static_cast<std::int64_t>(remainder * static_cast<std::uint64_t>(
                                                  Duration::kTicksPerSecond) /
                                  rate);
    return Duration::fromNanoseconds(ticks);
}

std::uint64_t durationToFrames(Duration d, int sampleRate) noexcept {
    if (sampleRate <= 0 || d.ticks() <= 0) return 0;
    const auto rate = static_cast<std::uint64_t>(sampleRate);
    const auto ticks = static_cast<std::uint64_t>(d.ticks());
    const std::uint64_t wholeSeconds = ticks / static_cast<std::uint64_t>(Duration::kTicksPerSecond);
    const std::uint64_t remainder = ticks % static_cast<std::uint64_t>(Duration::kTicksPerSecond);
    // Round the sub-second part to the nearest frame.
    const std::uint64_t tail =
        (remainder * rate + static_cast<std::uint64_t>(Duration::kTicksPerSecond) / 2) /
        static_cast<std::uint64_t>(Duration::kTicksPerSecond);
    return wholeSeconds * rate + tail;
}

// ---------------------------------------------------------------------------
// NullAudioSink
// ---------------------------------------------------------------------------

NullAudioSink::NullAudioSink(SteadyClock clock) : clock_(std::move(clock)) {
    if (!clock_) clock_ = systemSteadyClock();
}

Result<void> NullAudioSink::start(const AudioSinkConfig& config) {
    if (running_.load()) {
        return makeError(ErrorCode::FailedPrecondition, "the null audio sink is already started");
    }
    if (!config.format.isValid()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the audio sink format must declare a positive sample rate and channel "
                         "count");
    }
    if (config.quantumFrames == 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "the audio sink buffer quantum must be at least one frame");
    }

    format_ = config.format;
    quantum_ = config.quantumFrames;
    submitted_.store(0);
    buffers_.store(0);
    played_.store(0);
    startedAt_ = clock_();
    running_.store(true);
    return ok();
}

void NullAudioSink::stop() noexcept {
    if (!running_.load()) return;
    // Latch the final position before the clock stops contributing, so a reader
    // that observes the sink after stop() sees where playback actually ended.
    const std::uint64_t final = playedFrames();
    running_.store(false);
    played_.store(final);
}

Result<void> NullAudioSink::submit(const AudioBuffer& buffer) {
    if (!running_.load()) {
        return makeError(ErrorCode::FailedPrecondition,
                         "the null audio sink is not started; start() it before submitting");
    }
    if (buffer.frameCount() == 0) {
        // An empty buffer is a no-op, not an error: a mix window can legitimately
        // produce no frames (a zero-length range).
        return ok();
    }
    if (buffer.sampleRate() != format_.sampleRate || buffer.channels() != format_.channels) {
        return makeError(ErrorCode::InvalidArgument,
                         "submitted buffer format (" + std::to_string(buffer.sampleRate()) +
                             " Hz, " + std::to_string(buffer.channels()) +
                             " channels) does not match the sink format (" +
                             std::to_string(format_.sampleRate) + " Hz, " +
                             std::to_string(format_.channels) + " channels)");
    }

    // Consume and discard: the samples are dropped, only the accounting advances.
    submitted_.fetch_add(static_cast<std::uint64_t>(buffer.frameCount()));
    buffers_.fetch_add(1);
    return ok();
}

std::uint64_t NullAudioSink::playedFrames() const noexcept {
    const std::uint64_t latched = played_.load();
    if (!running_.load()) return latched;

    std::uint64_t elapsedFrames = 0;
    try {
        const auto now = clock_();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - startedAt_).count();
        if (elapsed > 0) {
            elapsedFrames =
                durationToFrames(Duration::fromNanoseconds(elapsed), format_.sampleRate);
        }
    } catch (...) {
        // A throwing injected clock must not take the process down; the last
        // latched position is the honest answer.
        return latched;
    }

    // Never claim to have played more than was submitted, and never regress.
    const std::uint64_t candidate = std::min(elapsedFrames, submitted_.load());
    std::uint64_t previous = latched;
    while (candidate > previous && !played_.compare_exchange_weak(previous, candidate)) {
        // previous refreshed by compare_exchange_weak; retry.
    }
    return std::max(candidate, previous);
}

} // namespace palmier::media
