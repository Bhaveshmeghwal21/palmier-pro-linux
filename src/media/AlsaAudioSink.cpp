// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/AlsaAudioSink.cpp — the libasound2 backend (task 8.6; Requirements 6.2,
// 6.7).
//
// Two implementations of one class, chosen by PALMIER_HAVE_ALSA. With the guard,
// `default` is opened non-blocking as an interleaved 32-bit float PCM at the
// engine's output format and `submit()` writes straight through; without it,
// `start()` reports `Unsupported` and the selector moves on.

#include "media/AlsaAudioSink.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "core/Error.hpp"

#ifdef PALMIER_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

namespace palmier::media {
namespace {

[[nodiscard]] Result<void> validateSinkConfig(const AudioSinkConfig& config) {
    if (!config.format.isValid()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the audio sink format must declare a positive sample rate and channel "
                         "count");
    }
    if (config.quantumFrames == 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "the audio sink buffer quantum must be at least one frame");
    }
    return ok();
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct AlsaAudioSink::Impl {
    explicit Impl(Options opts) : options(std::move(opts)) {}

    Options                    options;
    AudioFormat                format{0, 0, SampleFormat::F32};
    std::size_t                quantum{0};
    std::atomic<bool>          active{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> played{0};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<std::uint64_t> overflow{0};

#ifdef PALMIER_HAVE_ALSA
    std::mutex deviceMutex;
    snd_pcm_t* pcm{nullptr};

    void closeDevice() noexcept {
        std::lock_guard<std::mutex> lock(deviceMutex);
        if (pcm != nullptr) {
            snd_pcm_close(pcm);
            pcm = nullptr;
        }
    }
#endif
};

#ifdef PALMIER_HAVE_ALSA
namespace {

/// Swallow libasound's own stderr chatter: an absent card is a normal, expected
/// outcome here (Requirement 6.7), not something to print during a test run.
void silentAlsaError(const char*, int, const char*, int, const char*, ...) {}

void installSilentAlsaErrorHandler() {
    static std::once_flag once;
    std::call_once(once, [] { snd_lib_error_set_handler(&silentAlsaError); });
}

} // namespace
#endif

// ---------------------------------------------------------------------------
// Construction / accessors (identical in both builds).
// ---------------------------------------------------------------------------

AlsaAudioSink::AlsaAudioSink(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

AlsaAudioSink::~AlsaAudioSink() {
    stop();
}

bool AlsaAudioSink::running() const noexcept {
    return impl_->active.load();
}

AudioFormat AlsaAudioSink::format() const noexcept {
    return impl_->format;
}

std::size_t AlsaAudioSink::quantumFrames() const noexcept {
    return impl_->quantum;
}

std::uint64_t AlsaAudioSink::submittedFrames() const noexcept {
    return impl_->submitted.load();
}

std::uint64_t AlsaAudioSink::underrunCount() const noexcept {
    return impl_->underruns.load();
}

std::uint64_t AlsaAudioSink::overflowFrames() const noexcept {
    return impl_->overflow.load();
}

// ---------------------------------------------------------------------------
// start / stop / submit / playedFrames
// ---------------------------------------------------------------------------

#ifdef PALMIER_HAVE_ALSA

Result<void> AlsaAudioSink::start(const AudioSinkConfig& config) {
    if (impl_->active.load()) {
        return makeError(ErrorCode::FailedPrecondition, "the ALSA sink is already started");
    }
    if (Result<void> valid = validateSinkConfig(config); valid.isError()) {
        return valid;
    }

    installSilentAlsaErrorHandler();

    snd_pcm_t* pcm = nullptr;
    int        rc = snd_pcm_open(&pcm, impl_->options.deviceName.c_str(), SND_PCM_STREAM_PLAYBACK,
                                SND_PCM_NONBLOCK);
    if (rc < 0 || pcm == nullptr) {
        return makeError(ErrorCode::NotFound,
                         "ALSA audio output is unavailable: opening PCM device '" +
                             impl_->options.deviceName + "' failed (" + snd_strerror(rc) + ")");
    }

    // Requested latency: the quantum times the configured buffer depth, in
    // microseconds. That is how "a quantum of at most 512 frames above 48 fps"
    // (design.md D7) reaches the device.
    const auto quantaDepth = std::max<std::size_t>(1, impl_->options.bufferQuanta);
    const auto latencyUs = static_cast<unsigned int>(
        (static_cast<std::uint64_t>(config.quantumFrames) * quantaDepth * 1'000'000ULL) /
        static_cast<std::uint64_t>(config.format.sampleRate));

    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            static_cast<unsigned int>(config.format.channels),
                            static_cast<unsigned int>(config.format.sampleRate),
                            /*soft_resample=*/1, latencyUs == 0 ? 100'000U : latencyUs);
    if (rc < 0) {
        snd_pcm_close(pcm);
        return makeError(ErrorCode::Unsupported,
                         "ALSA audio output is unavailable: device '" + impl_->options.deviceName +
                             "' does not support " + std::to_string(config.format.sampleRate) +
                             " Hz / " + std::to_string(config.format.channels) +
                             " channel float output (" + snd_strerror(rc) + ")");
    }

    rc = snd_pcm_prepare(pcm);
    if (rc < 0) {
        snd_pcm_close(pcm);
        return makeError(ErrorCode::Io, "ALSA audio output is unavailable: preparing device '" +
                                           impl_->options.deviceName + "' failed (" +
                                           snd_strerror(rc) + ")");
    }

    impl_->format = config.format;
    impl_->quantum = config.quantumFrames;
    impl_->submitted.store(0);
    impl_->written.store(0);
    impl_->played.store(0);
    impl_->underruns.store(0);
    impl_->overflow.store(0);
    {
        std::lock_guard<std::mutex> lock(impl_->deviceMutex);
        impl_->pcm = pcm;
    }
    impl_->active.store(true);
    return ok();
}

void AlsaAudioSink::stop() noexcept {
    if (!impl_->active.exchange(false)) {
        impl_->closeDevice();
        return;
    }
    // Latch the final position before the device goes away.
    const std::uint64_t finalPosition = playedFrames();
    impl_->closeDevice();
    impl_->played.store(finalPosition);
}

Result<void> AlsaAudioSink::submit(const AudioBuffer& buffer) {
    if (!impl_->active.load()) {
        return makeError(ErrorCode::FailedPrecondition,
                         "the ALSA sink is not started; start() it before submitting");
    }
    if (buffer.frameCount() == 0) {
        return ok();
    }
    if (buffer.sampleRate() != impl_->format.sampleRate ||
        buffer.channels() != impl_->format.channels) {
        return makeError(ErrorCode::InvalidArgument,
                         "submitted buffer format (" + std::to_string(buffer.sampleRate()) +
                             " Hz, " + std::to_string(buffer.channels()) +
                             " channels) does not match the sink format (" +
                             std::to_string(impl_->format.sampleRate) + " Hz, " +
                             std::to_string(impl_->format.channels) + " channels)");
    }

    std::lock_guard<std::mutex> lock(impl_->deviceMutex);
    if (impl_->pcm == nullptr) {
        return makeError(ErrorCode::FailedPrecondition, "the ALSA device has been closed");
    }

    const float* cursor = buffer.samples().data();
    auto         remaining = static_cast<snd_pcm_uframes_t>(buffer.frameCount());
    const auto   channels = static_cast<std::size_t>(impl_->format.channels);
    auto         budget = static_cast<int>(impl_->options.writeBudget.count());
    bool         recovered = false;

    while (remaining > 0) {
        const snd_pcm_sframes_t frames =
            snd_pcm_writei(impl_->pcm, cursor, remaining);
        if (frames > 0) {
            const auto accepted = static_cast<snd_pcm_uframes_t>(frames);
            cursor += accepted * channels;
            remaining -= accepted;
            impl_->written.fetch_add(static_cast<std::uint64_t>(accepted));
            impl_->submitted.fetch_add(static_cast<std::uint64_t>(accepted));
            continue;
        }

        if (frames == -EAGAIN) {
            if (budget <= 0) break;
            const int waited = snd_pcm_wait(impl_->pcm, budget);
            if (waited <= 0) break;
            budget = 0; // one bounded wait per submission, then give up
            continue;
        }
        if (frames == -EPIPE) {
            // Underrun: re-prepare once and retry the remainder. A second
            // underrun in the same submission is reported as an overflow rather
            // than looped on.
            impl_->underruns.fetch_add(1);
            if (recovered) break;
            recovered = true;
            if (snd_pcm_prepare(impl_->pcm) < 0) break;
            continue;
        }
        if (frames == -ESTRPIPE) {
            if (snd_pcm_prepare(impl_->pcm) < 0) break;
            continue;
        }
        return makeError(ErrorCode::Io,
                         std::string("writing to the ALSA device failed: ") +
                             snd_strerror(static_cast<int>(frames)));
    }

    if (remaining > 0) {
        impl_->overflow.fetch_add(static_cast<std::uint64_t>(remaining));
    }
    return ok();
}

std::uint64_t AlsaAudioSink::playedFrames() const noexcept {
    const std::uint64_t latched = impl_->played.load();
    if (!impl_->active.load()) return latched;

    std::uint64_t candidate = latched;
    {
        std::lock_guard<std::mutex> lock(impl_->deviceMutex);
        if (impl_->pcm == nullptr) return latched;
        snd_pcm_sframes_t delay = 0;
        const std::uint64_t writtenFrames = impl_->written.load();
        if (snd_pcm_delay(impl_->pcm, &delay) == 0 && delay >= 0) {
            const auto queued = static_cast<std::uint64_t>(delay);
            candidate = writtenFrames > queued ? writtenFrames - queued : 0;
        } else {
            candidate = writtenFrames;
        }
    }

    // Never regress and never claim more than was submitted.
    candidate = std::min(candidate, impl_->submitted.load());
    std::uint64_t previous = latched;
    while (candidate > previous && !impl_->played.compare_exchange_weak(previous, candidate)) {
        // previous refreshed by compare_exchange_weak; retry.
    }
    return std::max(candidate, previous);
}

#else // !PALMIER_HAVE_ALSA

Result<void> AlsaAudioSink::start(const AudioSinkConfig& config) {
    if (Result<void> valid = validateSinkConfig(config); valid.isError()) {
        return valid;
    }
    return makeError(ErrorCode::Unsupported,
                     "ALSA audio output is not compiled in (PALMIER_HAVE_ALSA is undefined: "
                     "configure with -DPALMIER_ENABLE_ALSA=ON on a host with libasound2 "
                     "installed)");
}

void AlsaAudioSink::stop() noexcept {
    impl_->active.store(false);
}

Result<void> AlsaAudioSink::submit(const AudioBuffer& /*buffer*/) {
    return makeError(ErrorCode::FailedPrecondition,
                     "the ALSA sink is not started; ALSA audio output is not compiled in");
}

std::uint64_t AlsaAudioSink::playedFrames() const noexcept {
    return impl_->played.load();
}

#endif // PALMIER_HAVE_ALSA

} // namespace palmier::media
