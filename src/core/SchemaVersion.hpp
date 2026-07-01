// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/SchemaVersion.hpp — the .palmier project schema version.
//
// Project.version carries a SchemaVersion for forward/backward compatibility; a
// project's validation rule requires "version is a known, supported schema
// version" (design.md Data Models / Validation rules). Persistence (task 5.x)
// reads this to decide whether a stored project can be loaded. The scheme is
// major.minor with the standard semantic-versioning compatibility rule: a reader
// supports a stored version iff the major numbers match and the reader's minor is
// >= the stored minor.

#ifndef PALMIER_CORE_SCHEMAVERSION_HPP
#define PALMIER_CORE_SCHEMAVERSION_HPP

#include <cstdint>
#include <compare>
#include <optional>
#include <string>

namespace palmier {

struct SchemaVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;

    constexpr SchemaVersion() = default;
    constexpr SchemaVersion(std::uint32_t major_, std::uint32_t minor_) noexcept
        : major(major_), minor(minor_) {}

    /// The schema version this build writes and can fully round-trip.
    [[nodiscard]] static constexpr SchemaVersion current() noexcept {
        return SchemaVersion{1, 0};
    }

    /// Can a reader at `reader` load data written at `stored`?
    /// Same major, and the reader is at least as new (minor) as the data.
    [[nodiscard]] static constexpr bool isCompatible(SchemaVersion reader,
                                                     SchemaVersion stored) noexcept {
        return reader.major == stored.major && reader.minor >= stored.minor;
    }

    /// True iff this build (current()) can load a project stored at `*this`.
    [[nodiscard]] constexpr bool isSupported() const noexcept {
        return isCompatible(current(), *this);
    }

    /// Canonical "major.minor" rendering (e.g. "1.0").
    [[nodiscard]] std::string toString() const {
        return std::to_string(major) + '.' + std::to_string(minor);
    }

    /// Parse a "major.minor" string; std::nullopt if malformed.
    [[nodiscard]] static std::optional<SchemaVersion> parse(std::string_view text);

    [[nodiscard]] friend constexpr auto operator<=>(SchemaVersion, SchemaVersion) = default;
    [[nodiscard]] friend constexpr bool operator==(SchemaVersion, SchemaVersion) = default;
};

} // namespace palmier

#endif // PALMIER_CORE_SCHEMAVERSION_HPP
