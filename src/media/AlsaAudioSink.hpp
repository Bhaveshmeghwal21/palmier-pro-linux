// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AlsaAudioSink.hpp — the fallback real audio output (task 8.6;
// Requirements 6.2, 6.7).
//
// Second of the three sinks design.md D7 names, tried when PipeWire is either not
// compiled in or could not open a device: hosts without a sound server, and
// headless/CI images where only the ALSA device nodes exist.
//
// Licence note: `libasound2` is LGPL-2.1-or-later; the "or later" clause permits
// use under LGPL-3.0, which is compatible with GPLv3. The `default` PCM device is
// opened — never a raw `hw:` device — so the user's own sound server keeps
// arbitrating access.
//
// Optional at both levels, exactly as the PipeWire sink is:
//
//   * **Compile time.** `PALMIER_HAVE_ALSA` is defined only when
//     `PALMIER_ENABLE_ALSA` is ON *and* `libasound2` was found. Without it this
//     class still compiles and `start()` reports `Unsupported`.
//   * **Run time.** No sound card (this project's CI, and every container)
//     makes `snd_pcm_open` fail; `selectAudioSink()` then falls through to
//     `NullAudioSink` and playback continues with audio suppressed
//     (Requirement 6.7).
//
// The clock (Requirement 6.3): `playedFrames()` is `written - snd_pcm_delay()`,
// i.e. frames handed to the device minus the frames still queued in it — the
// device's own idea of how much audio has been played out. It is latched
// monotonic and clamped to `submittedFrames()`.
//
// `submit()` NEVER blocks the mixing thread: the device is opened non-blocking,
// a short bounded wait absorbs normal back-pressure, and anything that still does
// not fit is counted in `overflowFrames()` rather than waited on. An underrun
// (`-EPIPE`) is recovered by re-preparing the device once and retrying, which is
// what keeps a stall from turning into a permanent silence.
//
// No ALSA type appears in this header — the backend lives behind an opaque
// `Impl`, so callers need none of libasound's include paths.

#ifndef PALMIER_MEDIA_ALSAAUDIOSINK_HPP
#define PALMIER_MEDIA_ALSAAUDIOSINK_HPP

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

/// Configuration of an AlsaAudioSink. At namespace scope (with the customary
/// `AlsaAudioSink::Options` alias) because a default argument of a nested class
/// type is not usable while the enclosing class is still incomplete.
struct AlsaSinkOptions {
    /// PCM device name. `default` routes through whatever the host has configured
    /// (dmix, a PulseAudio/PipeWire shim, or the raw card).
    std::string deviceName = "default";
    /// Device buffer depth in quanta; also the requested latency.
    std::size_t bufferQuanta = 4;
    /// Upper bound on how long one `submit()` may wait for room in the device
    /// buffer before recording an overflow instead.
    std::chrono::milliseconds writeBudget{20};
};

class AlsaAudioSink final : public IAudioSink {
public:
    using Options = AlsaSinkOptions;

    explicit AlsaAudioSink(Options options = Options{});
    ~AlsaAudioSink() override;

    AlsaAudioSink(const AlsaAudioSink&)            = delete;
    AlsaAudioSink& operator=(const AlsaAudioSink&) = delete;

    /// True in a build where the libasound calls were compiled in.
    [[nodiscard]] static constexpr bool compiledIn() noexcept {
#ifdef PALMIER_HAVE_ALSA
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] std::string_view name() const noexcept override { return "alsa"; }

    [[nodiscard]] Result<void>   start(const AudioSinkConfig& config) override;
    void                         stop() noexcept override;
    [[nodiscard]] bool           running() const noexcept override;
    [[nodiscard]] AudioFormat    format() const noexcept override;
    [[nodiscard]] std::size_t    quantumFrames() const noexcept override;
    [[nodiscard]] Result<void>   submit(const AudioBuffer& buffer) override;
    [[nodiscard]] std::uint64_t  playedFrames() const noexcept override;
    [[nodiscard]] std::uint64_t  submittedFrames() const noexcept override;

    /// Device underruns recovered so far (Requirement 6.2's dropout metric as the
    /// device reports it).
    [[nodiscard]] std::uint64_t underrunCount() const noexcept;

    /// Frames that could not be written inside the write budget.
    [[nodiscard]] std::uint64_t overflowFrames() const noexcept;

    /// Opaque backend state; defined in the .cpp.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace palmier::media

#endif // PALMIER_MEDIA_ALSAAUDIOSINK_HPP
