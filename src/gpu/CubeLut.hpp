// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/CubeLut.hpp — a `.cube` 3D LUT table and its parser
// (monitoring-and-grading task 7; Requirement 7.4, 7.5, 7.6, 7.7).
//
// The parser is a pure function of TEXT, not of a path. Reading the file is the caller's
// job, which keeps every parsing rule testable without touching a filesystem and keeps
// Requirement 7.8's "a missing LUT renders un-graded and reports the path" a question
// about I/O rather than about parsing.
//
// WHAT IS ACCEPTED. Requirement 7.4 asks for "the format's documented essentials":
// LUT_3D_SIZE, the data table, comments (#) and blank lines. Also accepted, because they
// appear in real vendor files and ignoring them would reject a valid LUT: TITLE, and
// DOMAIN_MIN / DOMAIN_MAX when they are the default 0..1. A non-default domain is
// REJECTED rather than silently ignored — quietly treating a 0..4 HDR domain as 0..1
// would apply the look at the wrong scale, which looks like a bad LUT rather than like
// unsupported input.
//
// WHAT IS REJECTED, each naming the fault (7.4): a missing or non-positive LUT_3D_SIZE, a
// size outside the format's own 2..256 bounds, a row that is not three numbers, a row
// carrying a value outside 0..1, a declared size disagreeing with the actual row count in
// EITHER direction (7.5), a 1D LUT (LUT_1D_SIZE), and a second LUT_3D_SIZE.
//
// A partially read table is never returned. That is the point of 7.4's "rather than
// applying a partially read table": a LUT missing its last rows would render most of the
// image correctly and the highlights wrongly, which is far harder to diagnose than a
// refusal.

#ifndef PALMIER_GPU_CUBELUT_HPP
#define PALMIER_GPU_CUBELUT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"

namespace palmier::gpu {

/// One RGB triple of a LUT table, in the 0..1 domain the `.cube` format defines.
struct LutEntry {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    [[nodiscard]] bool operator==(const LutEntry&) const = default;
};

/// A parsed 3D LUT: `size^3` entries, indexed with RED VARYING FASTEST.
///
/// That index order is the `.cube` format's own and is the single most likely thing to get
/// wrong, because a transposed table still produces a plausible-looking image — so
/// `at(r,g,b)` exists rather than leaving callers to compute the offset.
class CubeLut {
public:
    /// The format's own bounds on LUT_3D_SIZE. 2 is the smallest table that defines a
    /// trilinear interpolation at all; 256 is where a table stops being smaller than the
    /// image it transforms.
    static constexpr int kMinSize = 2;
    static constexpr int kMaxSize = 256;

    CubeLut() = default;
    CubeLut(int size, std::vector<LutEntry> entries)
        : size_(size), entries_(std::move(entries)) {}

    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ <= 0 || entries_.empty(); }
    [[nodiscard]] const std::vector<LutEntry>& entries() const noexcept { return entries_; }

    /// The entry at lattice point (r,g,b). Out of range answers the identity for that
    /// point rather than reading past the end.
    [[nodiscard]] LutEntry at(int r, int g, int b) const noexcept;

    /// Whether this table is the identity to within `tolerance` per channel.
    ///
    /// Requirement 7.7 makes the identity LUT the shared correctness anchor of the parser
    /// and the interpolator, so being able to ASK is worth more than being able to build
    /// one: a table that parsed correctly but transposed is not the identity, and this is
    /// what notices.
    [[nodiscard]] bool isIdentity(float tolerance = 1.0f / 512.0f) const noexcept;

    /// Sample the table by trilinear interpolation at (r,g,b) in 0..1 (Requirement 7.6).
    ///
    /// Inputs are clamped into the domain rather than extrapolated: a LUT describes a
    /// transform over 0..1 and continuing its edge gradient beyond that is how a LUT
    /// blows a highlight nobody asked for.
    [[nodiscard]] LutEntry sample(float r, float g, float b) const noexcept;

private:
    int                   size_ = 0;
    std::vector<LutEntry> entries_;
};

/// The identity LUT of a given size, for tests and for a "no look" default.
[[nodiscard]] CubeLut identityCubeLut(int size);

/// Parse `.cube` text. Errors are InvalidArgument and name the fault (Requirement 7.4).
[[nodiscard]] Result<CubeLut> parseCubeLut(std::string_view text);

}  // namespace palmier::gpu

#endif  // PALMIER_GPU_CUBELUT_HPP
