// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/Compositor.cpp — software reference implementation of the compositing
// render graph (design.md "Compositing render pass"; Requirements 10.2, 10.7).
//
// The compositing math here is vendor-neutral and host-memory only, so it builds
// and runs with no Vulkan loader or GPU (matching the guard pattern used by
// GpuContext/FramePool). It is the reference the GPU path is validated against
// (property P5: GPU vs software parity). The Vulkan compute-shader compositing
// path — dispatching the registered SPIR-V effect kernels and blending on-GPU —
// is compiled under PALMIER_HAVE_VULKAN and added with the kernels in task 7.4;
// registerEffect() already records those modules so the surface is stable.

#include "gpu/Compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "core/Error.hpp"
#include "gpu/GpuContext.hpp"

namespace palmier::gpu {
namespace {

/// Clamp a floating value to the 0..255 byte range and round to nearest.
[[nodiscard]] std::uint8_t toByte(double v) noexcept {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(v + 0.5);
}

/// The clip covering `position` on a track, or nullptr if none does. A clip
/// covers `position` when timelineStart <= position < timelineEnd (the end is
/// exclusive so adjacent clips do not both claim the boundary frame).
[[nodiscard]] const Clip* clipAt(const Track& track, Duration position) noexcept {
    for (const Clip& clip : track.clips) {
        if (position >= clip.timelineStart && position < clip.timelineEnd()) {
            return &clip;
        }
    }
    return nullptr;
}

/// Alpha-composite `src` "over" the RGBA8 `target` (tw x th), scaling the
/// source's coverage by `opacity`. Straight (non-premultiplied) alpha; the
/// source is aligned to the target's top-left and clipped to the overlap
/// (scaling/transform is an effect-kernel concern, task 7.4). This is the step
/// that preserves the render-pass loop invariant: it blends one more layer onto
/// the accumulated lower-z result without disturbing pixels the layer does not
/// cover.
void blendOver(std::uint8_t* target, std::uint32_t tw, std::uint32_t th,
               const SourceFrame& src, double opacity) noexcept {
    const double op = std::clamp(opacity, 0.0, 1.0);
    const std::uint32_t w = std::min(tw, src.width);
    const std::uint32_t h = std::min(th, src.height);

    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t s = (static_cast<std::size_t>(y) * src.width + x) * 4u;
            const std::size_t d = (static_cast<std::size_t>(y) * tw + x) * 4u;

            const double sa = (static_cast<double>(src.rgba[s + 3]) / 255.0) * op; // effective src alpha
            const double da = static_cast<double>(target[d + 3]) / 255.0;
            const double inv = 1.0 - sa;

            for (int c = 0; c < 3; ++c) {
                const double sc = static_cast<double>(src.rgba[s + c]);
                const double dc = static_cast<double>(target[d + c]);
                target[d + c] = toByte(sc * sa + dc * inv);
            }
            const double outA = sa + da * inv;
            target[d + 3] = toByte(outA * 255.0);
        }
    }
}

/// Fill an RGBA8 buffer with a single color.
void fillColor(std::uint8_t* buf, std::size_t pixelCount, RgbaColor c) noexcept {
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t o = i * 4u;
        buf[o + 0] = c.r;
        buf[o + 1] = c.g;
        buf[o + 2] = c.b;
        buf[o + 3] = c.a;
    }
}

/// Look up a named scalar parameter on an effect, or return `fallback`.
[[nodiscard]] double paramOr(const Effect& effect, const char* key, double fallback) noexcept {
    const auto it = effect.parameters.find(key);
    return it == effect.parameters.end() ? fallback : it->second;
}

// --- Individual software effect kernels (the P5 references) ----------------
//
// Each mirrors the GLSL compute kernel of the same name in EffectKernels.cpp
// bit-for-bit (same channel order, same clamp/round, same edge handling) so a
// device and this CPU path agree within property P5's per-channel tolerance.

/// Brightness: shift each RGB channel by amount*255 (alpha preserved).
void applyBrightness(std::uint8_t* rgba, std::size_t pixels, double amount) noexcept {
    const double shift = amount * 255.0;
    if (shift == 0.0) return;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        for (int c = 0; c < 3; ++c) {
            rgba[o + c] = toByte(static_cast<double>(rgba[o + c]) + shift);
        }
    }
}

/// Contrast: scale each RGB channel about mid-gray (128) by (1 + amount).
void applyContrast(std::uint8_t* rgba, std::size_t pixels, double amount) noexcept {
    const double factor = 1.0 + amount;
    if (factor == 1.0) return;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        for (int c = 0; c < 3; ++c) {
            rgba[o + c] = toByte((static_cast<double>(rgba[o + c]) - 128.0) * factor + 128.0);
        }
    }
}

/// Box blur of RGB over a (2r+1)x(2r+1) window with edge-clamped sampling; alpha
/// is preserved. r = round(radius), clamped to [0, kMaxBlurRadius]. The window
/// pixel count varies at the borders (edge clamp), and both this and the GLSL
/// kernel compute the identical count/sum, so their rounding matches.
///
/// noexcept: the temporary source copy could throw on OOM; we catch and leave
/// the image untouched rather than terminate (blur is a best-effort filter).
void applyBlur(std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
               double radius) noexcept {
    constexpr int kMaxBlurRadius = 128;
    int r = static_cast<int>(radius + (radius >= 0 ? 0.5 : -0.5));
    if (r <= 0) return;
    if (r > kMaxBlurRadius) r = kMaxBlurRadius;

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);

    try {
        // Read from an immutable copy so the window average is not contaminated
        // by already-written output pixels.
        const std::vector<std::uint8_t> src(rgba, rgba + static_cast<std::size_t>(w) * h * 4u);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, x - r);
                const int x1 = std::min(w - 1, x + r);
                const int y0 = std::max(0, y - r);
                const int y1 = std::min(h - 1, y + r);
                const int count = (x1 - x0 + 1) * (y1 - y0 + 1);

                long sum[3] = {0, 0, 0};
                for (int sy = y0; sy <= y1; ++sy) {
                    for (int sx = x0; sx <= x1; ++sx) {
                        const std::size_t s = (static_cast<std::size_t>(sy) * w + sx) * 4u;
                        sum[0] += src[s + 0];
                        sum[1] += src[s + 1];
                        sum[2] += src[s + 2];
                    }
                }
                const std::size_t d = (static_cast<std::size_t>(y) * w + x) * 4u;
                for (int c = 0; c < 3; ++c) {
                    rgba[d + c] = toByte(static_cast<double>(sum[c]) / count);
                }
                // alpha (d+3) preserved.
            }
        }
    } catch (...) {
        // Allocation failed; leave the image unchanged (no-op blur).
    }
}

/// Crop to a normalized [0,1] visible rectangle; pixels outside become
/// transparent black. Bounds are half-open [x0,x1) x [y0,y1) in pixels, computed
/// by rounding the normalized edges to pixel coordinates. A (0,0,1,1) rect is a
/// no-op. The "transform" portion (scale/reposition) of a clip is applied by the
/// compositor's blend stage; this kernel realizes the crop.
void applyCropTransform(std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                        double cropLeft, double cropTop,
                        double cropRight, double cropBottom) noexcept {
    const double l = std::clamp(cropLeft, 0.0, 1.0);
    const double t = std::clamp(cropTop, 0.0, 1.0);
    const double rgt = std::clamp(cropRight, 0.0, 1.0);
    const double b = std::clamp(cropBottom, 0.0, 1.0);

    const auto toPx = [](double norm, std::uint32_t extent) -> std::uint32_t {
        double v = norm * static_cast<double>(extent) + 0.5; // round to nearest
        if (v <= 0.0) return 0;
        if (v >= static_cast<double>(extent)) return extent;
        return static_cast<std::uint32_t>(v);
    };

    const std::uint32_t x0 = toPx(l, width);
    const std::uint32_t x1 = toPx(rgt, width);
    const std::uint32_t y0 = toPx(t, height);
    const std::uint32_t y1 = toPx(b, height);

    // Full-frame visible rect: nothing to clear.
    if (x0 == 0 && y0 == 0 && x1 >= width && y1 >= height) return;

    for (std::uint32_t y = 0; y < height; ++y) {
        const bool rowInside = (y >= y0 && y < y1);
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool inside = rowInside && (x >= x0 && x < x1);
            if (inside) continue;
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4u;
            rgba[o + 0] = 0;
            rgba[o + 1] = 0;
            rgba[o + 2] = 0;
            rgba[o + 3] = 0; // transparent
        }
    }
}

/// Color grade: per-channel gain, an additive lift (lift*255), and a saturation
/// mix toward Rec.601 luma. Order: gain -> lift -> saturation. Alpha preserved.
void applyColorGrade(std::uint8_t* rgba, std::size_t pixels,
                     double gainR, double gainG, double gainB,
                     double lift, double saturation) noexcept {
    const double liftShift = lift * 255.0;
    const bool identity = gainR == 1.0 && gainG == 1.0 && gainB == 1.0 &&
                          liftShift == 0.0 && saturation == 1.0;
    if (identity) return;

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        double r = static_cast<double>(rgba[o + 0]) * gainR + liftShift;
        double g = static_cast<double>(rgba[o + 1]) * gainG + liftShift;
        double b = static_cast<double>(rgba[o + 2]) * gainB + liftShift;

        // Rec.601 luma; mix each channel toward gray by (1 - saturation).
        const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * saturation;
        g = luma + (g - luma) * saturation;
        b = luma + (b - luma) * saturation;

        rgba[o + 0] = toByte(r);
        rgba[o + 1] = toByte(g);
        rgba[o + 2] = toByte(b);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Default software effect application (P5 reference).
// ---------------------------------------------------------------------------
void applyEffectSoftware(const Effect& effect, std::uint8_t* rgba,
                         std::uint32_t width, std::uint32_t height) noexcept {
    if (rgba == nullptr || width == 0 || height == 0) return;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;

    switch (effect.type) {
        case EffectType::Brightness:
            applyBrightness(rgba, pixels, paramOr(effect, "amount", 0.0));
            break;
        case EffectType::Contrast:
            applyContrast(rgba, pixels, paramOr(effect, "amount", 0.0));
            break;
        case EffectType::Blur:
            applyBlur(rgba, width, height, paramOr(effect, "radius", 0.0));
            break;
        case EffectType::CropTransform:
            applyCropTransform(rgba, width, height,
                               paramOr(effect, "cropLeft", 0.0),
                               paramOr(effect, "cropTop", 0.0),
                               paramOr(effect, "cropRight", 1.0),
                               paramOr(effect, "cropBottom", 1.0));
            break;
        case EffectType::ColorGrade:
            applyColorGrade(rgba, pixels,
                            paramOr(effect, "gainR", 1.0),
                            paramOr(effect, "gainG", 1.0),
                            paramOr(effect, "gainB", 1.0),
                            paramOr(effect, "lift", 0.0),
                            paramOr(effect, "saturation", 1.0));
            break;
        case EffectType::Custom:
            // A Custom effect's behavior comes entirely from a caller-registered
            // kernel/hook; there is no built-in reference transform for it.
            break;
    }
}

void applyTransitionSoftware(const std::uint8_t* a, const std::uint8_t* b,
                             std::uint8_t* out, std::uint32_t width,
                             std::uint32_t height, double progress) noexcept {
    if (a == nullptr || b == nullptr || out == nullptr || width == 0 || height == 0) return;
    const double p = std::clamp(progress, 0.0, 1.0);
    const double inv = 1.0 - p;
    const std::size_t channels = static_cast<std::size_t>(width) * height * 4u;
    for (std::size_t i = 0; i < channels; ++i) {
        out[i] = toByte(static_cast<double>(a[i]) * inv + static_cast<double>(b[i]) * p);
    }
}

// ---------------------------------------------------------------------------
// Compositor.
// ---------------------------------------------------------------------------
Compositor::Compositor(GpuContext& context)
    : context_(context), pool_(context.framePool()), effectFn_(&applyEffectSoftware) {}

void Compositor::registerEffect(EffectId id, SpirvModule kernel) {
    effects_[id] = std::move(kernel);
}

bool Compositor::isEffectRegistered(EffectId id) const noexcept {
    return effects_.find(id) != effects_.end();
}

void Compositor::setSoftwareEffectFn(SoftwareEffectFn fn) {
    effectFn_ = fn ? std::move(fn) : SoftwareEffectFn(&applyEffectSoftware);
}

std::vector<Compositor::VisibleLayer> Compositor::gatherVisibleClips(const Project& project,
                                                                     Duration position) {
    std::vector<VisibleLayer> visible;
    // z = the track's index in Project.tracks (design: "sort ascending by z,
    // bottom track drawn first"). Only non-muted VIDEO tracks contribute.
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        const Track& track = project.tracks[i];
        if (track.kind != TrackKind::Video || track.muted) continue;
        if (const Clip* clip = clipAt(track, position)) {
            visible.push_back(VisibleLayer{clip, i});
        }
    }
    // Sort ascending by z. Iteration above already yields ascending z, but the
    // design calls for an explicit sort, and it keeps the contract robust if the
    // gathering order ever changes.
    std::stable_sort(visible.begin(), visible.end(),
                     [](const VisibleLayer& a, const VisibleLayer& b) { return a.z < b.z; });
    return visible;
}

Result<RenderedFrame> Compositor::renderAt(const Project& project, Duration position,
                                           const RenderTarget& target) {
    if (target.width == 0 || target.height == 0) {
        return err<RenderedFrame>(
            invalidArgument("Compositor::renderAt: render target has zero dimensions"));
    }
    if (target.format != FrameFormat::RGBA8) {
        return err<RenderedFrame>(unsupported(
            "Compositor::renderAt: only RGBA8 render targets are supported"));
    }

    const std::vector<VisibleLayer> layers = gatherVisibleClips(project, position);

    if (!layers.empty() && !provider_) {
        return err<RenderedFrame>(failedPrecondition(
            "Compositor::renderAt: visible clips present but no frame provider is installed"));
    }

    // Acquire the output frame from the pool (bounded by VRAM / host budget).
    Result<FrameLease> leaseResult = pool_.acquire(target.frameDesc());
    if (leaseResult.isError()) {
        return err<RenderedFrame>(std::move(leaseResult).error());
    }
    FrameLease lease = std::move(leaseResult).value();

    auto* out = static_cast<std::uint8_t*>(lease.frame().hostData());
    if (out == nullptr) {
        return err<RenderedFrame>(makeError(ErrorCode::Internal,
            "Compositor::renderAt: pooled frame has no host backing for software compositing"));
    }

    const std::size_t pixels = static_cast<std::size_t>(target.width) * target.height;

    // Clear the canvas (design "clearTarget"): establishes the k=0 base case of
    // the loop invariant — the target holds the blend of zero clips.
    fillColor(out, pixels, target.clearColor);

    // Painter's order: composite each visible clip onto the accumulated result.
    for (const VisibleLayer& layer : layers) {
        Result<SourceFrame> srcResult = provider_(*layer.clip, position);
        if (srcResult.isError()) {
            // Propagate; the lease releases the (discarded) frame on scope exit,
            // so no partially-composited frame escapes.
            return err<RenderedFrame>(std::move(srcResult).error());
        }
        SourceFrame frame = std::move(srcResult).value();
        if (!frame.valid()) {
            return err<RenderedFrame>(makeError(ErrorCode::Internal,
                "Compositor::renderAt: frame provider returned an invalid source frame"));
        }

        // Apply the clip's per-clip effects (design "dispatchEffectKernel"),
        // here via the software reference hook.
        for (const Effect& effect : layer.clip->effects) {
            effectFn_(effect, frame.rgba.data(), frame.width, frame.height);
        }

        // Alpha-composite onto the target scaled by opacity; preserves the
        // loop invariant for the next layer.
        blendOver(out, target.width, target.height, frame, layer.clip->opacity);
    }

    return RenderedFrame{std::move(lease), position, layers.size()};
}

} // namespace palmier::gpu
