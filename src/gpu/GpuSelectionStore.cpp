// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuSelectionStore.cpp — plain-text persistence of the GPU selection.

#include "gpu/GpuSelectionStore.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include <filesystem>

namespace palmier::gpu {

namespace {

constexpr std::string_view kModeAuto = "auto";
constexpr std::string_view kModePreferVendor = "prefer-vendor";
constexpr std::string_view kModeForceIndex = "force-index";
constexpr std::string_view kModeForceSoftware = "force-software";

[[nodiscard]] std::string_view modeToString(GpuSelectionMode mode) noexcept {
    switch (mode) {
        case GpuSelectionMode::Auto:          return kModeAuto;
        case GpuSelectionMode::PreferVendor:  return kModePreferVendor;
        case GpuSelectionMode::ForceIndex:    return kModeForceIndex;
        case GpuSelectionMode::ForceSoftware: return kModeForceSoftware;
    }
    return kModeAuto;
}

[[nodiscard]] std::optional<GpuSelectionMode> modeFromString(std::string_view s) noexcept {
    if (s == kModeAuto)          return GpuSelectionMode::Auto;
    if (s == kModePreferVendor)  return GpuSelectionMode::PreferVendor;
    if (s == kModeForceIndex)    return GpuSelectionMode::ForceIndex;
    if (s == kModeForceSoftware) return GpuSelectionMode::ForceSoftware;
    return std::nullopt;
}

[[nodiscard]] GpuVendor vendorFromName(std::string_view s) noexcept {
    if (s == "NVIDIA")   return GpuVendor::NVIDIA;
    if (s == "AMD")      return GpuVendor::AMD;
    if (s == "Intel")    return GpuVendor::Intel;
    if (s == "software") return GpuVendor::Software;
    return GpuVendor::Unknown;
}

// Trim leading/trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    std::size_t begin = 0;
    while (begin < s.size() && !notSpace(s[begin])) ++begin;
    std::size_t end = s.size();
    while (end > begin && !notSpace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

} // namespace

std::string defaultGpuSelectionPath() {
    namespace fs = std::filesystem;
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        base = fs::path(home) / ".config";
    } else {
        base = fs::temp_directory_path();
    }
    return (base / "palmier" / "gpu-selection.conf").string();
}

GpuSelectionStore::GpuSelectionStore() : path_(defaultGpuSelectionPath()) {}

GpuSelectionStore::GpuSelectionStore(std::string filePath) : path_(std::move(filePath)) {}

PersistedGpuSelection GpuSelectionStore::fromDevice(const GpuDeviceInfo& device) {
    return PersistedGpuSelection{
        GpuSelectionMode::ForceIndex,
        device.vendor,
        device.index,
        device.name,
    };
}

PersistedGpuSelection GpuSelectionStore::softwareSelection() {
    return PersistedGpuSelection{
        GpuSelectionMode::ForceSoftware,
        GpuVendor::Software,
        -1,
        std::string{vendorName(GpuVendor::Software)},
    };
}

Result<std::optional<PersistedGpuSelection>> GpuSelectionStore::load() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::path(path_), ec) || ec) {
        return std::optional<PersistedGpuSelection>{}; // Never saved: not an error.
    }

    std::ifstream in(path_);
    if (!in) {
        return err<std::optional<PersistedGpuSelection>>(
            makeError(ErrorCode::Io, "unable to open GPU selection file: " + path_));
    }

    PersistedGpuSelection sel;
    bool sawMode = false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view view = trim(line);
        if (view.empty() || view.front() == '#') continue;
        const auto eq = view.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(view.substr(0, eq));
        const std::string_view value = trim(view.substr(eq + 1));

        if (key == "mode") {
            if (const auto m = modeFromString(value)) {
                sel.mode = *m;
                sawMode = true;
            }
        } else if (key == "vendor") {
            sel.vendor = vendorFromName(value);
        } else if (key == "index") {
            std::stringstream ss{std::string(value)};
            int parsed = -1;
            if (ss >> parsed) sel.index = parsed;
        } else if (key == "device") {
            sel.deviceName = std::string(value);
        }
    }

    if (!sawMode) {
        return err<std::optional<PersistedGpuSelection>>(
            makeError(ErrorCode::Io,
                      "GPU selection file is present but unparseable: " + path_));
    }
    return std::optional<PersistedGpuSelection>{sel};
}

Result<void> GpuSelectionStore::save(const PersistedGpuSelection& selection) const {
    namespace fs = std::filesystem;
    const fs::path file(path_);
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return err(makeError(ErrorCode::Io,
                                 "unable to create config directory for " + path_ +
                                     ": " + ec.message()));
        }
    }

    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        return err(makeError(ErrorCode::Io, "unable to write GPU selection file: " + path_));
    }

    out << "# Palmier Pro Linux — persisted GPU selection (Requirement 10.6)\n";
    out << "mode=" << modeToString(selection.mode) << '\n';
    out << "vendor=" << vendorName(selection.vendor) << '\n';
    out << "index=" << selection.index << '\n';
    out << "device=" << selection.deviceName << '\n';
    if (!out) {
        return err(makeError(ErrorCode::Io, "failed while writing GPU selection file: " + path_));
    }
    return ok();
}

} // namespace palmier::gpu
