// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/still_frame_capture_test.cpp — services::captureFrame
// (usable-editor tasks.md task 14; still-frame capture). No dedicated
// Requirement; the acceptance criteria are task 14's own two subtasks.

#include "services/StillFrameCapture.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "core/Duration.hpp"
#include "core/Project.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"

namespace palmier::services {
namespace {

Project makeProjectWithOneVideoClip() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "test";
    project.canvas = Resolution{4, 4};
    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromSeconds(10.0);
    track.clips.push_back(clip);
    project.tracks.push_back(std::move(track));
    return project;
}

/// An encoder that records exactly what it was asked to write, rather than
/// touching a filesystem at all.
struct RecordingEncoder {
    std::vector<std::uint8_t> pixels;
    std::uint32_t             width = 0;
    std::uint32_t             height = 0;
    std::filesystem::path     path;
    int                       callCount = 0;

    ImageEncoder asEncoder() {
        return [this](const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                      const std::filesystem::path& p) -> Result<void> {
            pixels.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4u);
            width = w;
            height = h;
            path = p;
            ++callCount;
            return ok();
        };
    }
};

TEST(CaptureFrame, RendersThroughTheCompositorAndHandsPixelsToTheEncoder) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);
    compositor.setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(4, 4, gpu::RgbaColor{10, 20, 30, 255});
    });

    const Project project = makeProjectWithOneVideoClip();
    RecordingEncoder encoder;

    const Result<void> result = captureFrame(compositor, project, Duration::fromSeconds(1.0),
                                             "/tmp/frame.png", encoder.asEncoder());
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(encoder.callCount, 1);
    EXPECT_EQ(encoder.width, 4u);
    EXPECT_EQ(encoder.height, 4u);
    EXPECT_EQ(encoder.path, std::filesystem::path("/tmp/frame.png"));
    ASSERT_EQ(encoder.pixels.size(), 4u * 4u * 4u);
    // The rendered clip fills the whole canvas at full opacity, so the first
    // pixel is exactly the solid color the provider returned.
    EXPECT_EQ(encoder.pixels[0], 10);
    EXPECT_EQ(encoder.pixels[1], 20);
    EXPECT_EQ(encoder.pixels[2], 30);
    EXPECT_EQ(encoder.pixels[3], 255);
}

TEST(CaptureFrame, PropagatesARenderFailureWithoutCallingTheEncoder) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);  // no frame provider installed

    const Project project = makeProjectWithOneVideoClip();
    RecordingEncoder encoder;

    const Result<void> result = captureFrame(compositor, project, Duration::fromSeconds(1.0),
                                             "/tmp/frame.png", encoder.asEncoder());
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(encoder.callCount, 0);
}

TEST(CaptureFrame, PropagatesAnEncoderFailure) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);
    compositor.setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(4, 4, gpu::RgbaColor{0, 0, 0, 255});
    });

    const Project project = makeProjectWithOneVideoClip();
    const ImageEncoder failingEncoder =
        [](const std::uint8_t*, std::uint32_t, std::uint32_t,
           const std::filesystem::path&) -> Result<void> {
        return err(makeError(ErrorCode::Io, "disk full"));
    };

    const Result<void> result = captureFrame(compositor, project, Duration::fromSeconds(1.0),
                                             "/tmp/frame.png", failingEncoder);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::Io);
}

// Renders the SAME position through the SAME compositor instance twice —
// mirroring the "render at the playhead" premise task 14.2 asks the real
// tests to verify: a still-frame capture at a given position and a preview
// render at that same position, through the same Compositor, produce byte-
// identical pixels, because both calls are the identical renderAt() call with
// no other variable in play.
TEST(CaptureFrame, TwoCapturesOfTheIdenticalPositionProduceByteIdenticalPixels) {
    auto ctx = gpu::GpuContext::softwareFallback();
    gpu::Compositor compositor(ctx);
    compositor.setFrameProvider([](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        return gpu::SourceFrame::solid(4, 4, gpu::RgbaColor{77, 88, 99, 255});
    });

    const Project project = makeProjectWithOneVideoClip();
    RecordingEncoder first;
    RecordingEncoder second;

    ASSERT_TRUE(captureFrame(compositor, project, Duration::fromSeconds(2.0), "/tmp/a.png",
                             first.asEncoder())
                    .isOk());
    ASSERT_TRUE(captureFrame(compositor, project, Duration::fromSeconds(2.0), "/tmp/b.png",
                             second.asEncoder())
                    .isOk());

    EXPECT_EQ(first.pixels, second.pixels);
}

}  // namespace
}  // namespace palmier::services
