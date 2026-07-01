// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/ProjectValidation.cpp — implementation of the data-model validation rules
// declared in ProjectValidation.hpp (design.md Data Models "Validation rules").

#include "core/ProjectValidation.hpp"

#include <string>
#include <unordered_set>

#include "core/Error.hpp"

namespace palmier {

namespace {

// Short, stable label for a clip in error messages: its id, or "<nil>" for the
// nil clip id (so messages remain useful even before ids are assigned).
std::string clipLabel(const Clip& clip) {
    return clip.id.isNil() ? std::string{"<nil>"} : clip.id.toString();
}

} // namespace

Result<void> validateClip(const Clip& clip) {
    // sourceOut must be strictly greater than sourceIn (clip duration is positive).
    if (!(clip.sourceOut > clip.sourceIn)) {
        return outOfRange("Clip " + clipLabel(clip) +
                          ": sourceOut must be strictly greater than sourceIn");
    }

    // opacity must lie within the closed unit interval [0, 1].
    if (clip.opacity < 0.0 || clip.opacity > 1.0) {
        return outOfRange("Clip " + clipLabel(clip) +
                          ": opacity must be within [0, 1], got " +
                          std::to_string(clip.opacity));
    }

    // gain must be non-negative.
    if (clip.gain < 0.0) {
        return outOfRange("Clip " + clipLabel(clip) +
                          ": gain must be >= 0, got " + std::to_string(clip.gain));
    }

    // A present transition must define a non-negative region length.
    if (clip.transitionIn.has_value() && !clip.transitionIn->isValid()) {
        return outOfRange("Clip " + clipLabel(clip) +
                          ": transitionIn duration must be >= 0");
    }

    return ok();
}

Result<void> validateTrack(const Track& track) {
    for (const Clip& clip : track.clips) {
        if (auto r = validateClip(clip); !r) {
            return r;
        }
    }
    return ok();
}

Result<void> validateProject(const Project& project) {
    // timelineFps must be a positive, valid rational rate (numerator/denominator > 0).
    if (!project.timelineFps.isValid()) {
        return invalidArgument("Project timelineFps must be > 0");
    }

    // canvas must have strictly positive width and height.
    if (!project.canvas.isValid()) {
        return invalidArgument("Project canvas dimensions must be > 0 (got " +
                               std::to_string(project.canvas.width) + "x" +
                               std::to_string(project.canvas.height) + ")");
    }

    // version must be a known, supported schema version.
    if (!project.version.isSupported()) {
        return unsupported("Project schema version " + project.version.toString() +
                           " is not supported by this build (current " +
                           SchemaVersion::current().toString() + ")");
    }

    // Collect the set of asset identities the project declares, so each clip's
    // assetRef can be resolved against it.
    std::unordered_set<Uuid> assetIds;
    assetIds.reserve(project.assets.size());
    for (const MediaAssetRef& asset : project.assets) {
        assetIds.insert(asset.assetId);
    }

    // Per-track and per-clip rules, plus asset resolution for every clip.
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            if (auto r = validateClip(clip); !r) {
                return r;
            }
            if (assetIds.find(clip.assetRef.assetId) == assetIds.end()) {
                return notFound("Clip " + clipLabel(clip) +
                                ": assetRef does not resolve to any entry in "
                                "Project.assets");
            }
        }
    }

    return ok();
}

} // namespace palmier
