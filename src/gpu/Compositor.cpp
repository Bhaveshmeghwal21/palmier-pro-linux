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

#include "core/ToneCurve.hpp"
#include "gpu/CubeLut.hpp"
#include "gpu/LutCache.hpp"

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

/// Bilinear sample at source coordinate (u, v), clamped at the edges. Used when magnifying,
/// where a destination pixel covers less than one source pixel.
void sampleBilinear(const SourceFrame& src, double u, double v, double out[4]) noexcept {
    const auto maxX = static_cast<std::int64_t>(src.width) - 1;
    const auto maxY = static_cast<std::int64_t>(src.height) - 1;
    const std::int64_t x0 =
        std::clamp(static_cast<std::int64_t>(std::floor(u)), std::int64_t{0}, maxX);
    const std::int64_t y0 =
        std::clamp(static_cast<std::int64_t>(std::floor(v)), std::int64_t{0}, maxY);
    const std::int64_t x1 = std::min(x0 + 1, maxX);
    const std::int64_t y1 = std::min(y0 + 1, maxY);
    const double fx = std::clamp(u - static_cast<double>(x0), 0.0, 1.0);
    const double fy = std::clamp(v - static_cast<double>(y0), 0.0, 1.0);

    const std::size_t i00 =
        (static_cast<std::size_t>(y0) * src.width + static_cast<std::size_t>(x0)) * 4u;
    const std::size_t i10 =
        (static_cast<std::size_t>(y0) * src.width + static_cast<std::size_t>(x1)) * 4u;
    const std::size_t i01 =
        (static_cast<std::size_t>(y1) * src.width + static_cast<std::size_t>(x0)) * 4u;
    const std::size_t i11 =
        (static_cast<std::size_t>(y1) * src.width + static_cast<std::size_t>(x1)) * 4u;

    for (int c = 0; c < 4; ++c) {
        const double top = static_cast<double>(src.rgba[i00 + c]) * (1.0 - fx) +
                           static_cast<double>(src.rgba[i10 + c]) * fx;
        const double bot = static_cast<double>(src.rgba[i01 + c]) * (1.0 - fx) +
                           static_cast<double>(src.rgba[i11 + c]) * fx;
        out[c] = top * (1.0 - fy) + bot * fy;
    }
}

/// Average every source pixel covered by one destination pixel, whose footprint in source
/// coordinates is [u0, u1) x [v0, v1). Used when minifying: point-sampling a 4K source down to
/// 1080p would alias badly and discard three quarters of the image.
void sampleBox(const SourceFrame& src, double u0, double u1, double v0, double v1,
               double out[4]) noexcept {
    const auto width = static_cast<std::int64_t>(src.width);
    const auto height = static_cast<std::int64_t>(src.height);
    const std::int64_t x0 =
        std::clamp(static_cast<std::int64_t>(std::floor(u0)), std::int64_t{0}, width - 1);
    const std::int64_t y0 =
        std::clamp(static_cast<std::int64_t>(std::floor(v0)), std::int64_t{0}, height - 1);
    // Ceil the exclusive edge so a footprint of 2.0 pixels covers two, not one, and clamp to
    // at least one pixel past the start so the loops always execute.
    const std::int64_t x1 = std::clamp(static_cast<std::int64_t>(std::ceil(u1)), x0 + 1, width);
    const std::int64_t y1 = std::clamp(static_cast<std::int64_t>(std::ceil(v1)), y0 + 1, height);

    double acc[4] = {0.0, 0.0, 0.0, 0.0};
    std::size_t n = 0;
    for (std::int64_t y = y0; y < y1; ++y) {
        for (std::int64_t x = x0; x < x1; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * src.width + static_cast<std::size_t>(x)) * 4u;
            for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>(src.rgba[i + c]);
            ++n;
        }
    }
    const double inv = n == 0 ? 0.0 : 1.0 / static_cast<double>(n);
    for (int c = 0; c < 4; ++c) out[c] = acc[c] * inv;
}

/// Alpha-composite `src` "over" the RGBA8 `target` (tw x th) according to `placement`.
///
/// The source is fitted to the canvas preserving its aspect ratio and centred, so a source of
/// any resolution contributes its whole image rather than the top-left crop the previous
/// straight 1:1 blit produced. Straight (non-premultiplied) alpha. This is the step that
/// preserves the render-pass loop invariant: it blends one more layer onto the accumulated
/// lower-z result without disturbing pixels the layer does not cover.
///
/// When the source already matches the canvas the fit is the identity and the bilinear sample
/// lands exactly on integer coordinates, so the result is bit-identical to a 1:1 blit.
void blendOver(std::uint8_t* target, std::uint32_t tw, std::uint32_t th, const SourceFrame& src,
               const LayerPlacement& placement) noexcept {
    const double op = std::clamp(placement.opacity, 0.0, 1.0);
    const double reveal = std::clamp(placement.reveal, 0.0, 1.0);
    if (op <= 0.0 || reveal <= 0.0) return;  // contributes nothing to this frame
    if (src.width == 0 || src.height == 0 || tw == 0 || th == 0) return;

    const InverseMap map =
        inverseMapFor(src.width, src.height, tw, th, placement.transform, placement.slideX);
    if (map.isEmpty()) return;

    // Wipe: canvas columns at or beyond this edge are not yet revealed.
    const std::int64_t revealEdge = std::llround(reveal * static_cast<double>(tw));

    const bool minify = map.sourcePerCanvasX > 1.0 || map.sourcePerCanvasY > 1.0;
    const double halfFootprintU = map.sourcePerCanvasX / 2.0;
    const double halfFootprintV = map.sourcePerCanvasY / 2.0;
    const auto srcW = static_cast<double>(src.width);
    const auto srcH = static_cast<double>(src.height);

    for (std::int64_t dy = map.top; dy < map.bottom; ++dy) {
        const double yc = static_cast<double>(dy) + 0.5;

        // The map is affine, so along a row u advances by map.ux and v by map.vx per pixel.
        // Stepping them replaces four multiplies per pixel with two adds, which matters because
        // this is the innermost loop of every frame of every export. Recomputed from scratch each
        // row so the accumulated error cannot grow past one row's width.
        //
        // Edge coordinates, where pixel i spans [i, i+1) and its centre is i+0.5 -- the space
        // sampleBox's bounds are expressed in. sampleBilinear and the coverage test want index
        // coordinates, where integer i IS pixel i's centre, which is half a pixel lower.
        // Conflating the two widens a footprint by a pixel and bleeds neighbouring content into
        // every minified sample.
        const double firstX = static_cast<double>(map.left) + 0.5;
        double uEdge = map.ux * firstX + map.uy * yc + map.uc;
        double vEdge = map.vx * firstX + map.vy * yc + map.vc;

        for (std::int64_t dx = map.left; dx < map.right; ++dx, uEdge += map.ux, vEdge += map.vx) {
            if (dx >= revealEdge) continue;

            const double u = uEdge - 0.5;
            const double v = vEdge - 0.5;

            // Outside the source means this canvas pixel is not covered by the layer. The
            // iterated box is a rectangle, so a rotated picture leaves its corners uncovered
            // and this is what skips them.
            if (u < -0.5 || v < -0.5 || u > srcW - 0.5 || v > srcH - 0.5) continue;

            double c[4];
            if (minify) {
                sampleBox(src, uEdge - halfFootprintU, uEdge + halfFootprintU,
                          vEdge - halfFootprintV, vEdge + halfFootprintV, c);
            } else {
                sampleBilinear(src, u, v, c);
            }

            const std::size_t d =
                (static_cast<std::size_t>(dy) * tw + static_cast<std::size_t>(dx)) * 4u;

            const double sa = (c[3] / 255.0) * op;  // effective src alpha
            const double da = static_cast<double>(target[d + 3]) / 255.0;
            const double inv = 1.0 - sa;

            for (int ch = 0; ch < 3; ++ch) {
                target[d + ch] = toByte(c[ch] * sa + static_cast<double>(target[d + ch]) * inv);
            }
            target[d + 3] = toByte((sa + da * inv) * 255.0);
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

/// Edge-preserving bilateral denoise of RGB over a (2r+1)x(2r+1) window with
/// edge-clamped sampling; alpha is preserved.
///
/// The difference from applyBlur is one term: each neighbour's contribution is weighted by
/// a Gaussian of its LUMA DISTANCE from the centre pixel, so a neighbour across an edge
/// contributes almost nothing while a neighbour differing only by noise contributes almost
/// fully. That is what makes this noise reduction rather than softening.
///
/// Everything is computed in normalized [0,1] because the weight is non-linear: unlike the
/// box average, byte-space and normalized-space arithmetic do NOT agree here, and the GLSL
/// twin works in normalized space. Both sides therefore use the same units, the same Rec.601
/// luma coefficients and the same sigma, so they land within property P5's tolerance.
///
/// noexcept: the immutable source copy could throw on OOM; caught, leaving the image
/// untouched, exactly as blur does.
void applyDenoise(std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                  const DenoiseValues& values) noexcept {
    if (values.isIdentity()) return;

    const int r = values.radius;
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);

    // Range sigma in normalized luma. The 0.25 scale maps the full strength range onto
    // useful filtering: at strength 1 a mid-grey neighbour still contributes to a black
    // centre pixel, and at 0.1 only near-identical pixels do. sigma cannot reach 0 here
    // because isIdentity() already rejected strength <= 0, and even an extremely small
    // sigma is safe: the centre pixel's own weight is exp(0) == 1, so wsum >= 1.
    const double sigma = values.strength * 0.25;
    const double denom = 2.0 * sigma * sigma;

    const auto luma = [](double rr, double gg, double bb) noexcept {
        return 0.299 * rr + 0.587 * gg + 0.114 * bb;
    };

    try {
        // Read from an immutable copy so already-denoised output cannot feed back into
        // later windows -- the same reason applyBlur copies.
        const std::vector<std::uint8_t> src(rgba, rgba + static_cast<std::size_t>(w) * h * 4u);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t centre = (static_cast<std::size_t>(y) * w + x) * 4u;
                const double cr = static_cast<double>(src[centre + 0]) / 255.0;
                const double cg = static_cast<double>(src[centre + 1]) / 255.0;
                const double cb = static_cast<double>(src[centre + 2]) / 255.0;
                const double centreLuma = luma(cr, cg, cb);

                const int x0 = std::max(0, x - r);
                const int x1 = std::min(w - 1, x + r);
                const int y0 = std::max(0, y - r);
                const int y1 = std::min(h - 1, y + r);

                double sum[3] = {0.0, 0.0, 0.0};
                double weightSum = 0.0;
                for (int sy = y0; sy <= y1; ++sy) {
                    for (int sx = x0; sx <= x1; ++sx) {
                        const std::size_t s = (static_cast<std::size_t>(sy) * w + sx) * 4u;
                        const double sr = static_cast<double>(src[s + 0]) / 255.0;
                        const double sg = static_cast<double>(src[s + 1]) / 255.0;
                        const double sb = static_cast<double>(src[s + 2]) / 255.0;
                        const double d = luma(sr, sg, sb) - centreLuma;
                        const double weight = std::exp(-(d * d) / denom);
                        sum[0] += weight * sr;
                        sum[1] += weight * sg;
                        sum[2] += weight * sb;
                        weightSum += weight;
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    rgba[centre + c] = toByte(sum[c] / weightSum * 255.0);
                }
                // alpha (centre+3) preserved.
            }
        }
    } catch (...) {
        // Allocation failed; leave the image unchanged (no-op denoise).
    }
}

/// Crop to a normalized [0,1] visible rectangle; pixels outside become
/// transparent black. Bounds are half-open [x0,x1) x [y0,y1) in pixels, computed
/// by rounding the normalized edges to pixel coordinates. A (0,0,1,1) rect is a
/// no-op. The "transform" portion (scale/reposition) of a clip is applied by the
/// compositor's blend stage; this kernel realizes the crop.
void applyCropTransform(std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                        double cropLeft, double cropTop, double cropRight,
                        double cropBottom) noexcept {
    const double l = std::clamp(cropLeft, 0.0, 1.0);
    const double t = std::clamp(cropTop, 0.0, 1.0);
    const double rgt = std::clamp(cropRight, 0.0, 1.0);
    const double b = std::clamp(cropBottom, 0.0, 1.0);

    const auto toPx = [](double norm, std::uint32_t extent) -> std::uint32_t {
        double v = norm * static_cast<double>(extent) + 0.5;  // round to nearest
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
            rgba[o + 3] = 0;  // transparent
        }
    }
}

/// Invert colors: each 8-bit RGB channel becomes 255 - value; alpha untouched
/// (upstream PR 408; Requirements 14.4). Exact integer arithmetic — no clamping
/// or rounding is needed because 255 - v is representable for every v in
/// [0,255] — so the GLSL kernel's normalized form (1.0 - n, stored back with
/// round-to-nearest) reproduces it bit-for-bit.
void applyInvertColors(std::uint8_t* rgba, std::size_t pixels) noexcept {
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        for (int c = 0; c < 3; ++c) {
            rgba[o + c] = static_cast<std::uint8_t>(255u - rgba[o + c]);
        }
        // alpha (o+3) preserved.
    }
}

/// Color grade: per-channel gain, per-channel additive lift (lift*255 in byte
/// space), per-channel gamma, then a saturation mix toward Rec.601 luma.
///
/// ORDER: gain -> lift -> gamma -> saturation (Requirement 4.5). This mirrors
/// gpu::kColorGradeSrc line for line; see that kernel's comment for why the order is
/// fixed and why the gamma step is guarded rather than computed at an exponent of 1.
///
/// Gamma operates in NORMALISED [0,1] space even though this reference works in
/// bytes, because an exponent means something entirely different applied to 0-255:
/// pow(200, 1/2.2) is 10.6, not a brightened 200. So the gamma step alone
/// normalises, exponentiates and scales back.
///
/// Takes the already-resolved core::ColorGradeValues rather than the effect, so the
/// defaults — including Requirement 4.2's legacy-lift fallback — are stated in
/// exactly one place, which the Inspector reads too.
void applyColorGrade(std::uint8_t* rgba, std::size_t pixels, const ColorGradeValues& p) noexcept {
    if (p.isIdentity()) return;

    const double liftShiftR = p.liftR * 255.0;
    const double liftShiftG = p.liftG * 255.0;
    const double liftShiftB = p.liftB * 255.0;

    // Requirement 4.2: at default gamma this branch is never taken, so the arithmetic
    // below is exactly what it was before gamma existed — including for values that
    // a negative lift or a gain above 1 pushed outside [0,255], which the clamp
    // inside the gamma step would otherwise fold in before saturation reads them.
    const bool applyGamma = !p.isGammaIdentity();
    const double kMinGamma = 1.0 / 1024.0;  // matches the kernel's own floor
    const double invR = 1.0 / std::max(p.gammaR, kMinGamma);
    const double invG = 1.0 / std::max(p.gammaG, kMinGamma);
    const double invB = 1.0 / std::max(p.gammaB, kMinGamma);

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        double r = static_cast<double>(rgba[o + 0]) * p.gainR + liftShiftR;
        double g = static_cast<double>(rgba[o + 1]) * p.gainG + liftShiftG;
        double b = static_cast<double>(rgba[o + 2]) * p.gainB + liftShiftB;

        if (applyGamma) {
            r = std::pow(std::max(r / 255.0, 0.0), invR) * 255.0;
            g = std::pow(std::max(g / 255.0, 0.0), invG) * 255.0;
            b = std::pow(std::max(b / 255.0, 0.0), invB) * 255.0;
        }

        // Rec.601 luma; mix each channel toward gray by (1 - saturation).
        const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * p.saturation;
        g = luma + (g - luma) * p.saturation;
        b = luma + (b - luma) * p.saturation;

        rgba[o + 0] = toByte(r);
        rgba[o + 1] = toByte(g);
        rgba[o + 2] = toByte(b);
    }
}

/// Tone curve (monitoring-and-grading Requirement 5): three 256-entry lookups.
///
/// Mirrors gpu::kToneCurveSrc, but "mirrors" is stronger here than for the other
/// effects: both paths index the SAME table, baked once by core::bakeToneCurve, so
/// they agree exactly rather than within property P5's 1-LSB tolerance. There is no
/// arithmetic left in either path to diverge.
///
/// Clamping (Requirement 5.6) is a property of the table rather than of this loop:
/// every entry was produced by a clamped evaluation, so no index can yield an
/// out-of-range value and wrapping is impossible by construction.
/// Apply a parsed 3D LUT by trilinear interpolation (Requirement 7.6).
///
/// Reads the same table the GPU kernel is handed and performs the same eight-texel,
/// seven-mix blend, so the two agree within property P5's 1-LSB tolerance. The blend is
/// written out on both sides rather than delegated to a hardware sampler, because a
/// sampler's filtering precision is a device detail and the tolerance is not.
///
/// An empty table is a NO-OP rather than a black frame. That is Requirement 7.8's
/// behaviour reaching this far down: a LUT whose file is missing leaves the effect in the
/// chain and renders un-graded, and the way it does so is by carrying no table.
void applyLut(std::uint8_t* rgba, std::size_t pixels, const CubeLut& lut) {
    if (lut.empty()) {
        return;
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        const LutEntry out = lut.sample(static_cast<float>(rgba[i * 4 + 0]) / 255.0f,
                                        static_cast<float>(rgba[i * 4 + 1]) / 255.0f,
                                        static_cast<float>(rgba[i * 4 + 2]) / 255.0f);
        rgba[i * 4 + 0] = toByte(static_cast<double>(out.r) * 255.0);
        rgba[i * 4 + 1] = toByte(static_cast<double>(out.g) * 255.0);
        rgba[i * 4 + 2] = toByte(static_cast<double>(out.b) * 255.0);
        // Alpha untouched: a LUT is a colour transform.
    }
}

void applyToneCurve(std::uint8_t* rgba, std::size_t pixels,
                    const ToneCurveTables& tables) noexcept {
    if (tables.isIdentity()) return;

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        rgba[o + 0] = tables.red[rgba[o + 0]];
        rgba[o + 1] = tables.green[rgba[o + 1]];
        rgba[o + 2] = tables.blue[rgba[o + 2]];
        // alpha (o+3) preserved.
    }
}

/// Secondary grade: exactly ColorGrade's correction, applied in proportion to how strongly
/// an HSL qualifier selects each pixel.
///
/// The gain -> lift -> gamma -> saturation arithmetic below is deliberately the same as
/// applyColorGrade's, written out rather than shared, and the two are held together by a
/// test instead of by a common function: SecondaryGradeMatchesColorGradeWhenTheQualifier
/// SelectsEverything compares them byte for byte. A checked equivalence is stronger than a
/// shared helper, because it also pins the BLEND -- that a fully selected pixel is graded
/// in full, not merely nearly.
///
/// A mask of exactly 1 writes the graded value directly rather than interpolating to it.
/// That is what makes the equivalence exact rather than within a rounding step: computing
/// `src + (graded - src) * 1.0` can land one ULP away from `graded`.
///
/// An unselected pixel is skipped entirely, so a narrow qualifier costs almost nothing
/// beyond the mask itself.
void applySecondaryGrade(std::uint8_t* rgba, std::size_t pixels,
                         const SecondaryGradeValues& v) noexcept {
    if (v.isIdentity()) return;

    const ColorGradeValues& p = v.correction;
    const double liftShiftR = p.liftR * 255.0;
    const double liftShiftG = p.liftG * 255.0;
    const double liftShiftB = p.liftB * 255.0;

    const bool applyGamma = !p.isGammaIdentity();
    const double kMinGamma = 1.0 / 1024.0;  // matches applyColorGrade and the kernel
    const double invR = 1.0 / std::max(p.gammaR, kMinGamma);
    const double invG = 1.0 / std::max(p.gammaG, kMinGamma);
    const double invB = 1.0 / std::max(p.gammaB, kMinGamma);

    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4u;
        const double srcR = static_cast<double>(rgba[o + 0]);
        const double srcG = static_cast<double>(rgba[o + 1]);
        const double srcB = static_cast<double>(rgba[o + 2]);

        // The qualifier reads the UNGRADED pixel: the selection is of the source image,
        // so raising a gain cannot pull further pixels into its own selection.
        const double mask = qualifierMask(srcR / 255.0, srcG / 255.0, srcB / 255.0, v.qualifier);
        if (mask <= 0.0) continue;

        double r = srcR * p.gainR + liftShiftR;
        double g = srcG * p.gainG + liftShiftG;
        double b = srcB * p.gainB + liftShiftB;

        if (applyGamma) {
            r = std::pow(std::max(r / 255.0, 0.0), invR) * 255.0;
            g = std::pow(std::max(g / 255.0, 0.0), invG) * 255.0;
            b = std::pow(std::max(b / 255.0, 0.0), invB) * 255.0;
        }

        const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = luma + (r - luma) * p.saturation;
        g = luma + (g - luma) * p.saturation;
        b = luma + (b - luma) * p.saturation;

        if (mask >= 1.0) {
            rgba[o + 0] = toByte(r);
            rgba[o + 1] = toByte(g);
            rgba[o + 2] = toByte(b);
        } else {
            rgba[o + 0] = toByte(srcR + (r - srcR) * mask);
            rgba[o + 1] = toByte(srcG + (g - srcG) * mask);
            rgba[o + 2] = toByte(srcB + (b - srcB) * mask);
        }
        // alpha (o+3) preserved.
    }
}

/// Requirement 10.3's fixed default burn-in style: white text, bottom-centred,
/// a size proportional to neither font metrics nor canvas — TextRasterizer's
/// own point-size field is an absolute value, so a single documented default
/// (24pt, matching TextStyle's own default) is used regardless of canvas
/// resolution, exactly the same convention a text clip's own unstyled default
/// already carries. Captions carry no styling of their own (Requirement 10
/// asks for none), so nothing here is caller-configurable.
[[nodiscard]] TextStyle captionCueStyle(const std::string& text) {
    TextStyle style;
    style.content = text;
    style.alignment = TextAlignment::Center;
    style.x = 0.5;
    style.y = 0.9;  // bottom-centred, the standard subtitle placement.
    return style;
}

}  // namespace

// ---------------------------------------------------------------------------
// Default software effect application (P5 reference).
// ---------------------------------------------------------------------------
void applyEffectSoftware(const Effect& effect, std::uint8_t* rgba, std::uint32_t width,
                         std::uint32_t height) noexcept {
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
            applyCropTransform(rgba, width, height, paramOr(effect, "cropLeft", 0.0),
                               paramOr(effect, "cropTop", 0.0), paramOr(effect, "cropRight", 1.0),
                               paramOr(effect, "cropBottom", 1.0));
            break;
        case EffectType::InvertColors:
            applyInvertColors(rgba, pixels);
            break;
        case EffectType::ColorGrade:
            // Requirement 4.2 and 4.3 are carried by core::colorGradeValues, which
            // resolves each per-channel lift to the legacy scalar `lift` when the
            // per-channel name is absent — as it is in every project saved before
            // this change. Resolved there rather than here so the Inspector, which
            // must display the same numbers, cannot drift from what is rendered.
            applyColorGrade(rgba, pixels, colorGradeValues(effect));
            break;
        case EffectType::ToneCurve:
            // The interpolation between control points happens inside
            // core::toneCurveTables, once, in double precision — this path and the
            // GPU kernel then both do nothing but index the result.
            applyToneCurve(rgba, pixels, toneCurveTables(effect.parameters));
            break;
        case EffectType::Lut:
            // The table is parsed and cached by the caller (gpu::LutCache) and handed in
            // through the effect's resourcePath, because reading a file is not something
            // a per-pixel loop should do. An unreadable path yields an empty table, which
            // renders un-graded -- Requirement 7.8.
            applyLut(rgba, pixels, lutForEffect(effect));
            break;
        case EffectType::Denoise:
            // Bilateral rather than box: the weighting is what preserves edges, and the
            // defaults are resolved by core::denoiseValues so the Inspector and the GLSL
            // push constants cannot disagree with what renders here.
            applyDenoise(rgba, width, height, denoiseValues(effect));
            break;
        case EffectType::SecondaryGrade:
            // The correction is ColorGrade's; the qualifier decides where it lands. Both
            // are resolved by core::secondaryGradeValues, whose qualifier defaults select
            // the whole frame -- so an unset qualifier narrows nothing rather than
            // selecting nothing.
            applySecondaryGrade(rgba, pixels, secondaryGradeValues(effect));
            break;
        case EffectType::Custom:
            // A Custom effect's behavior comes entirely from a caller-registered
            // kernel/hook; there is no built-in reference transform for it.
            break;
    }
}

void applyTransitionSoftware(const std::uint8_t* a, const std::uint8_t* b, std::uint8_t* out,
                             std::uint32_t width, std::uint32_t height, double progress) noexcept {
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
    //
    // Every clip covering the position contributes, not just the first: inside a transition
    // region two clips on one track legitimately overlap (core/Transition.hpp — "the one place
    // adjacent clips on a track are permitted to overlap"), and the blend needs both the
    // outgoing and the incoming clip. Track.hpp keeps a track's clips sorted by timelineStart,
    // so the outgoing clip is appended first and the stable sort below preserves that, which is
    // what lets layerPlacements identify the outgoing clip as the one drawn immediately before.
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        const Track& track = project.tracks[i];
        if (track.kind != TrackKind::Video || track.muted) continue;
        for (const Clip& clip : track.clips) {
            if (position >= clip.timelineStart && position < clip.timelineEnd()) {
                visible.push_back(VisibleLayer{&clip, i});
            }
        }
    }
    // Sort ascending by z. Iteration above already yields ascending z, but the
    // design calls for an explicit sort, and it keeps the contract robust if the
    // gathering order ever changes.
    std::stable_sort(visible.begin(), visible.end(),
                     [](const VisibleLayer& a, const VisibleLayer& b) { return a.z < b.z; });
    return visible;
}

std::vector<Compositor::VisibleLayer> Compositor::gatherVisibleTextClips(const Project& project,
                                                                         Duration position) {
    std::vector<VisibleLayer> visible;
    // Same z convention as gatherVisibleClips: only non-muted TEXT tracks
    // contribute, tagged with the track's own index so a title composites in
    // the identical painter's-order sequence as every video layer (Requirement
    // 9.4). A text-track clip that somehow carries no TextStyle is skipped
    // (see the header doc comment) rather than rasterized as an empty title.
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        const Track& track = project.tracks[i];
        if (track.kind != TrackKind::Text || track.muted) continue;
        if (const Clip* clip = clipAt(track, position); clip != nullptr && clip->isTextClip()) {
            visible.push_back(VisibleLayer{clip, i});
        }
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [](const VisibleLayer& a, const VisibleLayer& b) { return a.z < b.z; });
    return visible;
}

std::vector<Compositor::VisibleLayer> Compositor::gatherVisibleCaptionCues(const Project& project,
                                                                           Duration position) {
    std::vector<VisibleLayer> visible;
    // Same z convention again (usable-editor task 13; Requirement 10): only
    // non-muted CAPTION tracks contribute. A caption-track clip that somehow
    // carries no captionText is skipped rather than rasterized as an empty cue
    // — the identical defensive posture gatherVisibleTextClips takes for a
    // text-track clip carrying no TextStyle.
    for (std::size_t i = 0; i < project.tracks.size(); ++i) {
        const Track& track = project.tracks[i];
        if (track.kind != TrackKind::Caption || track.muted) continue;
        if (const Clip* clip = clipAt(track, position); clip != nullptr && clip->isCaptionCue()) {
            visible.push_back(VisibleLayer{clip, i});
        }
    }
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
        return err<RenderedFrame>(
            unsupported("Compositor::renderAt: only RGBA8 render targets are supported"));
    }

    // Video, text and caption layers are gathered separately (each has nothing
    // in common with the others' pixel source — a ClipFrameProvider decodes
    // one, a TextRasterizer rasterizes the other two from two different styles)
    // and then merged into one z-ordered painter's-order sequence, so a title
    // or a burned-in caption composites above or below any video track purely
    // by its position in Project.tracks, exactly like today's multi-video-track
    // compositing.
    std::vector<VisibleLayer> layers = gatherVisibleClips(project, position);
    const std::vector<VisibleLayer> textLayers = gatherVisibleTextClips(project, position);
    const std::vector<VisibleLayer> captionLayers = gatherVisibleCaptionCues(project, position);
    layers.insert(layers.end(), textLayers.begin(), textLayers.end());
    layers.insert(layers.end(), captionLayers.begin(), captionLayers.end());
    std::stable_sort(layers.begin(), layers.end(),
                     [](const VisibleLayer& a, const VisibleLayer& b) { return a.z < b.z; });

    const bool anyVideoLayer = std::any_of(layers.begin(), layers.end(), [](const VisibleLayer& l) {
        return !l.clip->isTextClip() && !l.clip->isCaptionCue();
    });
    const bool anyRasterizedLayer = std::any_of(
        layers.begin(), layers.end(),
        [](const VisibleLayer& l) { return l.clip->isTextClip() || l.clip->isCaptionCue(); });
    if (anyVideoLayer && !provider_) {
        return err<RenderedFrame>(failedPrecondition(
            "Compositor::renderAt: visible clips present but no frame provider is installed"));
    }
    if (anyRasterizedLayer && !textRasterizer_) {
        return err<RenderedFrame>(failedPrecondition(
            "Compositor::renderAt: a visible text clip or caption cue is present but no text "
            "rasterizer is installed"));
    }

    // Acquire the output frame from the pool (bounded by VRAM / host budget).
    Result<FrameLease> leaseResult = pool_.acquire(target.frameDesc());
    if (leaseResult.isError()) {
        return err<RenderedFrame>(std::move(leaseResult).error());
    }
    FrameLease lease = std::move(leaseResult).value();

    auto* out = static_cast<std::uint8_t*>(lease.frame().hostData());
    if (out == nullptr) {
        return err<RenderedFrame>(makeError(
            ErrorCode::Internal,
            "Compositor::renderAt: pooled frame has no host backing for software compositing"));
    }

    const std::size_t pixels = static_cast<std::size_t>(target.width) * target.height;

    // Clear the canvas (design "clearTarget"): establishes the k=0 base case of
    // the loop invariant — the target holds the blend of zero clips.
    fillColor(out, pixels, target.clearColor);

    // Per-layer blend weights for this frame. Outside a transition region every placement is
    // the identity carrying only the clip's own opacity, so this is what makes a transition
    // visible at all: before it, transitionIn was stored, persisted and drawn in the timeline
    // panel but read by nothing in the render path.
    const std::vector<LayerPlacement> placements = layerPlacements(layers, position);

    // Painter's order: composite each visible clip onto the accumulated result.
    for (std::size_t li = 0; li < layers.size(); ++li) {
        const VisibleLayer& layer = layers[li];
        SourceFrame frame;
        if (layer.clip->isTextClip() || layer.clip->isCaptionCue()) {
            // A text clip's or caption cue's "source frame" is a full-canvas
            // rasterization of a TextStyle at the target's own resolution
            // (Requirement 9.4/10.3: normalized x/y needs the real canvas size
            // to resolve — rasterizing at the clip's own geometry would need a
            // second, separate placement step this avoids entirely). A caption
            // cue carries no TextStyle of its own (Requirement 10 asks for no
            // per-cue styling), so one is synthesized from its plain text with
            // the fixed default burn-in style — see captionCueStyle() below.
            // Per-clip effects still apply to either kind of layer exactly as
            // they do to a video layer: nothing about applyEffectSoftware
            // assumes decoded video pixels.
            const TextStyle style = layer.clip->isTextClip()
                                        ? *layer.clip->textStyle
                                        : captionCueStyle(*layer.clip->captionText);
            Result<SourceFrame> textResult = textRasterizer_(style, target.width, target.height);
            if (textResult.isError()) {
                return err<RenderedFrame>(std::move(textResult).error());
            }
            frame = std::move(textResult).value();
        } else {
            Result<SourceFrame> srcResult = provider_(*layer.clip, position);
            if (srcResult.isError()) {
                // Propagate; the lease releases the (discarded) frame on scope
                // exit, so no partially-composited frame escapes.
                return err<RenderedFrame>(std::move(srcResult).error());
            }
            frame = std::move(srcResult).value();
        }
        if (!frame.valid()) {
            return err<RenderedFrame>(
                makeError(ErrorCode::Internal,
                          "Compositor::renderAt: a frame source returned an invalid source frame"));
        }

        // Apply the clip's per-clip effects (design "dispatchEffectKernel"),
        // here via the software reference hook.
        for (const Effect& effect : layer.clip->effects) {
            effectFn_(effect, frame.rgba.data(), frame.width, frame.height);
        }

        // Alpha-composite onto the target according to this layer's placement (its opacity,
        // and any transition weight, wipe edge or slide offset); preserves the loop invariant
        // for the next layer.
        blendOver(out, target.width, target.height, frame, placements[li]);
    }

    return RenderedFrame{std::move(lease), position, layers.size()};
}

}  // namespace palmier::gpu
