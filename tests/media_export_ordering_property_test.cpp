// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/media_export_ordering_property_test.cpp — property test for export frame
// ordering (task 10.3; Requirement 11.1).
//
// Design property P6 (design.md "Correctness Properties"):
//
//     Export frame ordering — the rendered frames of an export are emitted in
//     strictly increasing presentation time.
//
// This is the export guarantee behind Requirement 11.1 ("render the complete
// timeline into a single output file"): a well-formed output stream presents its
// frames in monotonically advancing time, with no repeated or out-of-order
// presentation timestamps. The Export Engine's render loop (task 10.1,
// ExportEngine.cpp) submits frame i at frameRate.durationForFrames(i) — a
// strictly increasing sequence since the frame step is positive — and the
// MediaEncoder additionally rejects any presentation-time regression on submit().
//
// Strategy: over arbitrary timelines (clip length in whole frames), frame rates
// (both integer and NTSC-style rational rates), and output resolutions, run
// ExportEngine::run driving a real software Compositor (fed synthetic solid
// frames) and a MediaEncoder built behind the IEncodeBackend seam with a mock
// backend that records the presentation time of every submitted frame into
// test-owned state. After the export completes we assert — independently of the
// engine's own arithmetic — that the recorded presentation-time sequence is
// STRICTLY INCREASING (each frame's presentation time is greater than the
// previous frame's). Because the seams are software-only, this runs with no GPU,
// no FFmpeg, and no vendor SDK.
//
// _Requirements: 11.1_
//
// ---------------------------------------------------------------------------
// Task 9.6 — Property 38: two successive exports are identical (Requirement 7.8)
// ---------------------------------------------------------------------------
//
// The same seams carry a second, stronger property: determinism. An export is
// reproducible only if running it twice over the same timeline produces the same
// stream, so Property 38 runs TWO successive exports of one generated timeline —
// over all four containers, all three codecs and both encoder selections (hardware
// and software) — and compares the recorded per-frame presentation timestamps
// element by element, together with the frame count and the route the encoder bound
// to. Recording the timestamps in a test-owned trace, rather than asking either
// export what it did, is what makes the comparison independent of the code under
// test.
//
// The hardware selection is reachable on a host with neither a GPU nor a vendor SDK
// because gpu::GpuCaps and gpu::BridgeAvailability are VALUES on EncodeSpec: the
// property synthesizes a capable device and a compiled-in vendor path, so the
// hardware route is exercised in CI exactly as the software one is. Both exports
// write to ABSOLUTE per-case paths containing getpid(), so parallel ctest processes
// cannot collide — although this mock backend keeps its trace in memory and writes
// no bytes, so nothing here depends on the filesystem.
//
// _Requirements: 7.8, 11.1_

#include "media/ExportEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h> // getpid(), for per-process output paths

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/MediaEncoder.hpp"

namespace palmier::media {
namespace {

// --- Mock encode backend recording every submitted presentation time --------
//
// Records into test-owned state so the observations survive the MediaEncoder's
// destruction inside ExportEngine::run(). Mirrors the MockEncodeBackend used by
// the export-engine unit tests (media_export_engine_test.cpp) but keeps only the
// presentation-time trace this property needs.
struct RecordingState {
    std::vector<Duration> presentations{};
    /// The audio blocks' presentation times, for the determinism property (task
    /// 9.6): an export that includes audio must reproduce both streams' ordering.
    std::vector<Duration> audioPresentations{};
    /// The route the encoder bound to, so Property 38 can assert that two
    /// successive exports also made the SAME encoder selection.
    gpu::CodecRoute route{};
    /// How many backends were built — a hardware initialization retry would show up
    /// here, and it must be the same both times.
    int backendsBuilt{0};
};

class RecordingEncodeBackend final : public IEncodeBackend {
public:
    explicit RecordingEncodeBackend(RecordingState* state) : state_(state) {}

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        state_->presentations.push_back(frame.presentation);
        return ok();
    }

    [[nodiscard]] Result<void> encodeAudio(const EncoderInputAudio& audio) override {
        state_->audioPresentations.push_back(audio.presentation);
        return ok();
    }

    [[nodiscard]] Result<void> finish() override { return ok(); }

private:
    RecordingState* state_;
};

EncodeBackendFactory recordingFactory(RecordingState* state) {
    return [state](const EncodeSpec&, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<IEncodeBackend>> {
        state->route = route;
        ++state->backendsBuilt;
        return std::unique_ptr<IEncodeBackend>(
            std::make_unique<RecordingEncodeBackend>(state));
    };
}

// A single-video-track project whose one clip spans exactly `lengthFrames` whole
// frames at `fps`, starting at t=0. Using whole-frame geometry makes the planned
// frame count exactly `lengthFrames`, so every timeline position renders a frame.
Project makeProject(FrameRate fps, std::int64_t lengthFrames, Resolution canvas) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::zero();
    c.sourceIn = Duration::zero();
    c.sourceOut = fps.durationForFrames(lengthFrames);
    c.opacity = 1.0;

    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.clips = {c};

    Project p;
    p.id = Uuid::generateV4();
    p.name = "export-ordering-prop";
    p.timelineFps = fps;
    p.canvas = canvas;
    p.tracks = {t};
    return p;
}

// Feature: palmier-pro-linux, Property 6: Export frame ordering — rendered frames
// are emitted in strictly increasing presentation time.
// Validates: Requirements 11.1
RC_GTEST_PROP(ExportOrderingProperties,
              RenderedFramesHaveStrictlyIncreasingPresentationTime,
              ()) {
    // Arbitrary frame rate: an integer rate (num/1) or an NTSC-style rational
    // rate (num/1001). Both are strictly positive, hence valid, and give a
    // positive frame step — the precondition for strictly increasing times.
    const std::int64_t num = *rc::gen::inRange<std::int64_t>(1, 241);
    const std::int64_t den = *rc::gen::element<std::int64_t>(1, 1001);
    const FrameRate fps(num, den);
    RC_ASSERT(fps.isValid());

    // Arbitrary timeline length (whole frames) and output resolution. The
    // export codecs use 4:2:0 chroma subsampling, so ExportEngine only accepts
    // EVEN width and height; generate small even, positive dimensions so the
    // property is exercised over valid export targets (odd/1px sizes are
    // legitimately rejected by the engine and are out of this property's domain).
    const std::int64_t lengthFrames = *rc::gen::inRange<std::int64_t>(1, 250);
    const std::uint32_t width =
        static_cast<std::uint32_t>(2 * *rc::gen::inRange<int>(1, 5)); // {2,4,6,8}
    const std::uint32_t height =
        static_cast<std::uint32_t>(2 * *rc::gen::inRange<int>(1, 5)); // {2,4,6,8}
    const Resolution canvas{width, height};

    // Software Compositor fed synthetic solid frames at the export resolution,
    // so no GPU/decoder is needed and every visible position composites cleanly.
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor comp(ctx);
    comp.setFrameProvider([width, height](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(width, height, gpu::RgbaColor{10, 20, 30, 255});
    });

    ExportEngine engine(comp);

    ExportRequest req;
    req.codec = gpu::CodecId::H264;
    req.resolution = canvas;
    req.frameRate = fps;
    req.preferHardware = false; // exercise the software encode route.
    req.outputPath = "out.mp4";
    req.containerFormat = "mp4";
    req.progressInterval = std::chrono::milliseconds{0}; // does not affect ordering.

    RecordingState state;
    const std::size_t planned = ExportEngine::plannedFrameCount(makeProject(fps, lengthFrames, canvas), fps);
    Project project = makeProject(fps, lengthFrames, canvas);

    auto result = engine.run(project, req, recordingFactory(&state));
    RC_ASSERT(result.isOk());

    // Every planned frame is submitted exactly once.
    RC_ASSERT(state.presentations.size() == planned);
    RC_ASSERT(!state.presentations.empty());

    // Core property (P6): presentation times are STRICTLY increasing — each
    // frame is presented strictly after the one before it. The first frame is
    // presented at t=0.
    RC_ASSERT(state.presentations.front() == Duration::zero());
    for (std::size_t i = 1; i < state.presentations.size(); ++i) {
        RC_ASSERT(state.presentations[i - 1] < state.presentations[i]);
    }
}

// ---------------------------------------------------------------------------
// Task 9.6 support: the generated request space and a synthetic capable device
// ---------------------------------------------------------------------------

/// The three codecs the Encoder_Selector supports and the four containers an
/// export may target (Requirements 7.1, 8.2).
constexpr gpu::CodecId kCodecs[] = {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::VP9};
constexpr int          kCodecCount = 3;
constexpr const char*  kContainers[] = {"mp4", "mov", "mkv", "webm"};
constexpr int          kContainerCount = 4;

/// A device that advertises hardware encode for `codec`, supplied as a VALUE so the
/// hardware encoder selection is exercised on a host with no GPU and no vendor SDK.
[[nodiscard]] gpu::GpuCaps capableDevice(gpu::CodecId codec) {
    gpu::GpuCaps caps;
    caps.vendorId = gpu::GpuVendor::NVIDIA;
    caps.vendor = "NVIDIA";
    caps.supportsCompute = true;
    caps.hwDecode = true;
    caps.hwEncode = true;
    caps.decodeCodecs = {codec};
    caps.encodeCodecs = {codec};
    caps.vramBytes = 4ull * 1024 * 1024 * 1024;
    return caps;
}

/// An absolute, per-process output path. This mock backend writes no bytes, but the
/// path is still per-process so that a future backend that does cannot make two
/// parallel ctest processes collide.
[[nodiscard]] std::filesystem::path outputPathFor(const std::string& name) {
    static std::atomic<unsigned long long> counter{0};
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("palmier_export_ordering_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / (name + "_" + std::to_string(counter.fetch_add(1)) + ".out");
}

// Feature: end-to-end-editor-integration, Property 38: Two successive exports are identical — for
// all fixed timelines of up to 3600 frames, any valid export request and any encoder selection, two
// successive exports produce identical frame counts and identical per-frame presentation timestamps.
//
// **Validates: Requirements 7.8**
RC_GTEST_PROP(ExportOrderingProperties, TwoSuccessiveExportsAreIdentical, ()) {
    // The request space: integer and NTSC-style rational frame rates, all three
    // codecs, all four containers, both encoder selections, with and without audio.
    const std::int64_t num = *rc::gen::inRange<std::int64_t>(1, 121);
    const std::int64_t den = *rc::gen::element<std::int64_t>(1, 1001);
    const FrameRate    fps(num, den);
    RC_ASSERT(fps.isValid());

    const std::uint32_t width = static_cast<std::uint32_t>(2 * *rc::gen::inRange<int>(1, 9));
    const std::uint32_t height = static_cast<std::uint32_t>(2 * *rc::gen::inRange<int>(1, 9));
    const Resolution    canvas{width, height};

    const gpu::CodecId codec = kCodecs[*rc::gen::inRange<int>(0, kCodecCount)];
    const std::string  container = kContainers[*rc::gen::inRange<int>(0, kContainerCount)];
    const auto         bitrate = *rc::gen::inRange<std::int64_t>(100'000, 200'000'001);
    // Both encoder selections: a hardware route against a synthetic capable device,
    // and the software route.
    const bool preferHardware = *rc::gen::arbitrary<bool>();
    const bool includeAudio = *rc::gen::arbitrary<bool>();

    // Timeline length in whole frames. An export that includes audio mixes 48 kHz
    // stereo for every frame interval, so the length of THOSE timelines is bounded
    // by wall-clock duration (about two seconds of audio) rather than by frame
    // count: a 250-frame timeline at 1 fps would mix four minutes of audio twice per
    // generated case for no additional coverage of the ordering guarantee.
    const std::int64_t maxLength =
        includeAudio ? std::max<std::int64_t>(
                           1, std::min<std::int64_t>(240, static_cast<std::int64_t>(
                                                              fps.toDouble() * 2.0)))
                     : 250;
    const std::int64_t lengthFrames = *rc::gen::inRange<std::int64_t>(1, maxLength + 1);

    const Project     project = makeProject(fps, lengthFrames, canvas);
    const std::size_t planned = ExportEngine::plannedFrameCount(project, fps);
    RC_ASSERT(planned >= 1);

    // One export, driven entirely by software seams; run twice into two separate
    // test-owned traces so the comparison is independent of the code under test.
    auto runOnce = [&](RecordingState& state, const std::filesystem::path& output) {
        auto            ctx = gpu::GpuContext::softwareFallback();
        gpu::Compositor comp(ctx);
        comp.setFrameProvider([width, height](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            return gpu::SourceFrame::solid(width, height, gpu::RgbaColor{10, 20, 30, 255});
        });
        ExportEngine engine(comp);

        ExportRequest req;
        req.codec = codec;
        req.resolution = canvas;
        req.frameRate = fps;
        req.bitrateBitsPerSecond = bitrate;
        req.preferHardware = preferHardware;
        req.caps = preferHardware ? capableDevice(codec) : gpu::GpuCaps::software();
        req.availability = gpu::BridgeAvailability::all();
        req.outputPath = output;
        req.containerFormat = container;
        req.progressInterval = std::chrono::milliseconds{0};
        req.includeAudio = includeAudio;

        Project copy = project;
        return engine.run(copy, req, recordingFactory(&state));
    };

    RecordingState first;
    RecordingState second;
    const auto firstResult = runOnce(first, outputPathFor("determinism_a"));
    const auto secondResult = runOnce(second, outputPathFor("determinism_b"));
    RC_ASSERT(firstResult.isOk());
    RC_ASSERT(secondResult.isOk());

    // Non-vacuity: two empty traces would compare equal, so the trace must be the
    // planned frame count before the comparison means anything.
    RC_ASSERT(first.presentations.size() == planned);
    RC_ASSERT(firstResult.value().framesRendered == planned);

    // Identical frame counts...
    RC_ASSERT(secondResult.value().framesRendered == firstResult.value().framesRendered);
    RC_ASSERT(secondResult.value().totalFrames == firstResult.value().totalFrames);
    RC_ASSERT(second.presentations.size() == first.presentations.size());

    // ...and identical per-frame presentation timestamps, element by element.
    for (std::size_t i = 0; i < first.presentations.size(); ++i) {
        RC_ASSERT(second.presentations[i] == first.presentations[i]);
    }

    // The audio stream, when there is one, is equally reproducible.
    RC_ASSERT(secondResult.value().containsAudio == firstResult.value().containsAudio);
    RC_ASSERT(secondResult.value().audioFrames == firstResult.value().audioFrames);
    RC_ASSERT(second.audioPresentations.size() == first.audioPresentations.size());
    RC_ASSERT(first.audioPresentations.size() == (includeAudio ? planned : 0u));
    for (std::size_t i = 0; i < first.audioPresentations.size(); ++i) {
        RC_ASSERT(second.audioPresentations[i] == first.audioPresentations[i]);
    }

    // The encoder selection is reproducible too: the same route, the same hardware
    // decision and the same number of backend initializations both times, so two
    // successive exports cannot differ by having been encoded differently.
    RC_ASSERT(second.route.hardware == first.route.hardware);
    RC_ASSERT(second.route.backend == first.route.backend);
    RC_ASSERT(second.route.codec == first.route.codec);
    RC_ASSERT(second.backendsBuilt == first.backendsBuilt);
    RC_ASSERT(secondResult.value().usedHardwareEncode == firstResult.value().usedHardwareEncode);
    RC_ASSERT(secondResult.value().usedSoftwareFallback ==
              firstResult.value().usedSoftwareFallback);
}

}  // namespace
}  // namespace palmier::media
