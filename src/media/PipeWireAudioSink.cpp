// SPDX-License-Identifier: GPL-3.0-or-later
//
// media/PipeWireAudioSink.cpp — the libpipewire-0.3 backend (task 8.6;
// Requirements 6.2, 6.7).
//
// Two implementations of one class, chosen by PALMIER_HAVE_PIPEWIRE:
//
//   * with the guard — a `pw_thread_loop` carrying one `pw_stream` connected as a
//     48 kHz / 2-channel / F32 playback stream. `submit()` copies interleaved
//     samples into a bounded FIFO; the loop's `process` callback drains the FIFO
//     into the graph's buffer and advances the played-frame counter by exactly the
//     frames it drained.
//   * without the guard — the same class with a `start()` that reports
//     `Unsupported` naming the missing build option, so the selection order in
//     AudioSinkSelector needs no preprocessor branch and the class is always
//     constructible and always testable.

#include "media/PipeWireAudioSink.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "core/Error.hpp"

#ifdef PALMIER_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#endif

namespace palmier::media {
namespace {

/// Shared validation of a sink configuration, identical for every backend: the
/// format must be usable and the quantum must be at least one frame.
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
// Impl — counters shared by both builds, backend state only under the guard.
// ---------------------------------------------------------------------------

struct PipeWireAudioSink::Impl {
    explicit Impl(Options opts) : options(std::move(opts)) {}

    Options                    options;
    AudioFormat                format{0, 0, SampleFormat::F32};
    std::size_t                quantum{0};
    std::atomic<bool>          active{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> played{0};
    std::atomic<std::uint64_t> underrun{0};
    std::atomic<std::uint64_t> overflow{0};

#ifdef PALMIER_HAVE_PIPEWIRE
    // --- Backend state ------------------------------------------------------
    pw_thread_loop* loop{nullptr};
    pw_stream*      stream{nullptr};

    std::mutex         fifoMutex;
    std::deque<float>  fifo;      ///< Interleaved samples awaiting the graph.
    std::size_t        fifoLimit{0};

    std::mutex              stateMutex;
    std::condition_variable stateCv;
    bool                    streaming{false};
    bool                    failed{false};
    std::string             failure;

    /// Drain up to `frames` frames into `dst`, zero-filling what the FIFO cannot
    /// supply. Returns the frames actually drained (what counts as played).
    std::size_t drain(float* dst, std::size_t frames, int channels) {
        const std::size_t wanted = frames * static_cast<std::size_t>(channels);
        std::size_t taken = 0;
        {
            std::lock_guard<std::mutex> lock(fifoMutex);
            taken = std::min(wanted, fifo.size());
            for (std::size_t i = 0; i < taken; ++i) {
                dst[i] = fifo.front();
                fifo.pop_front();
            }
        }
        if (taken < wanted) {
            std::fill(dst + taken, dst + wanted, 0.0f);
        }
        return taken / static_cast<std::size_t>(channels);
    }

    void onProcess() {
        if (stream == nullptr) return;
        pw_buffer* pwBuffer = pw_stream_dequeue_buffer(stream);
        if (pwBuffer == nullptr) return;

        spa_buffer* buffer = pwBuffer->buffer;
        auto*       dst = static_cast<float*>(buffer->datas[0].data);
        const auto  channels = static_cast<std::size_t>(format.channels);
        const std::size_t stride = sizeof(float) * channels;

        if (dst == nullptr || stride == 0 || buffer->datas[0].maxsize < stride) {
            pw_stream_queue_buffer(stream, pwBuffer);
            return;
        }

        std::size_t frames = buffer->datas[0].maxsize / stride;
        if (pwBuffer->requested != 0) {
            frames = std::min<std::size_t>(frames, static_cast<std::size_t>(pwBuffer->requested));
        }

        const std::size_t drained = drain(dst, frames, format.channels);
        played.fetch_add(static_cast<std::uint64_t>(drained));
        underrun.fetch_add(static_cast<std::uint64_t>(frames - drained));

        buffer->datas[0].chunk->offset = 0;
        buffer->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
        buffer->datas[0].chunk->size = static_cast<std::uint32_t>(frames * stride);
        pw_stream_queue_buffer(stream, pwBuffer);
    }

    void onStateChanged(pw_stream_state state, const char* error) {
        std::lock_guard<std::mutex> lock(stateMutex);
        switch (state) {
            case PW_STREAM_STATE_STREAMING:
                streaming = true;
                break;
            case PW_STREAM_STATE_ERROR:
                failed = true;
                failure = error != nullptr ? error : "the PipeWire stream reported an error";
                break;
            case PW_STREAM_STATE_UNCONNECTED:
                // Only a failure once we have asked to connect; the initial
                // transition to this state happens before connect().
                if (!streaming) {
                    failed = true;
                    if (failure.empty()) {
                        failure = error != nullptr
                                      ? error
                                      : "the PipeWire stream could not be connected to a device";
                    }
                }
                break;
            default:
                break;
        }
        stateCv.notify_all();
    }

    /// Stop the loop and destroy the stream. Safe to call more than once.
    void teardown() noexcept {
        if (loop != nullptr) {
            pw_thread_loop_stop(loop);
        }
        if (stream != nullptr) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (loop != nullptr) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
        }
        std::lock_guard<std::mutex> lock(fifoMutex);
        fifo.clear();
    }
#endif // PALMIER_HAVE_PIPEWIRE
};

#ifdef PALMIER_HAVE_PIPEWIRE
namespace {

void pipewireProcess(void* userdata) {
    static_cast<PipeWireAudioSink::Impl*>(userdata)->onProcess();
}

void pipewireStateChanged(void* userdata, pw_stream_state /*old*/, pw_stream_state state,
                          const char* error) {
    static_cast<PipeWireAudioSink::Impl*>(userdata)->onStateChanged(state, error);
}

const pw_stream_events& streamEvents() {
    // Value-initialise the whole C struct (every unused callback slot becomes a
    // null pointer) and then set only the two callbacks this sink wants. Written
    // this way rather than with designated initializers so the code is clean under
    // -Wmissing-field-initializers, which PALMIER_WERROR promotes to an error.
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = pipewireStateChanged;
        e.process = pipewireProcess;
        return e;
    }();
    return events;
}

/// `pw_init` is process-global and is deliberately never matched with
/// `pw_deinit`: a second sink in the same process must find the library
/// initialised, and tearing the library down while any thread still holds a
/// reference is a documented hazard.
void initPipeWireOnce() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

} // namespace
#endif // PALMIER_HAVE_PIPEWIRE

// ---------------------------------------------------------------------------
// Construction / accessors (identical in both builds).
// ---------------------------------------------------------------------------

PipeWireAudioSink::PipeWireAudioSink(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PipeWireAudioSink::~PipeWireAudioSink() {
    stop();
}

bool PipeWireAudioSink::running() const noexcept {
    return impl_->active.load();
}

AudioFormat PipeWireAudioSink::format() const noexcept {
    return impl_->format;
}

std::size_t PipeWireAudioSink::quantumFrames() const noexcept {
    return impl_->quantum;
}

std::uint64_t PipeWireAudioSink::playedFrames() const noexcept {
    return impl_->played.load();
}

std::uint64_t PipeWireAudioSink::submittedFrames() const noexcept {
    return impl_->submitted.load();
}

std::uint64_t PipeWireAudioSink::underrunFrames() const noexcept {
    return impl_->underrun.load();
}

std::uint64_t PipeWireAudioSink::overflowFrames() const noexcept {
    return impl_->overflow.load();
}

// ---------------------------------------------------------------------------
// start / stop / submit
// ---------------------------------------------------------------------------

#ifdef PALMIER_HAVE_PIPEWIRE

Result<void> PipeWireAudioSink::start(const AudioSinkConfig& config) {
    if (impl_->active.load()) {
        return makeError(ErrorCode::FailedPrecondition, "the PipeWire sink is already started");
    }
    if (Result<void> valid = validateSinkConfig(config); valid.isError()) {
        return valid;
    }

    initPipeWireOnce();

    impl_->format = config.format;
    impl_->quantum = config.quantumFrames;
    impl_->submitted.store(0);
    impl_->played.store(0);
    impl_->underrun.store(0);
    impl_->overflow.store(0);
    impl_->streaming = false;
    impl_->failed = false;
    impl_->failure.clear();
    impl_->fifoLimit = config.quantumFrames *
                       static_cast<std::size_t>(config.format.channels) *
                       std::max<std::size_t>(1, impl_->options.queueCapacityQuanta);

    impl_->loop = pw_thread_loop_new("palmier-audio", nullptr);
    if (impl_->loop == nullptr) {
        return makeError(ErrorCode::Io, "PipeWire is unavailable: the thread loop could not be "
                                       "created");
    }

    // The requested quantum is advertised to the graph as the node latency, which
    // is how "a quantum of at most 512 frames above 48 fps" reaches the server
    // (design.md D7).
    const std::string latency =
        std::to_string(config.quantumFrames) + "/" + std::to_string(config.format.sampleRate);

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_LATENCY, latency.c_str(),
                          PW_KEY_APP_NAME, impl_->options.applicationName.c_str(), nullptr);

    pw_thread_loop_lock(impl_->loop);
    impl_->stream = pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop), "palmier-playback",
                                         props, &streamEvents(), impl_.get());
    if (impl_->stream == nullptr) {
        pw_thread_loop_unlock(impl_->loop);
        impl_->teardown();
        return makeError(ErrorCode::Io, "PipeWire is unavailable: the playback stream could not "
                                       "be created");
    }

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = static_cast<std::uint32_t>(config.format.sampleRate);
    info.channels = static_cast<std::uint32_t>(config.format.channels);

    std::uint8_t     podStorage[1024];
    spa_pod_builder  builder{};
    builder.data = podStorage;
    builder.size = sizeof(podStorage);

    const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    const int rc = pw_stream_connect(
        impl_->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
    pw_thread_loop_unlock(impl_->loop);

    if (rc < 0) {
        const std::string reason = spa_strerror(rc);
        impl_->teardown();
        return makeError(ErrorCode::NotFound,
                         "PipeWire is unavailable: connecting a playback stream failed (" + reason +
                             ")");
    }

    if (pw_thread_loop_start(impl_->loop) < 0) {
        impl_->teardown();
        return makeError(ErrorCode::Io,
                         "PipeWire is unavailable: the audio thread loop could not be started");
    }

    // Wait for the stream to reach the streaming state. An absent daemon reports
    // an error here (or from the connect above); a wedged one hits the timeout.
    bool        ready = false;
    std::string reason;
    {
        std::unique_lock<std::mutex> lock(impl_->stateMutex);
        impl_->stateCv.wait_for(lock, impl_->options.connectTimeout,
                                [this] { return impl_->streaming || impl_->failed; });
        ready = impl_->streaming;
        reason = impl_->failure.empty() ? "the stream did not start within the connect timeout"
                                        : impl_->failure;
    }

    if (!ready) {
        impl_->teardown();
        return makeError(ErrorCode::Io, "PipeWire is unavailable: " + reason);
    }

    impl_->active.store(true);
    return ok();
}

void PipeWireAudioSink::stop() noexcept {
    if (!impl_->active.exchange(false)) {
        // Not started (or already stopped): still release any partially built
        // backend state from a failed start.
        impl_->teardown();
        return;
    }
    impl_->teardown();
}

Result<void> PipeWireAudioSink::submit(const AudioBuffer& buffer) {
    if (!impl_->active.load()) {
        return makeError(ErrorCode::FailedPrecondition,
                         "the PipeWire sink is not started; start() it before submitting");
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

    const std::vector<float>& samples = buffer.samples();
    std::size_t               accepted = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->fifoMutex);
        const std::size_t room =
            impl_->fifoLimit > impl_->fifo.size() ? impl_->fifoLimit - impl_->fifo.size() : 0;
        accepted = std::min(room, samples.size());
        impl_->fifo.insert(impl_->fifo.end(), samples.begin(),
                           samples.begin() + static_cast<std::ptrdiff_t>(accepted));
    }

    const auto channels = static_cast<std::size_t>(impl_->format.channels);
    impl_->submitted.fetch_add(static_cast<std::uint64_t>(accepted / channels));
    if (accepted < samples.size()) {
        // Never block the mixing thread: the excess is recorded rather than
        // waited on, so a caller that mixes ahead of the device is visible but
        // never stalled.
        impl_->overflow.fetch_add(static_cast<std::uint64_t>((samples.size() - accepted) /
                                                            channels));
    }
    return ok();
}

#else // !PALMIER_HAVE_PIPEWIRE

Result<void> PipeWireAudioSink::start(const AudioSinkConfig& config) {
    if (Result<void> valid = validateSinkConfig(config); valid.isError()) {
        return valid;
    }
    return makeError(ErrorCode::Unsupported,
                     "PipeWire audio output is not compiled in (PALMIER_HAVE_PIPEWIRE is "
                     "undefined: configure with -DPALMIER_ENABLE_PIPEWIRE=ON on a host with "
                     "libpipewire-0.3 installed)");
}

void PipeWireAudioSink::stop() noexcept {
    impl_->active.store(false);
}

Result<void> PipeWireAudioSink::submit(const AudioBuffer& /*buffer*/) {
    return makeError(ErrorCode::FailedPrecondition,
                     "the PipeWire sink is not started; PipeWire audio output is not compiled in");
}

#endif // PALMIER_HAVE_PIPEWIRE

} // namespace palmier::media
