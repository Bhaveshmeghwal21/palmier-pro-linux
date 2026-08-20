// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerationModelCatalog.hpp — the provider-grouped model catalog
// PR 406 asks for (.kiro/specs/usable-editor tasks.md Phase 2 task 7;
// docs/PORT_BACKLOG.md PR 406, PR 396, PR 395).
//
// Why this exists
// ---------------
// Upstream drives source-video preparation and model choice from a catalog tied
// to its closed hosted service. This tree cannot port that catalog — the hosted
// service is out of tree (Requirement 12.6) and no upstream catalog data was
// observed — so this is the Linux-side ADAPTATION PR 406's own rationale names:
// a small, in-tree, provider-grouped listing that expresses the same choice
// (a model id, grouped under the provider that serves it) across the hosted
// backend, the BYOK backend and the offline default, without depending on any
// of them being reachable. Listing a model here is not a claim that the
// selected backend can currently submit to it — that is `unmetPrecondition()`'s
// job (services/GenerativeBackendRegistry.hpp) — it is a claim about what MODEL
// IDS `generation.generate` and `generation.list_models` will recognise.
//
// What is deliberately NOT here: a live, provider-fed listing. Nothing in this
// tree calls out to a provider to enumerate its models (doing so would need a
// second, listing-specific transport contract no requirement asks for), so this
// catalog is compiled-in, static data — honestly a fixed table, not a directory
// service. It is still real in the sense the acceptance checks below ask for: a
// model absent from it is refused by name, and a model present in it declares
// the capabilities (`servesUpscale`, an audio duration range) the coordinator
// enforces before ever contacting a backend.
//
// The three fields each acceptance check actually exercises:
//   * PR 406 — `listModels()` groups every model under its provider, and an id
//     absent from `findModel()` is what `generation.generate` names in its
//     refusal.
//   * PR 396 — `CatalogModel::servesUpscale` is what makes `mode: "upscale"`
//     acceptable for a model; absent, the coordinator refuses the mode by name.
//   * PR 395 — `CatalogModel::audioDurationRange` is the permitted
//     [min, max] an audio request's `requestedDuration` is checked against;
//     absent, the model does not serve audio at all.

#ifndef PALMIER_SERVICES_GENERATIONMODELCATALOG_HPP
#define PALMIER_SERVICES_GENERATIONMODELCATALOG_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "services/GenerativeClient.hpp"

namespace palmier::services {

/// One entry of the catalog: a model id, the provider that serves it, the media
/// type it produces, and the two capability flags/ranges the coordinator checks
/// before submitting a request naming this model.
struct CatalogModel {
    std::string          id;       ///< e.g. "sota-video-1" — what `generation.generate`'s `model` names.
    std::string          provider; ///< e.g. "palmier-hosted" — the grouping key `listModels()` uses.
    GenerationMediaType  mediaType = GenerationMediaType::Video;

    /// True iff this model may be selected with `mode: "upscale"` (PR 396).
    bool servesUpscale = false;

    /// The permitted [min, max] requested duration for an audio request naming
    /// this model (PR 395). std::nullopt for a model that does not serve audio
    /// generation at all (including every non-Audio model: the range is only
    /// meaningful alongside `mediaType == Audio`).
    std::optional<std::pair<Duration, Duration>> audioDurationRange;
};

/// The provider-grouped, in-memory model catalog (PR 406). Copyable, comparable
/// by value, and entirely free of I/O: every method is a pure lookup over data
/// fixed at construction, so nothing here can block or fail for a network
/// reason. The default constructor seeds the built-in listing described below;
/// a caller that wants a different (e.g. test-only, single-model) catalog
/// supplies its own model list to the other constructor.
class GenerationModelCatalog {
public:
    /// The built-in listing: two providers, each with at least one video/image
    /// model, plus one upscale-capable model and one audio model — enough for
    /// every one of PR 406/396/395's acceptance checks to be satisfiable against
    /// the SAME catalog a real composition installs, not a test-only stand-in.
    GenerationModelCatalog();

    /// A catalog over exactly `models`, replacing the built-in listing. Used by
    /// tests that need a minimal or deliberately-broken catalog (e.g. exactly one
    /// provider, to prove `listModels()` would fail an "at least two providers"
    /// assertion if it undercounted).
    explicit GenerationModelCatalog(std::vector<CatalogModel> models);

    /// Every model, in declaration order. `generation.list_models`'s handler
    /// groups this by `provider` when it renders the response; the catalog
    /// itself makes no claim about the order providers are grouped in.
    [[nodiscard]] const std::vector<CatalogModel>& listModels() const noexcept {
        return models_;
    }

    /// The model named `id`, or std::nullopt when no model in this catalog has
    /// that id. Case-sensitive: a model id is an opaque identifier, not a
    /// user-facing label, matching every other id this project parses at a
    /// boundary (JobId, Uuid) rather than normalising.
    [[nodiscard]] const CatalogModel* findModel(std::string_view id) const noexcept;

    /// True iff `id` names a model in this catalog. A thin, self-documenting
    /// wrapper over `findModel`, matching `isGenerativeBackendId`'s naming
    /// convention in services/GenerativeBackendRegistry.hpp.
    [[nodiscard]] bool hasModel(std::string_view id) const noexcept {
        return findModel(id) != nullptr;
    }

private:
    std::vector<CatalogModel> models_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_GENERATIONMODELCATALOG_HPP
