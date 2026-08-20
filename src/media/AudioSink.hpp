// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioSink.hpp — the audio output seam plus the always-compiled
// NullAudioSink (task 8.3; Requirements 6.2, 6.3, 6.7).
//
// design.md D7 "Decision — output sink" names three implementations chosen in
// order at startup: PipeWire, ALSA, and NullAudioSink. This header defines the
// interface all three satisfy and the third one, which lands FIRST and on
// purpose:
//
//   * It makes "the audio output device is unavailable" (Requirement 6.7) a
//     NORMAL code path rather than an error path: the engine keeps a working
//     clock, video keeps presenting at the project frame rate, and the only
//     visible consequence is a notice.
//   * It lets the whole audio stage — mixing, gain, mute, A/V sync, export
//     rendering — be exercised in CI on a machine with no sound card, which is
//     exactly the machine the suite runs on.
//
// **The sink is the master clock** (design.md D7 "Decision — A/V sync"). Every
// sink reports `playedFrames()`, a monotonic count of output frames that have
// been played out; `AudioEngine::presentationPosition()` converts it to a
// Duration at the 48 kHz output rate and PreviewController slews video to it. A
// real sink reads that counter from the audio server. NullAudioSink derives it
// from a **steady clock**, so the position advances in real time even though the
// samples are discarded — video timing is therefore unchanged when no device
// could be opened.
//
// The steady clock is INJECTABLE (`SteadyClock`). That is what keeps the audio
// tests free of `sleep`: a test supplies a clock it advances by hand, so the
// 40 ms sync bound (Requirement 6.3), the 200 ms mute/gain latency
// (Requirement 6.4) and the 2 s notice budget (Requirement 6.7) are all checked
// deterministically and instantly. Nothing in this header reads the wall clock
// unless the caller asks for the default clock.
//
// Dependencies: the standard library plus media/AudioGraph.hpp (AudioFormat /
// AudioBuffer). No FFmpeg, no PipeWire, no ALSA, no GPU — the real sinks arrive
// in task 8.6 behind PALMIER_HAVE_PIPEWIRE / PALMIER_HAVE_ALSA and are additive.

#ifndef PALMIER_MEDIA_AUDIOSINK_HPP
#define PALMIER_MEDIA_AUDIOSINK_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/Duration.hpp"
#include "core/Result.hpp"
#include "media/AudioGraph.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Injectable steady clock
// ---------------------------------------------------------------------------

/// Reads a monotonic time point. Production code passes systemSteadyClock();
/// tests pass a clock they advance explicitly, which is why no audio test ever
/// sleeps to make a timing requirement come true.
using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

/// The default clock: std::chrono::steady_clock::now(). Steady (never adjusted
/// backwards by NTP or a timezone change), which is what a playback position
/// must be derived from.
[[nodiscard]] SteadyClock systemSteadyClock();

// ---------------------------------------------------------------------------
// Output format and buffer geometry
// ---------------------------------------------------------------------------

/// The engine's fixed output format: 48 000 samples per second, 2 interleaved
/// channels, 32-bit float (Requirement 6.2). Declared here because the sink and
/// the engine must agree on it and the sink is the lower layer.
[[nodiscard]] constexpr AudioFormat audioOutputFormat() noexcept {
    return AudioFormat{48'000, 2, SampleFormat::F32};
}

/// Default buffer quantum: 1024 frames at 48 kHz = 21.3 ms.
constexpr std::size_t kDefaultQuantumFrames = 1024;

/// Quantum used when the project frame rate exceeds 48 fps: 512 frames = 10.7 ms
/// (design.md D7 — keeps one frame interval plus one quantum inside the 40 ms
/// sync bound of Requirement 6.3 at high frame rates).
constexpr std::size_t kHighFrameRateQuantumFrames = 512;

/// Frame rate above which the smaller quantum is requested.
constexpr double kHighFrameRateThresholdFps = 48.0;

/// The quantum a sink should request for a project running at
/// `projectFrameRateFps`: at most 512 frames above 48 fps, otherwise 1024.
[[nodiscard]] std::size_t preferredQuantumFrames(double projectFrameRateFps) noexcept;

/// Longest gap between buffers delivered to the sink that Requirement 6.2
/// tolerates ("no dropout longer than 100 milliseconds").
constexpr auto kMaxDropout = std::chrono::milliseconds{100};

/// The A/V sync bound of Requirement 6.3.
constexpr auto kMaxAvSkew = std::chrono::milliseconds{40};

/// The mute/gain latency bound of Requirement 6.4.
constexpr auto kMaxControlLatency = std::chrono::milliseconds{200};

/// The notice budget of Requirement 6.7.
constexpr auto kMaxNoticeLatency = std::chrono::seconds{2};

/// Exact conversion of an output frame count to a Duration at `sampleRate`.
/// Integer arithmetic throughout, so a long playback run accumulates no
/// floating-point drift in the master clock.
[[nodiscard]] Duration framesToDuration(std::uint64_t frames, int sampleRate) noexcept;

/// Frames spanned by `d` at `sampleRate`, rounded to nearest. Negative or
/// non-positive inputs yield 0.
[[nodiscard]] std::uint64_t durationToFrames(Duration d, int sampleRate) noexcept;

/// What a sink is asked to open with.
struct AudioSinkConfig {
    AudioFormat format = audioOutputFormat();
    /// Frames per submitted buffer. A sink may honour a smaller quantum than
    /// requested but reports what it actually uses through quantumFrames().
    std::size_t quantumFrames = kDefaultQuantumFrames;
};

// ---------------------------------------------------------------------------
// IAudioSink
// ---------------------------------------------------------------------------

/// An audio output device the engine writes mixed buffers to, and the clock the
/// rest of the presentation pipeline slews to.
///
/// Contract:
///   * start() opens the device. A failure is NOT fatal anywhere: AudioEngine
///     turns it into the Requirement 6.7 path (suppress audio, keep video, raise
///     a notice).
///   * submit() accepts one interleaved-float buffer in the started format.
///     Buffers are delivered in presentation order.
///   * playedFrames() is monotonic non-decreasing while started and never
///     exceeds submittedFrames() — you cannot have played what was never
///     written. stop() freezes it; a later start() resets the run to zero.
///   * Every accessor is safe to call from another thread than the one
///     submitting, because PreviewController reads the clock from the
///     presentation thread.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    /// Stable identifier of the backend ("null", "pipewire", "alsa"). Reported in
    /// notices and logs.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Open the device. Errors: InvalidArgument for an invalid format or a zero
    /// quantum; FailedPrecondition when already started; Io / Unsupported /
    /// NotFound from the underlying audio system.
    [[nodiscard]] virtual Result<void> start(const AudioSinkConfig& config) = 0;

    /// Close the device. Idempotent. Freezes playedFrames() at its final value.
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// The format the sink was started with (all-zero before the first start).
    [[nodiscard]] virtual AudioFormat format() const noexcept = 0;

    /// Frames per buffer the sink actually uses.
    [[nodiscard]] virtual std::size_t quantumFrames() const noexcept = 0;

    /// Hand one mixed buffer to the device. The buffer's sample rate and channel
    /// count must equal format()'s. Errors: FailedPrecondition when not started;
    /// InvalidArgument on a format mismatch; Io from the device.
    [[nodiscard]] virtual Result<void> submit(const AudioBuffer& buffer) = 0;

    /// Output frames played out so far in this run — the master clock.
    [[nodiscard]] virtual std::uint64_t playedFrames() const noexcept = 0;

    /// Output frames accepted by submit() so far in this run.
    [[nodiscard]] virtual std::uint64_t submittedFrames() const noexcept = 0;

    /// playedFrames() as a Duration at the started sample rate.
    [[nodiscard]] Duration playedPosition() const noexcept {
        return framesToDuration(playedFrames(), format().sampleRate);
    }
};

// ---------------------------------------------------------------------------
// NullAudioSink
// ---------------------------------------------------------------------------

/// The always-compiled sink: consumes and discards buffers while advancing a
/// monotonic sample position from a steady clock.
///
/// Position rule: `played = min(elapsed_frames, submitted_frames)`, latched to
/// its own maximum. Both terms are non-decreasing, so the result is monotonic;
/// clamping to submitted frames keeps the sink honest (it never claims to have
/// played audio the engine never mixed), which is what makes an engine that
/// stops submitting visibly stall the clock instead of silently drifting.
///
/// Thread-safe: the counters are atomics and the clock is read without a lock.
class NullAudioSink final : public IAudioSink {
public:
    /// `clock` defaults to the real steady clock; tests inject their own.
    explicit NullAudioSink(SteadyClock clock = systemSteadyClock());

    [[nodiscard]] std::string_view name() const noexcept override { return "null"; }

    [[nodiscard]] Result<void> start(const AudioSinkConfig& config) override;
    void                       stop() noexcept override;
    [[nodiscard]] bool         running() const noexcept override { return running_.load(); }
    [[nodiscard]] AudioFormat  format() const noexcept override { return format_; }
    [[nodiscard]] std::size_t  quantumFrames() const noexcept override { return quantum_; }
    [[nodiscard]] Result<void> submit(const AudioBuffer& buffer) override;
    [[nodiscard]] std::uint64_t playedFrames() const noexcept override;
    [[nodiscard]] std::uint64_t submittedFrames() const noexcept override {
        return submitted_.load();
    }

    /// Buffers accepted in this run (observability for the dropout check).
    [[nodiscard]] std::uint64_t submittedBuffers() const noexcept { return buffers_.load(); }

private:
    SteadyClock                                    clock_;
    AudioFormat                                    format_{0, 0, SampleFormat::F32};
    std::size_t                                    quantum_{0};
    std::atomic<bool>                              running_{false};
    std::chrono::steady_clock::time_point          startedAt_{};
    std::atomic<std::uint64_t>                     submitted_{0};
    std::atomic<std::uint64_t>                     buffers_{0};
    mutable std::atomic<std::uint64_t>             played_{0};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_AUDIOSINK_HPP
