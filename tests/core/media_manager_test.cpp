// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the MediaManager (task 6.1):
//   * the project media library — importing assets and making them available for
//     placement (Requirement 3.1);
//   * per-clip generated-clip version history — retaining prior versions when a
//     generated clip replaces a clip, keeping at least the 10 most recent versions
//     selectable (Requirement 3.4).
//
// The dedicated property test for generated-clip version retention (design
// property P12) is added separately by task 6.2; these are example-based unit
// tests covering the library surface, the >= 10 retention bound, and selectability.
//
// _Requirements: 3.1, 3.4_

#include "core/MediaManager.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

MediaAssetRef makeAsset(std::string path = "clip.mp4") {
    return MediaAssetRef(Uuid::generateV4(), std::move(path));
}

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// --- Media library (Requirement 3.1) ---------------------------------------

TEST(MediaManager, ImportAddsAssetAndMakesItAvailable) {
    MediaManager mgr;
    const auto asset = makeAsset("import.mp4");

    ASSERT_TRUE(mgr.importAsset(asset).isOk());

    EXPECT_TRUE(mgr.hasAsset(asset.assetId));
    EXPECT_EQ(mgr.assetCount(), 1u);
    ASSERT_EQ(mgr.library().size(), 1u);
    EXPECT_EQ(mgr.library().front().assetId, asset.assetId);

    const auto found = mgr.asset(asset.assetId);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->sourcePath, "import.mp4");
}

TEST(MediaManager, ImportRejectsNilAssetIdAndLeavesLibraryUnchanged) {
    MediaManager mgr;
    const MediaAssetRef nil;  // default-constructed => nil id

    const auto result = mgr.importAsset(nil);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(mgr.assetCount(), 0u);
}

TEST(MediaManager, ImportRejectsDuplicateAssetId) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());

    const auto result = mgr.importAsset(asset);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(mgr.assetCount(), 1u);
}

TEST(MediaManager, UnknownAssetLookupReturnsNullopt) {
    MediaManager mgr;
    EXPECT_FALSE(mgr.hasAsset(Uuid::generateV4()));
    EXPECT_FALSE(mgr.asset(Uuid::generateV4()).has_value());
}

// --- Registering a clip's base version -------------------------------------

TEST(MediaManager, RegisterClipEstablishesSelectableBaseVersion) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());

    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, asset, ms(0), ms(1000)).isOk());

    EXPECT_TRUE(mgr.tracksClip(clip));
    EXPECT_EQ(mgr.versionCount(clip), 1u);
    ASSERT_TRUE(mgr.selectedVersionIndex(clip).has_value());
    EXPECT_EQ(*mgr.selectedVersionIndex(clip), 0u);

    const auto selected = mgr.selectedVersion(clip);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->assetRef, asset);
    EXPECT_FALSE(selected->generated);
    EXPECT_EQ(selected->duration(), ms(1000));
}

TEST(MediaManager, RegisterClipRejectsInvalidSourceRange) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());

    const auto result = mgr.registerClip(Uuid::generateV4(), asset, ms(500), ms(500));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(MediaManager, RegisterClipRejectsAssetNotInLibrary) {
    MediaManager mgr;
    const auto asset = makeAsset();  // never imported

    const auto result = mgr.registerClip(Uuid::generateV4(), asset, ms(0), ms(1000));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(MediaManager, RegisterClipRejectsDuplicateClip) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, asset, ms(0), ms(1000)).isOk());

    const auto result = mgr.registerClip(clip, asset, ms(0), ms(1000));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

// --- Generated-clip replacement + retention (Requirement 3.4) --------------

TEST(MediaManager, ReplaceRetainsPriorVersionAndSelectsGenerated) {
    MediaManager mgr;
    const auto original = makeAsset("original.mp4");
    const auto generated = makeAsset("generated.mp4");
    ASSERT_TRUE(mgr.importAsset(original).isOk());
    ASSERT_TRUE(mgr.importAsset(generated).isOk());

    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, original, ms(0), ms(1000)).isOk());

    ASSERT_TRUE(mgr.replaceWithGeneratedClip(clip, generated, ms(0), ms(2000)).isOk());

    // Both versions are retained; the generated one is selected.
    EXPECT_EQ(mgr.versionCount(clip), 2u);
    const auto selected = mgr.selectedVersion(clip);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->assetRef, generated);
    EXPECT_TRUE(selected->generated);

    // The prior version remains retained and selectable at index 0.
    const auto prior = mgr.versionAt(clip, 0);
    ASSERT_TRUE(prior.has_value());
    EXPECT_EQ(prior->assetRef, original);
    EXPECT_FALSE(prior->generated);
}

TEST(MediaManager, ReplaceRejectsWhenClipNotTracked) {
    MediaManager mgr;
    const auto generated = makeAsset();
    ASSERT_TRUE(mgr.importAsset(generated).isOk());

    const auto result = mgr.replaceWithGeneratedClip(Uuid::generateV4(), generated, ms(0), ms(1000));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::FailedPrecondition);
}

TEST(MediaManager, ReplaceRejectsGeneratedAssetNotInLibrary) {
    MediaManager mgr;
    const auto original = makeAsset();
    ASSERT_TRUE(mgr.importAsset(original).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, original, ms(0), ms(1000)).isOk());

    const auto generated = makeAsset();  // not imported
    const auto result = mgr.replaceWithGeneratedClip(clip, generated, ms(0), ms(1000));
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    // Unchanged: still just the base version.
    EXPECT_EQ(mgr.versionCount(clip), 1u);
}

TEST(MediaManager, SelectingARetainedVersionMakesItActive) {
    MediaManager mgr;
    const auto original = makeAsset("original.mp4");
    const auto generated = makeAsset("generated.mp4");
    ASSERT_TRUE(mgr.importAsset(original).isOk());
    ASSERT_TRUE(mgr.importAsset(generated).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, original, ms(0), ms(1000)).isOk());
    ASSERT_TRUE(mgr.replaceWithGeneratedClip(clip, generated, ms(0), ms(2000)).isOk());

    // Roll back to the prior (original) version.
    ASSERT_TRUE(mgr.selectVersion(clip, 0).isOk());
    const auto selected = mgr.selectedVersion(clip);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->assetRef, original);
    EXPECT_EQ(*mgr.selectedVersionIndex(clip), 0u);
}

TEST(MediaManager, SelectVersionRejectsUnknownClipAndOutOfRange) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, asset, ms(0), ms(1000)).isOk());

    EXPECT_EQ(mgr.selectVersion(Uuid::generateV4(), 0).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(mgr.selectVersion(clip, 5).error().code(), ErrorCode::OutOfRange);
}

// The core Requirement 3.4 bound: after many generated replacements, at least the
// 10 most recent versions are preserved and remain individually selectable.
TEST(MediaManager, PreservesAtLeastTenMostRecentVersionsAndKeepsThemSelectable) {
    MediaManager mgr;  // default retention capacity == kMinRetainedVersions (10)
    const auto original = makeAsset("v0.mp4");
    ASSERT_TRUE(mgr.importAsset(original).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, original, ms(0), ms(1000)).isOk());

    // Perform 20 generated replacements (well beyond the 10-version floor).
    constexpr int kReplacements = 20;
    std::vector<MediaAssetRef> generated;
    for (int i = 1; i <= kReplacements; ++i) {
        auto asset = makeAsset("gen" + std::to_string(i) + ".mp4");
        ASSERT_TRUE(mgr.importAsset(asset).isOk());
        ASSERT_TRUE(mgr.replaceWithGeneratedClip(clip, asset, ms(0), ms(1000 + i)).isOk());
        generated.push_back(asset);
    }

    // At least the 10 most recent versions are retained.
    EXPECT_GE(mgr.versionCount(clip), MediaManager::kMinRetainedVersions);

    // The newest generation is the selected version.
    const auto selected = mgr.selectedVersion(clip);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->assetRef, generated.back());

    // Every retained version resolves to one of the most-recent generations and is
    // individually selectable.
    const auto versions = mgr.versions(clip);
    ASSERT_GE(versions.size(), MediaManager::kMinRetainedVersions);
    for (std::size_t i = 0; i < versions.size(); ++i) {
        ASSERT_TRUE(mgr.selectVersion(clip, i).isOk());
        EXPECT_EQ(*mgr.selectedVersionIndex(clip), i);
        EXPECT_EQ(mgr.selectedVersion(clip)->assetRef, versions[i].assetRef);
    }

    // The 10 most recent generated assets are all still present as versions.
    for (int i = 0; i < static_cast<int>(MediaManager::kMinRetainedVersions); ++i) {
        const auto& wanted = generated[generated.size() - 1 - i];
        const bool present = std::any_of(versions.begin(), versions.end(),
            [&](const ClipVersion& v) { return v.assetRef == wanted; });
        EXPECT_TRUE(present) << "recent generated version " << i << " should be retained";
    }
}

TEST(MediaManager, RetentionCapacityIsRaisedToTheFloor) {
    MediaManager mgr(3);  // below the 10-version floor
    EXPECT_EQ(mgr.retentionCapacity(), MediaManager::kMinRetainedVersions);
}

TEST(MediaManager, HigherRetentionCapacityKeepsMoreVersions) {
    MediaManager mgr(15);
    const auto original = makeAsset("v0.mp4");
    ASSERT_TRUE(mgr.importAsset(original).isOk());
    const ClipId clip = Uuid::generateV4();
    ASSERT_TRUE(mgr.registerClip(clip, original, ms(0), ms(1000)).isOk());

    for (int i = 1; i <= 20; ++i) {
        auto asset = makeAsset("gen" + std::to_string(i) + ".mp4");
        ASSERT_TRUE(mgr.importAsset(asset).isOk());
        ASSERT_TRUE(mgr.replaceWithGeneratedClip(clip, asset, ms(0), ms(1000)).isOk());
    }
    EXPECT_EQ(mgr.versionCount(clip), 15u);
}

// --- Untracked-clip queries are safe ---------------------------------------

TEST(MediaManager, QueriesOnUntrackedClipAreEmpty) {
    MediaManager mgr;
    const ClipId clip = Uuid::generateV4();
    EXPECT_FALSE(mgr.tracksClip(clip));
    EXPECT_EQ(mgr.versionCount(clip), 0u);
    EXPECT_TRUE(mgr.versions(clip).empty());
    EXPECT_FALSE(mgr.versionAt(clip, 0).has_value());
    EXPECT_FALSE(mgr.selectedVersionIndex(clip).has_value());
    EXPECT_FALSE(mgr.selectedVersion(clip).has_value());
}

// --- Media organisation (usable-editor tasks.md task 15; no dedicated
// Requirement) ---------------------------------------------------------------

TEST(MediaManager, SetAssetTagsReplacesTheTagListWholesale) {
    MediaManager mgr;
    const auto asset = makeAsset();
    ASSERT_TRUE(mgr.importAsset(asset).isOk());

    ASSERT_TRUE(mgr.setAssetTags(asset.assetId, {"b-roll", "outdoor"}).isOk());
    std::optional<MediaAssetRef> found = mgr.asset(asset.assetId);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->tags, (std::vector<std::string>{"b-roll", "outdoor"}));

    // A second call REPLACES rather than appends.
    ASSERT_TRUE(mgr.setAssetTags(asset.assetId, {"interview"}).isOk());
    found = mgr.asset(asset.assetId);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->tags, (std::vector<std::string>{"interview"}));

    // An empty list clears the tags.
    ASSERT_TRUE(mgr.setAssetTags(asset.assetId, {}).isOk());
    found = mgr.asset(asset.assetId);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->tags.empty());
}

TEST(MediaManager, SetAssetTagsRejectsAnAssetNotInTheLibrary) {
    MediaManager mgr;
    const Uuid unknown = Uuid::generateV4();

    const Result<void> result = mgr.setAssetTags(unknown, {"anything"});

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

}  // namespace
}  // namespace palmier
