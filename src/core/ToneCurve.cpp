// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ToneCurve.cpp — implementation of the tone-curve control points, the
// piecewise-linear interpolation and the 256-entry baking
// (monitoring-and-grading Requirement 5). See ToneCurve.hpp for why the curve is
// baked, why interpolation is linear, and how points are encoded in the parameter map.

#include "core/ToneCurve.hpp"

#include <algorithm>
#include <utility>

namespace palmier {
namespace {

/// Round half-up into a byte, clamping. Deliberately identical in form to
/// gpu::toByte, so a table entry and a directly-rendered pixel round the same way.
[[nodiscard]] std::uint8_t toByte(double v) noexcept {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(v + 0.5);
}

}  // namespace

const char* curveChannelName(CurveChannel channel) noexcept {
    switch (channel) {
        case CurveChannel::Master: return "Master";
        case CurveChannel::Red:    return "Red";
        case CurveChannel::Green:  return "Green";
        case CurveChannel::Blue:   return "Blue";
    }
    return "Master";
}

const char* curveChannelToolName(CurveChannel channel) noexcept {
    switch (channel) {
        case CurveChannel::Master: return "master";
        case CurveChannel::Red:    return "red";
        case CurveChannel::Green:  return "green";
        case CurveChannel::Blue:   return "blue";
    }
    return "master";
}

std::optional<CurveChannel> parseCurveChannelToolName(std::string_view name) {
    // Derived from curveChannelToolName rather than written out again, so a name added
    // to one direction cannot be missing from the other.
    for (const CurveChannel channel : kCurveChannels) {
        if (name == curveChannelToolName(channel)) {
            return channel;
        }
    }
    return std::nullopt;
}

const std::vector<std::string>& curveChannelToolNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> built;
        built.reserve(kCurveChannels.size());
        for (const CurveChannel channel : kCurveChannels) {
            built.emplace_back(curveChannelToolName(channel));
        }
        return built;
    }();
    return names;
}

std::string curveChannelParameterPrefix(CurveChannel channel) {
    std::string prefix = "curve";
    prefix += curveChannelName(channel);
    prefix += 'P';
    return prefix;
}

std::string curvePointParameterName(CurveChannel channel, std::size_t index, bool isY) {
    // Built on the prefix rather than repeating it, so the two cannot disagree: a
    // prefix that did not actually prefix the names would make "every parameter of
    // this channel" silently select nothing.
    std::string name = curveChannelParameterPrefix(channel);
    name += std::to_string(index);
    name += isY ? 'Y' : 'X';
    return name;
}

std::vector<CurvePoint> curvePoints(const std::map<std::string, double>& parameters,
                                    CurveChannel channel) {
    std::vector<CurvePoint> points;
    for (std::size_t index = 0;; ++index) {
        const auto x = parameters.find(curvePointParameterName(channel, index, /*isY=*/false));
        const auto y = parameters.find(curvePointParameterName(channel, index, /*isY=*/true));
        // Both coordinates or nothing: a half-written point is not a point, and the
        // list is a contiguous prefix so a gap terminates rather than being closed up.
        if (x == parameters.end() || y == parameters.end()) {
            break;
        }
        points.push_back(CurvePoint{x->second, y->second});
    }
    return points;
}

double evaluateToneCurve(const std::vector<CurvePoint>& points, double x) {
    // Requirement 5.5: nothing to interpolate between, so the identity.
    if (points.size() < 2) {
        return std::clamp(x, 0.0, 1.0);
    }

    std::vector<CurvePoint> sorted = points;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });

    // Held flat outside the end points rather than extrapolated: continuing the last
    // two points off the end of the range is how an aggressive curve blows a highlight
    // the user never asked for.
    if (x < sorted.front().x) return std::clamp(sorted.front().y, 0.0, 1.0);
    if (x > sorted.back().x) return std::clamp(sorted.back().y, 0.0, 1.0);

    // The last point at or before x. Taking the LAST rather than the first is what
    // makes two points sharing an x a deterministic vertical step (the later point
    // wins) instead of depending on which segment the search happened to reach.
    std::size_t j = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i].x <= x) {
            j = i;
        } else {
            break;
        }
    }

    // Landing exactly on a point needs no interpolation, and handling it here is also
    // what removes the possibility of a zero-span division below: if sorted[j].x != x
    // then sorted[j].x < x, and sorted[j+1].x > x (an equal one would have advanced j),
    // so the span is strictly positive.
    if (sorted[j].x == x) {
        return std::clamp(sorted[j].y, 0.0, 1.0);
    }

    const CurvePoint& lo = sorted[j];
    const CurvePoint& hi = sorted[j + 1];
    const double t = (x - lo.x) / (hi.x - lo.x);
    return std::clamp(lo.y + (hi.y - lo.y) * t, 0.0, 1.0);
}

ToneCurveTable identityToneCurveTable() noexcept {
    ToneCurveTable table{};
    for (std::size_t v = 0; v < table.size(); ++v) {
        table[v] = static_cast<std::uint8_t>(v);
    }
    return table;
}

ToneCurveTable bakeToneCurve(const std::vector<CurvePoint>& points) {
    if (points.size() < 2) {
        return identityToneCurveTable();
    }
    ToneCurveTable table{};
    for (std::size_t v = 0; v < table.size(); ++v) {
        const double x = static_cast<double>(v) / 255.0;
        table[v] = toByte(evaluateToneCurve(points, x) * 255.0);
    }
    return table;
}

bool ToneCurveTables::isIdentity() const noexcept {
    for (std::size_t v = 0; v < red.size(); ++v) {
        const auto byte = static_cast<std::uint8_t>(v);
        if (red[v] != byte || green[v] != byte || blue[v] != byte) {
            return false;
        }
    }
    return true;
}

ToneCurveTables toneCurveTables(const std::map<std::string, double>& parameters) {
    const ToneCurveTable master = bakeToneCurve(curvePoints(parameters, CurveChannel::Master));
    const ToneCurveTable red = bakeToneCurve(curvePoints(parameters, CurveChannel::Red));
    const ToneCurveTable green = bakeToneCurve(curvePoints(parameters, CurveChannel::Green));
    const ToneCurveTable blue = bakeToneCurve(curvePoints(parameters, CurveChannel::Blue));

    // Per-channel first, then master (see CHANNEL COMPOSITION in the header). Two
    // lookups compose into one, which is why the kernel needs a single table per
    // channel and no notion of a master curve at all.
    ToneCurveTables tables;
    for (std::size_t v = 0; v < tables.red.size(); ++v) {
        tables.red[v] = master[red[v]];
        tables.green[v] = master[green[v]];
        tables.blue[v] = master[blue[v]];
    }
    return tables;
}

}  // namespace palmier
