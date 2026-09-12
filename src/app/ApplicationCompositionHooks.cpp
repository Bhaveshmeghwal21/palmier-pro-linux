// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/ApplicationCompositionHooks.cpp - the injected tool handlers.
//
// Bodies moved verbatim out of app/ApplicationComposition.cpp, which had grown to a thousand
// lines by carrying both the wiring and these adapters. See the header for why they are
// separated and why they are in `detail`.

#include "app/ApplicationCompositionHooks.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"

namespace palmier::app::detail {

[[nodiscard]] Error offlineError(const char* feature) {
    return makeError(ErrorCode::Unsupported,
                     std::string(feature) +
                         " backend is not configured (offline build); no network was contacted");
}

// ---------------------------------------------------------------------------
// generation.generate tool hook
// ---------------------------------------------------------------------------
//
// Adapts the shared tool surface's `generation.generate` handler onto the
// composed GenerativeMediaCoordinator (Requirement 6). It maps the JSON tool
// arguments to a GenerationRequest + GenerationPlacement, runs the coordinator's
// validate -> gate -> generate -> place pipeline, and renders the placement back
// as JSON. Any coordinator error (unconfigured backend, unauthorized, invalid
// prompt/bounds) is forwarded verbatim so the executor's rollback/error policy
// applies uniformly.
// Every `generation.generate` invocation — from the tool surface, from the MCP
// endpoint and from the in-app agent — arrives HERE, because all three dispatch
// through the one `ToolRegistry` this hook is registered on (Requirement 12.1).
// That is why the Requirement 12.4 rejection is implemented as the hook's first
// act rather than deeper down: asking the SELECTED backend for its unmet
// precondition costs one secret-store read and no network activity, and returning
// before the coordinator runs is what makes "no media library entry, no clip and
// no undo-history entry" true by construction rather than by inspection.
//
// `backend` is null only when the caller injected a raw `IGenerativeBackend`; in
// that case the injected backend answers for itself and the coordinator runs as
// before.
[[nodiscard]] services::Tool::Handler makeGenerateHook(
    services::GenerativeMediaCoordinator& coordinator, const services::GenerativeBackend* backend) {
    return [&coordinator, backend](const services::Json& input) -> Result<services::Json> {
        using namespace palmier::services;

        if (backend != nullptr) {
            const std::string unmet = backend->unmetPrecondition();
            if (!unmet.empty()) {
                return err<Json>(failedPrecondition(unmet));
            }
        }

        GenerationRequest request;
        request.model = input.stringOr("model");
        request.prompt = input.stringOr("prompt");
        const std::string mediaTypeText = input.stringOr("mediaType", "video");
        if (mediaTypeText == "image") {
            request.mediaType = GenerationMediaType::Image;
        } else if (mediaTypeText == "audio") {
            request.mediaType = GenerationMediaType::Audio;
        } else {
            request.mediaType = GenerationMediaType::Video;
        }
        request.mode = generationModeFromStringView(input.stringOr("mode", "generate"))
                           .value_or(GenerationMode::Generate);
        if (const std::optional<Uuid> sourceAssetId = Uuid::parse(input.stringOr("sourceAssetId"));
            sourceAssetId.has_value()) {
            request.sourceAssetId = *sourceAssetId;
        }
        const std::int64_t targetWidth = input.intOr("targetWidth", 0);
        const std::int64_t targetHeight = input.intOr("targetHeight", 0);
        if (targetWidth > 0 && targetHeight > 0) {
            request.targetResolution = Resolution{static_cast<std::uint32_t>(targetWidth),
                                                  static_cast<std::uint32_t>(targetHeight)};
        }
        request.requestedDuration =
            Duration::fromNanoseconds(input.intOr("requestedDurationTicks", 0));
        if (const Json* params = input.find("params"); params != nullptr && params->isObject()) {
            for (const auto& [key, value] : params->asObject()) {
                if (value.isString()) request.params.emplace(key, value.asString());
            }
        }

        GenerationPlacement placement;
        const std::optional<Uuid> trackId = Uuid::parse(input.stringOr("trackId"));
        if (!trackId.has_value()) {
            return err<Json>(
                invalidArgument("generation.generate: 'trackId' must be a valid UUID"));
        }
        placement.trackId = *trackId;
        placement.framePosition = input.intOr("framePosition", 0);
        placement.sourceIn = Duration::fromNanoseconds(input.intOr("sourceInTicks", 0));
        placement.sourceOut = Duration::fromNanoseconds(input.intOr("sourceOutTicks", 0));

        Result<GeneratedMediaPlacement> placed = coordinator.generateAndPlace(request, placement);
        if (placed.isError()) {
            return err<Json>(placed.error());
        }

        const GeneratedMediaPlacement& result = placed.value();
        Json out = Json::object();
        out.set("assetId", result.asset.assetId.toString());
        out.set("sourcePath", result.asset.sourcePath);
        out.set("clipId", result.clipId.toString());
        out.set("timelineStartTicks", static_cast<std::int64_t>(result.timelineStart.ticks()));
        return out;
    };
}

/// The `generation.list_models` hook (usable-editor Phase 2 task 7; PR 406):
/// render the catalog's models grouped by provider. Pure and network-free — the
/// catalog is fixed, in-tree data — so this hook needs no precondition check the
/// way `makeGenerateHook` needs the backend's `unmetPrecondition()`: listing what
/// models EXIST is independent of whether the currently selected backend can
/// currently reach one of them.
[[nodiscard]] services::Tool::Handler makeListModelsHook(
    const services::GenerationModelCatalog& catalog) {
    return [&catalog](const services::Json&) -> Result<services::Json> {
        using namespace palmier::services;

        // Group by provider while preserving each provider's first-seen order,
        // so the response is deterministic for a fixed catalog without depending
        // on any particular sort. Grouped in a plain map first (Json::find has no
        // mutable overload) and converted to the response's Json shape once.
        std::vector<std::pair<std::string, Json::Array>> byProvider;
        for (const CatalogModel& model : catalog.listModels()) {
            Json entry = Json::object();
            entry.set("id", model.id);
            entry.set("mediaType", std::string(toStringView(model.mediaType)));
            entry.set("servesUpscale", model.servesUpscale);
            if (model.audioDurationRange.has_value()) {
                const auto& [minDuration, maxDuration] = *model.audioDurationRange;
                entry.set("minDurationTicks", static_cast<std::int64_t>(minDuration.ticks()));
                entry.set("maxDurationTicks", static_cast<std::int64_t>(maxDuration.ticks()));
            }

            const auto existing =
                std::find_if(byProvider.begin(), byProvider.end(),
                             [&model](const auto& p) { return p.first == model.provider; });
            if (existing != byProvider.end()) {
                existing->second.push_back(std::move(entry));
            } else {
                byProvider.emplace_back(model.provider, Json::Array{std::move(entry)});
            }
        }

        Json providers = Json::object();
        for (auto& [provider, models] : byProvider) {
            providers.set(provider, Json::array(std::move(models)));
        }

        Json out = Json::object();
        out.set("providers", std::move(providers));
        return out;
    };
}

/// `timeline.transcribe_to_captions`'s hook (usable-editor task 13; Requirement
/// 10.4): transcribe `sourceClipId`'s audio via the bound TranscriptionService
/// and place the resulting segments as caption cues on `captionTrackId`, each
/// cue placed through the identical AddClipCommand every caption cue already
/// uses. `session` is the one ProjectSession every other hook already resolves
/// its engine from.
///
/// Known limitation, documented rather than hidden: this build has no media
/// probe wired into this seam, so TranscriptionAudioSource is derived directly
/// from the source clip's own timeline duration and assumes audio is present
/// (hasAudioTrack = true) — accurate for a genuinely audio-bearing clip, but
/// not a substitute for the real audio-track detection a bundled recognizer
/// would need. Since no real recognizer is bundled either (the backend this
/// hook is bound to is always UnavailableTranscriptionBackend), the effective
/// behaviour today is unconditionally Unsupported regardless of this
/// approximation; wiring a real backend later should also wire real media
/// probing into this seam at the same time.
[[nodiscard]] services::Tool::Handler makeTranscribeToCaptionsHook(
    services::TranscriptionService& transcription, services::ProjectSession& session) {
    return [&transcription, &session](const services::Json& in) -> Result<services::Json> {
        using namespace palmier::services;

        const Json* sourceClipIdArg = in.find("sourceClipId");
        if (sourceClipIdArg == nullptr || !sourceClipIdArg->isString()) {
            return err<Json>(invalidArgument("missing or non-string field 'sourceClipId'"));
        }
        const std::optional<Uuid> sourceClipId = Uuid::parse(sourceClipIdArg->asString());
        if (!sourceClipId.has_value()) {
            return err<Json>(invalidArgument("field 'sourceClipId' is not a valid UUID"));
        }
        const Json* captionTrackIdArg = in.find("captionTrackId");
        if (captionTrackIdArg == nullptr || !captionTrackIdArg->isString()) {
            return err<Json>(invalidArgument("missing or non-string field 'captionTrackId'"));
        }
        const std::optional<Uuid> captionTrackId = Uuid::parse(captionTrackIdArg->asString());
        if (!captionTrackId.has_value()) {
            return err<Json>(invalidArgument("field 'captionTrackId' is not a valid UUID"));
        }

        TimelineEngine& engine = session.engine();
        const std::optional<Clip> sourceClip = engine.clip(*sourceClipId);
        if (!sourceClip.has_value()) {
            return err<Json>(notFound("timeline.transcribe_to_captions: clip " +
                                      sourceClipId->toString() + " not found"));
        }

        TranscriptionAudioSource source;
        source.hasAudioTrack = true;  // see this function's own doc comment
        source.duration = sourceClip->duration();

        Result<Transcript> transcript = transcription.transcribe(*sourceClipId, source);
        if (transcript.isError()) {
            return err<Json>(std::move(transcript).error());
        }

        Json out = Json::object();
        if (transcript.value().noAudioFound()) {
            out.set("status", std::string{"no_audio_found"});
            out.set("cuesAdded", static_cast<std::int64_t>(0));
            return out;
        }

        std::int64_t cuesAdded = 0;
        for (const TextSegment& segment : transcript.value().segments) {
            Clip cue;
            cue.id = Uuid::generateV4();
            cue.timelineStart = Duration::fromMilliseconds(segment.startMs);
            cue.sourceIn = Duration::zero();
            cue.sourceOut = Duration::fromMilliseconds(segment.durationMs());
            cue.captionText = segment.text;
            const CommandResult placed =
                engine.apply(std::make_unique<AddClipCommand>(*captionTrackId, std::move(cue)));
            if (placed.isError()) {
                return err<Json>(placed.error());
            }
            ++cuesAdded;
        }

        out.set("status", std::string{"transcribed"});
        out.set("cuesAdded", cuesAdded);
        return out;
    };
}

/// `timeline.capture_frame`'s hook (usable-editor tasks.md task 14): render
/// the timeline at a given position through the live `gpu::Compositor` (the
/// SAME instance the preview surface renders through — this is what makes the
/// written image match the preview frame rather than merely resemble it) and
/// hand the pixels to the configured `services::ImageEncoder`.
[[nodiscard]] services::Tool::Handler makeCaptureFrameHook(gpu::Compositor& compositor,
                                                           services::ProjectSession& session,
                                                           services::ImageEncoder encoder) {
    return [&compositor, &session,
            encoder = std::move(encoder)](const services::Json& in) -> Result<services::Json> {
        using namespace palmier::services;

        const Json* outputPathArg = in.find("outputPath");
        if (outputPathArg == nullptr || !outputPathArg->isString() ||
            outputPathArg->asString().empty()) {
            return err<Json>(invalidArgument("missing or empty field 'outputPath'"));
        }
        const Json* positionArg = in.find("positionNs");
        if (positionArg == nullptr || !positionArg->isNumber()) {
            return err<Json>(invalidArgument("missing or non-numeric field 'positionNs'"));
        }
        const Duration position =
            Duration::fromNanoseconds(static_cast<std::int64_t>(positionArg->asDouble()));

        const Project project = session.engine().snapshot();
        const std::filesystem::path outputPath(outputPathArg->asString());
        Result<void> captured = captureFrame(compositor, project, position, outputPath, encoder);
        if (captured.isError()) {
            return err<Json>(std::move(captured).error());
        }

        Json out = Json::object();
        out.set("outputPath", outputPath.string());
        return out;
    };
}

}  // namespace palmier::app::detail
