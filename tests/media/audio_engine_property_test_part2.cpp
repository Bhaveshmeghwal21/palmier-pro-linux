// SPDX-License-Identifier: GPL-3.0-or-later
//
// Part 2 of the audio_engine_property_test cases, split out of audio_engine_property_test.cpp.
//
// Suites here: AudioEngineTest, AudioSinkTest, NullAudioSinkTest.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "audio_engine_property_test_support.hpp"

namespace palmier::media {

using namespace palmier::media::
    testsupport_audioenginepropertytest;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Unit tests — NullAudioSink (task 8.3)
// ---------------------------------------------------------------------------

TEST(NullAudioSinkTest, AdvancesMonotonicPositionFromTheInjectedSteadyClock) {
    TestClock clock;
    NullAudioSink sink{clock.fn()};

    ASSERT_TRUE(sink.start(AudioSinkConfig{audioOutputFormat(), 480}).isOk());
    EXPECT_TRUE(sink.running());
    EXPECT_EQ(sink.playedFrames(), 0u);

    // Submitting without advancing the clock plays nothing yet.
    const AudioBuffer block(48'000, 2, 4'800);  // 100 ms
    ASSERT_TRUE(sink.submit(block).isOk());
    EXPECT_EQ(sink.submittedFrames(), 4'800u);
    EXPECT_EQ(sink.playedFrames(), 0u);

    // 50 ms of steady clock plays exactly half of it.
    clock.advance(std::chrono::milliseconds{50});
    EXPECT_EQ(sink.playedFrames(), 2'400u);
    EXPECT_EQ(sink.playedPosition(), Duration::fromMilliseconds(50));

    // The position never exceeds what was submitted, however long the clock runs.
    clock.advance(std::chrono::seconds{10});
    EXPECT_EQ(sink.playedFrames(), 4'800u);

    // ... and never regresses when the clock stalls.
    EXPECT_EQ(sink.playedFrames(), 4'800u);

    sink.stop();
    EXPECT_FALSE(sink.running());
    EXPECT_EQ(sink.playedFrames(), 4'800u);
}
TEST(NullAudioSinkTest, RejectsMisuseAndReportsTheConfiguredGeometry) {
    TestClock clock;
    NullAudioSink sink{clock.fn()};

    const AudioBuffer block(48'000, 2, 128);
    EXPECT_TRUE(sink.submit(block).isError());  // not started

    EXPECT_TRUE(sink.start(AudioSinkConfig{AudioFormat{0, 0, SampleFormat::F32}, 128}).isError());
    EXPECT_TRUE(sink.start(AudioSinkConfig{audioOutputFormat(), 0}).isError());

    ASSERT_TRUE(sink.start(AudioSinkConfig{audioOutputFormat(), 512}).isOk());
    EXPECT_EQ(sink.quantumFrames(), 512u);
    EXPECT_EQ(sink.format(), audioOutputFormat());
    EXPECT_TRUE(sink.start(AudioSinkConfig{audioOutputFormat(), 512}).isError());  // already open

    // A buffer in the wrong format is refused rather than silently accepted.
    const AudioBuffer wrong(44'100, 2, 128);
    EXPECT_TRUE(sink.submit(wrong).isError());
    // An empty buffer is a no-op, not an error.
    EXPECT_TRUE(sink.submit(AudioBuffer{48'000, 2, 0}).isOk());
    EXPECT_EQ(sink.submittedFrames(), 0u);
}
TEST(AudioSinkTest, RequestsASmallerQuantumAboveFortyEightFramesPerSecond) {
    EXPECT_EQ(preferredQuantumFrames(24.0), kDefaultQuantumFrames);
    EXPECT_EQ(preferredQuantumFrames(48.0), kDefaultQuantumFrames);
    EXPECT_EQ(preferredQuantumFrames(60.0), kHighFrameRateQuantumFrames);
    EXPECT_EQ(preferredQuantumFrames(120.0), kHighFrameRateQuantumFrames);
}
// ---------------------------------------------------------------------------
// Unit tests — AudioEngine (task 8.4)
// ---------------------------------------------------------------------------

TEST(AudioEngineTest, UnavailableOutputSuppressesAudioKeepsTheClockAndRaisesANotice) {
    // Requirement 6.7.
    TestClock clock;
    DecoderTeardownQueue teardown;
    AssetLibrary library;
    Project project = makeProject();

    AssetScript script;
    script.level = 0.5f;
    const MediaAssetRef asset = library.add(script);
    addAudioClip(project, asset, Duration::zero(), Duration::fromSeconds(5.0), 1.0, false);

    AudioEngineOptions options;
    options.quantumFrames = 480;  // 10 ms
    AudioEngine engine{[&project] { return &project; },
                       std::make_unique<UnavailableSink>(),
                       teardown,
                       library.factory(),
                       options,
                       clock.fn()};

    const auto startedAt = clock.now();
    ASSERT_TRUE(engine.start(Duration::zero()).isOk());  // not an error (6.7)
    EXPECT_FALSE(engine.outputAvailable());
    ASSERT_TRUE(engine.notice().has_value());
    EXPECT_NE(engine.notice()->find("audio output is unavailable"), std::string::npos);

    // The notice was raised well inside the 2 second budget, measured on the
    // injected clock — no sleeping involved.
    ASSERT_TRUE(engine.noticeRaisedAt().has_value());
    EXPECT_LE(*engine.noticeRaisedAt() - startedAt, std::chrono::seconds{2});

    // Audio is suppressed but the master clock keeps advancing, so video keeps
    // presenting at the project frame rate.
    ASSERT_TRUE(engine.pump().isOk());
    clock.advance(std::chrono::milliseconds{10});
    EXPECT_EQ(engine.presentationPosition(), Duration::fromMilliseconds(10));
    EXPECT_TRUE(engine.lastQuantum().suppressed);
    EXPECT_TRUE(engine.lastQuantum().contributions.empty());

    // The notice is retained until a device opens successfully.
    ASSERT_TRUE(engine.pump().isOk());
    EXPECT_TRUE(engine.notice().has_value());
}
TEST(AudioEngineTest, RetiresAudioDecodersThroughTheTeardownQueue) {
    // Requirement 14.8 / upstream PR 405: the audio decoder reuses the queue.
    TestClock clock;
    DecoderTeardownQueue teardown;
    AssetLibrary library;
    Project project = makeProject();

    for (int i = 0; i < 3; ++i) {
        AssetScript script;
        script.level = 0.1f;
        const MediaAssetRef asset = library.add(script);
        addAudioClip(project, asset, Duration::zero(), Duration::fromSeconds(5.0), 1.0, false);
    }

    AudioEngineOptions options;
    options.quantumFrames = 480;
    options.decoderCacheCapacity = 2;  // forces an eviction

    AudioEngine engine{[&project] { return &project; },
                       std::make_unique<NullAudioSink>(clock.fn()),
                       teardown,
                       library.factory(),
                       options,
                       clock.fn()};

    ASSERT_TRUE(engine.start(Duration::zero()).isOk());
    ASSERT_TRUE(engine.pump().isOk());
    EXPECT_LE(engine.residentDecoderCount(), 2u);
    EXPECT_GE(engine.stats().decodersOpened, 3u);
    EXPECT_GE(engine.stats().decodersRetired, 1u);

    engine.stop();
    EXPECT_EQ(engine.residentDecoderCount(), 0u);
    ASSERT_TRUE(teardown.drainFor(std::chrono::seconds{5}));
    EXPECT_EQ(teardown.pending(), 0u);
    EXPECT_EQ(teardown.acceptedCount(), teardown.retiredCount());
}
TEST(AudioEngineTest, MixesAtTheFixedOutputFormatAndRefusesMisuse) {
    TestClock clock;
    DecoderTeardownQueue teardown;
    AssetLibrary library;
    Project project = makeProject();

    AudioEngineOptions options;
    options.quantumFrames = 256;
    AudioEngine engine{[&project] { return &project; },
                       std::make_unique<NullAudioSink>(clock.fn()),
                       teardown,
                       library.factory(),
                       options,
                       clock.fn()};

    EXPECT_EQ(AudioEngine::outputFormat(), audioOutputFormat());
    EXPECT_TRUE(engine.pump().isError());  // not running
    EXPECT_TRUE(engine.start(Duration::fromMilliseconds(-1)).isError());

    ASSERT_TRUE(engine.start(Duration::fromSeconds(1.0)).isOk());
    EXPECT_TRUE(engine.start(Duration::zero()).isError());  // already running
    EXPECT_EQ(engine.startPosition(), Duration::fromSeconds(1.0));
    EXPECT_EQ(engine.presentationPosition(), Duration::fromSeconds(1.0));

    auto delivered = engine.pump();
    ASSERT_TRUE(delivered.isOk());
    EXPECT_EQ(delivered.value(), 256u);
    EXPECT_EQ(engine.mixPosition(), Duration::fromSeconds(1.0) + framesToDuration(256, 48'000));

    // An empty project mixes to silence with no error.
    EXPECT_TRUE(engine.errors().empty());
    EXPECT_TRUE(engine.lastQuantum().contributions.empty());

    // renderRange refuses an inverted range and yields exact silence for an empty
    // project (Requirement 6.11).
    EXPECT_TRUE(
        engine.renderRange(project, Duration::fromSeconds(1.0), Duration::zero()).isError());
    auto silent = engine.renderRange(project, Duration::zero(), Duration::fromSeconds(0.5));
    ASSERT_TRUE(silent.isOk());
    EXPECT_EQ(silent.value().frameCount(), 24'000u);
    EXPECT_EQ(peak(silent.value().samples()), 0.0f);
}
TEST(AudioEngineTest, AnAssetThatCannotBeOpenedSilencesTheClipAndNamesIt) {
    // Requirement 6.9 for the "decoder cannot even be opened" case.
    TestClock clock;
    DecoderTeardownQueue teardown;
    AssetLibrary library;
    Project project = makeProject();

    // An asset the synthetic factory knows nothing about.
    MediaAssetRef missing{Uuid::generateV4(), "synthetic://not-registered"};
    addAudioClip(project, missing, Duration::zero(), Duration::fromSeconds(1.0), 1.0, false);

    AudioEngineOptions options;
    options.quantumFrames = 480;
    AudioEngine engine{[&project] { return &project; },
                       std::make_unique<NullAudioSink>(clock.fn()),
                       teardown,
                       library.factory(),
                       options,
                       clock.fn()};

    ASSERT_TRUE(engine.start(Duration::zero()).isOk());
    ASSERT_TRUE(engine.pump().isOk());  // mixing continues
    ASSERT_EQ(engine.errors().size(), 1u);
    EXPECT_NE(engine.errors()[0].find("synthetic://not-registered"), std::string::npos);
    ASSERT_EQ(engine.lastQuantum().contributions.size(), 1u);
    EXPECT_TRUE(engine.lastQuantum().contributions[0].failed);

    // The failure is not repeated for the rest of the clip: one error, silence
    // thereafter.
    ASSERT_TRUE(engine.pump().isOk());
    EXPECT_EQ(engine.errors().size(), 1u);
}

}  // namespace palmier::media
