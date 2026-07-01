// SPDX-License-Identifier: GPL-3.0-or-later
//
// gpu/GpuSelectionStore.hpp — persistence of the user's GPU selection.
//
// Requirement 10.6: when more than one compatible GPU is present the user may
// choose which one is used for acceleration, and that choice must persist across
// restarts. This small store reads/writes the selection to a plain-text config
// file (default: $XDG_CONFIG_HOME/palmier/gpu-selection.conf, falling back to
// ~/.config/...). It has no third-party dependencies so it works headlessly and
// in tests (a caller may point it at any path).
//
// The persisted record captures the chosen selection mode plus enough device
// identity (vendor + name + enumeration index) to re-match the device on the
// next launch even if enumeration order shifts slightly.

#ifndef PALMIER_GPU_GPUSELECTIONSTORE_HPP
#define PALMIER_GPU_GPUSELECTIONSTORE_HPP

#include <optional>
#include <string>

#include "core/Result.hpp"
#include "gpu/GpuTypes.hpp"

namespace palmier::gpu {

/// A persisted GPU selection record.
struct PersistedGpuSelection {
    GpuSelectionMode mode{GpuSelectionMode::Auto};
    GpuVendor        vendor{GpuVendor::Unknown};
    int              index{-1};
    std::string      deviceName{};

    [[nodiscard]] friend bool operator==(const PersistedGpuSelection&,
                                         const PersistedGpuSelection&) = default;
};

/// Reads and writes the user's GPU selection to a config file.
class GpuSelectionStore {
public:
    /// Construct a store backed by the default per-user config path.
    GpuSelectionStore();

    /// Construct a store backed by an explicit file path (used by tests and by
    /// callers that manage their own config location).
    explicit GpuSelectionStore(std::string filePath);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Load the persisted selection. Returns std::nullopt (not an error) when no
    /// selection has ever been saved. Returns an Error only on a genuine read/
    /// parse failure of an existing file.
    [[nodiscard]] Result<std::optional<PersistedGpuSelection>> load() const;

    /// Persist a selection, creating parent directories as needed. Overwrites any
    /// previous selection.
    [[nodiscard]] Result<void> save(const PersistedGpuSelection& selection) const;

    /// Build a persisted record from a chosen device and the policy in effect.
    [[nodiscard]] static PersistedGpuSelection fromDevice(const GpuDeviceInfo& device);

    /// Build a persisted record representing the software fallback selection.
    [[nodiscard]] static PersistedGpuSelection softwareSelection();

private:
    std::string path_;
};

/// Resolve the default per-user config file path for the GPU selection.
[[nodiscard]] std::string defaultGpuSelectionPath();

} // namespace palmier::gpu

#endif // PALMIER_GPU_GPUSELECTIONSTORE_HPP
