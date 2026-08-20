// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AudioSinkSelector.hpp — startup selection of the audio output
// (task 8.6; Requirements 6.2, 6.7).
//
// design.md D7 fixes the order: **PipeWire → ALSA → Null**. This is the one
// place that order lives, and it is deliberately a free function over injectable
// candidate factories rather than logic inside `AudioEngine` or the composition
// root, so it can be asserted exhaustively on a host with no sound card at all —
// which is every CI runner this project has.
//
// ## What "selection" means
//
// A sink is selected only if it actually OPENS. Compiled-in is not enough: a
// build with PipeWire compiled in, running in a container with no daemon, must
// fall through to ALSA and then to `NullAudioSink` — if selection stopped at
// "compiled in", ALSA would never be tried on such a host. So each candidate in
// order is constructed, `start()`ed with the resolved configuration, and, when
// that succeeds, immediately `stop()`ped again and handed back to the caller
// unstarted. `AudioEngine::start()` opens it for real at transport start; the
// probe exists so the ORDER is decided by what works, not by what was compiled.
//
// Every attempt is recorded in `AudioSinkSelection::attempts` with its reason, so
// the configuration summary, a log line and the tests all read the same
// explanation of why a given sink is in use.
//
// ## The quantum (Requirement 6.3)
//
// The resolved configuration carries `preferredQuantumFrames(projectFrameRateFps)`
// from task 8.3: at most 512 frames when the project frame rate exceeds 48 fps,
// otherwise 1024. That is what keeps one video frame interval plus one sink
// quantum inside the 40 ms A/V bound at high frame rates.
//
// ## Fallback is a normal outcome
//
// `selectAudioSink()` cannot fail. Its worst case is a `NullAudioSink` plus a
// populated `notice` naming why no device could be opened — the Requirement 6.7
// path, which is also what the shell's audio-unavailable status-bar notice shows.

#ifndef PALMIER_MEDIA_AUDIOSINKSELECTOR_HPP
#define PALMIER_MEDIA_AUDIOSINKSELECTOR_HPP

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "media/AudioSink.hpp"

namespace palmier::media {

/// The three sinks of design.md D7, in selection order.
enum class AudioSinkKind {
    PipeWire,
    Alsa,
    Null,
};

[[nodiscard]] std::string_view toString(AudioSinkKind kind) noexcept;

/// One candidate considered during selection.
struct AudioSinkAttempt {
    AudioSinkKind kind{AudioSinkKind::Null};
    /// The sink's own `name()` ("pipewire", "alsa", "null").
    std::string name{};
    /// Whether the backend was compiled into this build at all.
    bool compiledIn = false;
    /// Whether the device actually opened.
    bool opened = false;
    /// Why it was skipped or why it failed; empty when it opened.
    std::string reason{};
};

/// The outcome of startup selection. `sink` is never null.
struct AudioSinkSelection {
    std::unique_ptr<IAudioSink>   sink{};
    AudioSinkKind                 kind{AudioSinkKind::Null};
    std::string                   name{};
    /// True when a real output device was opened, false for the null fallback.
    bool                          realDevice = false;
    /// The configuration the sink was probed with and should be started with —
    /// notably the quantum resolved from the project frame rate.
    AudioSinkConfig               config{};
    std::vector<AudioSinkAttempt> attempts{};
    /// Populated only when selection fell through to `NullAudioSink`: the
    /// user-facing Requirement 6.7 notice, naming every candidate's reason.
    std::optional<std::string>    notice{};
};

/// Builds one candidate sink. Returning nullptr means "this backend is not
/// available in this build", which is recorded as `compiledIn = false`.
using AudioSinkFactory = std::function<std::unique_ptr<IAudioSink>()>;

struct AudioSinkSelectorOptions {
    /// The project's timeline frame rate, which fixes the quantum
    /// (Requirement 6.3).
    double projectFrameRateFps = 30.0;
    /// Clock for the `NullAudioSink` fallback. Empty = the real steady clock.
    SteadyClock clock{};
    /// Candidate factories. Empty means "use the real backend for this slot",
    /// which is itself a no-op when that backend was not compiled in. Tests
    /// inject fakes here to assert the order on any host.
    AudioSinkFactory pipewireFactory{};
    AudioSinkFactory alsaFactory{};
};

/// True in a build where the corresponding backend's calls were compiled in.
[[nodiscard]] bool pipewireSinkCompiledIn() noexcept;
[[nodiscard]] bool alsaSinkCompiledIn() noexcept;

/// Resolve the sink configuration for a project frame rate: the engine's fixed
/// output format plus `preferredQuantumFrames(fps)`.
[[nodiscard]] AudioSinkConfig audioSinkConfigFor(double projectFrameRateFps) noexcept;

/// Choose the output sink in the order PipeWire → ALSA → Null. Never fails.
[[nodiscard]] AudioSinkSelection selectAudioSink(AudioSinkSelectorOptions options = {});

} // namespace palmier::media

#endif // PALMIER_MEDIA_AUDIOSINKSELECTOR_HPP
