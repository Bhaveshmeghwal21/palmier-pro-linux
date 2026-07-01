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

#include "media/ExportEngine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

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
};

class RecordingEncodeBackend final : public IEncodeBackend {
public:
    explicit RecordingEncodeBackend(RecordingState* state) : state_(state) {}

    [[nodiscard]] Result<void> encode(const EncoderInputFrame& frame) override {
        state_->presentations.push_back(frame.presentation);
        return ok();
    }

    [[nodiscard]] Result<void> finish() override { return ok(); }

private:
    RecordingState* state_;
};

EncodeBackendFactory recordingFactory(RecordingState* state) {
    return [state](const EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<IEncodeBackend>> {
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

    // Arbitrary timeline length (whole frames) and output resolution.
    const std::int64_t lengthFrames = *rc::gen::inRange<std::int64_t>(1, 250);
    const std::uint32_t width =
        static_cast<std::uint32_t>(*rc::gen::inRange<int>(1, 9));
    const std::uint32_t height =
        static_cast<std::uint32_t>(*rc::gen::inRange<int>(1, 9));
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

}  // namespace
}  // namespace palmier::media
