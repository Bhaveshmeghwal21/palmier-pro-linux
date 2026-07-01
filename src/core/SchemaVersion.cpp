// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/SchemaVersion.cpp — parsing of the "major.minor" schema version string.

#include "core/SchemaVersion.hpp"

#include <charconv>
#include <cstdint>

namespace palmier {

std::optional<SchemaVersion> SchemaVersion::parse(std::string_view text) {
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
        return std::nullopt;
    }

    const std::string_view majorText = text.substr(0, dot);
    const std::string_view minorText = text.substr(dot + 1);

    std::uint32_t major = 0;
    std::uint32_t minor = 0;

    auto parseUint = [](std::string_view sv, std::uint32_t& out) -> bool {
        // Reject leading '+'/'-' or any non-digit by relying on from_chars,
        // then confirm the whole span was consumed.
        const char* begin = sv.data();
        const char* end = sv.data() + sv.size();
        const auto [ptr, ec] = std::from_chars(begin, end, out);
        return ec == std::errc{} && ptr == end;
    };

    if (!parseUint(majorText, major) || !parseUint(minorText, minor)) {
        return std::nullopt;
    }
    return SchemaVersion{major, minor};
}

} // namespace palmier
