// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerationModelCatalog.cpp — implementation. See the header for the
// design rationale.

#include "services/GenerationModelCatalog.hpp"

#include <algorithm>
#include <utility>

namespace palmier::services {

namespace {

/// Seconds-to-Duration helper kept local to this translation unit: the built-in
/// listing below is the only place that needs to spell an audio duration range
/// as a literal, and every other Duration construction in this tree already
/// goes through a named factory (fromSeconds/fromMilliseconds/...), so this adds
/// no new public surface.
constexpr Duration seconds(double value) { return Duration::fromSeconds(value); }

std::vector<CatalogModel> builtInModels() {
    std::vector<CatalogModel> models;

    // Provider 1: the hosted Palmier catalog's two headline models — one video,
    // one image — matching the naming convention the generative-lifecycle test
    // rig already uses (kVideoModel/kImageModel in
    // tests/services/generative_lifecycle_property_test.cpp), so a request built
    // against either fixture and this catalog names the same models.
    models.push_back(CatalogModel{
        .id = "sota-video-1",
        .provider = "palmier-hosted",
        .mediaType = GenerationMediaType::Video,
        .servesUpscale = true,  // PR 396's upscale-capable model.
        .audioDurationRange = std::nullopt,
    });
    models.push_back(CatalogModel{
        .id = "sota-image-1",
        .provider = "palmier-hosted",
        .mediaType = GenerationMediaType::Image,
        .servesUpscale = false,
        .audioDurationRange = std::nullopt,
    });

    // Provider 2: a second, distinct provider (PR 406's "at least two
    // providers"), serving an audio model with a declared duration range
    // (PR 395) plus a second, non-upscale video model so a caller can compare a
    // model that DOES serve upscale against one that does not.
    models.push_back(CatalogModel{
        .id = "sota-audio-1",
        .provider = "palmier-audio",
        .mediaType = GenerationMediaType::Audio,
        .servesUpscale = false,
        .audioDurationRange = std::make_pair(seconds(1.0), seconds(120.0)),
    });
    models.push_back(CatalogModel{
        .id = "sota-video-lite-1",
        .provider = "palmier-audio",
        .mediaType = GenerationMediaType::Video,
        .servesUpscale = false,
        .audioDurationRange = std::nullopt,
    });

    return models;
}

}  // namespace

GenerationModelCatalog::GenerationModelCatalog() : models_(builtInModels()) {}

GenerationModelCatalog::GenerationModelCatalog(std::vector<CatalogModel> models)
    : models_(std::move(models)) {}

const CatalogModel* GenerationModelCatalog::findModel(std::string_view id) const noexcept {
    const auto it = std::find_if(models_.begin(), models_.end(),
                                 [id](const CatalogModel& m) { return m.id == id; });
    return it == models_.end() ? nullptr : &*it;
}

}  // namespace palmier::services
