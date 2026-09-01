// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ToneCurve.hpp — tone-curve control points, interpolation and baking
// (monitoring-and-grading Requirement 5).
//
// A tone curve is the one grading operation no combination of lift, gamma and gain
// can express: lifting shadows while leaving highlights where they are. It maps input
// intensity to output intensity through a set of user-placed control points.
//
// THREE DECISIONS SHAPE EVERYTHING HERE, and each is a decision rather than an
// obvious consequence, so each is written down.
//
// 1. THE CURVE IS BAKED TO A 256-ENTRY TABLE, and both render paths do nothing but
//    index it.
//
//    The pipeline is RGBA8, so the input to a curve has exactly 256 possible values
//    per channel. A 256-entry table is therefore not an approximation of the transfer
//    function — it IS the transfer function, completely and exactly. That single fact
//    disposes of Requirement 5.3 and 5.4 by construction rather than by testing:
//    interpolation happens once, on the CPU, in double precision, and the GPU kernel
//    and the software reference then agree EXACTLY (not within 1 LSB) because they
//    read the same bytes. There is no float-versus-double divergence to bound,
//    because neither path evaluates the curve at all.
//
//    It also means a variable-length point list never has to reach a shader, which
//    matters: push constants are only guaranteed to be 128 bytes, so four channels of
//    control points would not fit, and a curve of "up to 16 points shared across all
//    channels" is a limit imposed by a transport rather than by the design.
//
// 2. INTERPOLATION IS PIECEWISE LINEAR between points sorted by x, held flat outside
//    the first and last point.
//
//    Monotone cubic (Fritsch-Carlson) is what a dedicated colour tool uses and would
//    look smoother under a coarse set of points. It is deliberately not used here.
//    Requirement 5.4 asks for interpolation that is DOCUMENTED and DETERMINISTIC, and
//    says nothing about smoothness; piecewise linear is both, is exactly reproducible
//    on any host with no dependence on evaluation order or fused multiply-add, and is
//    simple enough that a test can state a whole expected transfer function in closed
//    form. A cubic would buy visual smoothness at the cost of a much larger surface
//    for the two paths to disagree — and with the baking above, "the two paths" is no
//    longer where the risk lives, but "the same result on every host" still is.
//
//    Held flat outside the end points rather than extrapolated: extrapolating a
//    user's last two points off the end of the range is how an aggressive curve
//    produces a blown highlight the user never asked for.
//
// 3. A CURVE WITH FEWER THAN TWO POINTS IS THE IDENTITY (Requirement 5.5).
//
//    Zero points is obvious. One point is the interesting case: it defines no segment,
//    so there is nothing to interpolate. Clamping the whole range to that one point's
//    y would turn the image into a single flat value, which is emphatically not what a
//    user placing their first point expects to see, and reporting an error for a curve
//    mid-construction would make the control unusable. Identity is the only sensible
//    reading, and the requirement says so explicitly.
//
// PARAMETER ENCODING. Control points live in the effect's existing
// `std::map<std::string, double> parameters` under indexed names:
//
//     curveMasterP0X, curveMasterP0Y, curveMasterP1X, ...
//     curveRedP0X,    curveRedP0Y,    ...      (also Green, Blue)
//
// This is why tone curves need no schema bump and no serializer change: ProjectStore
// already persists that map by iterating it, so both the coordinates AND their order
// round-trip (Requirement 5.8) because the order is carried in the name. A point's
// index is its identity; the point list is read as p0, p1, ... until an index is
// missing either coordinate, so a gap terminates the list rather than being skipped.
//
// CHANNEL COMPOSITION. The per-channel curve is applied first, then the master curve
// is applied to the result. Both are table lookups, so the two compose into a SINGLE
// table per channel: combined[c][v] = master[channel_c[v]]. Documented here because
// the opposite order gives a different image and nothing in the data records which
// was intended.

#ifndef PALMIER_CORE_TONECURVE_HPP
#define PALMIER_CORE_TONECURVE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palmier {

/// One control point, in normalised [0,1] input/output intensity.
///
/// Stored normalised rather than as bytes so a curve is independent of the bit depth
/// it happens to be applied at, and so a point can sit between two byte values.
struct CurvePoint {
    double x = 0.0;  ///< input intensity
    double y = 0.0;  ///< output intensity

    friend bool operator==(const CurvePoint& a, const CurvePoint& b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
};

/// The four curves a tone-curve effect carries.
enum class CurveChannel { Master, Red, Green, Blue };

/// Every channel, for iteration.
inline constexpr std::array<CurveChannel, 4> kCurveChannels{
    CurveChannel::Master, CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue};

/// The infix used in parameter names: "Master", "Red", "Green", "Blue".
[[nodiscard]] const char* curveChannelName(CurveChannel channel) noexcept;

/// The channel's name on the tool surface and in the UI, e.g. "red".
///
/// Lowercase here and capitalised in curveChannelName(), which is deliberate rather
/// than drift: that one spells the PERSISTED parameter names ("curveRedP0X") and so
/// cannot change without breaking every saved project, while this one follows the
/// convention every other closed value set on the surface already uses
/// ("crop_transform", "invert_colors"). Both live here so the tool surface, the
/// Inspector and any future scripting layer cannot disagree about either.
[[nodiscard]] const char* curveChannelToolName(CurveChannel channel) noexcept;

/// Parse a tool-surface channel name; nullopt when it names no channel.
[[nodiscard]] std::optional<CurveChannel> parseCurveChannelToolName(std::string_view name);

/// The four tool-surface channel names, in kCurveChannels order.
[[nodiscard]] const std::vector<std::string>& curveChannelToolNames();

/// The name prefix every parameter belonging to one channel's points shares,
/// e.g. "curveRedP".
///
/// Exists so a caller that must operate on "all of this channel's points" -- an
/// undoable edit capturing the prior state, for instance -- can find them without
/// re-deriving the encoding and without guessing how many there are.
[[nodiscard]] std::string curveChannelParameterPrefix(CurveChannel channel);

/// The parameter name for one coordinate of one point, e.g. "curveRedP2Y".
///
/// The single place the encoding is spelled out. Everything that reads or writes a
/// control point goes through here, so the scheme cannot drift between the renderer,
/// the tool surface and the Inspector.
[[nodiscard]] std::string curvePointParameterName(CurveChannel channel, std::size_t index,
                                                  bool isY);

/// Read one channel's control points out of an effect's parameter map.
///
/// Reads p0, p1, ... and STOPS at the first index missing either coordinate, so the
/// list is always a contiguous prefix. A gap therefore truncates rather than being
/// silently closed up: a half-written point (an X with no Y) is not a point, and
/// treating it as one would render a curve the user never described.
///
/// Returned in the stored index order, NOT sorted — `bakeToneCurve` sorts. Callers
/// that present or edit points need the stored order so that "point 2" means the same
/// thing to the Inspector and to an undo entry.
[[nodiscard]] std::vector<CurvePoint> curvePoints(const std::map<std::string, double>& parameters,
                                                  CurveChannel channel);

/// Evaluate the piecewise-linear curve at `x`, both in [0,1].
///
/// Fewer than two points is the identity (Requirement 5.5). Outside the first/last
/// point the value is held flat. The result is clamped to [0,1] (Requirement 5.6), so
/// a point placed outside the range cannot push a value past it.
[[nodiscard]] double evaluateToneCurve(const std::vector<CurvePoint>& points, double x);

/// A baked transfer function: output byte for each of the 256 input bytes.
using ToneCurveTable = std::array<std::uint8_t, 256>;

/// The identity table (table[v] == v).
[[nodiscard]] ToneCurveTable identityToneCurveTable() noexcept;

/// Bake one channel's points into a 256-entry table.
///
/// Deterministic and host-independent: each entry is evaluated at v/255 and rounded
/// half-up, in double precision, with no accumulation across entries.
[[nodiscard]] ToneCurveTable bakeToneCurve(const std::vector<CurvePoint>& points);

/// The three tables an RGBA8 image is graded with, each pre-composing that channel's
/// own curve with the master curve (see CHANNEL COMPOSITION above).
struct ToneCurveTables {
    ToneCurveTable red{};
    ToneCurveTable green{};
    ToneCurveTable blue{};

    /// True when all three are the identity, so the effect is a no-op and the whole
    /// per-pixel loop can be skipped.
    [[nodiscard]] bool isIdentity() const noexcept;
};

/// Resolve an effect's parameters to the three tables its pixels are graded with.
[[nodiscard]] ToneCurveTables toneCurveTables(const std::map<std::string, double>& parameters);

}  // namespace palmier

#endif  // PALMIER_CORE_TONECURVE_HPP
