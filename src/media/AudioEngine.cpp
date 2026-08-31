// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioEngine.cpp — decode → AudioGraph mix → sink (task 8.4;
// Requirements 6.2, 6.3, 6.4, 6.6, 6.7, 6.8, 6.9, 6.11).
//
// All resampling, channel mapping, gain and clamping is delegated to
// media::AudioGraph. This file's job is purely the timeline work: which clips
// contribute to a window, where their source samples live, how a decoder cursor
// is kept, and what a failure means.

#include "media/AudioEngine.hpp"

#include <algorithm>
#include <utility>

#include "core/Error.hpp"
#include "core/Track.hpp"

namespace palmier::media {
namespace {

/// Human-readable label for an asset in an error message: the recorded source
/// path when there is one, otherwise the asset id (Requirements 6.9).
[[nodiscard]] std::string assetLabel(const MediaAssetRef& ref) {
    if (!ref.sourcePath.empty()) return ref.sourcePath;
    return ref.assetId.toString();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(ProjectProvider project, std::unique_ptr<IAudioSink> sink,
                         DecoderTeardownQueue& teardown, DecodeBackendFactory factory,
                         Options options, SteadyClock clock, AssetPathResolver resolver)
    : project_(std::move(project)),
      preferred_(std::move(sink)),
      teardown_(teardown),
      factory_(std::move(factory)),
      options_(options),
      clock_(std::move(clock)),
      resolver_(std::move(resolver)) {
    if (!clock_) clock_ = systemSteadyClock();
    if (options_.quantumFrames == 0) options_.quantumFrames = kDefaultQuantumFrames;
    if (options_.decoderCacheCapacity == 0) options_.decoderCacheCapacity = 1;
    if (!preferred_) {
        // No sink supplied at all is the same situation as a sink that cannot be
        // opened (Requirement 6.7): the null fallback takes over at start().
        preferred_ = nullptr;
    }
    // Until start() decides, reads of the clock must be well defined, so the null
    // fallback is the active sink from construction.
    fallback_ = std::make_unique<NullAudioSink>(clock_);
    active_ = fallback_.get();
}

AudioEngine::~AudioEngine() {
    if (running_) {
        active_->stop();
        running_ = false;
    }
    retireAllDecoders();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

Result<void> AudioEngine::start(Duration from) {
    if (running_) {
        return makeError(ErrorCode::FailedPrecondition, "the audio engine is already running");
    }
    if (from.isNegative()) {
        return makeError(ErrorCode::InvalidArgument,
                         "playback cannot start before timeline position zero");
    }

    errors_.clear();
    failedClips_.clear();
    lastDeliveryAt_.reset();
    maxGap_ = Duration::zero();
    lastQuantum_ = AudioQuantumReport{};
    mixedFrames_ = 0;
    startFrom_ = from;
    cursor_ = from;

    // Every resident decoder must re-seek for the new run.
    for (auto& [id, entry] : assets_) {
        entry.positioned = false;
        entry.endOfStream = false;
        entry.pending = AudioBuffer{};
        entry.pendingStart = Duration::zero();
    }

    const AudioSinkConfig config{outputFormat(), options_.quantumFrames};

    Result<void> started = preferred_
                               ? preferred_->start(config)
                               : Result<void>(makeError(ErrorCode::FailedPrecondition,
                                                        "no audio output sink is configured"));
    if (started.isOk()) {
        active_ = preferred_.get();
        outputAvailable_ = true;
        notice_.reset();
        noticeAt_.reset();
    } else {
        // Requirement 6.7: an unopenable device is a normal path. Audio is
        // suppressed, the null sink keeps the master clock advancing from the
        // steady clock so video keeps presenting at the project frame rate, and a
        // notice is raised immediately (well inside the 2 s budget).
        outputAvailable_ = false;
        fallback_ = std::make_unique<NullAudioSink>(clock_);
        if (auto nullStarted = fallback_->start(config); nullStarted.isError()) {
            return makeError(ErrorCode::Internal,
                             "the null audio sink refused to start: " +
                                 nullStarted.error().message());
        }
        active_ = fallback_.get();
        raiseNotice("audio output is unavailable: " + started.error().message());
    }

    running_ = true;
    return ok();
}

void AudioEngine::stop() {
    if (!running_) return;
    active_->stop();
    running_ = false;
    retireAllDecoders();
}

Duration AudioEngine::presentationPosition() const noexcept {
    return startFrom_ + framesToDuration(active_->playedFrames(), kOutputSampleRate);
}

Result<std::size_t> AudioEngine::pump() {
    if (!running_) {
        return err<std::size_t>(
            makeError(ErrorCode::FailedPrecondition, "the audio engine is not running"));
    }

    // Exact window bounds derived from the frame counter, so a long run
    // accumulates no rounding drift in the mix cursor.
    const Duration from = startFrom_ + framesToDuration(mixedFrames_, kOutputSampleRate);
    const Duration to =
        startFrom_ + framesToDuration(mixedFrames_ + static_cast<std::uint64_t>(
                                                         options_.quantumFrames),
                                      kOutputSampleRate);

    AudioBuffer        buffer;
    AudioQuantumReport report{};
    report.from = from;
    report.to = to;

    if (!outputAvailable_) {
        // Audio suppressed (Requirement 6.7): mix nothing, but submit silence so
        // the sink's clock — and therefore video pacing — keeps advancing.
        buffer = AudioBuffer(kOutputSampleRate, kOutputChannels, options_.quantumFrames);
        report.frames = buffer.frameCount();
        report.suppressed = true;
    } else {
        const Project* project = project_ ? project_() : nullptr;
        auto           mixed = mixWindow(project, from, to, report);
        if (mixed.isError()) return err<std::size_t>(std::move(mixed).error());
        buffer = std::move(mixed).value();
        report.frames = buffer.frameCount();
    }

    // Requirement 1.2: measured from the exact buffer this quantum submits, so
    // the reported levels cannot diverge from what was heard. Read-only — the
    // samples and the frame count handed to the sink below are untouched
    // (Requirement 1.8).
    report.levels = measureLevels(buffer);

    if (auto submitted = active_->submit(buffer); submitted.isError()) {
        return err<std::size_t>(std::move(submitted).error());
    }
    // Dropout accounting on the injected clock (Requirement 6.2).
    const auto now = clock_();
    if (lastDeliveryAt_.has_value()) {
        const auto gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *lastDeliveryAt_)
                             .count();
        const Duration gapDuration = Duration::fromNanoseconds(gap);
        if (gapDuration > maxGap_) maxGap_ = gapDuration;
    }
    lastDeliveryAt_ = now;

    report.submitted = true;
    mixedFrames_ += static_cast<std::uint64_t>(buffer.frameCount());
    cursor_ = startFrom_ + framesToDuration(mixedFrames_, kOutputSampleRate);

    ++stats_.quanta;
    stats_.framesMixed += static_cast<std::uint64_t>(buffer.frameCount());
    stats_.framesSubmitted += static_cast<std::uint64_t>(buffer.frameCount());
    lastQuantum_ = std::move(report);
    return buffer.frameCount();
}

// ---------------------------------------------------------------------------
// Export path
// ---------------------------------------------------------------------------

Result<AudioBuffer> AudioEngine::renderRange(const Project& project, Duration from, Duration to) {
    if (to < from) {
        return err<AudioBuffer>(makeError(
            ErrorCode::InvalidArgument,
            "the export range end must not precede its start"));
    }
    AudioQuantumReport report{};
    auto               mixed = mixWindow(&project, from, to, report);
    if (mixed.isError()) return mixed;
    // The export path publishes lastQuantum() too, so it reports levels on the
    // same terms the playback path does rather than leaving them at zero and
    // making a reader guess whether that meant silence.
    report.levels = measureLevels(mixed.value());
    lastQuantum_ = std::move(report);
    return mixed;
}

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------

Result<AudioBuffer> AudioEngine::mixWindow(const Project* project, Duration from, Duration to,
                                           AudioQuantumReport& report) {
    report.from = from;
    report.to = to;
    report.contributions.clear();

    const Duration    span = to - from;
    const std::size_t outFrames =
        static_cast<std::size_t>(durationToFrames(span, kOutputSampleRate));
    report.frames = outFrames;

    AudioGraph               graph{outputFormat()};
    std::vector<AudioBuffer> inputs;

    if (project != nullptr && outFrames > 0) {
        for (const Track& track : project->tracks) {
            // "every clip on an unmuted audio-bearing track" (Requirement 6.2).
            if (track.kind != TrackKind::Audio || track.muted) continue;

            for (const Clip& clip : track.clips) {
                if (clip.timelineEnd() <= from || clip.timelineStart >= to) continue;

                AudioContribution contribution;
                contribution.clipId = clip.id;
                contribution.assetId = clip.assetRef.assetId;
                contribution.assetPath = clip.assetRef.sourcePath;
                contribution.gain = clip.gain >= 0.0 ? clip.gain : 0.0;

                // Requirement 6.9: once a clip's audio decode has failed, the rest
                // of its timeline range is silence.
                if (failedClips_.find(clip.id) != failedClips_.end()) {
                    contribution.failed = true;
                    report.contributions.push_back(std::move(contribution));
                    continue;
                }

                auto entryResult = ensureAsset(clip.assetRef);
                if (entryResult.isError()) {
                    failedClips_.insert(clip.id);
                    ++stats_.failedClips;
                    errors_.push_back("audio decode failed for asset '" +
                                      assetLabel(clip.assetRef) + "': " +
                                      entryResult.error().message());
                    contribution.failed = true;
                    report.contributions.push_back(std::move(contribution));
                    continue;
                }

                AssetEntry& entry = *entryResult.value();

                // Requirement 6.6: an asset with no audio stream contributes
                // exactly silence and reports NO error.
                if (!entry.hasAudio) {
                    contribution.silent = true;
                    ++stats_.silentClips;
                    report.contributions.push_back(std::move(contribution));
                    continue;
                }

                contribution.sourceSampleRate = entry.sampleRate;
                contribution.sourceChannels = entry.channels;

                const Duration overlapFrom = std::max(from, clip.timelineStart);
                const Duration overlapTo = std::min(to, clip.timelineEnd());

                // One input buffer per contributing clip, spanning the WHOLE
                // window at the source's own rate: leading and trailing silence
                // place the clip's samples where they belong inside the window, and
                // AudioGraph then resamples the whole window to the output rate.
                const std::size_t inFrames =
                    static_cast<std::size_t>(durationToFrames(span, entry.sampleRate));
                AudioBuffer input(entry.sampleRate, entry.channels, inFrames);
                const std::size_t destOffset = static_cast<std::size_t>(
                    durationToFrames(overlapFrom - from, entry.sampleRate));

                const Duration srcFrom = clip.sourceIn + (overlapFrom - clip.timelineStart);
                const Duration srcTo = srcFrom + (overlapTo - overlapFrom);

                auto filled = fillFromSource(entry, srcFrom, srcTo, destOffset, input);
                if (filled.isError()) {
                    // Requirement 6.9: keep the samples decoded before the failure
                    // (the buffer's tail is already silent), silence the rest of the
                    // clip from now on, and report an error naming the asset. The
                    // other clips are untouched and mixing continues.
                    failedClips_.insert(clip.id);
                    ++stats_.failedClips;
                    errors_.push_back("audio decode failed for asset '" +
                                      assetLabel(clip.assetRef) + "': " +
                                      filled.error().message());
                    contribution.failed = true;
                } else {
                    contribution.decodedFrames = filled.value();
                }

                auto source = graph.addSource(entry.sampleRate, entry.channels, contribution.gain);
                if (source.isError()) {
                    return err<AudioBuffer>(std::move(source).error());
                }
                inputs.push_back(std::move(input));
                report.contributions.push_back(std::move(contribution));
            }
        }
    }

    // AudioGraph does the resampling, gain, summing and [-1, 1] clamping
    // (Requirements 6.2, 6.8); a window with no sources yields exactly
    // `outFrames` frames of silence (Requirements 6.6, 6.11).
    return graph.render(inputs, outFrames);
}

Result<std::size_t> AudioEngine::fillFromSource(AssetEntry& entry, Duration srcFrom, Duration srcTo,
                                                std::size_t destOffset, AudioBuffer& out) {
    const int rate = entry.sampleRate;
    const int channels = entry.channels;
    if (rate <= 0 || channels <= 0 || out.frameCount() == 0) return std::size_t{0};

    const std::uint64_t base = durationToFrames(srcFrom, rate);
    const std::uint64_t end = durationToFrames(srcTo, rate);
    const std::uint64_t need = end > base ? end - base : 0;
    if (need == 0) return std::size_t{0};

    // Decode straight ahead when the request continues where the decoder left off;
    // seek otherwise (a jump, a new run, or the first window).
    const Duration delta = srcFrom - entry.cursor;
    if (!entry.positioned || delta.abs() > options_.seekTolerance) {
        if (auto sought = entry.decoder->seekAudio(srcFrom); sought.isError()) {
            return err<std::size_t>(std::move(sought).error());
        }
        ++stats_.seeks;
        entry.pending = AudioBuffer{};
        entry.pendingStart = Duration::zero();
        entry.endOfStream = false;
        entry.cursor = srcFrom;
        entry.positioned = true;
    }

    const std::size_t outFrames = out.frameCount();
    const int         outChannels = out.channels();
    std::vector<float>& samples = out.samples();

    std::size_t   copied = 0;
    std::uint64_t filledUpTo = base;
    // Bound the loop so a pathological backend (empty non-EOS blocks) cannot spin.
    std::size_t guard = static_cast<std::size_t>(need) + 1024;

    while (filledUpTo < base + need && guard-- > 0) {
        if (entry.pending.frameCount() == 0) {
            if (entry.endOfStream) break;
            auto frame = entry.decoder->nextAudioFrame();
            if (frame.isError()) {
                return err<std::size_t>(std::move(frame).error());
            }
            AudioFrame decoded = std::move(frame).value();
            if (decoded.endOfStream) {
                entry.endOfStream = true;
                break;
            }
            entry.pendingStart = decoded.presentation;
            entry.pending = std::move(decoded.buffer);
            if (entry.pending.frameCount() == 0) continue;
        }

        const std::uint64_t blockStart = durationToFrames(entry.pendingStart, rate);
        const std::uint64_t blockFrames = static_cast<std::uint64_t>(entry.pending.frameCount());

        if (blockStart + blockFrames <= filledUpTo) {
            // Entirely behind the window: stale, drop it and pull the next block.
            entry.pending = AudioBuffer{};
            continue;
        }
        if (blockStart >= base + need) {
            // Entirely ahead: keep it for a later window; the rest stays silent.
            break;
        }

        const std::uint64_t copyFrom = std::max(blockStart, filledUpTo);
        const std::uint64_t copyTo = std::min(blockStart + blockFrames, base + need);
        for (std::uint64_t i = copyFrom; i < copyTo; ++i) {
            const std::size_t dst = destOffset + static_cast<std::size_t>(i - base);
            if (dst >= outFrames) break;
            const std::size_t src = static_cast<std::size_t>(i - blockStart);
            const int         copyChannels = std::min(channels, outChannels);
            for (int ch = 0; ch < copyChannels; ++ch) {
                samples[dst * static_cast<std::size_t>(outChannels) +
                        static_cast<std::size_t>(ch)] = entry.pending.at(src, ch);
            }
            ++copied;
        }

        if (blockStart + blockFrames <= base + need) {
            filledUpTo = blockStart + blockFrames;
            entry.pending = AudioBuffer{};
        } else {
            // The block extends past the window: keep the whole block pending, the
            // next window's blockStart arithmetic will resume inside it.
            filledUpTo = base + need;
        }
    }

    entry.cursor = srcTo;
    entry.positioned = true;
    return copied;
}

// ---------------------------------------------------------------------------
// Decoder residency
// ---------------------------------------------------------------------------

Result<AudioEngine::AssetEntry*> AudioEngine::ensureAsset(const MediaAssetRef& ref) {
    if (!ref.isValid()) {
        return err<AssetEntry*>(
            makeError(ErrorCode::InvalidArgument, "the clip names no media asset"));
    }

    if (auto it = assets_.find(ref.assetId); it != assets_.end()) {
        touch(ref.assetId);
        return &it->second;
    }

    std::filesystem::path path;
    if (resolver_) {
        auto resolved = resolver_(ref);
        if (resolved.isError()) return err<AssetEntry*>(std::move(resolved).error());
        path = std::move(resolved).value();
    } else {
        path = ref.sourcePath;
    }
    if (path.empty()) {
        return err<AssetEntry*>(makeError(ErrorCode::NotFound,
                                          "asset '" + assetLabel(ref) +
                                              "' resolves to no media path"));
    }

    auto opened = MediaDecoder::open(path, options_.prefs, factory_);
    if (opened.isError()) return err<AssetEntry*>(std::move(opened).error());

    AssetEntry entry;
    entry.decoder = std::make_unique<MediaDecoder>(std::move(opened).value());
    ++stats_.decodersOpened;

    if (entry.decoder->hasAudio()) {
        if (auto selected = entry.decoder->openAudioStream(); selected.isError()) {
            teardown_.retire(std::move(entry.decoder));
            ++stats_.decodersRetired;
            return err<AssetEntry*>(std::move(selected).error());
        }
        entry.hasAudio = true;

        // Prefer the container's declared parameters; a source that declares none
        // reveals them in its first block, so decode one and keep it pending.
        for (const auto& stream : entry.decoder->info().streams) {
            if (stream.isAudio() && stream.index == entry.decoder->audioStreamIndex()) {
                entry.sampleRate = stream.sampleRate;
                entry.channels = stream.channels;
                break;
            }
        }
        if (entry.sampleRate <= 0 || entry.channels <= 0) {
            auto frame = entry.decoder->nextAudioFrame();
            if (frame.isError()) {
                teardown_.retire(std::move(entry.decoder));
                ++stats_.decodersRetired;
                return err<AssetEntry*>(std::move(frame).error());
            }
            AudioFrame decoded = std::move(frame).value();
            if (decoded.endOfStream) {
                // A stream that yields nothing at all is silence, not a failure
                // (Requirement 6.6).
                entry.hasAudio = false;
                entry.endOfStream = true;
            } else {
                entry.sampleRate = decoded.sampleRate();
                entry.channels = decoded.channels();
                entry.pendingStart = decoded.presentation;
                entry.pending = std::move(decoded.buffer);
            }
        }
    }

    auto [it, inserted] = assets_.emplace(ref.assetId, std::move(entry));
    lru_.push_front(ref.assetId);
    while (lru_.size() > options_.decoderCacheCapacity) {
        evictLeastRecentlyUsed();
    }
    // Eviction never removes the entry just inserted (it is at the LRU front), so
    // the pointer stays valid.
    it = assets_.find(ref.assetId);
    if (it == assets_.end()) {
        return err<AssetEntry*>(
            makeError(ErrorCode::Internal, "the audio decoder cache dropped a live asset"));
    }
    return &it->second;
}

void AudioEngine::touch(const Uuid& assetId) {
    auto it = std::find(lru_.begin(), lru_.end(), assetId);
    if (it == lru_.end()) {
        lru_.push_front(assetId);
        return;
    }
    lru_.splice(lru_.begin(), lru_, it);
}

void AudioEngine::evictLeastRecentlyUsed() {
    if (lru_.empty()) return;
    const Uuid victim = lru_.back();
    lru_.pop_back();
    auto it = assets_.find(victim);
    if (it == assets_.end()) return;
    if (it->second.decoder) {
        // Requirement 14.8 / upstream PR 405: closing a decode context must never
        // block the thread that is mixing audio.
        teardown_.retire(std::move(it->second.decoder));
        ++stats_.decodersRetired;
    }
    assets_.erase(it);
}

void AudioEngine::retireAllDecoders() {
    for (auto& [id, entry] : assets_) {
        if (entry.decoder) {
            teardown_.retire(std::move(entry.decoder));
            ++stats_.decodersRetired;
        }
    }
    assets_.clear();
    lru_.clear();
}

void AudioEngine::raiseNotice(std::string message) {
    notice_ = std::move(message);
    noticeAt_ = clock_();
}

} // namespace palmier::media
