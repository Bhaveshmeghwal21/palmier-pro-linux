// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Uuid.hpp — a 128-bit RFC 4122 universally unique identifier.
//
// Projects, tracks, clips, and media assets are keyed by stable identifiers
// (design.md Data Models: Project.id / Track.id are Uuid; Clip.id / ClipId and
// MediaAssetRef build on the same identity primitive). Uuid stores 16 raw bytes,
// supports random (version 4) generation, canonical 8-4-4-4-12 hex formatting and
// parsing, ordering, and std::hash so it can key unordered containers.

#ifndef PALMIER_CORE_UUID_HPP
#define PALMIER_CORE_UUID_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace palmier {

class Uuid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    /// Constructs the nil UUID (all zero bytes).
    constexpr Uuid() = default;

    /// Constructs from 16 raw bytes.
    explicit constexpr Uuid(const Bytes& bytes) : bytes_(bytes) {}

    /// Generates a new random (RFC 4122 version 4) UUID.
    [[nodiscard]] static Uuid generateV4();

    /// Parses the canonical 8-4-4-4-12 hex form (with or without surrounding
    /// braces). Returns std::nullopt if the text is not a well-formed UUID.
    [[nodiscard]] static std::optional<Uuid> parse(std::string_view text);

    /// Canonical lowercase 8-4-4-4-12 rendering.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

    /// True iff every byte is zero (the nil UUID).
    [[nodiscard]] bool isNil() const noexcept;

    [[nodiscard]] friend constexpr auto operator<=>(const Uuid&, const Uuid&) = default;
    [[nodiscard]] friend constexpr bool operator==(const Uuid&, const Uuid&) = default;

private:
    Bytes bytes_{}; // value-initialized to all zeros (nil)
};

} // namespace palmier

// Enable use as a key in unordered_map / unordered_set.
template <>
struct std::hash<palmier::Uuid> {
    std::size_t operator()(const palmier::Uuid& id) const noexcept;
};

#endif // PALMIER_CORE_UUID_HPP
