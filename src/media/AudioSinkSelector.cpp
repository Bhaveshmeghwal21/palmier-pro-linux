// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioSinkSelector.cpp — PipeWire → ALSA → Null (task 8.6;
// Requirements 6.2, 6.7).

#include "media/AudioSinkSelector.hpp"

#include <utility>

#include "media/AlsaAudioSink.hpp"
#include "media/PipeWireAudioSink.hpp"

namespace palmier::media {
namespace {

/// Probe one candidate: construct it, try to open it with `config`, and close it
/// again on success so the engine can open it for real. Records the outcome.
///
/// A null factory result means the backend is not in this build at all, which is
/// distinguished from "compiled in but the device would not open" — the summary
/// and the notice say different things about the two.
[[nodiscard]] std::unique_ptr<IAudioSink> probeCandidate(AudioSinkKind kind,
                                                         const AudioSinkFactory& factory,
                                                         const AudioSinkConfig& config,
                                                         AudioSinkAttempt& attempt) {
    attempt.kind = kind;
    attempt.name = std::string(toString(kind));
    attempt.compiledIn = false;
    attempt.opened = false;

    std::unique_ptr<IAudioSink> sink = factory ? factory() : nullptr;
    if (!sink) {
        attempt.reason = attempt.name + " audio output is not compiled into this build";
        return nullptr;
    }
    attempt.compiledIn = true;
    attempt.name = std::string(sink->name());

    Result<void> opened = sink->start(config);
    if (opened.isError()) {
        attempt.reason = opened.error().message();
        return nullptr;
    }

    // It opens. Give the device back so AudioEngine::start() owns the open.
    sink->stop();
    attempt.opened = true;
    attempt.reason.clear();
    return sink;
}

} // namespace

std::string_view toString(AudioSinkKind kind) noexcept {
    switch (kind) {
        case AudioSinkKind::PipeWire: return "pipewire";
        case AudioSinkKind::Alsa: return "alsa";
        case AudioSinkKind::Null: break;
    }
    return "null";
}

bool pipewireSinkCompiledIn() noexcept {
    return PipeWireAudioSink::compiledIn();
}

bool alsaSinkCompiledIn() noexcept {
    return AlsaAudioSink::compiledIn();
}

AudioSinkConfig audioSinkConfigFor(double projectFrameRateFps) noexcept {
    AudioSinkConfig config;
    config.format = audioOutputFormat();
    config.quantumFrames = preferredQuantumFrames(projectFrameRateFps);
    return config;
}

AudioSinkSelection selectAudioSink(AudioSinkSelectorOptions options) {
    AudioSinkSelection selection;
    selection.config = audioSinkConfigFor(options.projectFrameRateFps);

    // Slot defaults: the real backend, which is itself a no-op when its guard is
    // absent (start() reports Unsupported and the attempt is recorded).
    AudioSinkFactory pipewire = options.pipewireFactory;
    if (!pipewire && PipeWireAudioSink::compiledIn()) {
        pipewire = [] { return std::unique_ptr<IAudioSink>(new PipeWireAudioSink()); };
    }
    AudioSinkFactory alsa = options.alsaFactory;
    if (!alsa && AlsaAudioSink::compiledIn()) {
        alsa = [] { return std::unique_ptr<IAudioSink>(new AlsaAudioSink()); };
    }

    // 1. PipeWire.
    AudioSinkAttempt pipewireAttempt;
    if (std::unique_ptr<IAudioSink> sink = probeCandidate(AudioSinkKind::PipeWire, pipewire,
                                                         selection.config, pipewireAttempt)) {
        selection.attempts.push_back(std::move(pipewireAttempt));
        selection.sink = std::move(sink);
        selection.kind = AudioSinkKind::PipeWire;
        selection.name = std::string(selection.sink->name());
        selection.realDevice = true;
        return selection;
    }
    selection.attempts.push_back(std::move(pipewireAttempt));

    // 2. ALSA.
    AudioSinkAttempt alsaAttempt;
    if (std::unique_ptr<IAudioSink> sink =
            probeCandidate(AudioSinkKind::Alsa, alsa, selection.config, alsaAttempt)) {
        selection.attempts.push_back(std::move(alsaAttempt));
        selection.sink = std::move(sink);
        selection.kind = AudioSinkKind::Alsa;
        selection.name = std::string(selection.sink->name());
        selection.realDevice = true;
        return selection;
    }
    selection.attempts.push_back(std::move(alsaAttempt));

    // 3. NullAudioSink — always available, so this branch always terminates
    //    selection (Requirement 6.7: audio output unavailable is a normal path).
    selection.sink = std::make_unique<NullAudioSink>(options.clock ? options.clock
                                                                  : systemSteadyClock());
    selection.kind = AudioSinkKind::Null;
    selection.name = std::string(selection.sink->name());
    selection.realDevice = false;

    std::string notice = "Audio output is unavailable; playback continues without sound.";
    for (const AudioSinkAttempt& attempt : selection.attempts) {
        notice += " " + attempt.name + ": " + attempt.reason;
    }
    selection.notice = notice;
    return selection;
}

} // namespace palmier::media
