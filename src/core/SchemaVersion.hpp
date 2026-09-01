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
//
// The current version is 1.5. Its addition over 1.4 is Effect::resourcePath
// (monitoring-and-grading task 7; Requirement 7.2): one optional string field on
// an effect naming an external resource, which the LUT effect uses to carry a
// `.cube` file's path. Like tags before it, an effect carrying no
// "resourcePath" key at all (every 1.0-1.4 document) is unaffected, so a 1.5
// build reads every earlier document unchanged with an empty path, and a 1.4
// build reads a 1.5 document unchanged too — an unrecognised extra key on an
// effect object is ignored rather than rejected.
//
// That last part is only true for an effect whose TYPE the older reader knows.
// A 1.5 document carrying a `lut` effect meets the same rule every earlier
// addition established: an unrecognised effect kind is rejected, because
// rendering a clip while silently dropping a look the colourist applied is
// worse than refusing to open it. So the compatibility story is the same as
// tags' for the FIELD and the same as `invert_colors`' for the TYPE.
//
// A LUT is deliberately NOT modelled as a core::MediaAssetRef (Requirement 7.2,
// audit finding 14): an asset is timeline content with a duration, a decoder and
// a media library entry, and a LUT is none of those. Reusing the type would put
// LUTs in the media browser and give them import semantics they cannot satisfy.
//
// The version PRIOR to 1.5 was 1.4. Its addition over 1.3 is MediaAssetRef::tags
// (usable-editor tasks.md task 15; no dedicated Requirement): an asset carrying
// no "tags" key at all (every 1.0/1.1/1.2/1.3 document) is unaffected, so a 1.4
// build reads every earlier document unchanged with an empty tag list, and a
// 1.3 build reads a 1.4 document unchanged too, since an unrecognised extra
// key on an asset object is simply ignored rather than rejected — unlike each
// PRIOR addition (a new track/clip KIND a reader must recognise or reject),
// tags are a per-asset field a reader with no notion of them can harmlessly
// skip, which is why this is the first addition in the chain that does not
// need the older reader to REJECT the newer document.
//
// The version PRIOR to 1.4 was 1.3. Its addition over 1.2 is Clip::captionText
// and the TrackKind::Caption track kind it requires (usable-editor task 13;
// Requirement 10): a clip carrying no captionText key at all (every
// 1.0/1.1/1.2 document) is unaffected, so a 1.3 build reads every earlier
// document unchanged, and a 1.2 build rejects a 1.3 document as unsupported
// the moment it meets an unrecognised "caption" track kind — the same
// cross-version behaviour 1.2's own addition (Clip::textStyle,
// TrackKind::Text) already established for a 1.1 reader, which itself
// extended what 1.1's own additions (`tracks[].name`, `clipGroups`,
// `invert_colors`) established for a 1.0 reader.

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
        return SchemaVersion{1, 5};
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
