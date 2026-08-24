// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/MediaAssetRef.hpp — a project-scoped reference to an imported media asset.
//
// A Project owns a table of referenced media (Project.assets), and every Clip
// points at one of them through its assetRef (design.md Data Models). The stable
// identity within that table is the assetId (a Uuid); the sourcePath is the
// filesystem location (or generated-asset locator) the media was imported from and
// is informational. A project's validation rule requires that "every Clip.assetRef
// resolves to an entry in assets" — resolution is by assetId, so two refs are
// considered the same asset iff their assetIds match.

#ifndef PALMIER_CORE_MEDIAASSETREF_HPP
#define PALMIER_CORE_MEDIAASSETREF_HPP

#include <string>
#include <vector>

#include "core/Uuid.hpp"

namespace palmier {

struct MediaAssetRef {
    Uuid                     assetId;    ///< Stable identity within Project.assets.
    std::string              sourcePath; ///< Origin path / locator of the media (informational).
    /// Free-form organisation labels over the flat media library (usable-editor
    /// tasks.md task 15; no dedicated Requirement). Order is preserved but not
    /// meaningful; duplicates are permitted and left as the caller wrote them —
    /// this field is deliberately as unopinionated as the "flat library" it
    /// extends, mirroring MediaAssetRef's own sourcePath in being informational
    /// rather than validated. Persisted from schema 1.4 onward and empty by
    /// default, so an asset with no tags round-trips identically to one from a
    /// pre-1.4 document.
    std::vector<std::string> tags;

    MediaAssetRef() = default;
    explicit MediaAssetRef(Uuid id, std::string path = {})
        : assetId(id), sourcePath(std::move(path)) {}

    /// A reference is valid iff it names a non-nil asset identity.
    [[nodiscard]] bool isValid() const noexcept { return !assetId.isNil(); }

    /// Two references denote the same asset iff their identities match. The
    /// sourcePath and tags are intentionally excluded so a clip's ref resolves
    /// against an asset-table entry by identity alone.
    [[nodiscard]] friend bool operator==(const MediaAssetRef& a, const MediaAssetRef& b) noexcept {
        return a.assetId == b.assetId;
    }
    [[nodiscard]] friend bool operator!=(const MediaAssetRef& a, const MediaAssetRef& b) noexcept {
        return !(a == b);
    }
};

} // namespace palmier

#endif // PALMIER_CORE_MEDIAASSETREF_HPP
