// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/PlatformCompatibility.hpp — Qt-free launch-time platform compatibility check.
//
// Requirement 1 governs whether Palmier_Pro_Linux may show its editor on the host:
//   1.1  runs on x86-64 (and, per the port, aarch64) Linux with glibc >= 2.31.
//   1.3  on a supported host the editor appears within 15 seconds WITHOUT a network
//        connection.
//   1.4  if the minimum runtime dependencies are not met, show a message that names
//        EACH missing dependency and exit without the editor.
//   1.5  on an unsupported CPU architecture or glibc < 2.31, show a message that the
//        platform is unsupported and exit without the editor.
//   13.3/13.4 the open-source editor must start and run with NO network connection.
//
// The *decision logic* that turns a set of observed platform facts into a
// pass/fail verdict (naming each unmet item) is plain C++ with no Qt, no FFmpeg,
// no Vulkan, and — critically — no I/O of any kind. It is therefore trivially
// unit-testable on a machine without Qt or a GPU (as in CI/the sandbox), and it
// cannot perform a network call, satisfying the "start without a network
// connection" requirement by construction: the launch gate never touches the
// network.
//
// The observed facts (CPU architecture, glibc version, which required shared
// libraries are loadable) are gathered by a separate, injectable probe so tests
// can drive supported/unsupported cases deterministically. The real probe
// (probePlatformFacts) uses uname(2), the glibc version, and dlopen(3); it is
// guarded so this translation unit still builds where those are unavailable,
// mirroring the project's PALMIER_HAVE_VULKAN / PALMIER_HAVE_FFMPEG guard style.

#ifndef PALMIER_APP_PLATFORMCOMPATIBILITY_HPP
#define PALMIER_APP_PLATFORMCOMPATIBILITY_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Duration.hpp"

namespace palmier::app {

// ---------------------------------------------------------------------------
// glibc version
// ---------------------------------------------------------------------------

/// A parsed glibc (GNU C Library) version, e.g. {2, 31}. Only major/minor are
/// meaningful for the minimum-version gate (Requirements 1.1, 1.5).
struct GlibcVersion {
    unsigned major = 0;
    unsigned minor = 0;

    /// Lexicographic (major, then minor) ordering so `>=` expresses the gate.
    friend constexpr auto operator<=>(const GlibcVersion&,
                                      const GlibcVersion&) = default;

    /// "2.31" style rendering for user-facing messages.
    [[nodiscard]] std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor);
    }
};

/// Parse a glibc version string such as "2.31" or "2.35.9000" (extra components
/// are ignored). Returns std::nullopt when the leading "<major>.<minor>" cannot
/// be read.
[[nodiscard]] std::optional<GlibcVersion> parseGlibcVersion(const std::string& text);

// ---------------------------------------------------------------------------
// Requirements the host is checked against
// ---------------------------------------------------------------------------

/// A shared library the editor needs present at runtime. `name` is the
/// human-facing dependency name used in the "missing dependency" message
/// (Requirement 1.4); `soname` is what the probe attempts to load.
struct RuntimeDependency {
    std::string name;    ///< e.g. "Qt 6 Widgets"
    std::string soname;  ///< e.g. "libQt6Widgets.so.6"
};

/// The platform contract the host must satisfy to run the editor. Defaults come
/// from defaultRequirements(); tests may construct narrower contracts.
struct CompatibilityRequirements {
    /// Accepted CPU architectures (as reported by uname -m). The macOS edition
    /// targets Apple Silicon; the Linux port supports 64-bit x86 and ARM.
    std::vector<std::string> supportedArchitectures;

    /// Minimum glibc version (Requirement 1.1: 2.31 or later).
    GlibcVersion minGlibc;

    /// Shared libraries that must be loadable at launch (Requirement 1.4).
    std::vector<RuntimeDependency> requiredDependencies;
};

/// The project's default platform contract: x86_64 / aarch64, glibc >= 2.31, and
/// the runtime shared libraries the editor and MCP server link (Qt 6, FFmpeg,
/// Vulkan, shaderc, lcms2, libsecret) — see design.md "Dependencies".
[[nodiscard]] const CompatibilityRequirements& defaultRequirements();

/// The maximum time the editor is permitted to take to appear on a supported
/// host (Requirement 1.3). Exposed so the shell can budget/measure startup.
inline constexpr Duration kLaunchBudget = Duration::fromMilliseconds(15'000);

// ---------------------------------------------------------------------------
// Observed platform facts (produced by an injectable probe)
// ---------------------------------------------------------------------------

/// A snapshot of the host as observed at launch. Kept as plain data so the
/// verdict logic is a pure function of it and tests can supply any scenario.
struct PlatformFacts {
    /// CPU architecture as reported by the OS (uname -m), e.g. "x86_64",
    /// "aarch64", "i686". Empty when it could not be determined.
    std::string architecture;

    /// Detected glibc version, or std::nullopt when the C library is not glibc
    /// or its version could not be read (treated as failing the glibc gate).
    std::optional<GlibcVersion> glibc;

    /// The sonames (from the requirements' requiredDependencies) that were found
    /// loadable. A required dependency whose soname is absent from this list is
    /// reported as missing by name (Requirement 1.4).
    std::vector<std::string> loadableSonames;
};

/// Probe the real host for the facts needed to evaluate `requirements`.
///
/// Reads the CPU architecture (uname), the glibc version, and attempts to load
/// each required soname (dlopen). Performs NO network activity. Where the
/// underlying facilities are unavailable at build time the corresponding fact is
/// left empty/nullopt (which the verdict treats conservatively as unmet), so
/// this function always compiles.
[[nodiscard]] PlatformFacts probePlatformFacts(
    const CompatibilityRequirements& requirements);

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

/// The outcome of checking PlatformFacts against CompatibilityRequirements.
///
/// Distinguishes the two Requirement-1 failure modes so the shell can craft the
/// right message: an unsupported *platform* (architecture or glibc — Requirement
/// 1.5) versus missing runtime *dependencies* named individually (Requirement
/// 1.4). When compatible() is true the editor may launch (Requirement 1.3).
struct CompatibilityReport {
    /// The architecture that was observed (echoed for messaging).
    std::string architecture;
    /// True when `architecture` is one of the supported architectures (1.1/1.5).
    bool architectureSupported = false;

    /// The glibc version that was observed, if any (echoed for messaging).
    std::optional<GlibcVersion> detectedGlibc;
    /// The minimum glibc required (echoed for messaging).
    GlibcVersion requiredGlibc;
    /// True when a glibc version was detected and it meets the minimum (1.1/1.5).
    bool glibcSupported = false;

    /// Human-facing names of every required dependency found missing (1.4). Empty
    /// when all required dependencies are present.
    std::vector<std::string> missingDependencies;

    /// True when the CPU architecture is unsupported or glibc is too old (or
    /// undetectable) — the Requirement 1.5 failure mode.
    [[nodiscard]] bool unsupportedPlatform() const noexcept {
        return !architectureSupported || !glibcSupported;
    }

    /// True when the platform is supported but one or more required runtime
    /// dependencies are missing — the Requirement 1.4 failure mode.
    [[nodiscard]] bool hasMissingDependencies() const noexcept {
        return !missingDependencies.empty();
    }

    /// True only when the host is fully compatible and the editor may launch.
    [[nodiscard]] bool compatible() const noexcept {
        return !unsupportedPlatform() && !hasMissingDependencies();
    }

    /// One human-readable line per unmet item: the unsupported architecture, the
    /// too-old/undetectable glibc, and each missing dependency by name. Empty
    /// when compatible(). Used to build the exit message (Requirements 1.4, 1.5).
    [[nodiscard]] std::vector<std::string> issues() const;

    /// A single multi-line message summarizing every unmet item, suitable for a
    /// dialog or stderr. Empty when compatible().
    [[nodiscard]] std::string message() const;
};

/// Pure verdict: evaluate observed `facts` against `requirements`.
///
/// No I/O, no network, deterministic — this is the unit-testable heart of the
/// launch gate. A glibc that was not detected (std::nullopt) fails the glibc
/// gate conservatively.
[[nodiscard]] CompatibilityReport evaluateCompatibility(
    const PlatformFacts& facts, const CompatibilityRequirements& requirements);

/// Convenience: probe the real host and evaluate it against `requirements`
/// (defaulting to defaultRequirements()). Combines probePlatformFacts() and
/// evaluateCompatibility(); performs no network activity.
[[nodiscard]] CompatibilityReport checkPlatformCompatibility();
[[nodiscard]] CompatibilityReport checkPlatformCompatibility(
    const CompatibilityRequirements& requirements);

} // namespace palmier::app

#endif // PALMIER_APP_PLATFORMCOMPATIBILITY_HPP
