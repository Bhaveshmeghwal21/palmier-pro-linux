// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/PlatformCompatibility.cpp — implementation of the launch-time platform
// compatibility check (Requirements 1.1, 1.3, 1.4, 1.5, 13.3, 13.4).
//
// The verdict logic (parseGlibcVersion / evaluateCompatibility / report
// rendering) is pure and dependency-free. The real host probe uses uname(2), the
// glibc version, and dlopen(3); each is guarded so this translation unit builds
// even where a facility is missing, mirroring the project's PALMIER_HAVE_VULKAN /
// PALMIER_HAVE_FFMPEG guard style. Nothing here touches the network, which is how
// the launch gate honors "start without a network connection" (13.3/13.4).

#include "app/PlatformCompatibility.hpp"

#include <algorithm>
#include <cctype>

// Real-host probe facilities. glibc exposes gnu_get_libc_version() via
// <gnu/libc-version.h> and defines __GLIBC__; uname(2) reports the CPU
// architecture; dlopen(3) tests whether a shared library is loadable. These are
// only compiled into the real probe path and are unnecessary for the pure
// verdict logic the unit tests exercise.
#if defined(__has_include)
#  if __has_include(<sys/utsname.h>)
#    include <sys/utsname.h>
#    define PALMIER_HAVE_UNAME 1
#  endif
#  if __has_include(<dlfcn.h>)
#    include <dlfcn.h>
#    define PALMIER_HAVE_DLOPEN 1
#  endif
#  if defined(__GLIBC__) && __has_include(<gnu/libc-version.h>)
#    include <gnu/libc-version.h>
#    define PALMIER_HAVE_GLIBC_VERSION 1
#  endif
#endif

namespace palmier::app {

// ---------------------------------------------------------------------------
// glibc version parsing
// ---------------------------------------------------------------------------

std::optional<GlibcVersion> parseGlibcVersion(const std::string& text) {
    // Expect "<major>.<minor>" with optional trailing ".<patch>..." or suffix.
    std::size_t i = 0;
    const std::size_t n = text.size();

    auto readNumber = [&](unsigned& out) -> bool {
        std::size_t start = i;
        unsigned value = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + static_cast<unsigned>(text[i] - '0');
            ++i;
        }
        if (i == start) return false;  // no digits consumed
        out = value;
        return true;
    };

    // Skip any leading whitespace.
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;

    GlibcVersion version;
    if (!readNumber(version.major)) return std::nullopt;
    if (i >= n || text[i] != '.') return std::nullopt;
    ++i;  // consume '.'
    if (!readNumber(version.minor)) return std::nullopt;
    return version;
}

// ---------------------------------------------------------------------------
// Default platform contract
// ---------------------------------------------------------------------------

const CompatibilityRequirements& defaultRequirements() {
    // Constructed once, on first use. The required sonames are the runtime shared
    // libraries the open-source editor + MCP server link (design.md
    // "Dependencies"); vendor HW-codec libraries are intentionally NOT required
    // here because the editor degrades gracefully to the software path when they
    // are absent (Requirement 10.4), so their absence must not block launch.
    static const CompatibilityRequirements kRequirements = [] {
        CompatibilityRequirements req;
        req.supportedArchitectures = {"x86_64", "aarch64"};
        req.minGlibc = GlibcVersion{2, 31};
        req.requiredDependencies = {
            {"Qt 6 Core", "libQt6Core.so.6"},
            {"Qt 6 Gui", "libQt6Gui.so.6"},
            {"Qt 6 Widgets", "libQt6Widgets.so.6"},
            {"FFmpeg libavcodec", "libavcodec.so"},
            {"FFmpeg libavformat", "libavformat.so"},
            {"FFmpeg libavutil", "libavutil.so"},
            {"Vulkan loader", "libvulkan.so.1"},
            {"LittleCMS (lcms2)", "liblcms2.so.2"},
            {"libsecret", "libsecret-1.so.0"},
        };
        return req;
    }();
    return kRequirements;
}

// ---------------------------------------------------------------------------
// Pure verdict
// ---------------------------------------------------------------------------

CompatibilityReport evaluateCompatibility(
    const PlatformFacts& facts, const CompatibilityRequirements& requirements) {
    CompatibilityReport report;

    // --- Architecture (Requirements 1.1 / 1.5) -----------------------------
    report.architecture = facts.architecture;
    report.architectureSupported =
        std::find(requirements.supportedArchitectures.begin(),
                  requirements.supportedArchitectures.end(),
                  facts.architecture) != requirements.supportedArchitectures.end();

    // --- glibc (Requirements 1.1 / 1.5) ------------------------------------
    report.detectedGlibc = facts.glibc;
    report.requiredGlibc = requirements.minGlibc;
    // An undetectable glibc fails the gate conservatively.
    report.glibcSupported =
        facts.glibc.has_value() && *facts.glibc >= requirements.minGlibc;

    // --- Runtime dependencies (Requirement 1.4) ----------------------------
    for (const RuntimeDependency& dep : requirements.requiredDependencies) {
        const bool present =
            std::find(facts.loadableSonames.begin(), facts.loadableSonames.end(),
                      dep.soname) != facts.loadableSonames.end();
        if (!present) {
            report.missingDependencies.push_back(dep.name);
        }
    }

    return report;
}

// ---------------------------------------------------------------------------
// Report rendering
// ---------------------------------------------------------------------------

std::vector<std::string> CompatibilityReport::issues() const {
    std::vector<std::string> lines;
    if (compatible()) return lines;

    if (!architectureSupported) {
        std::string arch = architecture.empty() ? "unknown" : architecture;
        lines.push_back("Unsupported CPU architecture: " + arch +
                        " (supported: x86_64, aarch64).");
    }
    if (!glibcSupported) {
        if (detectedGlibc.has_value()) {
            lines.push_back("glibc " + detectedGlibc->toString() +
                            " is too old (requires " + requiredGlibc.toString() +
                            " or later).");
        } else {
            lines.push_back(
                "glibc version could not be determined (requires " +
                requiredGlibc.toString() + " or later).");
        }
    }
    for (const std::string& dep : missingDependencies) {
        lines.push_back("Missing required dependency: " + dep + ".");
    }
    return lines;
}

std::string CompatibilityReport::message() const {
    const std::vector<std::string> lines = issues();
    if (lines.empty()) return {};

    std::string header = unsupportedPlatform()
                             ? "Palmier Pro cannot start on this platform:"
                             : "Palmier Pro is missing required dependencies:";
    std::string out = header;
    for (const std::string& line : lines) {
        out += "\n  - ";
        out += line;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Real-host probe (guarded facilities; no network activity)
// ---------------------------------------------------------------------------

namespace {

std::string probeArchitecture() {
#if defined(PALMIER_HAVE_UNAME)
    struct utsname info{};
    if (::uname(&info) == 0) {
        return std::string(info.machine);
    }
#endif
    // Compile-time fallback when uname is unavailable: infer from the target the
    // binary was built for. This is only reached where <sys/utsname.h> is absent.
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__i386__)
    return "i686";
#else
    return {};
#endif
}

std::optional<GlibcVersion> probeGlibc() {
#if defined(PALMIER_HAVE_GLIBC_VERSION)
    return parseGlibcVersion(std::string(::gnu_get_libc_version()));
#elif defined(__GLIBC__)
    // Headers expose the compile-time glibc version macros even without the
    // runtime accessor; use them as a floor.
    return GlibcVersion{static_cast<unsigned>(__GLIBC__),
                        static_cast<unsigned>(__GLIBC_MINOR__)};
#else
    return std::nullopt;
#endif
}

bool sonameLoadable([[maybe_unused]] const std::string& soname) {
#if defined(PALMIER_HAVE_DLOPEN)
    // RTLD_LAZY loads the library without resolving every symbol; RTLD_NOLOAD is
    // not used because we want to detect availability on disk, not just already
    // loaded libraries. dlopen performs no network activity.
    void* handle = ::dlopen(soname.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) {
        ::dlclose(handle);
        return true;
    }
    return false;
#else
    // Without dlopen we cannot probe; report unavailable so the dependency is
    // named as missing rather than silently assumed present.
    return false;
#endif
}

} // namespace

PlatformFacts probePlatformFacts(const CompatibilityRequirements& requirements) {
    PlatformFacts facts;
    facts.architecture = probeArchitecture();
    facts.glibc = probeGlibc();
    for (const RuntimeDependency& dep : requirements.requiredDependencies) {
        if (sonameLoadable(dep.soname)) {
            facts.loadableSonames.push_back(dep.soname);
        }
    }
    return facts;
}

CompatibilityReport checkPlatformCompatibility(
    const CompatibilityRequirements& requirements) {
    return evaluateCompatibility(probePlatformFacts(requirements), requirements);
}

CompatibilityReport checkPlatformCompatibility() {
    return checkPlatformCompatibility(defaultRequirements());
}

} // namespace palmier::app
