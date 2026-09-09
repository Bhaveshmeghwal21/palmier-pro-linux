// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/GenerationExportTools.cpp — the hook-backed tools: generative media, model listing, transcription,
// still-frame capture and export.
//
// Split out of services/ToolRegistry.cpp, which held the entire tool surface in
// one 2767-line translation unit. The internal contract these units share is
// services/ToolRegistryInternal.hpp; the public one is unchanged and remains
// services/ToolRegistry.hpp. Every definition below was moved rather than
// rewritten, so the surface this file contributes is byte-identical to before.
//
// One include, because ToolRegistryInternal.hpp states the surface's shared
// dependency set once for all of these files.

#include "services/ToolRegistryInternal.hpp"

namespace palmier::services::detail {

// ---------------------------------------------------------------------------
// Used only by this file, so still internal. Anything another translation unit
// needs is declared in ToolRegistryInternal.hpp instead.
// ---------------------------------------------------------------------------

namespace {

/// As above for a tool that has no default implementation: absent its hook the
/// capability is simply not configured in this build, which is an Unsupported
/// configuration report rather than an argument or state error. The no-project
/// refusal still comes first, so a caller with no project open always hears about
/// that rather than about the missing backend.
Tool::Handler guardedHookHandler(ProjectSession* session, std::string_view tool, Tool::Handler hook,
                                 std::string unavailable) {
    return sessionGuarded(session, tool,
                          [hook = std::move(hook),
                           unavailable = std::move(unavailable)](const Json& in) -> Result<Json> {
                              if (!hook) return err<Json>(unsupported(unavailable));
                              return hook(in);
                          });
}

}  // namespace

/// Wrap `handler` in the Requirement 3.5 guard: while no project is current the
/// tool refuses with "no project is open" before `handler` can run. Used by every
/// tool whose handler is not written inline (the hook-backed ones and the
/// session-backed defaults) except `project.create` and `project.open`, which
/// Requirement 3.5 exempts.
Tool::Handler sessionGuarded(ProjectSession* session, std::string_view tool,
                             Tool::Handler handler) {
    return [session, name = std::string(tool),
            handler = std::move(handler)](const Json& in) -> Result<Json> {
        if (session == nullptr) return err<Json>(noProjectOpen(name));
        return handler(in);
    };
}

Tool makeGenerateTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kGenerate;
    t.description = "Trigger generative media (image/video) from a prompt and place it.";
    // The hook's rules, lifted: the prompt length gate (1..2000 characters), the
    // closed media-type set, a canonical-UUID `trackId` the hook *requires*, and a
    // non-negative `framePosition`. `params`, `sourceInTicks` and `sourceOutTicks`
    // are read by the composition root's hook but were never declared; they are
    // declared here so the advertised schema names every accepted argument
    // (Requirement 9.3) and they survive `additionalProperties: false`.
    //
    // NOT expressible: `sourceOutTicks > sourceInTicks` (a cross-field relation) and
    // "the model id is one the configured backend serves" (an open, backend-defined
    // set) remain with the coordinator.
    //
    // `mode`, `sourceAssetId`, `targetWidth`/`targetHeight` and
    // `requestedDurationTicks` are all additive (usable-editor Phase 2 task 7;
    // PR 406/396/395): every argument that predates them is unchanged, and a
    // request that sets none of them behaves exactly as it did before their
    // addition. `mediaType`'s enum widens to admit `audio` for the same reason.
    t.schema
        .arg(ArgSpec{.name = "prompt",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Generation prompt (1..2000 characters).",
                     .minLength = 1,
                     .maxLength = 2000})
        .arg(stringArg("model", true, "Selected SOTA model id (e.g. 'veo', 'gpt-image')."))
        .arg(ArgSpec{.name = "mediaType",
                     .kind = JsonKind::String,
                     .description = "Requested media type: 'video', 'image' or 'audio'.",
                     .enumValues = {"video", "image", "audio"}})
        .arg(ArgSpec{.name = "mode",
                     .kind = JsonKind::String,
                     .description = "Generation mode: 'generate' (default) or 'upscale'.",
                     .enumValues = {"generate", "upscale"}})
        .arg(uuidArg("sourceAssetId", false,
                     "Source clip asset id for 'upscale' mode, or for audio "
                     "generated FROM a clip rather than a prompt alone."))
        .arg(
            intArg("targetWidth", false, "Requested output width in pixels for 'upscale' mode.", 1))
        .arg(intArg("targetHeight", false, "Requested output height in pixels for 'upscale' mode.",
                    1))
        .arg(intArg("requestedDurationTicks", false,
                    "Requested audio duration in nanoseconds for mediaType 'audio'.", 1))
        .arg(ArgSpec{.name = "params",
                     .kind = JsonKind::Object,
                     .description = "Model-specific string parameters."})
        .arg(uuidArg("trackId", true, "UUID of the track to place the generated clip on."))
        .arg(intArg("framePosition", false, "Placement position in frames from timeline start.", 0))
        .arg(intArg("sourceInTicks", false, "Source in-point of the generated clip in nanoseconds.",
                    0))
        .arg(intArg("sourceOutTicks", false,
                    "Source out-point of the generated clip in nanoseconds "
                    "(> sourceInTicks).",
                    1));
    t.handler = guardedHookHandler(
        session, kGenerate, std::move(hook),
        "generation.generate is not available: no generative backend is configured");
    return t;
}

Tool makeListModelsTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kListModels;
    t.description = "List every model in the generation model catalog, grouped by "
                    "provider (usable-editor Phase 2 task 7; PR 406).";
    // No arguments: the catalog is fixed, in-tree data (see
    // services/GenerationModelCatalog.hpp), not a per-request query.
    t.handler = guardedHookHandler(
        session, kListModels, std::move(hook),
        "generation.list_models is not available: no model catalog is configured");
    return t;
}

Tool makeTranscribeToCaptionsTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kTranscribeToCaptions;
    t.description = "Transcribe a clip's audio and place the resulting segments as caption "
                    "cues on a caption track (usable-editor task 13; Requirement 10.4).";
    // NOT expressible: "the named source clip exists and has decodable audio" is
    // a state-dependent rule the schema cannot see; the hook (an adapter over
    // services::TranscriptionService::transcribe) reports it as NotFound / a
    // Requirement 4.4-style "no audio found" indication.
    t.schema.arg(uuidArg("sourceClipId", true, "UUID of the clip whose audio is transcribed."))
        .arg(uuidArg("captionTrackId", true,
                     "UUID of the target CAPTION track the produced "
                     "cues are placed on."));
    // Requirement 10.5: absent the hook (no recognizer backend configured), this
    // reports that precondition by name and leaves the project unchanged,
    // without touching any of the OTHER three caption tools, none of which need
    // this hook — hand-authored caption editing stays available either way.
    t.handler = guardedHookHandler(
        session, kTranscribeToCaptions, std::move(hook),
        "timeline.transcribe_to_captions is not available: no recognizer backend is "
        "configured");
    return t;
}

Tool makeCaptureFrameTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kCaptureFrame;
    t.description = "Write the frame at a given timeline position to an image file "
                    "(usable-editor tasks.md task 14).";
    // NOT expressible: "the parent directory exists and is writable" is a
    // filesystem-state rule the schema cannot see; the hook (an adapter over
    // services::captureFrame) reports it as whatever the underlying encoder
    // reports.
    t.schema.arg(stringArg("outputPath", true, "Destination image file path."))
        .arg(intArg("positionNs", true, "Timeline position to capture, in nanoseconds.", 0));
    // Absent the hook (no image encoder configured), this reports that
    // precondition by name; every OTHER tool is unaffected either way.
    t.handler = guardedHookHandler(
        session, kCaptureFrame, std::move(hook),
        "timeline.capture_frame is not available: no image encoder is configured");
    return t;
}

Tool makeExportTool(ProjectSession* session, Tool::Handler hook) {
    Tool t;
    t.name = kExport;
    t.description = "Render the timeline to an output file at a selected container, codec, "
                    "resolution, frame rate and bit rate.";
    // Task 9.7 wires the hook to `services::ExportCoordinator`, and Requirement 7.2
    // states what a caller may ask for: "an output path, container format, video
    // codec, resolution, frame rate and bit rate". Three of those six had no
    // argument to arrive in, so the declaration below ADDS `codec`, `fps` and
    // `bitrateKbps`, plus the three booleans the export requirements make a caller
    // responsible for: `includeAudio`, `preferHardware` (Requirement 8.2's "an
    // export request specifies hardware encoding") and `overwrite` (Requirement
    // 7.11's explicit acknowledgement, which must be expressible or an existing
    // destination could never be replaced through the tool surface). Every addition
    // is OPTIONAL, so every argument object that was valid before this task is
    // still valid and means the same thing; each omitted argument is defaulted from
    // the CURRENT PROJECT (canvas, timeline frame rate) or from the documented
    // default in `services::exportRequestFromToolArguments`.
    //
    // `width` and `height` also gain their real bounds. They previously published
    // `minimum: 1`, which advertised resolutions the Export_Coordinator has always
    // rejected; the published range is now exactly Requirement 7.1's, so an
    // out-of-range geometry is refused by the advertised schema instead of only by
    // the handler (design.md D3). The numbers are the `ExportCoordinator` constants
    // (`kMinExportWidth` … `kMaxExportBitrateKbps`); they are restated here rather
    // than included so this surface keeps depending only on `Palmier::core`, and
    // `ExportToolSchema.MatchesTheCoordinatorRanges` asserts the two agree.
    //
    // NOT expressible, and therefore still checked by `ExportCoordinator::validate`
    // (which owns the message naming the parameter and its accepted values):
    // the container vocabulary (mp4 | mov | mkv | webm) — `format` has always been
    // published as a free string and narrowing it would invalidate argument objects
    // that are accepted today — the parent directory's existence and writability,
    // and whether the destination already exists.
    t.schema
        .arg(ArgSpec{.name = "outputPath",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Destination file path for the rendered output.",
                     .minLength = 1})
        .arg(stringArg("format", true, "Output container/codec format."))
        .arg(ArgSpec{.name = "width",
                     .kind = JsonKind::Integer,
                     .description = "Output width in pixels (128..3840); defaults to the "
                                    "project canvas width.",
                     .minInt = kMinExportWidthPixels,
                     .maxInt = kMaxExportWidthPixels})
        .arg(ArgSpec{.name = "height",
                     .kind = JsonKind::Integer,
                     .description = "Output height in pixels (128..2160); defaults to the "
                                    "project canvas height.",
                     .minInt = kMinExportHeightPixels,
                     .maxInt = kMaxExportHeightPixels})
        .arg(ArgSpec{.name = "codec",
                     .kind = JsonKind::String,
                     .description = "Video codec; defaults to vp9 for a webm container and "
                                    "h264 otherwise.",
                     .enumValues = exportCodecValues()})
        .arg(ArgSpec{.name = "fps",
                     .kind = JsonKind::Number,
                     .description = "Output frame rate in frames per second (1..120); "
                                    "defaults to the project timeline frame rate.",
                     .minNum = kMinExportFramesPerSecond,
                     .maxNum = kMaxExportFramesPerSecond})
        .arg(ArgSpec{.name = "bitrateKbps",
                     .kind = JsonKind::Integer,
                     .description = "Video bit rate in kilobits per second (100..200000); "
                                    "defaults to 8000.",
                     .minInt = kMinExportBitrateKilobits,
                     .maxInt = kMaxExportBitrateKilobits})
        .arg(ArgSpec{.name = "includeAudio",
                     .kind = JsonKind::Bool,
                     .description = "Write an audio stream; defaults to true."})
        .arg(ArgSpec{.name = "preferHardware",
                     .kind = JsonKind::Bool,
                     .description = "Request hardware encoding when a compiled-in vendor path "
                                    "and a capable device are present; defaults to true."})
        .arg(ArgSpec{.name = "overwrite",
                     .kind = JsonKind::Bool,
                     .description = "Acknowledge replacing an existing destination file; "
                                    "defaults to false, which preserves it."});
    t.handler =
        guardedHookHandler(session, kExport, std::move(hook),
                           "timeline.export is not available: no export engine is configured");
    return t;
}

}  // namespace palmier::services::detail
