// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/CodecBridge.hpp — hardware decode/encode routing + software fallback.
//
// This is the "HW Decode/Encode Bridge" node from design.md's architecture
// diagram and the "Vendor backend mapping for hardware codecs" table. It sits
// between the Media Engine's decoder/encoder (tasks 8.2/8.3) and the vendor
// hardware codec paths, answering two questions:
//
//   1. *Routing (a pure decision):* given the selected device's capabilities and
//      a (codec, operation) pair, which backend should run it? — NVDEC/NVENC for
//      NVIDIA, VAAPI for AMD, Quick Sync (or VAAPI) for Intel, and FFmpeg
//      software (x264/x265/SVT-AV1) whenever hardware is unavailable or the codec
//      is not hardware-supported (design "HW decode unsupported -> SW fallback").
//
//   2. *Fallback execution:* run a caller-supplied operation on the routed
//      backend and, if it is a hardware backend that fails, retry the operation
//      exactly once on the CPU (software) path, record the failure in the log,
//      and preserve the operation's inputs so no edit data is lost
//      (Requirement 10.5; design "export encoder failure -> retry SW").
//
// The bridge owns *no* media state: the actual decode/encode work lives in the
// caller's CodecOpFn (the future MediaDecoder/MediaEncoder), so a CPU retry
// simply re-invokes that callable with the same inputs. This keeps the
// routing/fallback policy free of any FFmpeg or vendor-SDK dependency and makes
// it fully unit-testable with mock operations on a machine that has no GPU and
// no FFmpeg (e.g. CI/sandbox).
//
// Which concrete hardware backends are *compiled in* is governed by the
// PALMIER_HAVE_VAAPI / PALMIER_HAVE_NVENC / PALMIER_HAVE_QSV build defines
// (derived from the PALMIER_ENABLE_* options); BridgeAvailability captures that
// at runtime and can be overridden freely in tests. The concrete FFmpeg-backed
// decoder/encoder that consume these routes are implemented by tasks 8.2/8.3 —
// this file is only the routing/bridge layer they build on.

#ifndef PALMIER_GPU_CODECBRIDGE_HPP
#define PALMIER_GPU_CODECBRIDGE_HPP

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {

/// Whether a codec operation decodes a compressed stream in or encodes frames out.
enum class CodecOperation {
    Decode,
    Encode,
};

/// The concrete decode/encode backend a codec operation is routed to.
///
/// Hardware backends mirror design.md's "Vendor backend mapping for hardware
/// codecs" table; FFmpegSoftware is the always-available CPU fallback (which for
/// encode uses x264/x265/SVT-AV1 depending on the codec).
enum class CodecBackend {
    None = 0,        ///< No backend selectable (defensive; software is always available).
    Nvdec,           ///< NVIDIA hardware decode (FFmpeg cuvid).
    Nvenc,           ///< NVIDIA hardware encode (NVENC).
    Vaapi,           ///< AMD / Intel hardware decode+encode (libva).
    QuickSync,       ///< Intel Quick Sync Video (oneVPL / QSV).
    FFmpegSoftware,  ///< CPU decode/encode fallback (x264 / x265 / SVT-AV1 encoders).
};

/// Stable, human-readable backend name (for logs / UI).
[[nodiscard]] constexpr std::string_view backendName(CodecBackend b) noexcept {
    switch (b) {
        case CodecBackend::Nvdec:          return "NVDEC";
        case CodecBackend::Nvenc:          return "NVENC";
        case CodecBackend::Vaapi:          return "VAAPI";
        case CodecBackend::QuickSync:      return "Quick Sync";
        case CodecBackend::FFmpegSoftware: return "FFmpeg software";
        case CodecBackend::None:           return "none";
    }
    return "none";
}

/// True for the vendor hardware backends (everything but None / FFmpegSoftware).
[[nodiscard]] constexpr bool isHardwareBackend(CodecBackend b) noexcept {
    return b != CodecBackend::None && b != CodecBackend::FFmpegSoftware;
}

/// FFmpeg software encoder library name for a codec ("libx264" | "libx265" |
/// "libsvtav1" | "libvpx-vp9"); empty for codecs without a mapped SW encoder.
[[nodiscard]] std::string_view softwareEncoderName(CodecId codec) noexcept;

/// The concrete hardware backends compiled into this build.
///
/// In production these mirror the PALMIER_HAVE_* build defines (see
/// fromBuildConfig); tests construct them freely so the routing/fallback logic
/// is verifiable without any vendor SDK present. The FFmpeg software path is
/// always compiled in.
struct BridgeAvailability {
    bool nvdec{false};           ///< PALMIER_HAVE_NVENC (NVIDIA decode path).
    bool nvenc{false};           ///< PALMIER_HAVE_NVENC (NVIDIA encode path).
    bool vaapi{false};           ///< PALMIER_HAVE_VAAPI.
    bool quickSync{false};       ///< PALMIER_HAVE_QSV.
    bool ffmpegSoftware{true};   ///< Always available CPU fallback.

    /// Availability determined by the build-time PALMIER_HAVE_* defines.
    [[nodiscard]] static BridgeAvailability fromBuildConfig() noexcept;

    /// Availability with only the software fallback (all hardware paths off).
    [[nodiscard]] static BridgeAvailability softwareOnly() noexcept { return {}; }

    /// Convenience: every vendor path plus the software fallback available.
    [[nodiscard]] static BridgeAvailability all() noexcept {
        return {true, true, true, true, true};
    }
};

/// The outcome of a routing decision: which backend runs a given (codec,
/// operation), whether it is hardware, and — for a software encode — the FFmpeg
/// encoder library to use.
struct CodecRoute {
    CodecId        codec{CodecId::Unknown};
    CodecOperation operation{CodecOperation::Decode};
    CodecBackend   backend{CodecBackend::FFmpegSoftware};
    bool           hardware{false};
    /// FFmpeg software encoder library for a software *encode* route
    /// ("libx264" | "libx265" | "libsvtav1" | "libvpx-vp9"); empty otherwise.
    std::string    softwareEncoder{};
    /// Human-readable reason for logs / UI, e.g. "NVIDIA NVENC" or
    /// "FFmpeg software (codec not hardware-supported)".
    std::string    detail{};

    [[nodiscard]] friend bool operator==(const CodecRoute&, const CodecRoute&) = default;
};

/// Sink for bridge log lines (Requirement 10.5: "record the failure in the
/// application log"). When none is installed, lines accumulate in the bridge's
/// internal buffer, readable via CodecBridge::log().
using BridgeLogSink = std::function<void(std::string_view)>;

/// A callable that performs the actual decode/encode using a chosen route.
///
/// The concrete MediaDecoder / MediaEncoder (tasks 8.2/8.3) provide this; tests
/// supply mocks. It MUST be safe to re-invoke with identical inputs so that a
/// CPU retry after a hardware failure reprocesses the *same* input, losing no
/// edit data (Requirement 10.5).
using CodecOpFn = std::function<Result<void>(const CodecRoute&)>;

/// What happened when an operation was run through the bridge with fallback.
struct CodecExecution {
    Result<void>         result{ok()};    ///< Final result (after any CPU retry).
    CodecRoute           route{};         ///< Backend that produced `result`.
    CodecRoute           primaryRoute{};  ///< Backend first attempted.
    bool                 retriedOnCpu{false};
    std::optional<Error> hardwareError{}; ///< The GPU failure that forced a retry, if any.
};

/// Routes hardware decode/encode to the appropriate vendor backend and falls
/// back to FFmpeg software when hardware is unavailable/unsupported, applying
/// the retry-once-on-CPU-then-log policy on a hardware failure.
///
/// Requirements 10.2 (route to the GPU backend when available) and 10.5 (retry
/// on CPU at most once, preserve input/project state, log the failure).
class CodecBridge {
public:
    /// Construct a bridge for a device with capabilities `caps`. `availability`
    /// defaults to the compiled-in backends (PALMIER_HAVE_* defines).
    explicit CodecBridge(GpuCaps caps,
                         BridgeAvailability availability = BridgeAvailability::fromBuildConfig());

    /// Decide which backend a (codec, operation) routes to, given the device
    /// capabilities and the compiled-in backends. Pure: performs no work and
    /// mutates nothing. Falls back to FFmpegSoftware whenever hardware is
    /// unavailable, the vendor is software/unknown, or the codec is not among the
    /// device's hardware-supported codecs.
    [[nodiscard]] CodecRoute route(CodecId codec, CodecOperation operation) const;

    /// The software (CPU) route for a (codec, operation) — the fallback target.
    [[nodiscard]] CodecRoute softwareRoute(CodecId codec, CodecOperation operation) const;

    /// Run `op` for (codec, operation): first on the routed backend; if that is a
    /// hardware backend and it fails, log the failure and retry exactly once on
    /// the CPU (software) path (Requirement 10.5). The bridge never touches the
    /// caller's inputs, so the retry reprocesses the same input. A failure on a
    /// software primary is returned without further retry (already on the CPU).
    [[nodiscard]] CodecExecution execute(CodecId codec, CodecOperation operation,
                                         const CodecOpFn& op);

    [[nodiscard]] const GpuCaps& capabilities() const noexcept { return caps_; }
    [[nodiscard]] const BridgeAvailability& availability() const noexcept { return avail_; }

    /// Install a custom log sink. When unset, entries accumulate internally.
    void setLogSink(BridgeLogSink sink) { sink_ = std::move(sink); }

    /// The internally buffered log lines (empty when a custom sink is installed
    /// and the caller routes lines elsewhere; lines are always buffered too).
    [[nodiscard]] const std::vector<std::string>& log() const noexcept { return log_; }

private:
    void logLine(std::string line);

    /// The hardware backend this device/vendor would use for `operation`, or
    /// CodecBackend::None when no compiled-in hardware backend applies.
    [[nodiscard]] CodecBackend hardwareBackendFor(CodecOperation operation) const;

    GpuCaps                  caps_;
    BridgeAvailability       avail_;
    BridgeLogSink            sink_{};
    std::vector<std::string> log_{};
};

} // namespace palmier::gpu

#endif // PALMIER_GPU_CODECBRIDGE_HPP
