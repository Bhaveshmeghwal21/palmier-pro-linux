// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/app/platform_compatibility_test.cpp — unit tests for the Qt-free
// launch-time platform compatibility checker (task 19.1; Requirements 1.1, 1.3,
// 1.4, 1.5, 13.3, 13.4).
//
// These exercise the PURE verdict logic (evaluateCompatibility) with injected
// PlatformFacts so every supported/unsupported scenario is deterministic and
// runs without Qt, a GPU, or the real host's libraries:
//   * a fully supported host passes and yields no issues (1.1, 1.3);
//   * an unsupported CPU architecture is reported as an unsupported platform
//     (1.5);
//   * a glibc older than 2.31 — and an undetectable glibc — is reported as an
//     unsupported platform (1.1, 1.5);
//   * EACH missing runtime dependency is named individually (1.4);
//   * multiple simultaneous failures are all reported together;
//   * the check is a pure function of its inputs (deterministic, no I/O) and
//     completes far within the launch budget, which is how the launch gate
//     honors "start without a network connection" (1.3, 13.3, 13.4).

#include "app/PlatformCompatibility.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using palmier::app::CompatibilityReport;
using palmier::app::CompatibilityRequirements;
using palmier::app::evaluateCompatibility;
using palmier::app::GlibcVersion;
using palmier::app::parseGlibcVersion;
using palmier::app::PlatformFacts;
using palmier::app::RuntimeDependency;

// A small, explicit contract used by most tests: x86_64/aarch64, glibc >= 2.31,
// and two named dependencies. Keeping it local makes the expected messages
// deterministic and independent of the (larger) production default set.
CompatibilityRequirements testRequirements() {
    CompatibilityRequirements req;
    req.supportedArchitectures = {"x86_64", "aarch64"};
    req.minGlibc = GlibcVersion{2, 31};
    req.requiredDependencies = {
        {"Qt 6 Widgets", "libQt6Widgets.so.6"},
        {"FFmpeg libavcodec", "libavcodec.so"},
    };
    return req;
}

// Facts describing a fully supported host for the contract above.
PlatformFacts supportedFacts() {
    PlatformFacts facts;
    facts.architecture = "x86_64";
    facts.glibc = GlibcVersion{2, 35};
    facts.loadableSonames = {"libQt6Widgets.so.6", "libavcodec.so"};
    return facts;
}

bool containsSubstring(const std::vector<std::string>& lines,
                       const std::string& needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

// --- glibc version parsing --------------------------------------------------

TEST(GlibcVersion, ParsesMajorMinorAndIgnoresTrailingComponents) {
    auto v = parseGlibcVersion("2.31");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 2u);
    EXPECT_EQ(v->minor, 31u);

    auto v2 = parseGlibcVersion("2.35.9000");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->major, 2u);
    EXPECT_EQ(v2->minor, 35u);
}

TEST(GlibcVersion, RejectsMalformedStrings) {
    EXPECT_FALSE(parseGlibcVersion("").has_value());
    EXPECT_FALSE(parseGlibcVersion("2").has_value());
    EXPECT_FALSE(parseGlibcVersion("abc").has_value());
    EXPECT_FALSE(parseGlibcVersion("2.").has_value());
}

TEST(GlibcVersion, OrdersLexicographically) {
    EXPECT_TRUE((GlibcVersion{2, 31}) >= (GlibcVersion{2, 31}));
    EXPECT_TRUE((GlibcVersion{2, 35}) >= (GlibcVersion{2, 31}));
    EXPECT_TRUE((GlibcVersion{3, 0}) >= (GlibcVersion{2, 31}));
    EXPECT_FALSE((GlibcVersion{2, 30}) >= (GlibcVersion{2, 31}));
    EXPECT_FALSE((GlibcVersion{1, 99}) >= (GlibcVersion{2, 31}));
}

// --- Supported host (Requirements 1.1, 1.3) --------------------------------

TEST(PlatformCompatibility, SupportedHostIsCompatibleWithNoIssues) {
    const CompatibilityReport report =
        evaluateCompatibility(supportedFacts(), testRequirements());

    EXPECT_TRUE(report.compatible());
    EXPECT_FALSE(report.unsupportedPlatform());
    EXPECT_FALSE(report.hasMissingDependencies());
    EXPECT_TRUE(report.issues().empty());
    EXPECT_TRUE(report.message().empty());
}

TEST(PlatformCompatibility, GlibcExactlyAtMinimumIsSupported) {
    PlatformFacts facts = supportedFacts();
    facts.glibc = GlibcVersion{2, 31};

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());
    EXPECT_TRUE(report.compatible());
}

TEST(PlatformCompatibility, Aarch64IsSupported) {
    PlatformFacts facts = supportedFacts();
    facts.architecture = "aarch64";

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());
    EXPECT_TRUE(report.compatible());
}

// --- Unsupported architecture (Requirement 1.5) ----------------------------

TEST(PlatformCompatibility, UnsupportedArchitectureIsReportedByName) {
    PlatformFacts facts = supportedFacts();
    facts.architecture = "i686";

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_TRUE(report.unsupportedPlatform());
    EXPECT_FALSE(report.architectureSupported);
    EXPECT_TRUE(containsSubstring(report.issues(), "i686"));
    EXPECT_TRUE(containsSubstring(report.issues(), "architecture"));
}

TEST(PlatformCompatibility, EmptyArchitectureIsUnsupported) {
    PlatformFacts facts = supportedFacts();
    facts.architecture = "";

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());
    EXPECT_TRUE(report.unsupportedPlatform());
    EXPECT_FALSE(report.compatible());
}

// --- Too-old / undetectable glibc (Requirements 1.1, 1.5) ------------------

TEST(PlatformCompatibility, OldGlibcIsReportedAsUnsupportedPlatform) {
    PlatformFacts facts = supportedFacts();
    facts.glibc = GlibcVersion{2, 30};

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_TRUE(report.unsupportedPlatform());
    EXPECT_FALSE(report.glibcSupported);
    EXPECT_TRUE(containsSubstring(report.issues(), "2.30"));
    EXPECT_TRUE(containsSubstring(report.issues(), "2.31"));
}

TEST(PlatformCompatibility, UndetectableGlibcFailsConservatively) {
    PlatformFacts facts = supportedFacts();
    facts.glibc = std::nullopt;

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_TRUE(report.unsupportedPlatform());
    EXPECT_FALSE(report.glibcSupported);
    EXPECT_TRUE(containsSubstring(report.issues(), "could not be determined"));
}

// --- Missing runtime dependencies (Requirement 1.4) ------------------------

TEST(PlatformCompatibility, MissingSingleDependencyIsNamed) {
    PlatformFacts facts = supportedFacts();
    facts.loadableSonames = {"libQt6Widgets.so.6"};  // libavcodec absent

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_FALSE(report.unsupportedPlatform());  // platform itself is fine
    EXPECT_TRUE(report.hasMissingDependencies());
    ASSERT_EQ(report.missingDependencies.size(), 1u);
    EXPECT_EQ(report.missingDependencies.front(), "FFmpeg libavcodec");
    EXPECT_TRUE(containsSubstring(report.issues(), "FFmpeg libavcodec"));
}

TEST(PlatformCompatibility, EveryMissingDependencyIsNamedIndividually) {
    PlatformFacts facts = supportedFacts();
    facts.loadableSonames = {};  // both dependencies absent

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_EQ(report.missingDependencies.size(), 2u);
    EXPECT_TRUE(containsSubstring(report.issues(), "Qt 6 Widgets"));
    EXPECT_TRUE(containsSubstring(report.issues(), "FFmpeg libavcodec"));
}

// --- Combined failures ------------------------------------------------------

TEST(PlatformCompatibility, MultipleFailuresAreAllReported) {
    PlatformFacts facts;
    facts.architecture = "riscv64";
    facts.glibc = GlibcVersion{2, 28};
    facts.loadableSonames = {};

    const CompatibilityReport report =
        evaluateCompatibility(facts, testRequirements());

    EXPECT_FALSE(report.compatible());
    EXPECT_TRUE(report.unsupportedPlatform());
    EXPECT_TRUE(report.hasMissingDependencies());

    const std::vector<std::string> issues = report.issues();
    // arch + glibc + 2 deps = 4 distinct issue lines.
    EXPECT_EQ(issues.size(), 4u);
    EXPECT_TRUE(containsSubstring(issues, "riscv64"));
    EXPECT_TRUE(containsSubstring(issues, "2.28"));
    EXPECT_TRUE(containsSubstring(issues, "Qt 6 Widgets"));
    EXPECT_TRUE(containsSubstring(issues, "FFmpeg libavcodec"));
}

// --- Determinism / launch budget / no-network (Requirements 1.3, 13.3/13.4) -

TEST(PlatformCompatibility, VerdictIsDeterministicAndPure) {
    const CompatibilityRequirements req = testRequirements();
    const PlatformFacts facts = supportedFacts();

    // Same inputs -> identical outputs across repeated calls (a pure function of
    // its inputs; no hidden I/O or network dependence).
    const CompatibilityReport a = evaluateCompatibility(facts, req);
    const CompatibilityReport b = evaluateCompatibility(facts, req);
    EXPECT_EQ(a.compatible(), b.compatible());
    EXPECT_EQ(a.issues(), b.issues());
    EXPECT_EQ(a.message(), b.message());
}

TEST(PlatformCompatibility, EvaluationCompletesWithinLaunchBudget) {
    const CompatibilityRequirements req = testRequirements();
    const PlatformFacts facts = supportedFacts();

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        const CompatibilityReport report = evaluateCompatibility(facts, req);
        ASSERT_TRUE(report.compatible());
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // The pure verdict is trivially fast; 100k evaluations must be far under the
    // 15-second launch budget (Requirement 1.3).
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_EQ(palmier::app::kLaunchBudget.milliseconds(), 15'000);
}

// --- Production default contract -------------------------------------------

TEST(PlatformCompatibility, DefaultRequirementsAreSaneAndFrozen) {
    const CompatibilityRequirements& req = palmier::app::defaultRequirements();

    EXPECT_EQ(req.minGlibc, (GlibcVersion{2, 31}));
    EXPECT_TRUE(containsSubstring(req.supportedArchitectures, "x86_64"));
    EXPECT_TRUE(containsSubstring(req.supportedArchitectures, "aarch64"));
    EXPECT_FALSE(req.requiredDependencies.empty());

    // A host presenting every default dependency on a supported arch/glibc passes.
    PlatformFacts facts;
    facts.architecture = "x86_64";
    facts.glibc = GlibcVersion{2, 31};
    for (const RuntimeDependency& dep : req.requiredDependencies) {
        facts.loadableSonames.push_back(dep.soname);
    }
    const CompatibilityReport report = evaluateCompatibility(facts, req);
    EXPECT_TRUE(report.compatible());
}

} // namespace
