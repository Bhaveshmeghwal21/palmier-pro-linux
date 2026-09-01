// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/gpu_graceful_degradation_property_test.cpp — RapidCheck property test
// for correctness property P7 (graceful degradation / software-only lane),
// task 7.7; Requirements 10.4, 13.3.
//
//   Feature: palmier-pro-linux, Property 7: Graceful degradation — with no
//   supported GPU (or when ForceSoftware is selected), decode, composite
//   (renderAt), and encode all remain fully functional via the software path,
//   for any source frame and effect parameters.
//
//   Validates: Requirements 10.4, 13.3
//
// ---------------------------------------------------------------------------
// The software-only lane, end to end
// ---------------------------------------------------------------------------
// Requirement 10.4 mandates that when no compatible GPU_Backend is available (or
// detection times out) all media operations route to the CPU processing path and
// stay functional; the design's P7 folds in 13.3's "full functionality with no
// acceleration" guarantee. This test exercises exactly that lane — the one the
// sandbox always runs, since it has no Vulkan loader, GPU, or FFmpeg — over
// arbitrary inputs:
//
//   * No supported GPU is modeled two ways, matching the two triggers P7 names:
//       - the software-fallback GpuContext (GpuContext::softwareFallback(), whose
//         capabilities advertise no compute / no hardware codecs), and
//       - GpuSelectionPolicy::forceSoftware() applied even when a capable
//         discrete GPU IS present in enumeration (the explicit ForceSoftware
//         trigger), which must still yield the software fallback.
//     In both, the hardware codec bridge is limited to BridgeAvailability::
//     softwareOnly() (no vendor backends compiled/available).
//
//   * Decode — a MediaDecoder driven through the IDecodeBackend seam with
//     software-only capabilities must produce a CPU frame (never a GPU-resident
//     one), route to the software backend, and take no HW->CPU retry (there is
//     no hardware to try). The decode never errors.
//
//   * Composite — the real software Compositor (the vendor-neutral host-memory
//     reference) composites the generated source frame + effect through renderAt
//     over the software-fallback context, producing a host-memory RGBA8 frame of
//     the requested geometry. renderAt never errors.
//
//   * Encode — a MediaEncoder driven through the IEncodeBackend seam with
//     software-only capabilities must bind to the FFmpeg software backend (never
//     hardware), accept the composited frame via submit(), and finish() cleanly.
//
// The property asserts the whole decode -> composite -> encode chain succeeds
// and stays on the software path for every generated (frame, effect, codec,
// hardware-preference) tuple: no throw, no error, correct output geometry.
//
// Everything runs with no GPU, FFmpeg, or vendor SDK — which is the point:
// graceful degradation is precisely the behavior that must hold in that
// environment.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/Uuid.hpp"
#include "gpu/CodecBridge.hpp"
#include "gpu/Compositor.hpp"
#include "gpu/FramePool.hpp"
#include "gpu/GpuContext.hpp"
#include "gpu/GpuTypes.hpp"
#include "media/MediaDecoder.hpp"
#include "media/MediaEncoder.hpp"
#include "media/MediaInfo.hpp"

namespace palmier {
namespace {

// A timeline position that lies inside every generated clip's [0, 10s) span.
constexpr Duration kAt = Duration::fromSeconds(1.0);

// ---------------------------------------------------------------------------
// Scriptable software-lane decode backend
// ---------------------------------------------------------------------------
// Mirrors the IDecodeBackend mock used by the decoder's unit tests, trimmed to
// the software lane: a hardware decode attempt (which the software-only routing
// must never make here) reports an error, while the software decode always
// yields a CPU pixel buffer of the requested geometry.
class SoftwareDecodeBackend final : public media::IDecodeBackend {
public:
    SoftwareDecodeBackend(media::MediaCodecId codec, gpu::FrameDesc desc) : desc_(desc) {
        media::MediaStreamInfo video;
        video.index = 0;
        video.type = media::MediaStreamType::Video;
        video.codec = codec;
        video.codecName = "mock";
        video.resolution = Resolution{desc.width, desc.height};
        info_.streams.push_back(video);
        backing_.resize(desc.byteSize());
    }

    [[nodiscard]] const media::MediaInfo& info() const override { return info_; }

    [[nodiscard]] Result<media::BackendFrame> decode(bool useHardware) override {
        hwHistory.push_back(useHardware);
        if (useHardware) {
            // No hardware exists on this lane; if it were ever attempted the
            // bridge would retry on the CPU. It must not be attempted here.
            return err<media::BackendFrame>(
                Error(ErrorCode::Internal, "no hardware decode on the software lane"));
        }
        media::BackendFrame f;
        f.hardware = false;
        f.desc = desc_;
        f.timestamp = Duration::fromMilliseconds(10);
        f.cpuPixels.resize(desc_.byteSize());
        return f;
    }

    [[nodiscard]] Result<void> seek(Duration) override { return ok(); }

    std::vector<bool> hwHistory{};

private:
    gpu::FrameDesc         desc_;
    media::MediaInfo       info_{};
    std::vector<std::byte> backing_{};
};

media::DecodeBackendFactory softwareDecodeFactory(media::MediaCodecId codec,
                                                  gpu::FrameDesc desc,
                                                  SoftwareDecodeBackend** out) {
    return [codec, desc, out](const std::filesystem::path&, const media::DecodePrefs&)
               -> Result<std::unique_ptr<media::IDecodeBackend>> {
        auto backend = std::make_unique<SoftwareDecodeBackend>(codec, desc);
        if (out != nullptr) *out = backend.get();
        return std::unique_ptr<media::IDecodeBackend>(std::move(backend));
    };
}

// ---------------------------------------------------------------------------
// Scriptable software-lane encode backend
// ---------------------------------------------------------------------------
// Accepts every frame and records that it was invoked; the factory always
// succeeds so create() binds to whatever route it is handed (which, on the
// software lane, must be the FFmpeg software backend).
class SoftwareEncodeBackend final : public media::IEncodeBackend {
public:
    [[nodiscard]] Result<void> encode(const media::EncoderInputFrame& frame) override {
        ++encodeCalls;
        sawHostData = sawHostData || (frame.hostData != nullptr);
        return ok();
    }
    [[nodiscard]] Result<void> finish() override {
        ++finishCalls;
        return ok();
    }

    int  encodeCalls{0};
    int  finishCalls{0};
    bool sawHostData{false};
};

media::EncodeBackendFactory softwareEncodeFactory(SoftwareEncodeBackend** out,
                                                  gpu::CodecRoute* boundRoute) {
    return [out, boundRoute](const media::EncodeSpec&, const gpu::CodecRoute& route)
               -> Result<std::unique_ptr<media::IEncodeBackend>> {
        if (boundRoute != nullptr) *boundRoute = route;
        auto backend = std::make_unique<SoftwareEncodeBackend>();
        if (out != nullptr) *out = backend.get();
        return std::unique_ptr<media::IEncodeBackend>(std::move(backend));
    };
}

// ---------------------------------------------------------------------------
// Project / frame helpers
// ---------------------------------------------------------------------------

Clip makeClip(std::vector<Effect> effects) {
    Clip c;
    c.id = Uuid::generateV4();
    c.timelineStart = Duration::zero();
    c.sourceIn = Duration::zero();
    c.sourceOut = Duration::fromSeconds(10.0);
    c.opacity = 1.0;
    c.effects = std::move(effects);
    return c;
}

Track makeVideoTrack(Clip clip) {
    Track t;
    t.id = Uuid::generateV4();
    t.kind = TrackKind::Video;
    t.muted = false;
    t.clips = {std::move(clip)};
    return t;
}

Project makeProject(Track track) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "graceful-degradation";
    p.timelineFps = FrameRate::fps30();
    p.canvas = Resolution::hd1080();
    p.tracks = {std::move(track)};
    return p;
}

// A discrete, fully capable NVIDIA GPU present in enumeration — used to prove
// ForceSoftware bypasses even a strong device.
gpu::GpuDeviceInfo capableDiscreteGpu() {
    gpu::GpuDeviceInfo d;
    d.index = 0;
    d.name = "NVIDIA RTX";
    d.vendor = gpu::GpuVendor::NVIDIA;
    d.type = gpu::GpuDeviceType::DiscreteGpu;
    d.caps.vendor = "NVIDIA";
    d.caps.vendorId = gpu::GpuVendor::NVIDIA;
    d.caps.supportsCompute = true;
    d.caps.hwDecode = true;
    d.caps.hwEncode = true;
    d.caps.decodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC, gpu::CodecId::AV1};
    d.caps.encodeCodecs = {gpu::CodecId::H264, gpu::CodecId::HEVC};
    d.caps.vramBytes = 8ull * 1024 * 1024 * 1024;
    return d;
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

struct GenFrame {
    std::uint32_t             width{};
    std::uint32_t             height{};
    std::vector<std::uint8_t> rgba{};
};

[[nodiscard]] GenFrame genFrame() {
    const auto w = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9)); // 1..8
    const auto h = static_cast<std::uint32_t>(*rc::gen::inRange(1, 9)); // 1..8
    const std::size_t n = static_cast<std::size_t>(w) * h * 4u;
    auto rgba =
        *rc::gen::container<std::vector<std::uint8_t>>(n, rc::gen::arbitrary<std::uint8_t>());
    return GenFrame{w, h, std::move(rgba)};
}

[[nodiscard]] double genScaled(int loMilli, int hiMilli) {
    return static_cast<double>(*rc::gen::inRange(loMilli, hiMilli + 1)) / 1000.0;
}

[[nodiscard]] Effect genEffect() {
    const EffectType type =
        *rc::gen::element(EffectType::Brightness, EffectType::Contrast, EffectType::Blur,
                          EffectType::CropTransform, EffectType::ColorGrade, EffectType::Custom);
    const auto make = [&](std::map<std::string, double> params) {
        return Effect{Uuid::generateV4(), type, std::move(params)};
    };
    switch (type) {
        case EffectType::Brightness:
            return make({{"amount", genScaled(-1000, 1000)}});
        case EffectType::Contrast:
            return make({{"amount", genScaled(-1000, 1000)}});
        case EffectType::Blur:
            return make({{"radius", static_cast<double>(*rc::gen::inRange(0, 4))}});
        case EffectType::CropTransform:
            return make({{"cropLeft", genScaled(0, 1000)},
                         {"cropTop", genScaled(0, 1000)},
                         {"cropRight", genScaled(0, 1000)},
                         {"cropBottom", genScaled(0, 1000)}});
        case EffectType::ColorGrade:
            return make({{"gainR", genScaled(0, 2000)},
                         {"gainG", genScaled(0, 2000)},
                         {"gainB", genScaled(0, 2000)},
                         {"lift", genScaled(-500, 500)},
                         {"saturation", genScaled(0, 2000)}});
        case EffectType::InvertColors:
            return make({}); // parameterless (not drawn by the generator above)
        case EffectType::Lut:
        case EffectType::ToneCurve:
            // Also not drawn by the generator above: this property is about graceful
            // degradation, not about curve arithmetic, and leaving tone curves out
            // keeps its effect mix as it was rather than diluting it.
            return make({});
        case EffectType::Custom:
            return make({{"anything", genScaled(-1000, 1000)}});
    }
    return make({});
}

[[nodiscard]] media::MediaCodecId genVideoCodec() {
    return *rc::gen::element(media::MediaCodecId::H264, media::MediaCodecId::HEVC,
                             media::MediaCodecId::AV1, media::MediaCodecId::VP9);
}

// ---------------------------------------------------------------------------
// Reusable lane drivers (return true = the lane stayed fully functional on the
// software path). Assertions live in the callers via RC_ASSERT / EXPECT.
// ---------------------------------------------------------------------------

/// Drive the decode lane on the software-only path and return the decoded frame
/// result plus the backend for inspection.
struct DecodeOutcome {
    bool                   ok{false};
    bool                   producedCpuFrame{false};
    bool                   producedGpuFrame{false};
    bool                   retriedOnCpu{false};
    bool                   routeIsHardware{true};
    std::vector<bool>      hwHistory{};
};

DecodeOutcome runSoftwareDecode(const gpu::GpuCaps& swCaps, bool preferHardware,
                                media::MediaCodecId codec, gpu::FrameDesc desc,
                                gpu::FramePool* pool) {
    SoftwareDecodeBackend* backend = nullptr;
    media::DecodePrefs prefs;
    prefs.preferHardware = preferHardware;
    prefs.caps = swCaps;                              // no GPU: software caps
    prefs.availability = gpu::BridgeAvailability::softwareOnly();
    prefs.framePool = pool;                           // even with a pool, no HW caps -> CPU

    DecodeOutcome out;
    auto opened = media::MediaDecoder::open("clip.mp4", prefs,
                                            softwareDecodeFactory(codec, desc, &backend));
    if (opened.isError()) return out;
    media::MediaDecoder decoder = std::move(opened).value();

    out.routeIsHardware =
        decoder.bridge().route(decoder.videoCodec(), gpu::CodecOperation::Decode).hardware;

    auto frameRes = decoder.nextFrame();
    if (frameRes.isError()) return out;
    media::DecodedFrame frame = std::move(frameRes).value();

    out.ok = true;
    out.producedCpuFrame = frame.isCpu();
    out.producedGpuFrame = frame.isGpuResident();
    out.retriedOnCpu = decoder.lastFrameRetriedOnCpu();
    out.hwHistory = backend != nullptr ? backend->hwHistory : std::vector<bool>{};
    return out;
}

} // namespace

// ===========================================================================
// Property 7: Graceful degradation — full software-only pipeline
// ===========================================================================

// Feature: palmier-pro-linux, Property 7: Graceful degradation — with no
// supported GPU, decode + composite (renderAt) + encode remain fully functional
// on the software path for any source frame and effect parameters.
// Validates: Requirements 10.4, 13.3
RC_GTEST_PROP(GpuGracefulDegradation,
              SoftwareOnlyLaneDecodeCompositeEncodeRemainFullyFunctional,
              ()) {
    // "No supported GPU": the always-available software-fallback context, whose
    // capabilities advertise no compute and no hardware codecs.
    auto ctx = gpu::GpuContext::softwareFallback();
    RC_ASSERT(ctx.isSoftwareFallback());
    const gpu::GpuCaps swCaps = ctx.capabilities();
    RC_ASSERT(swCaps.vendorId == gpu::GpuVendor::Software);
    RC_ASSERT(!swCaps.supportsCompute);

    const GenFrame frame = genFrame();
    const Effect effect = genEffect();
    const media::MediaCodecId codec = genVideoCodec();
    const bool preferHardware = *rc::gen::arbitrary<bool>();
    const gpu::FrameDesc desc{frame.width, frame.height, gpu::FrameFormat::RGBA8};

    RC_TAG(static_cast<int>(effect.type)); // distribution of exercised effect kinds

    // --- Decode lane: must yield a CPU frame on the software route ----------
    const DecodeOutcome decode =
        runSoftwareDecode(swCaps, preferHardware, codec, desc, &ctx.framePool());
    RC_ASSERT(decode.ok);                 // decode did not error
    RC_ASSERT(decode.producedCpuFrame);   // software decode produced host pixels
    RC_ASSERT(!decode.producedGpuFrame);  // nothing GPU-resident with no GPU
    RC_ASSERT(!decode.retriedOnCpu);      // no hardware to fail and retry from
    RC_ASSERT(!decode.routeIsHardware);   // routed to the software backend
    RC_ASSERT(!decode.hwHistory.empty());
    RC_ASSERT(!decode.hwHistory.front()); // the very first attempt was software

    // --- Composite lane: renderAt over the software fallback context --------
    gpu::Compositor comp(ctx);
    comp.setFrameProvider(
        [&](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            gpu::SourceFrame f;
            f.width = frame.width;
            f.height = frame.height;
            f.rgba = frame.rgba;
            return f;
        });

    Project project = makeProject(makeVideoTrack(makeClip({effect})));
    gpu::RenderTarget target(frame.width, frame.height, gpu::RgbaColor::opaqueBlack());

    auto renderRes = comp.renderAt(project, kAt, target);
    RC_ASSERT(renderRes.isOk());          // compositing did not error
    gpu::RenderedFrame rendered = std::move(renderRes).value();
    RC_ASSERT(rendered.layerCount() == 1u);
    RC_ASSERT(rendered.width() == frame.width);
    RC_ASSERT(rendered.height() == frame.height);
    // software produced host-memory pixels.
    // Evaluate the void* check into a bool first: RC_ASSERT stringifies its
    // operands via rc::show(), and rc::show(void*) is ill-formed (void* is not
    // dereferenceable). A bool operand Shows fine.
    const bool producedHostPixels = (rendered.hostData() != nullptr);
    RC_ASSERT(producedHostPixels);

    // --- Encode lane: must bind to the FFmpeg software backend --------------
    SoftwareEncodeBackend* encBackend = nullptr;
    gpu::CodecRoute boundRoute;
    media::EncodeSpec spec;
    spec.codec = gpu::CodecId::H264;
    spec.bitrateBitsPerSecond = 4'000'000;
    spec.resolution = Resolution{frame.width, frame.height};
    spec.frameRate = FrameRate::fps30();
    spec.preferHardware = preferHardware;
    spec.caps = swCaps;
    spec.availability = gpu::BridgeAvailability::softwareOnly();

    auto encRes = media::MediaEncoder::create(spec, softwareEncodeFactory(&encBackend, &boundRoute));
    RC_ASSERT(encRes.isOk());             // encoder init did not error
    media::MediaEncoder enc = std::move(encRes).value();
    RC_ASSERT(!enc.isHardware());         // bound to the software encoder
    RC_ASSERT(enc.route().backend == gpu::CodecBackend::FFmpegSoftware);
    RC_ASSERT(!boundRoute.hardware);

    RC_ASSERT(enc.submit(rendered).isOk()); // the composited frame was accepted
    RC_ASSERT(enc.submittedFrameCount() == 1u);
    RC_ASSERT(enc.finish().isOk());         // stream finalized cleanly
    RC_ASSERT(encBackend != nullptr);
    RC_ASSERT(encBackend->encodeCalls == 1);
    RC_ASSERT(encBackend->sawHostData);     // encoded from the CPU frame's pixels
}

// ===========================================================================
// Property 7: Graceful degradation — explicit ForceSoftware bypasses a GPU
// ===========================================================================

// Feature: palmier-pro-linux, Property 7: Graceful degradation — when
// ForceSoftware is selected, a capable GPU present in enumeration is bypassed
// and compositing remains fully functional on the software path for any frame.
// Validates: Requirements 10.4, 13.3
RC_GTEST_PROP(GpuGracefulDegradation,
              ForceSoftwareBypassesPresentGpuAndCompositesInSoftware,
              ()) {
    const gpu::GpuDeviceInfo present = capableDiscreteGpu();
    gpu::PhysicalDeviceEnumerator enumerate = [present]() {
        return std::vector<gpu::GpuDeviceInfo>{present};
    };

    auto ctxRes = gpu::GpuContext::createWith(gpu::GpuSelectionPolicy::forceSoftware(),
                                              enumerate, /*store=*/nullptr);
    RC_ASSERT(ctxRes.isOk());
    gpu::GpuContext ctx = std::move(ctxRes).value();

    // ForceSoftware must ignore the capable device and land on the CPU path.
    RC_ASSERT(ctx.isSoftwareFallback());
    RC_ASSERT(ctx.selectedDeviceIndex() == -1);
    RC_ASSERT(!ctx.capabilities().supportsCompute);

    const GenFrame frame = genFrame();
    const Effect effect = genEffect();
    RC_TAG(static_cast<int>(effect.type));

    gpu::Compositor comp(ctx);
    comp.setFrameProvider(
        [&](const Clip&, Duration) -> Result<gpu::SourceFrame> {
            gpu::SourceFrame f;
            f.width = frame.width;
            f.height = frame.height;
            f.rgba = frame.rgba;
            return f;
        });

    Project project = makeProject(makeVideoTrack(makeClip({effect})));
    gpu::RenderTarget target(frame.width, frame.height, gpu::RgbaColor::opaqueBlack());

    auto renderRes = comp.renderAt(project, kAt, target);
    RC_ASSERT(renderRes.isOk());
    gpu::RenderedFrame rendered = std::move(renderRes).value();
    RC_ASSERT(rendered.layerCount() == 1u);
    RC_ASSERT(rendered.width() == frame.width);
    RC_ASSERT(rendered.height() == frame.height);
    // Evaluate the void* check into a bool first: rc::show(void*) is ill-formed
    // because void* cannot be dereferenced, so a direct RC_ASSERT on it would
    // fail to compile. A bool operand Shows fine.
    const bool producedHostPixels = (rendered.hostData() != nullptr);
    RC_ASSERT(producedHostPixels);
}

// ===========================================================================
// Deterministic companion examples (concrete witnesses of P7).
// ===========================================================================

TEST(GpuGracefulDegradationExamples, SoftwareFallbackContextAdvertisesNoAcceleration) {
    auto ctx = gpu::GpuContext::softwareFallback();
    EXPECT_TRUE(ctx.isSoftwareFallback());
    EXPECT_EQ(ctx.selectedDeviceIndex(), -1);
    EXPECT_EQ(ctx.capabilities().vendorId, gpu::GpuVendor::Software);
    EXPECT_FALSE(ctx.capabilities().supportsCompute);
    EXPECT_FALSE(ctx.capabilities().hwDecode);
    EXPECT_FALSE(ctx.capabilities().hwEncode);
}

TEST(GpuGracefulDegradationExamples, DecodeRoutesToSoftwareWithNoGpu) {
    auto ctx = gpu::GpuContext::softwareFallback();
    const DecodeOutcome decode =
        runSoftwareDecode(ctx.capabilities(), /*preferHardware=*/true,
                          media::MediaCodecId::H264,
                          gpu::FrameDesc{16, 16, gpu::FrameFormat::RGBA8}, &ctx.framePool());
    EXPECT_TRUE(decode.ok);
    EXPECT_TRUE(decode.producedCpuFrame);
    EXPECT_FALSE(decode.producedGpuFrame);
    EXPECT_FALSE(decode.retriedOnCpu);
    EXPECT_FALSE(decode.routeIsHardware);
    ASSERT_FALSE(decode.hwHistory.empty());
    EXPECT_FALSE(decode.hwHistory.front());
}

TEST(GpuGracefulDegradationExamples, EncoderBindsToSoftwareBackendWithNoGpu) {
    SoftwareEncodeBackend* backend = nullptr;
    gpu::CodecRoute route;
    media::EncodeSpec spec;
    spec.codec = gpu::CodecId::H264;
    spec.resolution = Resolution{16, 16};
    spec.frameRate = FrameRate::fps30();
    spec.preferHardware = true; // prefers hardware, but there is none.
    spec.caps = gpu::GpuCaps::software();
    spec.availability = gpu::BridgeAvailability::softwareOnly();

    auto encRes = media::MediaEncoder::create(spec, softwareEncodeFactory(&backend, &route));
    ASSERT_TRUE(encRes.isOk());
    media::MediaEncoder enc = std::move(encRes).value();
    EXPECT_FALSE(enc.isHardware());
    EXPECT_EQ(enc.route().backend, gpu::CodecBackend::FFmpegSoftware);
    EXPECT_FALSE(route.hardware);
    EXPECT_TRUE(enc.finish().isOk());
}

} // namespace palmier
