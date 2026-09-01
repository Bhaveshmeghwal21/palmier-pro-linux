// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/CubeLut.cpp — see the header for what the parser accepts, what it rejects, and why
// a partially read table is never returned.

#include "gpu/CubeLut.hpp"

#include <cmath>
#include <cstdlib>
#include <utility>

#include "core/Error.hpp"

namespace palmier::gpu {

namespace {

/// Strip a trailing comment and surrounding whitespace, leaving the payload.
std::string_view trimmed(std::string_view line) {
    if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

/// Split on runs of whitespace.
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

/// Parse a float strictly: the WHOLE field must be consumed, so "0.5x" is a fault rather
/// than 0.5. A lenient parse here would accept a corrupt table and apply it.
bool parseFloat(std::string_view field, float& out) {
    const std::string text(field);
    char*             end = nullptr;
    const double      value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || text.empty()) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool parseInt(std::string_view field, long& out) {
    const std::string text(field);
    char*             end = nullptr;
    const long        value = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size() || text.empty()) {
        return false;
    }
    out = value;
    return true;
}

Error malformed(const std::string& what) {
    return invalidArgument("cube LUT: " + what);
}

}  // namespace

LutEntry CubeLut::at(int r, int g, int b) const noexcept {
    if (size_ <= 0 || r < 0 || g < 0 || b < 0 || r >= size_ || g >= size_ || b >= size_) {
        return LutEntry{};
    }
    // Red varies fastest -- the .cube format's own order. Wrong here and a transposed
    // table still renders a plausible image, which is why at() exists at all.
    const std::size_t index = static_cast<std::size_t>(r) +
                              static_cast<std::size_t>(g) * static_cast<std::size_t>(size_) +
                              static_cast<std::size_t>(b) * static_cast<std::size_t>(size_) *
                                  static_cast<std::size_t>(size_);
    if (index >= entries_.size()) {
        return LutEntry{};
    }
    return entries_[index];
}

bool CubeLut::isIdentity(float tolerance) const noexcept {
    if (empty()) {
        return false;  // an absent table is not the identity; it is nothing
    }
    const float last = static_cast<float>(size_ - 1);
    for (int b = 0; b < size_; ++b) {
        for (int g = 0; g < size_; ++g) {
            for (int r = 0; r < size_; ++r) {
                const LutEntry e = at(r, g, b);
                if (std::fabs(e.r - static_cast<float>(r) / last) > tolerance ||
                    std::fabs(e.g - static_cast<float>(g) / last) > tolerance ||
                    std::fabs(e.b - static_cast<float>(b) / last) > tolerance) {
                    return false;
                }
            }
        }
    }
    return true;
}

LutEntry CubeLut::sample(float r, float g, float b) const noexcept {
    if (empty()) {
        return LutEntry{r, g, b};  // no table is the identity transform
    }
    const auto clamp01 = [](float v) noexcept {
        // Clamped, not extrapolated: continuing a LUT's edge gradient past its domain is
        // how a look blows a highlight nobody asked for.
        if (!(v > 0.0f)) return 0.0f;  // also catches NaN
        return v < 1.0f ? v : 1.0f;
    };
    const float last = static_cast<float>(size_ - 1);
    const float fr = clamp01(r) * last;
    const float fg = clamp01(g) * last;
    const float fb = clamp01(b) * last;

    const int r0 = static_cast<int>(fr);
    const int g0 = static_cast<int>(fg);
    const int b0 = static_cast<int>(fb);
    const int r1 = r0 + 1 < size_ ? r0 + 1 : r0;
    const int g1 = g0 + 1 < size_ ? g0 + 1 : g0;
    const int b1 = b0 + 1 < size_ ? b0 + 1 : b0;
    const float dr = fr - static_cast<float>(r0);
    const float dg = fg - static_cast<float>(g0);
    const float db = fb - static_cast<float>(b0);

    const auto lerp = [](float a, float bb, float t) noexcept { return a + (bb - a) * t; };
    const auto blend = [&](float LutEntry::*channel) noexcept {
        const float c000 = at(r0, g0, b0).*channel;
        const float c100 = at(r1, g0, b0).*channel;
        const float c010 = at(r0, g1, b0).*channel;
        const float c110 = at(r1, g1, b0).*channel;
        const float c001 = at(r0, g0, b1).*channel;
        const float c101 = at(r1, g0, b1).*channel;
        const float c011 = at(r0, g1, b1).*channel;
        const float c111 = at(r1, g1, b1).*channel;
        const float c00 = lerp(c000, c100, dr);
        const float c10 = lerp(c010, c110, dr);
        const float c01 = lerp(c001, c101, dr);
        const float c11 = lerp(c011, c111, dr);
        return lerp(lerp(c00, c10, dg), lerp(c01, c11, dg), db);
    };
    return LutEntry{blend(&LutEntry::r), blend(&LutEntry::g), blend(&LutEntry::b)};
}

CubeLut identityCubeLut(int size) {
    if (size < CubeLut::kMinSize) {
        size = CubeLut::kMinSize;
    }
    const float last = static_cast<float>(size - 1);
    std::vector<LutEntry> entries;
    entries.reserve(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                    static_cast<std::size_t>(size));
    // Red fastest, matching at()'s index order.
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                entries.push_back(LutEntry{static_cast<float>(r) / last,
                                           static_cast<float>(g) / last,
                                           static_cast<float>(b) / last});
            }
        }
    }
    return CubeLut{size, std::move(entries)};
}

Result<CubeLut> parseCubeLut(std::string_view text) {
    long                  declaredSize = 0;
    bool                  sawSize = false;
    std::vector<LutEntry> entries;

    std::size_t pos = 0;
    std::size_t lineNumber = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lineNumber;

        const std::string_view line = trimmed(raw);
        if (line.empty()) {
            continue;  // blank lines and comment-only lines are legal
        }
        const std::vector<std::string_view> parts = fields(line);
        if (parts.empty()) {
            continue;
        }

        if (parts[0] == "TITLE") {
            continue;  // present in vendor files; carries no transform
        }
        if (parts[0] == "LUT_1D_SIZE") {
            return err<CubeLut>(malformed("this is a 1D LUT; only 3D LUTs are supported"));
        }
        if (parts[0] == "LUT_3D_SIZE") {
            if (sawSize) {
                return err<CubeLut>(malformed("LUT_3D_SIZE declared more than once"));
            }
            if (parts.size() != 2 || !parseInt(parts[1], declaredSize)) {
                return err<CubeLut>(
                    malformed("LUT_3D_SIZE on line " + std::to_string(lineNumber) +
                              " is not a single integer"));
            }
            if (declaredSize < CubeLut::kMinSize || declaredSize > CubeLut::kMaxSize) {
                return err<CubeLut>(malformed("LUT_3D_SIZE " + std::to_string(declaredSize) +
                                              " is outside the supported range " +
                                              std::to_string(CubeLut::kMinSize) + ".." +
                                              std::to_string(CubeLut::kMaxSize)));
            }
            sawSize = true;
            continue;
        }
        if (parts[0] == "DOMAIN_MIN" || parts[0] == "DOMAIN_MAX") {
            // Accepted only at the default 0..1. Silently ignoring a non-default domain
            // would apply the look at the wrong scale, which reads as a bad LUT rather
            // than as unsupported input.
            const float expected = parts[0] == "DOMAIN_MIN" ? 0.0f : 1.0f;
            if (parts.size() != 4) {
                return err<CubeLut>(malformed(std::string(parts[0]) + " on line " +
                                              std::to_string(lineNumber) +
                                              " does not carry three numbers"));
            }
            for (std::size_t i = 1; i < 4; ++i) {
                float value = 0.0f;
                if (!parseFloat(parts[i], value) || value != expected) {
                    return err<CubeLut>(
                        malformed("a non-default " + std::string(parts[0]) +
                                  " is not supported; only the 0..1 domain is"));
                }
            }
            continue;
        }

        // Anything else must be a data row: exactly three numbers in 0..1.
        if (!sawSize) {
            return err<CubeLut>(
                malformed("data row on line " + std::to_string(lineNumber) +
                          " appears before LUT_3D_SIZE"));
        }
        if (parts.size() != 3) {
            return err<CubeLut>(malformed("line " + std::to_string(lineNumber) + " has " +
                                          std::to_string(parts.size()) +
                                          " field(s); a data row must have exactly 3"));
        }
        LutEntry entry;
        float* const channels[3] = {&entry.r, &entry.g, &entry.b};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!parseFloat(parts[i], *channels[i])) {
                return err<CubeLut>(malformed("line " + std::to_string(lineNumber) + " field " +
                                              std::to_string(i + 1) + " ('" +
                                              std::string(parts[i]) + "') is not a number"));
            }
            if (*channels[i] < 0.0f || *channels[i] > 1.0f) {
                return err<CubeLut>(malformed("line " + std::to_string(lineNumber) + " field " +
                                              std::to_string(i + 1) +
                                              " is outside the 0..1 domain"));
            }
        }
        entries.push_back(entry);
    }

    if (!sawSize) {
        return err<CubeLut>(malformed("no LUT_3D_SIZE declaration"));
    }
    const std::size_t expected = static_cast<std::size_t>(declaredSize) *
                                 static_cast<std::size_t>(declaredSize) *
                                 static_cast<std::size_t>(declaredSize);
    if (entries.size() != expected) {
        // Requirement 7.5, and checked in BOTH directions: too few rows is a truncated
        // download, too many is a concatenated file, and either would render most of the
        // image right and part of it wrong.
        return err<CubeLut>(malformed("LUT_3D_SIZE " + std::to_string(declaredSize) +
                                      " needs " + std::to_string(expected) +
                                      " data rows but the file has " +
                                      std::to_string(entries.size())));
    }
    return ok(CubeLut{static_cast<int>(declaredSize), std::move(entries)});
}

}  // namespace palmier::gpu
