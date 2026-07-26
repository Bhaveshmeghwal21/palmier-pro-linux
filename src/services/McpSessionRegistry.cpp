// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/McpSessionRegistry.cpp — implementation of MCP session identity, the
// concurrent-session maximum and the idle timeout (task 5.1). See the header for
// the contract and its mapping to design.md D3 and Requirements 9.10, 9.11, 9.14,
// 9.15, 10.9, 10.11.

#include "services/McpSessionRegistry.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace palmier::services {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// One 256-bit value from `std::random_device`, rendered as 64 lowercase hex
/// characters. `std::random_device` is the non-deterministic source (design.md
/// D3); it is constructed per call so no generator state is shared or reused.
std::string randomHex256() {
    std::random_device device;
    std::string        out;
    out.reserve(McpSessionRegistry::kSessionIdLength);
    for (int word = 0; word < 8; ++word) {  // 8 x 32 bits = 256 bits
        const auto value = static_cast<std::uint32_t>(device());
        for (int nibble = 7; nibble >= 0; --nibble) {
            out.push_back(kHexDigits[(value >> (nibble * 4)) & 0xFu]);
        }
    }
    return out;
}

}  // namespace

McpSessionRegistry::McpSessionRegistry() : McpSessionRegistry(Options{}) {}

McpSessionRegistry::McpSessionRegistry(Options options)
    : maxSessions_(std::clamp(options.maxSessions, kMinSessions, kMaxSessions)),
      idleTimeout_(std::clamp(options.idleTimeout, kMinIdleTimeout, kMaxIdleTimeout)),
      clock_(std::move(options.clock)) {}

std::chrono::steady_clock::time_point McpSessionRegistry::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

bool McpSessionRegistry::isWellFormedId(std::string_view id) noexcept {
    if (id.size() != kSessionIdLength) return false;
    for (const char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

Result<std::string> McpSessionRegistry::mintIdLocked() {
    // The retained-issued set makes a collision astronomically unlikely rather
    // than impossible-by-assumption; a bounded retry keeps the guarantee total.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string candidate = randomHex256();
        if (issued_.find(candidate) == issued_.end()) {
            issued_.insert(candidate);
            return candidate;
        }
    }
    return err<std::string>(makeError(
        ErrorCode::Internal,
        "could not mint a unique MCP session identifier after 8 attempts"));
}

Result<std::string> McpSessionRegistry::create(std::string sourceAddress,
                                               std::string protocolVersion) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto timestamp = now();
    expireIdleLocked(timestamp);

    if (live_.size() >= maxSessions_) {
        return err<std::string>(failedPrecondition(
            "the MCP session limit of " + std::to_string(maxSessions_) +
            " concurrent sessions was reached; this request was refused and every "
            "established session remains active"));
    }

    Result<std::string> minted = mintIdLocked();
    if (minted.isError()) return minted;

    McpSessionRecord record;
    record.id = minted.value();
    record.initialized = false;
    record.protocolVersion = std::move(protocolVersion);
    record.createdAt = timestamp;
    record.lastSeen = timestamp;
    record.sourceAddress = std::move(sourceAddress);

    const std::string id = record.id;
    live_.emplace(id, std::move(record));
    return id;
}

Result<McpSessionRecord*> McpSessionRegistry::touch(std::string_view id) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto timestamp = now();

    const auto it = live_.find(std::string(id));
    if (it == live_.end()) {
        return err<McpSessionRecord*>(notFound(
            "MCP session identifier is not recognised; the client must repeat "
            "initialize"));
    }

    // Requirement 10.11: "no request for LONGER than the configured idle timeout"
    // — the comparison is strict, so a session idle for exactly the timeout is
    // still live.
    if (timestamp - it->second.lastSeen > idleTimeout_) {
        live_.erase(it);
        return err<McpSessionRecord*>(makeError(
            ErrorCode::Timeout,
            "MCP session expired after " + std::to_string(idleTimeout_.count()) +
                " seconds of inactivity and is no longer valid; the client must "
                "repeat initialize"));
    }

    it->second.lastSeen = timestamp;
    return &it->second;
}

void McpSessionRegistry::markInitialized(std::string_view id) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = live_.find(std::string(id));
    if (it == live_.end()) return;
    it->second.initialized = true;
    it->second.lastSeen = now();
}

std::size_t McpSessionRegistry::expireIdleLocked(std::chrono::steady_clock::time_point when) {
    std::vector<std::string> expired;
    for (const auto& [id, record] : live_) {
        if (when - record.lastSeen > idleTimeout_) expired.push_back(id);
    }
    for (const std::string& id : expired) live_.erase(id);
    return expired.size();
}

std::size_t McpSessionRegistry::expireIdle() {
    std::lock_guard<std::mutex> guard(mutex_);
    return expireIdleLocked(now());
}

std::size_t McpSessionRegistry::activeCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return live_.size();
}

bool McpSessionRegistry::close(std::string_view id) {
    std::lock_guard<std::mutex> guard(mutex_);
    return live_.erase(std::string(id)) > 0;
}

bool McpSessionRegistry::wasIssued(std::string_view id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return issued_.find(std::string(id)) != issued_.end();
}

std::size_t McpSessionRegistry::issuedCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return issued_.size();
}

}  // namespace palmier::services
