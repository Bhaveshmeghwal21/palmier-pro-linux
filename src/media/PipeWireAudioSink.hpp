// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PipeWireAudioSink.hpp — the primary real audio output (task 8.6;
// Requirements 6.2, 6.7).
//
// design.md D7 "Decision — output sink" names PipeWire first of three sinks:
// it is the default audio server on every currently supported desktop
// distribution family, it exposes a callback-driven float stream that matches
// the engine's internal 48 kHz / 2-channel / F32 format with no conversion, and
// it reports a played-out sample position, which the A/V sync check of
// Requirement 6.3 needs.
//
// Licence note: only the MIT-licensed core client library `libpipewire-0.3` is
// linked — not the separately-licensed JACK/ALSA/BlueZ compatibility components
// — and MIT is compatible with GPLv3.
//
// ## Optional at BOTH levels
//
//   * **Compile time.** The libpipewire calls are compiled only when
//     `PALMIER_HAVE_PIPEWIRE` is defined, which cmake/PalmierDependencies.cmake
//     defines only when `PALMIER_ENABLE_PIPEWIRE` is ON *and* the package was
//     actually found. With the guard absent this class still exists and still
//     compiles — `start()` simply reports `Unsupported`, so the selection order
//     of `selectAudioSink()` needs no `#ifdef` of its own. `compiledIn()` reports
//     which of the two builds you are in.
//   * **Run time.** No PipeWire daemon (a container, a CI runner, a headless
//     server) makes `start()` fail with a named error. That is NOT a fault
//     anywhere: `selectAudioSink()` moves on to ALSA and then to
//     `NullAudioSink`, and `AudioEngine::start()` independently degrades the same
//     way (Requirement 6.7).
//
// ## The clock (Requirement 6.3)
//
// `playedFrames()` counts frames the graph has actually consumed from this
// sink's queue in the `process` callback. It is therefore monotonic, never
// exceeds `submittedFrames()` (silence written into a buffer during an underrun
// is counted as `underrunFrames()`, never as played), and stalls if the engine
// stops mixing — which is exactly the honest behaviour the master clock needs.
// It lags true DAC output by at most the graph's own quantum, well inside the
// 40 ms bound.
//
// ## Threading
//
// `start()` / `stop()` are called from the thread that owns the engine.
// `submit()` is called from the mixing thread. The `process` callback runs on
// libpipewire's own thread-loop thread and takes the same small mutex `submit()`
// does; the stream is deliberately connected WITHOUT `PW_STREAM_FLAG_RT_PROCESS`
// so taking a mutex there is legitimate. Every counter is atomic, so
// `playedFrames()` is safe to read from the presentation thread.
//
// No PipeWire type appears in this header: the whole backend lives behind an
// opaque `Impl`, so translation units that merely construct a sink (the selector,
// the composition root, the tests) need none of libpipewire's include paths.

#ifndef PALMIER_MEDIA_PIPEWIREAUDIOSINK_HPP
#define PALMIER_MEDIA_PIPEWIREAUDIOSINK_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/Result.hpp"
#include "media/AudioGraph.hpp"
#include "media/AudioSink.hpp"

namespace palmier::media {

/// Configuration of a PipeWireAudioSink. At namespace scope (with the customary
/// `PipeWireAudioSink::Options` alias) because a default argument of a nested
/// class type is not usable while the enclosing class is still incomplete.
struct PipeWireSinkOptions {
    /// Reported to the audio server as the application name.
    std::string applicationName = "Palmier Pro";
    /// How long `start()` waits for the stream to reach the streaming state
    /// before declaring the device unavailable. A host with a running daemon
    /// reaches it in single-digit milliseconds and a host without one normally
    /// fails sooner still, with an error from the connect itself, so this is a
    /// backstop for a wedged server rather than an expected wait.
    std::chrono::milliseconds connectTimeout{500};
    /// Queue depth in quanta. `submit()` never blocks; a submission that would
    /// exceed this depth is counted in `overflowFrames()` instead.
    std::size_t queueCapacityQuanta = 4;
};

class PipeWireAudioSink final : public IAudioSink {
public:
    using Options = PipeWireSinkOptions;

    explicit PipeWireAudioSink(Options options = Options{});
    ~PipeWireAudioSink() override;

    PipeWireAudioSink(const PipeWireAudioSink&)            = delete;
    PipeWireAudioSink& operator=(const PipeWireAudioSink&) = delete;

    /// True in a build where the libpipewire calls were compiled in. When false
    /// every `start()` reports `Unsupported` naming the absent guard.
    [[nodiscard]] static constexpr bool compiledIn() noexcept {
#ifdef PALMIER_HAVE_PIPEWIRE
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] std::string_view name() const noexcept override { return "pipewire"; }

    [[nodiscard]] Result<void>   start(const AudioSinkConfig& config) override;
    void                         stop() noexcept override;
    [[nodiscard]] bool           running() const noexcept override;
    [[nodiscard]] AudioFormat    format() const noexcept override;
    [[nodiscard]] std::size_t    quantumFrames() const noexcept override;
    [[nodiscard]] Result<void>   submit(const AudioBuffer& buffer) override;
    [[nodiscard]] std::uint64_t  playedFrames() const noexcept override;
    [[nodiscard]] std::uint64_t  submittedFrames() const noexcept override;

    /// Frames of silence the graph consumed because the queue was empty — the
    /// dropout metric of Requirement 6.2 as seen from the device side.
    [[nodiscard]] std::uint64_t underrunFrames() const noexcept;

    /// Frames a submission could not enqueue because the queue was full. Non-zero
    /// means the caller is mixing ahead of the device faster than the queue depth
    /// allows; it never blocks and never fails.
    [[nodiscard]] std::uint64_t overflowFrames() const noexcept;

    /// Opaque backend state. Declared public only so libpipewire's C callbacks —
    /// which are free functions — can name the type they are handed; it is
    /// defined in the .cpp and no caller can do anything with it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_PIPEWIREAUDIOSINK_HPP
