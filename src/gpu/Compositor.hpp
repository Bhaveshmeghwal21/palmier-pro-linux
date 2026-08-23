// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/Compositor.hpp — the compositing render graph (design.md "Component 4:
// GPU Abstraction Layer (Vulkan)", the "Compositing render pass" pseudocode,
// and correctness property P5). Implements Requirements 10.2 and 10.7.
//
// The Compositor turns a Project + a timeline position into a single output
// frame by compositing every visible video clip in painter's order:
//
//   1. Gather the visible video clips at `position`: for each non-muted VIDEO
//      track, the clip (if any) whose [timelineStart, timelineEnd) span covers
//      `position`. Each carries a z-order equal to its track's index in
//      Project.tracks (design: "z = track.index").
//   2. Sort the layers ascending by z, so the bottom track is drawn first.
//   3. Clear the render target to its clear color (a transparent/black canvas).
//   4. For each layer, in ascending-z order: fetch the clip's source frame,
//      apply its per-clip effects, then alpha-composite the result onto the
//      target scaled by the clip's opacity.
//
//   Loop invariant (design + property text): after processing the first k
//   visible clips (sorted by z), the target holds exactly the alpha-composited
//   result of those k clips in painter's order. blendOver() preserves it: each
//   step composites layer k+1 "over" the accumulated lower-z result.
//
//   Text layers (usable-editor task 12; Requirement 9): a TEXT track's clips
//   are gathered and blended by the identical z-ordered loop, merged with the
//   video layers into one combined painter's-order sequence, so a title sits
//   above or below any video track purely by its position in Project.tracks —
//   no separate compositing pass exists for text. The one difference is how a
//   layer's pixels are produced: fetched from a ClipFrameProvider for a video
//   clip, rasterized from a TextRasterizer for a text clip (see
//   TextRasterizer's own doc comment for why that is a second, independent
//   injectable seam rather than a code path inside this module).
//
// Guarded build (mirrors GpuContext / FramePool): the compositing math is a
// vendor-neutral, host-memory RGBA8 reference implementation that runs with no
// Vulkan loader or GPU (e.g. CI/sandbox). It is *the* reference the GPU path is
// validated against (property P5: GPU vs software parity, Req 10.7). The Vulkan
// compute-shader compositing path is compiled only under PALMIER_HAVE_VULKAN and
// dispatches the SPIR-V effect kernels (task 7.4, see EffectKernels.hpp);
// registerEffect() stores those kernels. The GLSL sources for those kernels
// mirror the applyEffectSoftware math below, which is the parity reference. The
// SPIR-V bytecode itself lives in EffectKernels.{hpp,cpp}, not this file.
//
// Frame acquisition: source pixels for a clip come from an injectable
// ClipFrameProvider (the future MediaDecoder, task 8.2, fulfills this). Keeping
// it a seam lets the compositor — and its tests — run headlessly with synthetic
// source frames, exactly as the design's render-pass "decodeFrameForClip" step
// is abstracted away from the compositing logic.

#ifndef PALMIER_GPU_COMPOSITOR_HPP
#define PALMIER_GPU_COMPOSITOR_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Project.hpp"
#include "core/Result.hpp"
#include "core/TextStyle.hpp"
#include "gpu/FramePool.hpp"

namespace palmier::gpu {

class GpuContext;

/// Identity under which an effect kernel is registered with the Compositor.
///
/// The design's registerEffect(EffectId, SpirvModule) keys kernels by effect
/// kind; the core Effect model already enumerates exactly those kinds, so the
/// GPU layer reuses it rather than duplicating the taxonomy. A clip's effect is
/// routed to the kernel registered for its EffectType.
using EffectId = EffectType;

/// A compiled SPIR-V compute kernel for an effect (design "Effects as SPIR-V
/// compute kernels"). This is only the *container*: the actual kernel bytecode
/// is produced by task 7.4. `code` is the SPIR-V word stream; `entryPoint` names
/// the shader entry function.
struct SpirvModule {
    std::vector<std::uint32_t> code{};
    std::string                entryPoint{"main"};

    /// A module is usable once it carries at least one word of SPIR-V.
    [[nodiscard]] bool valid() const noexcept { return !code.empty(); }
};

/// A straight 8-bit-per-channel RGBA color (the compositor's working format).
struct RgbaColor {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t a{0};

    [[nodiscard]] static constexpr RgbaColor transparent() noexcept { return {0, 0, 0, 0}; }
    [[nodiscard]] static constexpr RgbaColor opaqueBlack() noexcept { return {0, 0, 0, 255}; }

    [[nodiscard]] friend constexpr bool operator==(RgbaColor, RgbaColor) = default;
};

/// The output surface renderAt() composites into. `format` is currently RGBA8
/// (the compositor's working format); other formats are rejected as Unsupported
/// until the GPU path adds them. The canvas is initialized to `clearColor`
/// before any clip is drawn.
struct RenderTarget {
    std::uint32_t width{0};
    std::uint32_t height{0};
    FrameFormat   format{FrameFormat::RGBA8};
    RgbaColor     clearColor{RgbaColor::transparent()};

    RenderTarget() = default;
    RenderTarget(std::uint32_t w, std::uint32_t h,
                 RgbaColor clear = RgbaColor::transparent()) noexcept
        : width(w), height(h), clearColor(clear) {}

    /// Build a target matching a Project canvas resolution.
    [[nodiscard]] static RenderTarget forCanvas(const Resolution& canvas,
                                                RgbaColor clear = RgbaColor::transparent()) noexcept {
        return RenderTarget{canvas.width, canvas.height, clear};
    }

    [[nodiscard]] FrameDesc frameDesc() const noexcept {
        return FrameDesc{width, height, format};
    }
};

/// Host-memory RGBA8 pixels for one clip at a timeline position — what a
/// ClipFrameProvider yields. `rgba` is row-major, tightly packed,
/// width*height*4 bytes. This is the compositor's decoded-frame stand-in that
/// keeps the compositing logic independent of the (later) media decoder.
struct SourceFrame {
    std::uint32_t             width{0};
    std::uint32_t             height{0};
    std::vector<std::uint8_t> rgba{};

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4u;
    }

    /// A `w` x `h` frame filled with a single color (handy for tests / fills).
    [[nodiscard]] static SourceFrame solid(std::uint32_t w, std::uint32_t h, RgbaColor c) {
        SourceFrame f;
        f.width = w;
        f.height = h;
        f.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
        for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
            f.rgba[i + 0] = c.r;
            f.rgba[i + 1] = c.g;
            f.rgba[i + 2] = c.b;
            f.rgba[i + 3] = c.a;
        }
        return f;
    }
};

/// Supplies the decoded source pixels for a clip at a timeline position. The
/// media decoder (task 8.2) provides the production implementation; tests inject
/// synthetic frames. Returning an error aborts the whole render (the compositor
/// never emits a partially-composited frame).
using ClipFrameProvider = std::function<Result<SourceFrame>(const Clip&, Duration)>;

/// Rasterizes a text clip's styled content into an RGBA8 buffer of exactly
/// `width` x `height` pixels (usable-editor task 12; Requirement 9). Glyph
/// shaping and rendering is Qt's — Qt is the one text-rendering technology
/// already in this tree (QPainter / QRawFont) and this module builds and tests
/// with no Vulkan loader, no GPU and, deliberately, no Qt dependency (see the
/// file header and gpu/CMakeLists.txt), so the rasterizer itself cannot live
/// here. Precisely like ClipFrameProvider, this is an injectable seam: the
/// Composition_Root (src/app, which does link Qt when the UI is built) installs
/// the production implementation via setTextRasterizer(), the exact same way it
/// installs the decoder-backed ClipFrameProvider; tests inject a synthetic
/// rasterizer producing a deterministic filled rectangle, so this module keeps
/// building and testing headlessly with no Qt anywhere in its own translation
/// units. Returning an error aborts the whole render, matching a
/// ClipFrameProvider failure.
using TextRasterizer =
    std::function<Result<SourceFrame>(const TextStyle&, std::uint32_t width, std::uint32_t height)>;

/// Applies one effect to a mutable RGBA8 image in place. This is the *software*
/// effect hook — the reference the GPU SPIR-V kernels (task 7.4) are validated
/// against (property P5). A default implementation (applyEffectSoftware) covers
/// the linear point effects; callers may override it.
using SoftwareEffectFn = std::function<void(const Effect&, std::uint8_t* rgba,
                                            std::uint32_t width, std::uint32_t height)>;

/// The composited output of renderAt(): a GPU-resident (pooled) frame plus its
/// presentation time and the number of clip layers blended into it. Move-only
/// because it owns a FrameLease that returns the frame to the pool on
/// destruction. The MediaEncoder (task 8.3) consumes this via submit().
class RenderedFrame {
public:
    RenderedFrame() = default;
    RenderedFrame(FrameLease lease, Duration presentation, std::size_t layerCount) noexcept
        : lease_(std::move(lease)), presentation_(presentation), layerCount_(layerCount) {}

    RenderedFrame(const RenderedFrame&) = delete;
    RenderedFrame& operator=(const RenderedFrame&) = delete;
    RenderedFrame(RenderedFrame&&) noexcept = default;
    RenderedFrame& operator=(RenderedFrame&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return lease_.valid(); }
    [[nodiscard]] const FrameDesc& desc() const noexcept { return lease_.frame().desc(); }
    [[nodiscard]] Duration presentationTime() const noexcept { return presentation_; }

    /// Number of visible clip layers composited into this frame (0 = the cleared
    /// canvas only, e.g. an empty timeline position).
    [[nodiscard]] std::size_t layerCount() const noexcept { return layerCount_; }

    [[nodiscard]] std::uint32_t width() const noexcept { return desc().width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return desc().height; }

    /// The binding handle downstream GPU stages (encoder) consume.
    [[nodiscard]] const ImageHandle& image() const noexcept { return lease_.frame().image(); }

    /// Host-memory RGBA8 pixels of the composited frame (for the software path,
    /// the encoder's CPU fallback, and tests). May be null on a pure-GPU import.
    [[nodiscard]] const void* hostData() const noexcept { return lease_.frame().hostData(); }
    [[nodiscard]] void* hostData() noexcept { return lease_.frame().hostData(); }

    /// Relinquish the underlying lease (e.g. to hand the frame elsewhere).
    [[nodiscard]] FrameLease takeLease() noexcept { return std::move(lease_); }

private:
    FrameLease  lease_{};
    Duration    presentation_{Duration::zero()};
    std::size_t layerCount_{0};
};

/// Default software effect application (the P5 reference). Handles every effect
/// kind the core model enumerates — Brightness, Contrast, Blur, CropTransform,
/// ColorGrade, and InvertColors — in place on an RGBA8 buffer. These host-memory
/// implementations are *the* reference the SPIR-V compute kernels (see
/// EffectKernels.hpp, task 7.4) must match within tolerance for property P5
/// (GPU/CPU parity, Req 10.7); each GLSL kernel mirrors the math below
/// bit-for-bit so a device and the CPU agree. `Custom` is a pass-through here
/// (its behavior comes entirely from a caller-registered kernel). RGB channels
/// are transformed; alpha is preserved, except CropTransform which clears
/// cropped-out pixels to transparent black.
///
/// Parameter conventions (all optional; documented defaults are no-ops):
///   * Brightness   — `amount` in [-1,1]: shift each RGB channel by amount*255.
///   * Contrast     — `amount` in [-1,1]: scale about mid-gray by (1+amount).
///   * Blur         — `radius` in pixels: clamped box blur over a (2r+1)^2
///                    window (edge-clamped); alpha preserved.
///   * CropTransform— `cropLeft`/`cropTop`/`cropRight`/`cropBottom` normalized
///                    [0,1] visible rect (defaults 0,0,1,1); pixels outside the
///                    rect become transparent black.
///   * ColorGrade   — `gainR`/`gainG`/`gainB` (default 1), `lift` (default 0,
///                    added as lift*255), `saturation` (default 1, mixes each
///                    pixel toward its Rec.601 luma).
///   * InvertColors — no parameters: each RGB channel becomes 255 minus the
///                    input value; alpha is unchanged (Requirements 14.4).
void applyEffectSoftware(const Effect& effect, std::uint8_t* rgba,
                         std::uint32_t width, std::uint32_t height) noexcept;

/// Software reference for a transition (cross-dissolve) between two RGBA8 frames
/// of identical geometry, at `progress` in [0,1]: out = round(a*(1-p) + b*p) per
/// channel including alpha. This is the P5 reference for the transition SPIR-V
/// kernel (EffectKernel::Transition). Transitions blend two inputs and so are
/// not part of applyEffectSoftware (which transforms a single image); the
/// Compositor invokes them where a clip carries a transition. `out` may alias
/// `a` or `b`. All three buffers hold width*height*4 bytes.
void applyTransitionSoftware(const std::uint8_t* a, const std::uint8_t* b,
                             std::uint8_t* out, std::uint32_t width,
                             std::uint32_t height, double progress) noexcept;

/// The compositing render graph (design "Component 4"). Composites all visible
/// video clips at a timeline position into a single RenderedFrame using the
/// GpuContext's FramePool for output memory. The compositing math is the
/// host-memory software reference; the Vulkan compute path (task 7.4) plugs in
/// behind the same interface.
class Compositor {
public:
    /// One visible clip layer gathered at a position: the clip and its z-order
    /// (its track index in Project.tracks; lower z is drawn first / underneath).
    struct VisibleLayer {
        const Clip* clip{nullptr};
        std::size_t z{0};
    };

    /// Construct against a GpuContext; the compositor draws output frames from
    /// the context's FramePool (bounded by the device's VRAM, or a host budget
    /// on the software fallback).
    explicit Compositor(GpuContext& context);

    // --- Effect kernel registration (design registerEffect) ----------------

    /// Register (or replace) the SPIR-V kernel for an effect kind. Consumed by
    /// the Vulkan compositing path (task 7.4); recorded now so the surface is
    /// stable. Registration does not affect the software reference path, which
    /// uses applyEffectSoftware (or an injected SoftwareEffectFn).
    void registerEffect(EffectId id, SpirvModule kernel);

    [[nodiscard]] bool isEffectRegistered(EffectId id) const noexcept;
    [[nodiscard]] std::size_t registeredEffectCount() const noexcept { return effects_.size(); }

    // --- Injection seams ---------------------------------------------------

    /// Install the source-frame provider (the future MediaDecoder). Required
    /// whenever a render position has at least one visible clip.
    void setFrameProvider(ClipFrameProvider provider) { provider_ = std::move(provider); }
    [[nodiscard]] bool hasFrameProvider() const noexcept { return static_cast<bool>(provider_); }

    /// Install the text rasterizer (the Composition_Root's Qt-based
    /// implementation in production). Required whenever a render position has
    /// at least one visible text clip (Requirement 9.3).
    void setTextRasterizer(TextRasterizer rasterizer) { textRasterizer_ = std::move(rasterizer); }
    [[nodiscard]] bool hasTextRasterizer() const noexcept {
        return static_cast<bool>(textRasterizer_);
    }

    /// Override the software effect application hook (defaults to
    /// applyEffectSoftware).
    void setSoftwareEffectFn(SoftwareEffectFn fn);

    // --- Rendering ---------------------------------------------------------

    /// Composite the visible clips at `position` into `target` and return the
    /// resulting frame. Painter's order, opacity-weighted alpha compositing; see
    /// the file header for the algorithm and loop invariant.
    ///
    /// Fails with:
    ///   * InvalidArgument   — degenerate target (zero width/height).
    ///   * Unsupported       — target format other than RGBA8.
    ///   * FailedPrecondition — a position has visible clips but no frame
    ///                          provider is installed.
    ///   * OutOfRange        — the output frame does not fit the FramePool budget.
    ///   * (provider errors)  — propagated unchanged; no partial frame is emitted.
    [[nodiscard]] Result<RenderedFrame> renderAt(const Project& project, Duration position,
                                                 const RenderTarget& target);

    /// Gather the visible video-clip layers at `position` (non-muted VIDEO
    /// tracks only), each tagged with its z = track index, sorted ascending by z
    /// (bottom track first). Exposed for the render pass and for tests that
    /// verify visible-clip gathering and z-ordering directly.
    [[nodiscard]] static std::vector<VisibleLayer> gatherVisibleClips(const Project& project,
                                                                      Duration position);

    /// Gather the visible text-clip layers at `position` (non-muted TEXT
    /// tracks only), each tagged with its z = track index, sorted ascending by z
    /// (Requirement 9.3/9.4 — a title composites in the same painter's-order
    /// layering every video track already uses, by its position in
    /// Project.tracks). A clip on a TEXT track that carries no TextStyle (which
    /// ProjectValidation forbids, but this gathering step is deliberately as
    /// defensive as gatherVisibleClips is about track kind) is skipped rather
    /// than treated as an empty title.
    [[nodiscard]] static std::vector<VisibleLayer> gatherVisibleTextClips(const Project& project,
                                                                          Duration position);

private:
    GpuContext&                    context_;
    FramePool&                     pool_;
    std::map<EffectId, SpirvModule> effects_{};
    ClipFrameProvider              provider_{};
    SoftwareEffectFn               effectFn_{};
    TextRasterizer                  textRasterizer_{};
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_COMPOSITOR_HPP
