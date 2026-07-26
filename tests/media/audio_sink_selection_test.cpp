// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media/audio_sink_selection_test.cpp — the PipeWire/ALSA/Null sink set and
// the startup selection order (task 8.6; Requirements 6.2, 6.7).
//
// design.md D7 fixes three things this file asserts:
//
//   1. the selection ORDER — PipeWire, then ALSA, then NullAudioSink;
//   2. that a candidate counts as selectable only if it actually OPENS, so a
//      build with a backend compiled in but no device present still falls
//      through (this is the whole reason ALSA is reachable on a PipeWire build);
//   3. that the resolved quantum is at most 512 frames when the project frame
//      rate exceeds 48 fps.
//
// The order is driven through INJECTED candidate factories, so every branch is
// asserted identically on a developer desktop with a sound server and on a CI
// runner with no sound card at all. Two further tests then exercise the REAL
// backends on whatever host is running: they assert the host-independent
// invariant — a real sink either opens or reports a named reason, and selection
// always ends with a usable sink — which on a machine with no audio device is
// precisely the Requirement 6.7 fall-through to NullAudioSink.
//
// Nothing here sleeps: the null fallback's clock is injected, so its position is
// a function of a clock the test moves by hand.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Error.hpp"
#include "media/AlsaAudioSink.hpp"
#include "media/AudioSink.hpp"
#include "media/AudioSinkSelector.hpp"
#include "media/PipeWireAudioSink.hpp"

namespace palmier::media {
namespace {

// ---------------------------------------------------------------------------
// A fake device standing in for PipeWire or ALSA.
// ---------------------------------------------------------------------------

/// Records how often it was opened and closed, and can be told to refuse to open
/// — which is what "the daemon is not running" / "there is no sound card" looks
/// like to the selector.
class FakeSink final : public IAudioSink {
public:
    FakeSink(std::string sinkName, bool openable)
        : name_(std::move(sinkName)), openable_(openable) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] Result<void> start(const AudioSinkConfig& config) override {
        ++startCalls;
        if (!openable_) {
            return makeError(ErrorCode::NotFound,
                             name_ + " audio output is unavailable: no device (synthetic)");
        }
        format_ = config.format;
        quantum_ = config.quantumFrames;
        running_ = true;
        return ok();
    }

    void stop() noexcept override {
        if (running_) ++stopCalls;
        running_ = false;
    }

    [[nodiscard]] bool          running() const noexcept override { return running_; }
    [[nodiscard]] AudioFormat   format() const noexcept override { return format_; }
    [[nodiscard]] std::size_t   quantumFrames() const noexcept override { return quantum_; }

    [[nodiscard]] Result<void> submit(const AudioBuffer& buffer) override {
        if (!running_) {
            return makeError(ErrorCode::FailedPrecondition, "not started");
        }
        submitted_ += buffer.frameCount();
        return ok();
    }

    [[nodiscard]] std::uint64_t playedFrames() const noexcept override { return submitted_; }
    [[nodiscard]] std::uint64_t submittedFrames() const noexcept override { return submitted_; }

    int startCalls = 0;
    int stopCalls = 0;

private:
    std::string   name_;
    bool          openable_;
    bool          running_ = false;
    AudioFormat   format_{0, 0, SampleFormat::F32};
    std::size_t   quantum_ = 0;
    std::uint64_t submitted_ = 0;
};

/// A factory yielding one fake sink; the pointer is published so the test can
/// inspect the open/close accounting afterwards.
AudioSinkFactory fakeFactory(const std::string& name, bool openable, FakeSink** out) {
    return [name, openable, out]() -> std::unique_ptr<IAudioSink> {
        auto sink = std::make_unique<FakeSink>(name, openable);
        if (out != nullptr) *out = sink.get();
        return sink;
    };
}

/// A factory that yields nothing — i.e. "this backend is not in this build".
AudioSinkFactory absentFactory() {
    return []() -> std::unique_ptr<IAudioSink> { return nullptr; };
}

/// A manually advanced steady clock for the null fallback.
class ManualSteadyClock {
public:
    [[nodiscard]] SteadyClock asClock() {
        return [this] { return now_; };
    }
    void advance(std::chrono::nanoseconds d) { now_ += d; }

private:
    std::chrono::steady_clock::time_point now_{};
};

[[nodiscard]] const AudioSinkAttempt* findAttempt(const AudioSinkSelection& selection,
                                                  AudioSinkKind kind) {
    for (const AudioSinkAttempt& attempt : selection.attempts) {
        if (attempt.kind == kind) return &attempt;
    }
    return nullptr;
}

} // namespace

// ===========================================================================
// Selection order (design.md D7: PipeWire -> ALSA -> Null)
// ===========================================================================

TEST(AudioSinkSelection, PrefersPipeWireWhenItOpens) {
    FakeSink* pipewire = nullptr;
    FakeSink* alsa = nullptr;

    AudioSinkSelectorOptions options;
    options.pipewireFactory = fakeFactory("pipewire", /*openable=*/true, &pipewire);
    options.alsaFactory = fakeFactory("alsa", /*openable=*/true, &alsa);

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    EXPECT_EQ(selection.kind, AudioSinkKind::PipeWire);
    EXPECT_EQ(selection.name, "pipewire");
    EXPECT_TRUE(selection.realDevice);
    EXPECT_FALSE(selection.notice.has_value());
    ASSERT_NE(selection.sink, nullptr);

    // ALSA was never even constructed: the order short-circuits on the first
    // candidate that opens.
    EXPECT_EQ(alsa, nullptr);
    ASSERT_NE(pipewire, nullptr);
    EXPECT_EQ(pipewire->startCalls, 1);
    // Handed back CLOSED, so AudioEngine::start() owns the real open.
    EXPECT_EQ(pipewire->stopCalls, 1);
    EXPECT_FALSE(selection.sink->running());

    ASSERT_EQ(selection.attempts.size(), 1u);
    EXPECT_TRUE(selection.attempts[0].compiledIn);
    EXPECT_TRUE(selection.attempts[0].opened);
    EXPECT_TRUE(selection.attempts[0].reason.empty());
}

TEST(AudioSinkSelection, FallsBackToAlsaWhenPipeWireCannotOpen) {
    FakeSink* alsa = nullptr;

    AudioSinkSelectorOptions options;
    options.pipewireFactory = fakeFactory("pipewire", /*openable=*/false, nullptr);
    options.alsaFactory = fakeFactory("alsa", /*openable=*/true, &alsa);

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    EXPECT_EQ(selection.kind, AudioSinkKind::Alsa);
    EXPECT_EQ(selection.name, "alsa");
    EXPECT_TRUE(selection.realDevice);
    EXPECT_FALSE(selection.notice.has_value());
    ASSERT_NE(alsa, nullptr);
    EXPECT_EQ(alsa->startCalls, 1);
    EXPECT_EQ(alsa->stopCalls, 1);

    // The PipeWire attempt is recorded, compiled in but unopened, with its reason.
    const AudioSinkAttempt* attempt = findAttempt(selection, AudioSinkKind::PipeWire);
    ASSERT_NE(attempt, nullptr);
    EXPECT_TRUE(attempt->compiledIn);
    EXPECT_FALSE(attempt->opened);
    EXPECT_NE(attempt->reason.find("unavailable"), std::string::npos);
}

TEST(AudioSinkSelection, FallsBackToNullWhenNeitherBackendOpens) {
    AudioSinkSelectorOptions options;
    options.pipewireFactory = fakeFactory("pipewire", /*openable=*/false, nullptr);
    options.alsaFactory = fakeFactory("alsa", /*openable=*/false, nullptr);

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    // Requirement 6.7: this is a normal outcome, not a failure.
    EXPECT_EQ(selection.kind, AudioSinkKind::Null);
    EXPECT_EQ(selection.name, "null");
    EXPECT_FALSE(selection.realDevice);
    ASSERT_NE(selection.sink, nullptr);

    // Both candidates were tried, in order, and both reasons reach the notice.
    ASSERT_EQ(selection.attempts.size(), 2u);
    EXPECT_EQ(selection.attempts[0].kind, AudioSinkKind::PipeWire);
    EXPECT_EQ(selection.attempts[1].kind, AudioSinkKind::Alsa);
    ASSERT_TRUE(selection.notice.has_value());
    EXPECT_NE(selection.notice->find("pipewire"), std::string::npos);
    EXPECT_NE(selection.notice->find("alsa"), std::string::npos);
    EXPECT_NE(selection.notice->find("Audio output is unavailable"), std::string::npos);
}

TEST(AudioSinkSelection, RecordsBackendsThatAreNotCompiledInAndKeepsGoing) {
    FakeSink* alsa = nullptr;

    AudioSinkSelectorOptions options;
    options.pipewireFactory = absentFactory();
    options.alsaFactory = fakeFactory("alsa", /*openable=*/true, &alsa);

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    EXPECT_EQ(selection.kind, AudioSinkKind::Alsa);
    const AudioSinkAttempt* attempt = findAttempt(selection, AudioSinkKind::PipeWire);
    ASSERT_NE(attempt, nullptr);
    EXPECT_FALSE(attempt->compiledIn);
    EXPECT_FALSE(attempt->opened);
    EXPECT_NE(attempt->reason.find("not compiled into this build"), std::string::npos);
}

TEST(AudioSinkSelection, NullFallbackIsSelectedWhenNoBackendIsCompiledIn) {
    AudioSinkSelectorOptions options;
    options.pipewireFactory = absentFactory();
    options.alsaFactory = absentFactory();

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    EXPECT_EQ(selection.kind, AudioSinkKind::Null);
    EXPECT_FALSE(selection.realDevice);
    ASSERT_EQ(selection.attempts.size(), 2u);
    EXPECT_FALSE(selection.attempts[0].compiledIn);
    EXPECT_FALSE(selection.attempts[1].compiledIn);
    ASSERT_TRUE(selection.notice.has_value());
}

TEST(AudioSinkSelection, SelectedSinkIsClosedAndStartableAgain) {
    AudioSinkSelectorOptions options;
    options.pipewireFactory = fakeFactory("pipewire", /*openable=*/true, nullptr);

    AudioSinkSelection selection = selectAudioSink(std::move(options));
    ASSERT_NE(selection.sink, nullptr);
    ASSERT_FALSE(selection.sink->running());

    // The probe must leave the sink in a state the engine can open for real.
    ASSERT_TRUE(selection.sink->start(selection.config).isOk());
    EXPECT_TRUE(selection.sink->running());
    EXPECT_EQ(selection.sink->format().sampleRate, 48'000);
    EXPECT_EQ(selection.sink->format().channels, 2);
    selection.sink->stop();
}

// ===========================================================================
// The quantum (Requirement 6.3; design.md D7)
// ===========================================================================

TEST(AudioSinkSelection, RequestsAtMost512FramesAboveFortyEightFps) {
    struct Case {
        double      fps;
        std::size_t expected;
    };
    // 48 fps is the threshold itself: "exceeds 48" is strict, so 48 keeps 1024.
    const Case cases[] = {
        {1.0, kDefaultQuantumFrames},    {24.0, kDefaultQuantumFrames},
        {30.0, kDefaultQuantumFrames},   {48.0, kDefaultQuantumFrames},
        {48.001, kHighFrameRateQuantumFrames},
        {50.0, kHighFrameRateQuantumFrames}, {59.94, kHighFrameRateQuantumFrames},
        {120.0, kHighFrameRateQuantumFrames},
    };

    for (const Case& c : cases) {
        const AudioSinkConfig config = audioSinkConfigFor(c.fps);
        EXPECT_EQ(config.quantumFrames, c.expected) << "at " << c.fps << " fps";
        EXPECT_LE(config.quantumFrames, kDefaultQuantumFrames);
        if (c.fps > kHighFrameRateThresholdFps) {
            EXPECT_LE(config.quantumFrames, 512u) << "at " << c.fps << " fps";
        }
        EXPECT_EQ(config.format.sampleRate, 48'000);
        EXPECT_EQ(config.format.channels, 2);
    }
}

TEST(AudioSinkSelection, TheProbeUsesTheResolvedQuantum) {
    FakeSink* pipewire = nullptr;

    AudioSinkSelectorOptions options;
    options.projectFrameRateFps = 60.0;
    options.pipewireFactory = fakeFactory("pipewire", /*openable=*/true, &pipewire);

    const AudioSinkSelection selection = selectAudioSink(std::move(options));

    EXPECT_EQ(selection.config.quantumFrames, kHighFrameRateQuantumFrames);
    ASSERT_NE(pipewire, nullptr);
    // The fake recorded the quantum it was opened with.
    EXPECT_EQ(pipewire->quantumFrames(), kHighFrameRateQuantumFrames);
}

// ===========================================================================
// The null fallback keeps the master clock running (Requirement 6.7)
// ===========================================================================

TEST(AudioSinkSelection, NullFallbackAdvancesOnTheInjectedClock) {
    ManualSteadyClock clock;

    AudioSinkSelectorOptions options;
    options.pipewireFactory = absentFactory();
    options.alsaFactory = absentFactory();
    options.clock = clock.asClock();

    AudioSinkSelection selection = selectAudioSink(std::move(options));
    ASSERT_EQ(selection.kind, AudioSinkKind::Null);
    ASSERT_TRUE(selection.sink->start(selection.config).isOk());

    // Submit half a second of silence, then move the clock a quarter second: the
    // clock — not a sleep — is what advances the position.
    AudioBuffer half(48'000, 2, 24'000);
    ASSERT_TRUE(selection.sink->submit(half).isOk());
    EXPECT_EQ(selection.sink->playedFrames(), 0u);

    clock.advance(std::chrono::milliseconds(250));
    EXPECT_EQ(selection.sink->playedFrames(), 12'000u);
    EXPECT_EQ(selection.sink->playedPosition(), Duration::fromMilliseconds(250));

    // Never more than was submitted, however far the clock runs on.
    clock.advance(std::chrono::seconds(10));
    EXPECT_EQ(selection.sink->playedFrames(), 24'000u);
    selection.sink->stop();
}

// ===========================================================================
// The real backends, on whatever host is running the suite
// ===========================================================================

TEST(AudioSinkBackends, CompiledInPredicatesAgreeWithTheBuild) {
    EXPECT_EQ(pipewireSinkCompiledIn(), PipeWireAudioSink::compiledIn());
    EXPECT_EQ(alsaSinkCompiledIn(), AlsaAudioSink::compiledIn());
}

TEST(AudioSinkBackends, RealSinksEitherOpenOrNameTheirReason) {
    // Whichever build and host this is, a real sink's start() is total: it either
    // opens (and can be closed again) or reports an error naming the backend. That
    // is the contract selectAudioSink() and AudioEngine::start() both rely on, and
    // on a machine with no audio device it is the failing branch that gets taken.
    const AudioSinkConfig config = audioSinkConfigFor(30.0);

    PipeWireAudioSink pipewire;
    if (Result<void> opened = pipewire.start(config); opened.isOk()) {
        EXPECT_TRUE(pipewire.running());
        pipewire.stop();
        EXPECT_FALSE(pipewire.running());
    } else {
        EXPECT_NE(opened.error().message().find("PipeWire"), std::string::npos);
        EXPECT_FALSE(pipewire.running());
    }

    AlsaAudioSink alsa;
    if (Result<void> opened = alsa.start(config); opened.isOk()) {
        EXPECT_TRUE(alsa.running());
        alsa.stop();
        EXPECT_FALSE(alsa.running());
    } else {
        EXPECT_NE(opened.error().message().find("ALSA"), std::string::npos);
        EXPECT_FALSE(alsa.running());
    }
}

TEST(AudioSinkBackends, RealSinksRejectAnInvalidConfigurationBeforeTouchingADevice) {
    AudioSinkConfig zeroQuantum;
    zeroQuantum.quantumFrames = 0;

    PipeWireAudioSink pipewire;
    Result<void>      pipewireResult = pipewire.start(zeroQuantum);
    ASSERT_TRUE(pipewireResult.isError());
    EXPECT_EQ(pipewireResult.error().code(), ErrorCode::InvalidArgument);

    AlsaAudioSink alsa;
    Result<void>  alsaResult = alsa.start(zeroQuantum);
    ASSERT_TRUE(alsaResult.isError());
    EXPECT_EQ(alsaResult.error().code(), ErrorCode::InvalidArgument);

    AudioSinkConfig badFormat;
    badFormat.format = AudioFormat{0, 0, SampleFormat::F32};
    Result<void> badPipewire = pipewire.start(badFormat);
    ASSERT_TRUE(badPipewire.isError());
    EXPECT_EQ(badPipewire.error().code(), ErrorCode::InvalidArgument);
}

TEST(AudioSinkBackends, SubmittingToAnUnstartedRealSinkFails) {
    AudioBuffer buffer(48'000, 2, 128);

    PipeWireAudioSink pipewire;
    Result<void>      pipewireResult = pipewire.submit(buffer);
    ASSERT_TRUE(pipewireResult.isError());
    EXPECT_EQ(pipewireResult.error().code(), ErrorCode::FailedPrecondition);

    AlsaAudioSink alsa;
    Result<void>  alsaResult = alsa.submit(buffer);
    ASSERT_TRUE(alsaResult.isError());
    EXPECT_EQ(alsaResult.error().code(), ErrorCode::FailedPrecondition);
}

TEST(AudioSinkBackends, SelectionOnThisHostAlwaysYieldsAUsableSink) {
    // The host-independent statement of the fall-through: selection never fails,
    // the chosen sink is one of the three, it is handed back closed, and a
    // non-real device always carries the Requirement 6.7 notice. On this suite's
    // usual host (no sound card, no audio daemon) the outcome is the null sink.
    AudioSinkSelection selection = selectAudioSink(AudioSinkSelectorOptions{});

    ASSERT_NE(selection.sink, nullptr);
    EXPECT_FALSE(selection.sink->running());
    EXPECT_EQ(selection.name, std::string(toString(selection.kind)));
    EXPECT_EQ(selection.realDevice, selection.kind != AudioSinkKind::Null);
    EXPECT_EQ(selection.notice.has_value(), selection.kind == AudioSinkKind::Null);

    // Whatever was chosen, it opens with the resolved configuration.
    ASSERT_TRUE(selection.sink->start(selection.config).isOk());
    AudioBuffer quantum(48'000, 2, selection.config.quantumFrames);
    EXPECT_TRUE(selection.sink->submit(quantum).isOk());
    EXPECT_LE(selection.sink->playedFrames(), selection.sink->submittedFrames());
    selection.sink->stop();
}

} // namespace palmier::media
