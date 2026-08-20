// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu/invert_colors_property_test.cpp — RapidCheck property tests for the
// ported invert-colors effect (upstream PR 408), task 1.4 of the
// end-to-end-editor-integration spec; Requirements 14.4, 14.5.
//
// Two properties live here, and only two:
//
//   * Property 73 — the per-channel arithmetic of the effect itself, asserted
//     over generated RGBA images against the production software reference
//     (applyEffectSoftware -> applyInvertColors in gpu/Compositor.cpp).
//   * Property 74 — the playback and export compositing paths agree, per
//     channel, within 1 of 255 when the same source frame carries the same
//     invert-colors effect.
//
// Lanes for Property 74 (following the pattern of the existing GPU parity
// property test, tests/gpu_gpu_cpu_parity_property_test.cpp): this sandbox has
// no Vulkan device, so both lanes run the vendor-neutral software compositing
// path — but they run it through the two *production* pipelines the requirement
// names, not through a hand-rolled comparison:
//
//   * playback lane — ui::PreviewController (the preview/player engine) driving
//     Compositor::renderAt at the playhead, with the composited pixels captured
//     from its PreviewFrameSink.
//   * export lane — media::ExportEngine::run driving Compositor::renderAt per
//     frame and submitting to a MediaEncoder built behind the IEncodeBackend
//     seam with a recording mock backend (the pattern used by
//     tests/media_export_engine_test.cpp), with the composited pixels captured
//     from the submitted EncoderInputFrame's host data.
//
// Both lanes therefore exercise the real target setup, frame stepping, clip
// effect application, and opacity blending of their respective pipelines, and
// the assertion — per-channel |playback - export| <= 1 — is unchanged when a
// real device makes the playback lane a GPU compute dispatch.
//
// Plain example-based unit tests for the effect (including the exhaustive
// all-256-byte-values check and the kernel/reference agreement check) already
// live in tests/gpu_effect_kernels_test.cpp (task 1.3) and are deliberately not
// duplicated here.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/ExportEngine.hpp"
#include "media/MediaEncoder.hpp"
#include "ui/PreviewController.hpp"

namespace palmier {
namespace {

// The bounded per-channel tolerance Requirement 14.5 allows between the
// playback and export compositing paths: at most 1 of 255.
constexpr int kPathTolerance = 1;

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

/// A generated RGBA8 image: dimensions plus row-major, tightly packed pixels.
struct GenImage {
    std::uint32_t             width{0};
    std::uint32_t             height{0};
    std::vector<std::uint8_t> rgba{};
};

using Quad = std::array<std::uint8_t, 4>;

/// Generate an RGBA image with dimensions in [1, maxExtent] on each axis.
///
/// The pixel data is tiled, from a generated offset, out of a palette that
/// always contains 0, 255 and mid-range values in *every* channel (the fixed
/// entries) plus four fully generated pixels. Tiling a generated palette keeps
/// a 256x256 case cheap to generate and shrink while still covering the channel
/// extremes and mid-range the generator for Property 73 calls for.
[[nodiscard]] GenImage genImage(int maxExtent) {
    const auto width = static_cast<std::uint32_t>(*rc::gen::inRange(1, maxExtent + 1));
    const auto height = static_cast<std::uint32_t>(*rc::gen::inRange(1, maxExtent + 1));

    std::vector<Quad> palette{
        Quad{0, 0, 0, 0},          // channel minimum everywhere
        Quad{255, 255, 255, 255},  // channel maximum everywhere
        Quad{128, 127, 129, 128},  // mid-range everywhere
        Quad{0, 255, 128, 64},     // mixed extremes / mid-range
        Quad{255, 0, 127, 191},
    };
    const auto generated = *rc::gen::container<std::vector<std::uint8_t>>(
        16, rc::gen::arbitrary<std::uint8_t>());
    for (std::size_t i = 0; i < 4; ++i) {
        palette.push_back(Quad{generated[i * 4 + 0], generated[i * 4 + 1],
                               generated[i * 4 + 2], generated[i * 4 + 3]});
    }
    const auto offset =
        static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(palette.size())));

    GenImage image;
    image.width = width;
    image.height = height;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    image.rgba.resize(pixels * 4u);
    for (std::size_t p = 0; p < pixels; ++p) {
        const Quad& q = palette[(p + offset) % palette.size()];
        for (std::size_t c = 0; c < 4; ++c) image.rgba[p * 4u + c] = q[c];
    }
    return image;
}

/// An invert-colors effect. The effect is parameterless by definition, so "any
/// invert-colors effect parameters" (Property 74) means any *stray* parameter
/// map: the generator sometimes attaches ignored parameters.
[[nodiscard]] Effect genInvertEffect() {
    Effect effect = Effect::invertColors();
    const int extras = *rc::gen::inRange(0, 3);
    static constexpr const char* kNames[] = {"amount", "radius", "saturation"};
    for (int i = 0; i < extras; ++i) {
        effect.parameters[kNames[i]] =
            static_cast<double>(*rc::gen::inRange(-1000, 1001)) / 1000.0;
    }
    return effect;
}

/// A single-video-track project whose one clip spans `frameCount` frames at
/// 30 fps, carries `effect`, and is composited at `opacity` onto `canvas`.
[[nodiscard]] Project makeProject(Resolution canvas, double opacity, int frameCount,
                                  const Effect& effect) {
    Clip clip;
    clip.id = Uuid::generateV4();
    clip.timelineStart = Duration::zero();
    clip.sourceIn = Duration::zero();
    clip.sourceOut = FrameRate::fps30().durationForFrames(frameCount);
    clip.opacity = opacity;
    clip.effects = {effect};

    Track track;
    track.id = Uuid::generateV4();
    track.kind = TrackKind::Video;
    track.clips = {clip};

    Project project;
    project.id = Uuid::generateV4();
    project.name = "invert-colors-parity";
    project.timelineFps = FrameRate::fps30();
    project.canvas = canvas;
    project.tracks = {track};
    return project;
}

/// Frame provider handing every visible clip the same generated source image.
[[nodiscard]] gpu::ClipFrameProvider provider(const GenImage& source) {
    return [&source](const Clip&, Duration) -> Result<gpu::SourceFrame> {
        gpu::SourceFrame frame;
        frame.width = source.width;
        frame.height = source.height;
        frame.rgba = source.rgba;
        return ok(std::move(frame));
    };
}

/// Copy the host-memory RGBA8 pixels of a composited frame out of a buffer.
[[nodiscard]] std::vector<std::uint8_t> copyPixels(const void* hostData, std::uint32_t width,
                                                   std::uint32_t height) {
    const auto* bytes = static_cast<const std::uint8_t*>(hostData);
    const std::size_t n = static_cast<std::size_t>(width) * height * 4u;
    if (bytes == nullptr) return {};
    return std::vector<std::uint8_t>(bytes, bytes + n);
}

/// Largest absolute per-channel difference between two equal-sized buffers.
[[nodiscard]] int maxChannelDiff(const std::vector<std::uint8_t>& a,
                                 const std::vector<std::uint8_t>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

// --- Recording encode backend (mirrors tests/media_export_engine_test.cpp) ---

/// Every frame the export lane submitted, as host-memory RGBA8 pixels.
struct ExportCapture {
    std::vector<std::vector<std::uint8_t>> frames{};
};

class RecordingEncodeBackend final : public media::IEncodeBackend {
public:
    explicit RecordingEncodeBackend(ExportCapture* capture) : capture_(capture) {}

    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        capture_->frames.push_back(
            copyPixels(frame.hostData, frame.desc.width, frame.desc.height));
        return ok();
    }

    [[nodiscard]] Result<void> finish() override { return ok(); }

private:
    ExportCapture* capture_;
};

[[nodiscard]] media::EncodeBackendFactory recordingFactory(ExportCapture* capture) {
    return [capture](const media::EncodeSpec&, const gpu::CodecRoute&)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        return Result<std::unique_ptr<media::IEncodeBackend>>(
            std::unique_ptr<media::IEncodeBackend>(
                std::make_unique<RecordingEncodeBackend>(capture)));
    };
}

// ===========================================================================
// Property 73
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 73: Invert-colors channel
// arithmetic — for any RGBA image, applying the invert-colors effect produces,
// for each pixel, red, green and blue equal to 255 minus the input value of that
// channel, and an alpha value equal to the input alpha.
// Validates: Requirements 14.4
RC_GTEST_PROP(InvertColorsProperties, ChannelArithmeticInvertsRgbAndPreservesAlpha, ()) {
    const GenImage image = genImage(256);

    std::vector<std::uint8_t> out = image.rgba;
    gpu::applyEffectSoftware(genInvertEffect(), out.data(), image.width, image.height);

    RC_ASSERT(out.size() == image.rgba.size());
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        RC_ASSERT(out[i + 0] == static_cast<std::uint8_t>(255 - image.rgba[i + 0]));
        RC_ASSERT(out[i + 1] == static_cast<std::uint8_t>(255 - image.rgba[i + 1]));
        RC_ASSERT(out[i + 2] == static_cast<std::uint8_t>(255 - image.rgba[i + 2]));
        RC_ASSERT(out[i + 3] == image.rgba[i + 3]);
    }
}

// ===========================================================================
// Property 74
// ===========================================================================

// Feature: end-to-end-editor-integration, Property 74: Invert-colors agrees
// between playback and export — for any source frame and any invert-colors
// effect parameters, the value produced by the playback compositing path and the
// value produced by the export compositing path differ by at most 1 of 255 per
// channel.
// Validates: Requirements 14.5
RC_GTEST_PROP(InvertColorsProperties, PlaybackAndExportAgreeWithinOneOf255, ()) {
    // Source frame as Property 73 (bounded to 32 px so a whole export run stays
    // cheap), x canvas sizes, x clip opacities. Canvas extents are even: the
    // export policy only accepts even output dimensions (4:2:0 chroma).
    const GenImage source = genImage(32);
    const Resolution canvas{static_cast<std::uint32_t>(*rc::gen::inRange(1, 17)) * 2u,
                            static_cast<std::uint32_t>(*rc::gen::inRange(1, 17)) * 2u};
    const double opacity = static_cast<double>(*rc::gen::inRange(0, 1001)) / 1000.0;
    const int frameCount = *rc::gen::inRange(1, 4);
    const int frameIndex = *rc::gen::inRange(0, frameCount);
    const Duration position = FrameRate::fps30().durationForFrames(frameIndex);

    const Project project = makeProject(canvas, opacity, frameCount, genInvertEffect());

    // --- Playback lane: PreviewController -> Compositor::renderAt ----------
    gpu::GpuContext playbackContext = gpu::GpuContext::softwareFallback();
    gpu::Compositor playbackCompositor(playbackContext);
    playbackCompositor.setFrameProvider(provider(source));

    ui::PreviewOptions options;
    options.clearColor = gpu::RgbaColor::opaqueBlack(); // matches the export canvas.
    ui::PreviewController controller(playbackCompositor, playbackContext,
                                     [&project]() { return project; }, options);

    std::vector<std::uint8_t> playbackPixels;
    controller.setFrameSink([&playbackPixels](const gpu::RenderedFrame& frame, ui::RenderPath) {
        playbackPixels = copyPixels(frame.hostData(), frame.width(), frame.height());
    });
    controller.seek(position);
    RC_ASSERT(controller.renderFrame().isOk());
    RC_ASSERT(!playbackPixels.empty());

    // --- Export lane: ExportEngine::run -> Compositor::renderAt -> encoder --
    gpu::GpuContext exportContext = gpu::GpuContext::softwareFallback();
    gpu::Compositor exportCompositor(exportContext);
    exportCompositor.setFrameProvider(provider(source));

    media::ExportRequest request;
    request.codec = gpu::CodecId::H264;
    request.resolution = canvas;
    request.frameRate = FrameRate::fps30();
    request.preferHardware = false;
    request.outputPath = "invert-colors-parity.mp4";
    request.containerFormat = "mp4";
    request.clearColor = gpu::RgbaColor::opaqueBlack();

    ExportCapture capture;
    media::ExportEngine engine(exportCompositor);
    const Result<media::ExportResult> exported =
        engine.run(project, request, recordingFactory(&capture));
    RC_ASSERT(exported.isOk());
    RC_ASSERT(capture.frames.size() == static_cast<std::size_t>(frameCount));

    const std::vector<std::uint8_t>& exportPixels = capture.frames[frameIndex];
    RC_ASSERT(exportPixels.size() == playbackPixels.size());
    RC_ASSERT(maxChannelDiff(playbackPixels, exportPixels) <= kPathTolerance);
}

} // namespace
} // namespace palmier
