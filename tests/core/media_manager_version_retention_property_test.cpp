// SPDX-License-Identifier: GPL-3.0-or-later
//
// Property-based test for generated-clip version retention (task 6.2).
//
// Design property P12 (design.md "Correctness Properties"):
//
//     For any clip replaced by a generated clip, the prior version remains
//     retained and selectable.
//
// This is the media-library counterpart to the versioning guarantee in
// Requirement 3.4 ("retain prior versions ... keeping at least the 10 most recent
// versions selectable"). The MediaManager mechanism under test is implemented in
// core/MediaManager.cpp (task 6.1); this file adds the dedicated RapidCheck
// property that exercises it across arbitrary sequences of generated-clip
// replacements.
//
// Strategy: register a clip from an imported base asset, then apply an arbitrary
// number of generated-clip replacements (each backed by a freshly imported
// generated asset). After EACH replacement we assert the two halves of P12:
//   * the immediately-prior version is still retained and individually
//     selectable (the "keeps its prior version" clause), and
//   * at least the kMinRetainedVersions (10) most recent versions — the newly
//     generated one plus its recent predecessors — remain retained and
//     selectable (the "at least the 10 most recent" bound of Requirement 3.4).
// We also confirm the newly generated version is the one selected after a
// replacement, and that selecting any retained version actually activates it.
//
// _Requirements: 3.4_

#include "core/MediaManager.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Duration.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Uuid.hpp"

namespace palmier {
namespace {

Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// Feature: palmier-pro-linux, Property 12: Generated-clip version retention — a
// clip replaced by a generated clip keeps its prior version retained and
// selectable (and at least the 10 most recent versions stay selectable).
// Validates: Requirements 3.4
RC_GTEST_PROP(MediaManagerVersionRetentionProperties,
              GeneratedReplacementRetainsPriorVersionAndRecentHistory,
              ()) {
    // Arbitrary number of generated-clip replacements to apply (>= 1 so at least
    // one replacement happens). Kept modest so each case stays fast while still
    // ranging well past the 10-version retention floor.
    const int replacements = *rc::gen::inRange(1, 40);
    // Arbitrary retention capacity; the manager raises anything below the floor
    // up to kMinRetainedVersions, so the "at least 10" guarantee always holds.
    const std::size_t requestedCapacity =
        static_cast<std::size_t>(*rc::gen::inRange(0, 25));

    MediaManager mgr(requestedCapacity);
    const std::size_t capacity = mgr.retentionCapacity();
    RC_ASSERT(capacity >= MediaManager::kMinRetainedVersions);

    // Base (imported original) version of the clip.
    const MediaAssetRef base(Uuid::generateV4(), "base.mp4");
    RC_ASSERT(mgr.importAsset(base).isOk());
    const ClipId clip = Uuid::generateV4();
    RC_ASSERT(mgr.registerClip(clip, base, ms(0), ms(1000)).isOk());

    // Full chronological history of the content we placed on the clip, oldest
    // first: the base, then each generated asset in replacement order.
    std::vector<MediaAssetRef> placed{base};

    for (int i = 1; i <= replacements; ++i) {
        // The content that will become the immediately-prior version after this
        // replacement is whatever is currently selected.
        const auto priorSelected = mgr.selectedVersion(clip);
        RC_ASSERT(priorSelected.has_value());
        const MediaAssetRef priorAsset = priorSelected->assetRef;

        const MediaAssetRef gen(Uuid::generateV4(), "gen" + std::to_string(i) + ".mp4");
        RC_ASSERT(mgr.importAsset(gen).isOk());
        // Vary the source range across iterations to exercise distinct versions.
        RC_ASSERT(mgr.replaceWithGeneratedClip(clip, gen, ms(0), ms(1000 + i)).isOk());
        placed.push_back(gen);

        const std::vector<ClipVersion> versions = mgr.versions(clip);

        // P12, clause 1: the newly generated version is now the selected one.
        const auto selected = mgr.selectedVersion(clip);
        RC_ASSERT(selected.has_value());
        RC_ASSERT(selected->assetRef == gen);
        RC_ASSERT(selected->generated);

        // P12, clause 2: the prior version is still retained and individually
        // selectable. It sits immediately before the just-added generated version,
        // i.e. at the second-to-last retained index.
        RC_ASSERT(versions.size() >= 2);
        const std::size_t priorIndex = versions.size() - 2;
        RC_ASSERT(versions[priorIndex].assetRef == priorAsset);

        const auto selectPrior = mgr.selectVersion(clip, priorIndex);
        RC_ASSERT(selectPrior.isOk());
        const auto reSelected = mgr.selectedVersion(clip);
        RC_ASSERT(reSelected.has_value());
        RC_ASSERT(reSelected->assetRef == priorAsset);
        RC_ASSERT(mgr.selectedVersionIndex(clip).has_value());
        RC_ASSERT(*mgr.selectedVersionIndex(clip) == priorIndex);

        // Requirement 3.4 bound: at least the min(placed, 10) most recent versions
        // are retained, and never more than the capacity.
        const std::size_t expectedRetained =
            placed.size() < capacity ? placed.size() : capacity;
        RC_ASSERT(versions.size() == expectedRetained);
        RC_ASSERT(versions.size() >=
                  (placed.size() < MediaManager::kMinRetainedVersions
                       ? placed.size()
                       : MediaManager::kMinRetainedVersions));

        // Every one of the retained versions is individually selectable, and the
        // retained window is exactly the most-recent suffix of the placed history
        // (older versions beyond capacity are dropped oldest-first).
        for (std::size_t v = 0; v < versions.size(); ++v) {
            const MediaAssetRef& expected = placed[placed.size() - versions.size() + v];
            RC_ASSERT(versions[v].assetRef == expected);
            RC_ASSERT(mgr.selectVersion(clip, v).isOk());
            RC_ASSERT(mgr.selectedVersion(clip).has_value());
            RC_ASSERT(mgr.selectedVersion(clip)->assetRef == expected);
        }

        // Restore selection to the freshly generated version before the next round
        // so the "prior version" of the next replacement is well-defined.
        RC_ASSERT(mgr.selectVersion(clip, versions.size() - 1).isOk());
    }
}

}  // namespace
}  // namespace palmier
