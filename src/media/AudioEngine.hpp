// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioEngine.hpp — the composed audio decode → mix → output pipeline
// (task 8.4; Requirements 6.2, 6.3, 6.4, 6.6, 6.7, 6.9).
//
// This is the audio counterpart of DecoderClipFrameProvider: the one component
// that knows about *timelines* on the audio side. It sits between three pieces
// that already exist and invents no parallel machinery:
//
//   * media::MediaDecoder's audio surface (task 8.1) — openAudioStream /
//     nextAudioFrame / seekAudio / hasAudio — supplies interleaved-float source
//     blocks at each asset's own rate and channel count.
//   * media::AudioGraph (already in the tree) — does ALL of the resampling,
//     channel up/down-mix, per-source gain, summing and [-1, 1] clamping. The
//     engine adds one graph source per contributing clip and calls render(); it
//     contains no mixing arithmetic of its own.
//   * media::IAudioSink (task 8.3) — receives the mixed 48 kHz / 2-channel / F32
//     buffers and IS the master clock (design.md D7 "Decision — A/V sync").
//
// Retired decoders go to media::DecoderTeardownQueue (task 7.1, upstream
// PR 405): closing an FFmpeg decode context can block for hundreds of
// milliseconds and must never do so on the thread that is mixing audio. The
// queue exists precisely so the audio decoder reuses it.
//
// ## Requirement mapping
//
//   * 6.2 — every clip on an unmuted audio-bearing track is mixed at its clip
//     gain into the fixed 48 000 Hz / 2-channel / F32 output, delivered to the
//     sink with no dropout longer than 100 ms. maxDeliveryGap() reports the
//     largest observed gap between deliveries, measured on the injected clock.
//   * 6.3 — presentationPosition() is the master clock, read straight from the
//     sink's played-out frame count. Video slews to it.
//   * 6.4 — the project is re-read from the ProjectProvider at the start of every
//     mixed quantum, so a mute or gain change is reflected by the next quantum
//     (≤ 21.3 ms, or ≤ 10.7 ms above 48 fps) — well inside 200 ms — and mixing a
//     quantum never moves the playhead: the playhead is the sink's clock, which a
//     project edit does not touch.
//   * 6.6 — an asset with no audio stream contributes exactly silence and reports
//     NO error. It is recorded as a `silent` contribution, not a failure.
//   * 6.7 — if the sink cannot be opened, start() still succeeds: the engine
//     falls back to a NullAudioSink on the same injected clock (so the clock and
//     therefore video keep running at the project frame rate), suppresses audio
//     by submitting silence, and raises notice() immediately — the notice
//     timestamp is available as noticeRaisedAt() so the 2 s budget is checkable
//     without sleeping. The notice is retained until a start() opens a device.
//   * 6.9 — an audio decode failure keeps the samples already decoded for the
//     window, contributes silence for the REST of that clip's timeline range,
//     leaves the other clips untouched, lets video continue, and appends an error
//     naming the failing asset to errors().
//   * 6.8 / 6.11 — renderRange() is the export path: it mixes an arbitrary
//     timeline range into one buffer whose length matches the range within one
//     audio frame, including a range with no clip on any unmuted audio-bearing
//     track, which yields full-length silence.
//
// ## Threading
//
// The engine has single-thread affinity for the mixing calls (start / pump /
// stop / renderRange): production wiring drives pump() from the playback pump,
// tests drive it directly. presentationPosition(), outputAvailable() and notice()
// are safe to read from another thread, because PreviewController reads the clock
// from the presentation thread. The engine spawns no threads of its own; the only
// other thread involved is the teardown queue's.
//
// ## Testability
//
// Nothing here needs FFmpeg, a GPU, a sound card, a media file or a sleep:
//   * decoders are opened through MediaDecoder's DecodeBackendFactory seam;
//   * the sink is injected (a fake or NullAudioSink);
//   * the steady clock is injected, so the 100 ms dropout bound, the 40 ms sync
//     bound and the 2 s notice budget are all driven by a clock the test moves.
//
// Composition-root wiring (making this engine's sink the master clock for
// PreviewController::pump) is task 8.7 and deliberately not done here.
//
// Note on the ProjectProvider seam. design.md sketches the constructor as
// `AudioEngine(ProjectSession&, ...)`, but `Palmier::services` (where
// ProjectSession lives) links `Palmier::media`, so taking that type here would
// invert the dependency and create a cycle. The engine therefore takes a
// `ProjectProvider` — a borrow of the current project — which task 8.7 binds to
// `session.engine().project()`. The behaviour design.md asks for (mute/gain
// changes seen within 200 ms without moving the playhead) is exactly what the
// re-read-per-quantum provider gives.

#ifndef PALMIER_MEDIA_AUDIOENGINE_HPP
#define PALMIER_MEDIA_AUDIOENGINE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "media/AudioGraph.hpp"
#include "media/AudioSink.hpp"
#include "media/DecoderTeardownQueue.hpp"
#include "media/MediaDecoder.hpp"

namespace palmier::media {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/// Configuration of an AudioEngine.
struct AudioEngineOptions {
    /// Frames per mixed/submitted buffer. Use preferredQuantumFrames(fps) to pick
    /// it from the project frame rate (Requirement 6.3).
    std::size_t quantumFrames = kDefaultQuantumFrames;
    /// Assets whose audio decoders stay resident; overflow retires the
    /// least-recently-used decoder through the teardown queue.
    std::size_t decoderCacheCapacity = 4;
    /// Decode preferences handed to every MediaDecoder the engine opens. Audio
    /// decode is always software, so only the non-hardware fields matter.
    DecodePrefs prefs{};
    /// How far the decoder cursor may differ from a requested source position
    /// before the engine seeks rather than decoding straight ahead.
    Duration seekTolerance = Duration::fromMilliseconds(5);
};

// ---------------------------------------------------------------------------
// Per-quantum report
// ---------------------------------------------------------------------------

/// What one clip contributed to one mixed window. This is the engine's
/// observability surface: it is how "exactly the clips on unmuted tracks, at
/// their gains" (6.2), "an audio-less asset contributes silence with no error"
/// (6.6) and "a failed clip contributes silence for the rest of its range" (6.9)
/// are inspectable rather than merely audible.
struct AudioContribution {
    ClipId      clipId{};
    Uuid        assetId{};
    std::string assetPath{};
    double      gain = 1.0;
    /// The asset carries no audio stream: silence, no error (Requirement 6.6).
    bool silent = false;
    /// Audio decode failed for this clip: silence for the remainder of its
    /// timeline range, error reported (Requirement 6.9).
    bool failed = false;
    /// Source frames the engine actually decoded into this window.
    std::size_t decodedFrames = 0;
    /// Source sample rate / channel count this clip's asset decodes at.
    int sourceSampleRate = 0;
    int sourceChannels = 0;
};

/// One mixed window: its timeline range, the frames produced, and every clip that
/// took part.
struct AudioQuantumReport {
    Duration                       from{};
    Duration                       to{};
    std::size_t                    frames = 0;
    bool                           submitted = false;
    /// True when the window was silence because no output device is available
    /// (Requirement 6.7) rather than because the timeline is silent.
    bool                           suppressed = false;
    std::vector<AudioContribution> contributions{};
};

/// Monotonic engine counters.
struct AudioEngineStats {
    std::uint64_t quanta = 0;          ///< pump() calls that produced a window.
    std::uint64_t framesMixed = 0;     ///< Output frames mixed.
    std::uint64_t framesSubmitted = 0; ///< Output frames handed to the sink.
    std::uint64_t decodersOpened = 0;
    std::uint64_t decodersRetired = 0;
    std::uint64_t seeks = 0;
    std::uint64_t silentClips = 0;  ///< Audio-less contributions (Requirement 6.6).
    std::uint64_t failedClips = 0;  ///< Decode failures (Requirement 6.9).
};

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------

class AudioEngine {
public:
    /// The fixed output format (Requirement 6.2).
    static constexpr int kOutputSampleRate = 48'000;
    static constexpr int kOutputChannels = 2;

    [[nodiscard]] static constexpr AudioFormat outputFormat() noexcept {
        return AudioFormat{kOutputSampleRate, kOutputChannels, SampleFormat::F32};
    }

    /// Borrows the project the engine mixes. Returns nullptr when no project is
    /// current, which mixes to silence rather than failing. Re-read at the start
    /// of every quantum, which is what bounds the mute/gain latency of
    /// Requirement 6.4.
    using ProjectProvider = std::function<const Project*()>;

    /// Resolves a clip's asset reference to a media path. The default resolver
    /// uses the reference's own `sourcePath`, which is what the project document
    /// records.
    using AssetPathResolver = std::function<Result<std::filesystem::path>(const MediaAssetRef&)>;

    using Options = AudioEngineOptions;
    using Stats   = AudioEngineStats;

    /// `sink` is the preferred output (PipeWire/ALSA in production, a fake or
    /// NullAudioSink in tests); a sink that fails to start is replaced by a
    /// NullAudioSink on `clock` and reported through notice() (Requirement 6.7).
    /// `factory` is MediaDecoder's backend factory seam.
    AudioEngine(ProjectProvider project, std::unique_ptr<IAudioSink> sink,
                DecoderTeardownQueue& teardown, DecodeBackendFactory factory,
                Options options = Options{}, SteadyClock clock = systemSteadyClock(),
                AssetPathResolver resolver = {});
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&)                 = delete;
    AudioEngine& operator=(AudioEngine&&)      = delete;

    // --- Transport ----------------------------------------------------------

    /// Open the output and begin a mixing run at timeline position `from`.
    ///
    /// A sink that cannot be opened is NOT an error (Requirement 6.7): the engine
    /// installs a NullAudioSink so the master clock keeps running, suppresses
    /// audio, raises notice() and still returns ok(). Errors: FailedPrecondition
    /// when already running; InvalidArgument for a negative `from`.
    [[nodiscard]] Result<void> start(Duration from);

    /// Stop the run, close the output and retire every resident decoder through
    /// the teardown queue. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_; }

    /// The timeline position the run started at.
    [[nodiscard]] Duration startPosition() const noexcept { return startFrom_; }

    /// The next timeline position to be mixed (the mix cursor). Always ahead of
    /// or equal to presentationPosition().
    [[nodiscard]] Duration mixPosition() const noexcept { return cursor_; }

    /// **The master clock** (Requirement 6.3): the timeline position of the audio
    /// the sink has played out, i.e. startPosition() plus the sink's played
    /// frames. Monotonic within a run; unaffected by mute/gain edits.
    [[nodiscard]] Duration presentationPosition() const noexcept;

    /// Mix and deliver exactly one quantum, advancing the mix cursor. Returns the
    /// output frames delivered. Errors: FailedPrecondition when not running; a
    /// sink or mix failure. An audio decode failure is NOT an error here — it is
    /// reported through errors() and silences the rest of that clip
    /// (Requirement 6.9).
    [[nodiscard]] Result<std::size_t> pump();

    // --- Export path --------------------------------------------------------

    /// Mix `[from, to)` of `project` into a single output-format buffer — the
    /// export path (Requirements 6.8, 6.11). The buffer length equals the range
    /// length within one audio frame and every sample lies in [-1, 1]. A range
    /// with no clip on an unmuted audio-bearing track yields full-length silence.
    /// Errors: InvalidArgument when `to` precedes `from`.
    [[nodiscard]] Result<AudioBuffer> renderRange(const Project& project, Duration from,
                                                  Duration to);

    // --- Output availability and notices (Requirement 6.7) ------------------

    /// True when a real output device is open and audio is being delivered.
    [[nodiscard]] bool outputAvailable() const noexcept { return outputAvailable_; }

    /// The retained "audio output is unavailable" notice, or nullopt. Cleared only
    /// when an output device is opened successfully.
    [[nodiscard]] const std::optional<std::string>& notice() const noexcept { return notice_; }

    /// When the current notice was raised, on the injected clock. Lets the 2 s
    /// budget of Requirement 6.7 be asserted without sleeping.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> noticeRaisedAt()
        const noexcept {
        return noticeAt_;
    }

    /// Errors reported during mixing, each naming the failing asset
    /// (Requirement 6.9). Cleared by start().
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

    // --- Observability ------------------------------------------------------

    /// The sink actually in use (the injected one, or the null fallback).
    [[nodiscard]] const IAudioSink& sink() const noexcept { return *active_; }

    /// The name of the sink in use ("null", "pipewire", ...).
    [[nodiscard]] std::string_view sinkName() const noexcept { return active_->name(); }

    [[nodiscard]] std::size_t quantumFrames() const noexcept { return options_.quantumFrames; }

    /// Timeline duration of one quantum at the output sample rate.
    [[nodiscard]] Duration quantumDuration() const noexcept {
        return framesToDuration(static_cast<std::uint64_t>(options_.quantumFrames),
                                kOutputSampleRate);
    }

    /// The longest gap observed between consecutive deliveries to the sink,
    /// measured on the injected clock — the dropout metric of Requirement 6.2.
    [[nodiscard]] Duration maxDeliveryGap() const noexcept { return maxGap_; }

    [[nodiscard]] const AudioQuantumReport& lastQuantum() const noexcept { return lastQuantum_; }
    [[nodiscard]] const Stats&              stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t residentDecoderCount() const noexcept { return lru_.size(); }

private:
    /// One asset's resident audio decoder and its cursor state.
    struct AssetEntry {
        std::unique_ptr<MediaDecoder> decoder{};
        bool                          hasAudio = false; ///< false ⇒ silent (6.6).
        int                           sampleRate = 0;
        int                           channels = 0;
        bool                          endOfStream = false;
        bool                          positioned = false;
        Duration                      cursor{};      ///< Next source position expected.
        AudioBuffer                   pending{};     ///< Undelivered tail of the last block.
        Duration                      pendingStart{};
    };

    /// Mix `[from, to)` of `project` into an output buffer, filling `report`.
    /// Decode failures are recorded in `errors_` / `failedClips_`, never returned.
    [[nodiscard]] Result<AudioBuffer> mixWindow(const Project* project, Duration from, Duration to,
                                                AudioQuantumReport& report);

    /// Make `ref`'s asset resident, opening a decoder on a miss. Returns nullptr
    /// with an ok() Result never — either an entry or an error naming the asset.
    [[nodiscard]] Result<AssetEntry*> ensureAsset(const MediaAssetRef& ref);

    /// Decode `[srcFrom, srcTo)` of `entry` into `out` starting at output frame
    /// `destOffset`. Frames the source does not provide stay silent.
    [[nodiscard]] Result<std::size_t> fillFromSource(AssetEntry& entry, Duration srcFrom,
                                                     Duration srcTo, std::size_t destOffset,
                                                     AudioBuffer& out);

    void touch(const Uuid& assetId);
    void evictLeastRecentlyUsed();
    void retireAllDecoders();
    void raiseNotice(std::string message);

    ProjectProvider                                     project_;
    std::unique_ptr<IAudioSink>                         preferred_;
    std::unique_ptr<IAudioSink>                         fallback_{};
    IAudioSink*                                         active_{nullptr};
    DecoderTeardownQueue&                               teardown_;
    DecodeBackendFactory                                factory_;
    Options                                             options_;
    SteadyClock                                         clock_;
    AssetPathResolver                                   resolver_;

    bool                                                running_{false};
    bool                                                outputAvailable_{false};
    Duration                                            startFrom_{};
    Duration                                            cursor_{};
    /// Output frames mixed in this run. The window bounds are derived from this
    /// counter rather than accumulated, so a long run drifts by exactly zero.
    std::uint64_t                                       mixedFrames_{0};
    std::optional<std::string>                          notice_{};
    std::optional<std::chrono::steady_clock::time_point> noticeAt_{};
    std::vector<std::string>                            errors_{};
    std::optional<std::chrono::steady_clock::time_point> lastDeliveryAt_{};
    Duration                                            maxGap_{};
    AudioQuantumReport                                  lastQuantum_{};
    Stats                                               stats_{};

    std::unordered_map<Uuid, AssetEntry>                assets_{};
    std::list<Uuid>                                     lru_{}; ///< front = most recent.
    std::unordered_set<Uuid>                            failedClips_{};
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_AUDIOENGINE_HPP
