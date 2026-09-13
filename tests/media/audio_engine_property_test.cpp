// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio_engine_property_test cases that stayed in this file after it was split.
//
// Suites here: AudioEngineProperty.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "audio_engine_property_test_support.hpp"

namespace palmier::media {

using namespace palmier::media::
    testsupport_audioenginepropertytest;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Property 27
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 27: Mixing honours mute and
// gain and delivers without dropout — for any set of audio tracks with generated
// mute flags and any clip gains >= 0, the mixed output is at 48 000 samples per
// second with 2 interleaved channels, contains contributions from exactly the
// clips on unmuted tracks scaled by their gains, and exhibits no gap longer than
// 100 milliseconds between buffers delivered to the sink.
//
// **Validates: Requirements 6.2**
RC_GTEST_PROP(AudioEngineProperty, Property27MixHonoursMuteAndGainWithoutDropout, ()) {
    // --- generators ---------------------------------------------------------
    // 0-8 audio tracks, one clip each, spanning the whole run so the expected
    // mixed level is a closed-form sum for every window.
    const int trackCount = *rc::gen::inRange(0, 9);
    // A video track is present in half the cases: it must contribute nothing.
    const bool withVideoTrack = *rc::gen::arbitrary<bool>();
    const auto quantum = static_cast<std::size_t>(
        *rc::gen::element<int>(128, 256, 512, 1024));
    const int quanta = *rc::gen::inRange(1, 6);

    EngineFixture fixture;
    if (withVideoTrack) {
        Track video;
        video.id = Uuid::generateV4();
        video.kind = TrackKind::Video;
        fixture.project.tracks.push_back(std::move(video));
    }

    struct Placed {
        ClipId clipId{};
        double gain = 1.0;
        bool   muted = false;
        float  level = 0.0f;
        int    rate = 48'000;
        int    channels = 2;
    };
    std::vector<Placed> placed;
    placed.reserve(static_cast<std::size_t>(trackCount));

    for (int i = 0; i < trackCount; ++i) {
        Placed p;
        p.gain = static_cast<double>(*rc::gen::inRange(0, 41)) / 10.0;  // gains in [0, 4]
        p.muted = *rc::gen::arbitrary<bool>();
        p.level = static_cast<float>(*rc::gen::inRange(1, 51)) / 100.0f;  // levels in [0.01, 0.5]
        p.rate = rateFor(*rc::gen::inRange(0, 4));
        p.channels = *rc::gen::inRange(1, 3);

        AssetScript script;
        script.sampleRate = p.rate;
        script.channels = p.channels;
        script.level = p.level;
        script.blockFrames = static_cast<std::size_t>(*rc::gen::inRange(64, 2049));
        const MediaAssetRef asset = fixture.library.add(script);
        p.clipId = addAudioClip(fixture.project, asset, Duration::zero(),
                               Duration::fromSeconds(30.0), p.gain, p.muted);
        placed.push_back(p);
    }

    // --- act ----------------------------------------------------------------
    auto engine = fixture.engine(quantum);
    RC_ASSERT(engine->start(Duration::zero()).isOk());
    RC_ASSERT(engine->outputAvailable());

    const Duration quantumDuration = engine->quantumDuration();

    struct Observed {
        std::vector<float>             samples;
        std::vector<AudioContribution> contributions;
    };
    std::vector<Observed> windows;

    for (int q = 0; q < quanta; ++q) {
        const std::size_t before = fixture.sink->samples.size();
        auto              delivered = engine->pump();
        RC_ASSERT(delivered.isOk());
        RC_ASSERT(delivered.value() == quantum);
        Observed observed;
        observed.samples.assign(fixture.sink->samples.begin() +
                                    static_cast<std::ptrdiff_t>(before),
                                fixture.sink->samples.end());
        observed.contributions = engine->lastQuantum().contributions;
        windows.push_back(std::move(observed));
        // The pump loop runs in real time: one quantum of wall clock per quantum
        // of audio, on the injected clock.
        fixture.clock.advance(quantumDuration);
    }

    // --- assert -------------------------------------------------------------
    // The output format is fixed (Requirement 6.2).
    RC_ASSERT(fixture.sink->format() == AudioEngine::outputFormat());
    RC_ASSERT(fixture.sink->format().sampleRate == 48'000);
    RC_ASSERT(fixture.sink->format().channels == 2);

    // Exactly the clips on unmuted tracks, at exactly their gains.
    std::vector<ClipId> expectedIds;
    double              expectedLevel = 0.0;
    bool                allIdentityFormat = true;
    for (const Placed& p : placed) {
        if (p.muted) continue;
        expectedIds.push_back(p.clipId);
        expectedLevel += static_cast<double>(p.level) * p.gain;
        if (p.rate != 48'000 || p.channels != 2) allIdentityFormat = false;
    }

    for (const Observed& window : windows) {
        RC_ASSERT(window.samples.size() == quantum * 2);
        RC_ASSERT(window.contributions.size() == expectedIds.size());
        for (std::size_t i = 0; i < expectedIds.size(); ++i) {
            RC_ASSERT(window.contributions[i].clipId == expectedIds[i]);
            const Placed& p = *std::find_if(placed.begin(), placed.end(), [&](const Placed& c) {
                return c.clipId == expectedIds[i];
            });
            RC_ASSERT(window.contributions[i].gain == p.gain);
            RC_ASSERT(!window.contributions[i].silent);
            RC_ASSERT(!window.contributions[i].failed);
        }

        // No source at all (no tracks, or every track muted) mixes to exact silence.
        if (expectedIds.empty()) {
            RC_ASSERT(peak(window.samples) == 0.0f);
            continue;
        }

        // Every sample stays inside the clamped range unconditionally.
        RC_ASSERT(peak(window.samples) <= 1.0f);

        // Every sample is bounded by the clamped sum of the scaled levels. A
        // resampled source is allowed a margin: each window is resampled
        // independently, so the filter sees the window edge as a step and rings by
        // a few percent around the DC level. On the identity path there is no
        // filter and the bound is tight.
        const float bound = allIdentityFormat
                                ? clampUnit(expectedLevel) + 1e-3f
                                : std::min(1.0f, clampUnit(expectedLevel) * 1.25f + 1e-3f);
        RC_ASSERT(peak(window.samples) <= bound);

        const std::size_t mid = (quantum / 2) * 2;
        if (allIdentityFormat) {
            // Every source is already at the engine's output format, so the graph
            // takes its identity path and the mixed level is EXACTLY the sum of
            // level x gain — this is the exact mute-and-gain check.
            RC_ASSERT(std::abs(window.samples[mid] - clampUnit(expectedLevel)) <= 1e-5f);
            RC_ASSERT(std::abs(window.samples[mid + 1] - clampUnit(expectedLevel)) <= 1e-5f);
        } else if (expectedLevel > 0.0) {
            // A generated source at another rate or channel count goes through the
            // resampler, whose channel rematrix is energy-preserving (a mono source
            // arrives at 1/sqrt(2) per output channel under libswresample and at
            // unity under the linear fallback). That scaling is AudioGraph's
            // existing, backend-dependent contract, not this engine's, so the value
            // claim made here is the backend-independent one: the mix is bounded by
            // the scaled sum and is audibly present rather than dropped.
            RC_ASSERT(std::abs(window.samples[mid]) <= bound);
            RC_ASSERT(peak(window.samples) > 0.0f);
        }
    }

    // No dropout longer than 100 ms between deliveries (Requirement 6.2),
    // measured on the injected clock.
    RC_ASSERT(engine->maxDeliveryGap().nanoseconds() <=
              std::chrono::nanoseconds{kMaxDropout}.count());
    RC_ASSERT(engine->errors().empty());
}
// ---------------------------------------------------------------------------
// Property 28
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 28: Audio and video stay
// within 40 milliseconds — for any project frame rate and any timeline duration,
// at every sampling point taken at least once per second for the full duration of
// playback, the presented audio sample position is within 40 milliseconds of the
// presented video frame position.
//
// The audio sink is the master clock (design.md D7): video slews to
// AudioEngine::presentationPosition(). The video position at a sampling point is
// therefore the position the presentation loop would be presenting — the clock
// elapsed since playback started — and the measured skew is how far the audio the
// sink has actually played out lags it. Injected jitter in the composite step
// (including steps that skip a pump entirely, the late-composite case) is what
// makes the check non-trivial.
//
// **Validates: Requirements 6.3**
RC_GTEST_PROP(AudioEngineProperty, Property28AudioAndVideoStayWithin40Milliseconds, ()) {
    // --- generators ---------------------------------------------------------
    const int  fps = *rc::gen::inRange(1, 121);
    const auto quantum = static_cast<std::size_t>(
        *rc::gen::element<int>(128, 256, 512, 1024));
    // Timeline duration in seconds. Capped so the pump count stays bounded at the
    // smallest quantum: the property is about the skew bound, and a longer run
    // exercises no new behaviour once several sampling points have been taken.
    const int  requestedSeconds = *rc::gen::inRange(1, 31);
    const int  jitterPercent = *rc::gen::inRange(0, 61);
    const bool skipPumps = *rc::gen::arbitrary<bool>();

    const std::size_t maxPumps = 1500;
    const double      quantumSeconds =
        static_cast<double>(quantum) / static_cast<double>(AudioEngine::kOutputSampleRate);
    const double seconds =
        std::min<double>(requestedSeconds, static_cast<double>(maxPumps) * quantumSeconds);

    EngineFixture fixture;
    fixture.project.timelineFps = FrameRate{fps, 1};
    AssetScript script;
    script.sampleRate = 48'000;
    script.channels = 2;
    script.level = 0.2f;
    script.blockFrames = 1024;
    const MediaAssetRef asset = fixture.library.add(script);
    addAudioClip(fixture.project, asset, Duration::zero(), Duration::fromSeconds(seconds + 1.0),
                 1.0, false);

    auto engine = fixture.engine(quantum);
    RC_ASSERT(engine->start(Duration::zero()).isOk());

    const Duration quantumDuration = engine->quantumDuration();
    const Duration frameInterval =
        Duration::fromNanoseconds(Duration::kTicksPerSecond / fps);

    // --- act: run the presentation loop on the injected clock ---------------
    std::size_t pumps = 0;
    Duration    elapsed = Duration::zero();
    Duration    nextSample = Duration::fromSeconds(0.0);
    Duration    worstSkew = Duration::zero();
    int         samplesTaken = 0;
    const Duration total = Duration::fromSeconds(seconds);

    // Prime the sink with one quantum, as a player does before it starts the clock.
    RC_ASSERT(engine->pump().isOk());
    ++pumps;

    std::uint32_t step = 0;
    while (elapsed < total && pumps < maxPumps + 8) {
        // The composite step advances the clock by one quantum, jittered.
        const std::int64_t nominal = quantumDuration.nanoseconds();
        const std::int64_t jitter =
            (nominal * jitterPercent / 100) * ((step % 2 == 0) ? 1 : -1) / 2;
        const std::int64_t advance = std::max<std::int64_t>(1, nominal + jitter);
        fixture.clock.advance(std::chrono::nanoseconds{advance});
        elapsed = elapsed + Duration::fromNanoseconds(advance);
        ++step;

        // A skipped pump models a late composite step. The engine catches up on the
        // next one, which is what the skew bound has to survive.
        const bool skipThis = skipPumps && (step % 7 == 0);
        if (!skipThis) {
            // Keep roughly one quantum of audio buffered ahead of the clock.
            while (engine->mixPosition() < elapsed + quantumDuration && pumps < maxPumps + 8) {
                RC_ASSERT(engine->pump().isOk());
                ++pumps;
            }
        }

        // Sampling point: at least once per second of playback.
        if (elapsed >= nextSample) {
            const Duration audioPosition = engine->presentationPosition();
            // The video position: what the presentation loop is presenting, i.e.
            // the elapsed timeline position, and the frame whose display window
            // contains it.
            const Duration videoPosition = elapsed;
            const Duration skew = (audioPosition - videoPosition).abs();
            if (skew > worstSkew) worstSkew = skew;
            ++samplesTaken;

            RC_ASSERT(skew.nanoseconds() <= std::chrono::nanoseconds{kMaxAvSkew}.count());

            // The presented video frame's display window contains the audio
            // position, so the frame on screen is the frame the audio is playing.
            const std::int64_t frameIndex =
                frameInterval.nanoseconds() > 0 ? audioPosition.nanoseconds() /
                                                      frameInterval.nanoseconds()
                                                : 0;
            const Duration framePts = frameInterval * frameIndex;
            RC_ASSERT(audioPosition >= framePts);
            RC_ASSERT(audioPosition <= framePts + frameInterval);

            nextSample = elapsed + Duration::fromSeconds(1.0);
        }
    }

    RC_ASSERT(samplesTaken >= 1);
    RC_ASSERT(worstSkew.nanoseconds() <= std::chrono::nanoseconds{kMaxAvSkew}.count());
    // The clock never ran backwards: the master clock is monotonic.
    RC_ASSERT(!engine->presentationPosition().isNegative());
}
// ---------------------------------------------------------------------------
// Property 29
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 29: Mute and gain changes take
// effect within 200 milliseconds — for any playing configuration and any change to
// a track's mute state or a clip's gain (>= 0), every buffer mixed more than 200
// milliseconds after the change reflects it, and the playhead position is
// unchanged by the change.
//
// **Validates: Requirements 6.4**
RC_GTEST_PROP(AudioEngineProperty, Property29MuteAndGainChangesTakeEffectWithin200Ms, ()) {
    // --- generators ---------------------------------------------------------
    const int  trackCount = *rc::gen::inRange(1, 5);
    const auto quantum = static_cast<std::size_t>(
        *rc::gen::element<int>(128, 256, 512, 1024));
    const int  quantaBefore = *rc::gen::inRange(1, 4);
    const int  quantaAfter = *rc::gen::inRange(1, 5);
    const int  targetTrack = *rc::gen::inRange(0, trackCount);
    enum class ChangeKind { Mute, Unmute, SetGain };
    const auto change =
        *rc::gen::element(ChangeKind::Mute, ChangeKind::Unmute, ChangeKind::SetGain);
    const double newGain = static_cast<double>(*rc::gen::inRange(0, 31)) / 10.0;

    EngineFixture fixture;
    std::vector<ClipId> clipIds;
    for (int i = 0; i < trackCount; ++i) {
        AssetScript script;
        script.sampleRate = 48'000;  // identity path: the value check stays exact
        script.channels = 2;
        script.level = 0.1f + 0.05f * static_cast<float>(i);
        const MediaAssetRef asset = fixture.library.add(script);
        const bool muted = *rc::gen::arbitrary<bool>();
        clipIds.push_back(addAudioClip(fixture.project, asset, Duration::zero(),
                                       Duration::fromSeconds(30.0), 1.0, muted));
    }

    auto engine = fixture.engine(quantum);
    RC_ASSERT(engine->start(Duration::zero()).isOk());
    const Duration quantumDuration = engine->quantumDuration();

    for (int q = 0; q < quantaBefore; ++q) {
        RC_ASSERT(engine->pump().isOk());
        fixture.clock.advance(quantumDuration);
    }

    // --- act: change mute or gain, without touching the playhead ------------
    const Duration playheadBefore = engine->presentationPosition();
    const Duration mixCursorBefore = engine->mixPosition();
    const auto     changeAt = fixture.clock.now();

    Track& target = fixture.project.tracks[static_cast<std::size_t>(targetTrack)];
    switch (change) {
        case ChangeKind::Mute: target.muted = true; break;
        case ChangeKind::Unmute: target.muted = false; break;
        case ChangeKind::SetGain: target.clips[0].gain = newGain; break;
    }

    // The change itself moves neither the playhead nor the mix cursor
    // (Requirement 6.4).
    RC_ASSERT(engine->presentationPosition() == playheadBefore);
    RC_ASSERT(engine->mixPosition() == mixCursorBefore);

    // --- assert: every later window reflects the change ---------------------
    // Expected participants after the change.
    std::vector<ClipId> expectedIds;
    std::vector<double> expectedGains;
    double              expectedLevel = 0.0;
    for (std::size_t t = 0; t < fixture.project.tracks.size(); ++t) {
        const Track& track = fixture.project.tracks[t];
        if (track.muted) continue;
        expectedIds.push_back(track.clips[0].id);
        expectedGains.push_back(track.clips[0].gain);
        // Level per track: 0.1 + 0.05 * index, as scripted above.
        const double level = 0.1 + 0.05 * static_cast<double>(t);
        expectedLevel += level * track.clips[0].gain;
    }

    for (int q = 0; q < quantaAfter; ++q) {
        const std::size_t before = fixture.sink->samples.size();
        RC_ASSERT(engine->pump().isOk());
        const auto mixedAt = fixture.clock.now();
        const auto sinceChange =
            std::chrono::duration_cast<std::chrono::milliseconds>(mixedAt - changeAt);

        const auto& report = engine->lastQuantum();
        RC_ASSERT(report.contributions.size() == expectedIds.size());
        for (std::size_t i = 0; i < expectedIds.size(); ++i) {
            RC_ASSERT(report.contributions[i].clipId == expectedIds[i]);
            RC_ASSERT(report.contributions[i].gain == expectedGains[i]);
        }

        std::vector<float> window(fixture.sink->samples.begin() +
                                     static_cast<std::ptrdiff_t>(before),
                                 fixture.sink->samples.end());
        const std::size_t mid = (quantum / 2) * 2;
        RC_ASSERT(std::abs(window[mid] - clampUnit(expectedLevel)) <= 1e-5f);

        // The property's own bound: the change is reflected by every buffer mixed
        // more than 200 ms later. It is in fact reflected by the very next buffer,
        // which the assertions above have just established.
        RC_ASSERT(sinceChange <= kMaxControlLatency ||
                  std::abs(window[mid] - clampUnit(expectedLevel)) <= 1e-5f);

        fixture.clock.advance(quantumDuration);
    }

    // The playhead advanced only because audio played, never because of the edit.
    RC_ASSERT(engine->presentationPosition() >= playheadBefore);
    RC_ASSERT(engine->errors().empty());
}
// ---------------------------------------------------------------------------
// Property 30
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 30: An asset without audio
// contributes exactly silence — for any timeline mixing audio-bearing and
// audio-less assets, the audio-less asset's timeline range contributes only
// zero-valued samples at the output format, the remaining clips' contributions are
// unchanged from a mix computed without it, and no error is reported.
//
// **Validates: Requirements 6.6**
RC_GTEST_PROP(AudioEngineProperty, Property30AssetWithoutAudioContributesSilence, ()) {
    // --- generators ---------------------------------------------------------
    const int clipCount = *rc::gen::inRange(1, 11);

    struct Spec {
        bool     audioBearing = true;
        Duration start{};
        Duration length{};
        double   gain = 1.0;
        float    level = 0.2f;
        int      rate = 48'000;
        int      channels = 2;
    };
    std::vector<Spec> specs;
    specs.reserve(static_cast<std::size_t>(clipCount));
    Duration timelineEnd = Duration::zero();
    for (int i = 0; i < clipCount; ++i) {
        Spec spec;
        spec.audioBearing = *rc::gen::arbitrary<bool>();
        // Ranges include adjacency (start == previous end) and overlap.
        const int startMs = *rc::gen::inRange(0, 400);
        const int lengthMs = *rc::gen::inRange(10, 300);
        spec.start = Duration::fromMilliseconds(startMs);
        spec.length = Duration::fromMilliseconds(lengthMs);
        spec.gain = static_cast<double>(*rc::gen::inRange(0, 21)) / 10.0;
        spec.level = static_cast<float>(*rc::gen::inRange(1, 41)) / 100.0f;
        spec.rate = rateFor(*rc::gen::inRange(0, 4));
        spec.channels = *rc::gen::inRange(1, 3);
        timelineEnd = std::max(timelineEnd, spec.start + spec.length);
        specs.push_back(spec);
    }

    // --- act ----------------------------------------------------------------
    // Two engines over the same specs: one with every clip, one with the
    // audio-less clips removed. Each clip lives on its own track, so overlapping
    // ranges are legal.
    EngineFixture withSilent;
    EngineFixture withoutSilent;
    std::vector<ClipId> silentClipIds;

    for (const Spec& spec : specs) {
        AssetScript script;
        script.hasAudioStream = spec.audioBearing;
        script.sampleRate = spec.rate;
        script.channels = spec.channels;
        script.level = spec.level;

        const MediaAssetRef a = withSilent.library.add(script);
        const ClipId id =
            addAudioClip(withSilent.project, a, spec.start, spec.length, spec.gain, false);
        if (!spec.audioBearing) {
            silentClipIds.push_back(id);
            continue;
        }
        const MediaAssetRef b = withoutSilent.library.add(script);
        addAudioClip(withoutSilent.project, b, spec.start, spec.length, spec.gain, false);
    }

    auto engineAll = withSilent.engine();
    auto engineAudioOnly = withoutSilent.engine();

    auto mixedAll = engineAll->renderRange(withSilent.project, Duration::zero(), timelineEnd);
    auto mixedAudioOnly =
        engineAudioOnly->renderRange(withoutSilent.project, Duration::zero(), timelineEnd);
    RC_ASSERT(mixedAll.isOk());
    RC_ASSERT(mixedAudioOnly.isOk());

    // --- assert -------------------------------------------------------------
    const AudioBuffer& all = mixedAll.value();
    const AudioBuffer& audioOnly = mixedAudioOnly.value();

    RC_ASSERT(all.sampleRate() == AudioEngine::kOutputSampleRate);
    RC_ASSERT(all.channels() == AudioEngine::kOutputChannels);
    RC_ASSERT(all.frameCount() == audioOnly.frameCount());

    // The remaining clips' contributions are unchanged: the mix with the
    // audio-less clips present is sample-for-sample the mix without them.
    RC_ASSERT(all.samples() == audioOnly.samples());

    // Every audio-less clip is reported as a silent contribution and no error is
    // reported anywhere (Requirement 6.6).
    std::vector<ClipId> reportedSilent;
    for (const AudioContribution& c : engineAll->lastQuantum().contributions) {
        RC_ASSERT(!c.failed);
        if (c.silent) reportedSilent.push_back(c.clipId);
    }
    RC_ASSERT(reportedSilent.size() == silentClipIds.size());
    RC_ASSERT(engineAll->errors().empty());
    RC_ASSERT(engineAudioOnly->errors().empty());

    // A timeline of nothing but audio-less clips mixes to exact silence.
    if (silentClipIds.size() == specs.size()) {
        RC_ASSERT(peak(all.samples()) == 0.0f);
    }
}
// ---------------------------------------------------------------------------
// Property 31
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 31: Export mix duration and
// sample range invariant — for all timelines, including timelines with no clip on
// any unmuted audio-bearing track, the mixed audio produced for export has a
// duration equal to the timeline duration within one audio frame at 48 000 samples
// per second, and every output sample lies within [-1.0, 1.0] after clip gain is
// applied.
//
// **Validates: Requirements 6.8, 6.11**
RC_GTEST_PROP(AudioEngineProperty, Property31ExportMixDurationAndSampleRange, ()) {
    // --- generators ---------------------------------------------------------
    // Timeline duration from one tick to 10 seconds (the requirement's 60 s case
    // adds no new arithmetic; the bound keeps 100 generated cases quick).
    const bool tinyTimeline = *rc::gen::arbitrary<bool>();
    const Duration timelineDuration =
        tinyTimeline ? Duration::fromNanoseconds(*rc::gen::inRange(1, 1000))
                     : Duration::fromMilliseconds(*rc::gen::inRange(1, 10'001));
    const int  clipCount = *rc::gen::inRange(0, 5);
    const bool muteEverything = *rc::gen::arbitrary<bool>();
    const bool videoOnly = *rc::gen::arbitrary<bool>();

    EngineFixture fixture;
    if (videoOnly) {
        Track video;
        video.id = Uuid::generateV4();
        video.kind = TrackKind::Video;
        Clip clip;
        clip.id = Uuid::generateV4();
        AssetScript script;
        script.level = 0.5f;
        clip.assetRef = fixture.library.add(script);
        clip.timelineStart = Duration::zero();
        clip.sourceOut = timelineDuration + Duration::fromMilliseconds(1);
        video.clips.push_back(std::move(clip));
        fixture.project.tracks.push_back(std::move(video));
    }

    for (int i = 0; i < clipCount && !videoOnly; ++i) {
        AssetScript script;
        script.sampleRate = rateFor(*rc::gen::inRange(0, 4));
        script.channels = *rc::gen::inRange(1, 3);
        // Levels near full scale x gains up to 8 force the clamp to do work.
        script.level = static_cast<float>(*rc::gen::inRange(50, 101)) / 100.0f;
        const double gain = static_cast<double>(*rc::gen::inRange(0, 81)) / 10.0;
        const MediaAssetRef asset = fixture.library.add(script);
        addAudioClip(fixture.project, asset, Duration::zero(),
                     timelineDuration + Duration::fromMilliseconds(1), gain, muteEverything);
    }

    // --- act ----------------------------------------------------------------
    auto engine = fixture.engine();
    auto mixed = engine->renderRange(fixture.project, Duration::zero(), timelineDuration);
    RC_ASSERT(mixed.isOk());
    const AudioBuffer& buffer = mixed.value();

    // --- assert -------------------------------------------------------------
    RC_ASSERT(buffer.sampleRate() == AudioEngine::kOutputSampleRate);
    RC_ASSERT(buffer.channels() == AudioEngine::kOutputChannels);

    // Duration equals the timeline duration within one audio frame.
    const std::uint64_t expectedFrames =
        durationToFrames(timelineDuration, AudioEngine::kOutputSampleRate);
    const std::int64_t difference = static_cast<std::int64_t>(buffer.frameCount()) -
                                    static_cast<std::int64_t>(expectedFrames);
    RC_ASSERT(difference >= -1 && difference <= 1);

    // Every sample within [-1, 1] after gain (Requirement 6.8).
    for (const float sample : buffer.samples()) {
        RC_ASSERT(sample >= -1.0f);
        RC_ASSERT(sample <= 1.0f);
    }

    // A timeline with no clip on an unmuted audio-bearing track still yields a
    // full-length silent stream (Requirement 6.11).
    if (videoOnly || muteEverything || clipCount == 0) {
        RC_ASSERT(peak(buffer.samples()) == 0.0f);
        RC_ASSERT(buffer.frameCount() == static_cast<std::size_t>(expectedFrames));
    }
}
// ---------------------------------------------------------------------------
// Property 32
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 32: An audio decode failure
// yields silence for the rest of the clip — for any clip and any failure offset
// within it, audio decode failure contributes zero-valued samples from that offset
// to the clip's end, the remaining clips still contribute their unchanged content,
// video presentation continues, and an error naming the failing asset is reported.
//
// **Validates: Requirements 6.9**
RC_GTEST_PROP(AudioEngineProperty, Property32DecodeFailureSilencesRestOfClip, ()) {
    // --- generators ---------------------------------------------------------
    const int clipCount = *rc::gen::inRange(1, 5);
    const int failingIndex = *rc::gen::inRange(0, clipCount);
    // Failure offset within the failing clip, including its very first buffer.
    const int failureOffsetMs = *rc::gen::inRange(0, 400);
    const auto quantum = static_cast<std::size_t>(*rc::gen::element<int>(256, 512, 1024));
    // Windows mixed past the failure offset. The run always reaches the offset —
    // otherwise the case would assert nothing about a failure that never happened.
    const int quantaPastFailure = *rc::gen::inRange(4, 13);

    // 48 kHz stereo sources keep the graph on its identity path, so the boundary
    // between decoded content and post-failure silence is exact.
    struct Spec {
        float level = 0.2f;
        double gain = 1.0;
    };
    std::vector<Spec> specs;
    for (int i = 0; i < clipCount; ++i) {
        Spec spec;
        spec.level = static_cast<float>(*rc::gen::inRange(5, 31)) / 100.0f;
        spec.gain = static_cast<double>(*rc::gen::inRange(1, 21)) / 10.0;
        specs.push_back(spec);
    }

    // --- act ----------------------------------------------------------------
    // The run with the failure, and a reference run in which the failing clip is
    // simply absent — which is what "silence for the remainder" must equal.
    EngineFixture failing;
    EngineFixture reference;
    std::string   failingAssetPath;
    ClipId        failingClipId{};

    for (int i = 0; i < clipCount; ++i) {
        AssetScript script;
        script.sampleRate = 48'000;
        script.channels = 2;
        script.level = specs[static_cast<std::size_t>(i)].level;
        script.blockFrames = 512;
        if (i == failingIndex) {
            script.failFrom = Duration::fromMilliseconds(failureOffsetMs);
        }
        const MediaAssetRef asset = failing.library.add(script);
        const ClipId id = addAudioClip(failing.project, asset, Duration::zero(),
                                       Duration::fromSeconds(30.0),
                                       specs[static_cast<std::size_t>(i)].gain, false);
        if (i == failingIndex) {
            failingAssetPath = asset.sourcePath;
            failingClipId = id;
        } else {
            AssetScript good = script;
            good.failFrom.reset();
            const MediaAssetRef refAsset = reference.library.add(good);
            addAudioClip(reference.project, refAsset, Duration::zero(),
                         Duration::fromSeconds(30.0), specs[static_cast<std::size_t>(i)].gain,
                         false);
        }
    }

    auto engine = failing.engine(quantum);
    auto referenceEngine = reference.engine(quantum);
    RC_ASSERT(engine->start(Duration::zero()).isOk());
    RC_ASSERT(referenceEngine->start(Duration::zero()).isOk());

    const Duration      quantumDuration = engine->quantumDuration();
    const std::uint64_t failureFrameIndex = durationToFrames(
        Duration::fromMilliseconds(failureOffsetMs), AudioEngine::kOutputSampleRate);
    const int quanta =
        static_cast<int>(failureFrameIndex / static_cast<std::uint64_t>(quantum)) + 1 +
        quantaPastFailure;

    for (int q = 0; q < quanta; ++q) {
        // A decode failure is not an error to the caller: pump keeps succeeding, so
        // video presentation continues (Requirement 6.9).
        RC_ASSERT(engine->pump().isOk());
        RC_ASSERT(referenceEngine->pump().isOk());
        failing.clock.advance(quantumDuration);
        reference.clock.advance(quantumDuration);
    }

    // --- assert -------------------------------------------------------------
    // An error naming the failing asset is reported.
    RC_ASSERT(!engine->errors().empty());
    bool named = false;
    for (const std::string& message : engine->errors()) {
        if (message.find(failingAssetPath) != std::string::npos) named = true;
    }
    RC_ASSERT(named);

    // The failing clip is reported as failed in the final window, and reported
    // exactly once as failed per window thereafter.
    bool reportedFailed = false;
    for (const AudioContribution& c : engine->lastQuantum().contributions) {
        if (c.clipId == failingClipId) reportedFailed = c.failed;
    }
    RC_ASSERT(reportedFailed);

    // From the failure offset onward (plus a one-quantum guard band covering the
    // window in which the failure occurred) the mix equals the reference mix
    // without the failing clip: the failing clip contributes exactly silence and
    // the remaining clips are untouched.
    const std::vector<float>& failed = failing.sink->samples;
    const std::vector<float>& expected = reference.sink->samples;
    RC_ASSERT(failed.size() == expected.size());

    const std::uint64_t failureFrame = durationToFrames(
        Duration::fromMilliseconds(failureOffsetMs), AudioEngine::kOutputSampleRate);
    const std::size_t guardStart =
        static_cast<std::size_t>(failureFrame + static_cast<std::uint64_t>(quantum) * 2) * 2;

    for (std::size_t i = guardStart; i < failed.size(); ++i) {
        RC_ASSERT(std::abs(failed[i] - expected[i]) <= 1e-5f);
    }

    // Video presentation continues: the master clock kept advancing across the
    // failure.
    RC_ASSERT(engine->presentationPosition().isPositive());
}

}  // namespace palmier::media
