// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/Uuid.cpp — random generation, parsing, formatting, and hashing for Uuid.

#include "core/Uuid.hpp"

#include <cstdint>
#include <cstdio>
#include <random>

namespace palmier {

namespace {

// A thread-local, well-seeded 64-bit PRNG. UUIDv4 needs 122 random bits; two
// 64-bit draws supply them (the version and variant nibbles are then fixed).
std::mt19937_64& engine() {
    static thread_local std::mt19937_64 eng{[] {
        std::random_device rd;
        // Combine several random_device draws into the 64-bit seed.
        std::uint64_t seed = rd();
        seed = (seed << 32) ^ (static_cast<std::uint64_t>(rd()) << 16) ^ rd();
        return seed;
    }()};
    return eng;
}

// Maps a hex character to its 0-15 value, or -1 if it is not a hex digit.
constexpr int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Uuid Uuid::generateV4() {
    const std::uint64_t hi = engine()();
    const std::uint64_t lo = engine()();

    Bytes b{};
    for (int i = 0; i < 8; ++i) {
        b[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((hi >> (8 * (7 - i))) & 0xFF);
        b[static_cast<std::size_t>(8 + i)] =
            static_cast<std::uint8_t>((lo >> (8 * (7 - i))) & 0xFF);
    }

    // Set the version to 4 (bits 12-15 of time_hi_and_version).
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    // Set the variant to RFC 4122 (bits 6-7 of clock_seq_hi_and_reserved).
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);

    return Uuid{b};
}

std::optional<Uuid> Uuid::parse(std::string_view text) {
    // Accept an optional surrounding pair of braces.
    if (text.size() >= 2 && text.front() == '{' && text.back() == '}') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    // Canonical form is exactly 36 chars: 8-4-4-4-12 with hyphens at 8,13,18,23.
    if (text.size() != 36) return std::nullopt;

    constexpr int kHyphenPositions[] = {8, 13, 18, 23};
    for (int pos : kHyphenPositions) {
        if (text[static_cast<std::size_t>(pos)] != '-') return std::nullopt;
    }

    Bytes b{};
    std::size_t byteIndex = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        const int hiNibble = hexValue(text[i]);
        const int loNibble = hexValue(text[i + 1]);
        if (hiNibble < 0 || loNibble < 0) return std::nullopt;
        if (byteIndex >= b.size()) return std::nullopt;
        b[byteIndex++] =
            static_cast<std::uint8_t>((hiNibble << 4) | loNibble);
        i += 2;
    }
    if (byteIndex != b.size()) return std::nullopt;
    return Uuid{b};
}

std::string Uuid::toString() const {
    // 36 characters + terminating NUL for snprintf.
    char buf[37];
    std::snprintf(
        buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes_[0], bytes_[1], bytes_[2], bytes_[3], bytes_[4], bytes_[5],
        bytes_[6], bytes_[7], bytes_[8], bytes_[9], bytes_[10], bytes_[11],
        bytes_[12], bytes_[13], bytes_[14], bytes_[15]);
    return std::string(buf, 36);
}

bool Uuid::isNil() const noexcept {
    for (std::uint8_t byte : bytes_) {
        if (byte != 0) return false;
    }
    return true;
}

} // namespace palmier

std::size_t std::hash<palmier::Uuid>::operator()(const palmier::Uuid& id) const noexcept {
    // FNV-1a over the 16 raw bytes — fast and well-distributed for hash tables.
    const auto& bytes = id.bytes();
    std::uint64_t h = 1469598103934665603ULL; // FNV offset basis
    for (std::uint8_t byte : bytes) {
        h ^= byte;
        h *= 1099511628211ULL; // FNV prime
    }
    return static_cast<std::size_t>(h);
}
