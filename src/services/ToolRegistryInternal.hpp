// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolRegistryInternal.hpp — the internal contract between the
// translation units that make up the editor tool surface.
//
// services/ToolRegistry.cpp used to declare the whole surface — every tool name,
// every shared helper and all 42 tool factories — in a single anonymous namespace
// in a single 2767-line file. The tools are now grouped one translation unit per
// subject, which means the factories can no longer have internal linkage: a factory
// defined in TimelineClipTools.cpp has to be callable from buildDefaultToolRegistry
// in ToolRegistry.cpp. This header is that seam.
//
// It is deliberately PRIVATE. It sits beside its .cpp files rather than in the
// public surface, `detail` says so in the name, and nothing outside src/services/
// includes it. The public contract is unchanged and remains exactly
// services/ToolRegistry.hpp — `Tool`, `ToolRegistry` and `buildDefaultToolRegistry`.
//
// Only symbols used by MORE THAN ONE of those translation units are declared here;
// which ones those are was derived from where each symbol is referenced, not chosen.
// A helper needed by one file stays in an anonymous namespace inside that file, so
// internal linkage is kept everywhere it is still correct.
//
// The split moved code and changed nothing else: the same 42 tools are registered in
// the same order with the same schemas and the same handlers, so `tools/list` output
// and docs/TOOLS.md are unaffected.

#ifndef PALMIER_SERVICES_TOOLREGISTRYINTERNAL_HPP
#define PALMIER_SERVICES_TOOLREGISTRYINTERNAL_HPP

// The declarations below are written in terms of the domain vocabulary — EffectType,
// CurveChannel, TransitionKind, TrackKind, ColorSpace, FrameRate, Uuid, Project,
// Clip, MediaAssetRef, Json, Result, ArgSpec, Tool — so this header needs those
// types and includes them itself. That is also why each .cpp beside it includes only
// this header: the tool surface has one common dependency set, and stating it once
// here is honest about that, where repeating the same thirty includes in eleven files
// would only invite them to drift apart.
#include "services/ToolRegistry.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/ToneCurve.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"

namespace palmier::services::detail {

// ---------------------------------------------------------------------------
// Tool names (MCP-style dotted namespaces, matching design.md examples).
// ---------------------------------------------------------------------------

inline constexpr const char* kReadTimeline = "timeline.read";
inline constexpr const char* kAddClip = "timeline.add_clip";
inline constexpr const char* kDeleteClip = "timeline.delete_clip";
inline constexpr const char* kMoveClip = "timeline.move_clip";
inline constexpr const char* kSetClipProperties = "timeline.set_clip_properties";
inline constexpr const char* kApplyLayout = "timeline.apply_layout";
inline constexpr const char* kSetKeyframes = "timeline.set_keyframes";
inline constexpr const char* kTrimClip = "timeline.trim_clip";
inline constexpr const char* kSplitClip = "timeline.split_clip";
inline constexpr const char* kReorderClips = "timeline.reorder_clips";
// Ripple editing and gap management (usable-editor Phase 3 task 8; Requirement 5).
inline constexpr const char* kRippleDelete = "timeline.ripple_delete";
inline constexpr const char* kRippleTrim = "timeline.ripple_trim";
inline constexpr const char* kCloseGap = "timeline.close_gap";
inline constexpr const char* kAddEffect = "timeline.add_effect";
// Effect lifecycle management (usable-editor Phase 3 task 9; Requirement 6).
inline constexpr const char* kRemoveEffect = "timeline.remove_effect";
inline constexpr const char* kReorderEffects = "timeline.reorder_effects";
inline constexpr const char* kSetEffectParameter = "timeline.set_effect_parameter";
inline constexpr const char* kEditCurvePoint = "timeline.edit_curve_point";
inline constexpr const char* kSetEffectResource = "timeline.set_effect_resource";
inline constexpr const char* kAddTransition = "timeline.add_transition";
// Text and titles (usable-editor Phase 4 task 12; Requirement 9).
inline constexpr const char* kAddTextClip = "timeline.add_text_clip";
inline constexpr const char* kSetTextContent = "timeline.set_text_content";
inline constexpr const char* kSetTextStyle = "timeline.set_text_style";
// Captions and transcription (usable-editor Phase 4 task 13; Requirement 10).
inline constexpr const char* kAddCaptionCue = "timeline.add_caption_cue";
inline constexpr const char* kSetCaptionText = "timeline.set_caption_text";
inline constexpr const char* kRetimeCaptionCue = "timeline.retime_caption_cue";
inline constexpr const char* kRemoveCaptionCue = "timeline.remove_caption_cue";
inline constexpr const char* kTranscribeToCaptions = "timeline.transcribe_to_captions";
inline constexpr const char* kGenerate = "generation.generate";
inline constexpr const char* kListModels = "generation.list_models";
inline constexpr const char* kExport = "timeline.export";
// Still-frame capture (usable-editor tasks.md task 14; no dedicated Requirement).
inline constexpr const char* kCaptureFrame = "timeline.capture_frame";
// Session, media-library and track tools (tasks 4.3-4.5; Requirement 3.1).
inline constexpr const char* kProjectCreate = "project.create";
inline constexpr const char* kProjectOpen = "project.open";
inline constexpr const char* kProjectSave = "project.save";
inline constexpr const char* kProjectInfo = "project.info";
// Mutable project settings (usable-editor Phase 3 task 10; Requirement 7).
inline constexpr const char* kSetProjectSettings = "project.set_settings";
inline constexpr const char* kMediaImport = "media.import";
inline constexpr const char* kMediaList = "media.list";
// Media organisation (usable-editor tasks.md task 15; no dedicated Requirement).
inline constexpr const char* kMediaSetTags = "media.set_tags";
inline constexpr const char* kAddTrack = "timeline.add_track";
inline constexpr const char* kRemoveTrack = "timeline.remove_track";
inline constexpr const char* kSetTrackMuted = "timeline.set_track_muted";
// Timeline markers (upstream parity: the `timeline` tool category's marker gap).
inline constexpr const char* kAddMarker = "timeline.add_marker";
inline constexpr const char* kRemoveMarker = "timeline.remove_marker";
inline constexpr const char* kUndo = "edit.undo";
inline constexpr const char* kRedo = "edit.redo";
// The accepted length of a filesystem path argument (`project.open`,
// `project.save`, `media.import`), matching the design's tool table.
inline constexpr std::size_t kMinPathLength = 1;
inline constexpr std::size_t kMaxPathLength = 4096;
// `timeline.export`'s accepted ranges (task 9.7; Requirement 7.1). These are the
// `services::ExportCoordinator` constants — `kMinExportWidth`, `kMaxExportWidth`,
// `kMinExportHeight`, `kMaxExportHeight`, `kMinExportFps`, `kMaxExportFps`,
// `kMinExportBitrateKbps`, `kMaxExportBitrateKbps` — restated rather than included
// so this tool surface keeps depending only on `Palmier::core`. The unit test
// `ExportToolSchema.MatchesTheCoordinatorRanges` (which links both) asserts each
// pair is equal, so the two declarations cannot drift.
inline constexpr std::int64_t kMinExportWidthPixels = 128;
inline constexpr std::int64_t kMaxExportWidthPixels = 3840;
inline constexpr std::int64_t kMinExportHeightPixels = 128;
inline constexpr std::int64_t kMaxExportHeightPixels = 2160;
inline constexpr double kMinExportFramesPerSecond = 1.0;
inline constexpr double kMaxExportFramesPerSecond = 120.0;
inline constexpr std::int64_t kMinExportBitrateKilobits = 100;
inline constexpr std::int64_t kMaxExportBitrateKilobits = 200'000;

// ---------------------------------------------------------------------------
// Shared helpers, each defined once in the file its subject names.
// ---------------------------------------------------------------------------

const std::vector<std::string>& exportCodecValues();

Error noProjectOpen(std::string_view tool);

std::string_view effectTypeName(EffectType type);

const std::vector<std::string>& effectTypeValues();

std::optional<EffectType> parseEffectType(std::string_view s);

const std::vector<std::string>& curveChannelValues();

std::optional<CurveChannel> parseCurveChannel(std::string_view s);

const std::vector<std::string>& curvePointOperationValues();

std::optional<EditCurvePointCommand::Operation> parseCurvePointOperation(std::string_view s);

std::string_view transitionKindName(TransitionKind kind);

const std::vector<std::string>& transitionKindValues();

/// The accepted `layout` values of `timeline.apply_layout`, declared once for the same
/// reason as transitionKindValues().
const std::vector<std::string>& layoutKindValues();

/// The accepted `property` values of `timeline.set_keyframes`, derived from the animatable
/// property set itself so the published list and the parser's accepted set cannot diverge.
const std::vector<std::string>& animatablePropertyValues();

/// The accepted `interpolation` values of a keyframe.
const std::vector<std::string>& interpolationValues();

std::optional<TransitionKind> parseTransitionKind(std::string_view s);

/// The LayoutKind named by `s`, or nullopt when it names none.
std::optional<LayoutKind> parseLayoutKind(std::string_view s);

std::string_view trackKindName(TrackKind kind);

const std::vector<std::string>& trackKindValues();

std::optional<TrackKind> parseTrackKind(std::string_view s);

const std::vector<std::string>& colorSpaceValues();

std::optional<ColorSpace> parseColorSpace(std::string_view s);

FrameRate frameRateFromFps(double fps);

std::size_t countClips(const Project& project);

Json serializeFrameRate(FrameRate fps);

Json serializeCanvas(Resolution canvas);

Json serializeDocumentPath(const std::optional<std::filesystem::path>& path);

Result<Uuid> requireUuid(const Json& in, std::string_view key);

Result<std::int64_t> requireInt(const Json& in, std::string_view key);

Result<double> requireNumber(const Json& in, std::string_view key);

Result<std::string> requireString(const Json& in, std::string_view key);

Json serializeProject(const Project& project);

Result<Json> applyCommand(TimelineEngine& engine, std::unique_ptr<EditCommand> cmd, Json success);

ArgSpec uuidArg(std::string name, bool required, std::string description);

ArgSpec intArg(std::string name, bool required, std::string description,
               std::optional<std::int64_t> minimum = std::nullopt);

ArgSpec stringArg(std::string name, bool required, std::string description);

Tool::Handler sessionGuarded(ProjectSession* session, std::string_view tool, Tool::Handler handler);

// ---------------------------------------------------------------------------
// Tool factories, one per published tool.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tool handler factories (each captures the SESSION and resolves its engine at
// invocation time).
//
// Task 3.4 / design.md D1: a handler must not capture a `TimelineEngine&` taken
// when the registry was built. The session owns one stable engine and swaps the
// project value inside it (TimelineEngine::reset), so resolving the engine per
// call is what makes a `project.open` that happens AFTER registration observable
// to these handlers.
//
// Task 4.3 / Requirement 3.5: the session is held as a POINTER, and a null
// pointer is the "no project is current" state — the same representation
// McpToolExecutor and MediaImportService already use. Every handler other than
// `project.create`'s and `project.open`'s therefore opens with
//
//     if (session == nullptr) return err<Json>(noProjectOpen(<tool>));
//     TimelineEngine& engine = session->engine();
//
// so the refusal happens before any argument is parsed, any command is
// constructed or any library is touched: nothing can have changed by the time the
// error is returned.
// ---------------------------------------------------------------------------

Tool makeReadTool(ProjectSession* session);
Tool makeAddClipTool(ProjectSession* session);
Tool makeDeleteClipTool(ProjectSession* session);
Tool makeMoveClipTool(ProjectSession* session);
Tool makeSetClipPropertiesTool(ProjectSession* session);
Tool makeApplyLayoutTool(ProjectSession* session);
Tool makeSetKeyframesTool(ProjectSession* session);
Tool makeTrimClipTool(ProjectSession* session);
Tool makeRippleDeleteTool(ProjectSession* session);
Tool makeRippleTrimTool(ProjectSession* session);
Tool makeCloseGapTool(ProjectSession* session);
Tool makeSplitClipTool(ProjectSession* session);
Tool makeReorderClipsTool(ProjectSession* session);
Tool makeAddEffectTool(ProjectSession* session);
Tool makeRemoveEffectTool(ProjectSession* session);
Tool makeReorderEffectsTool(ProjectSession* session);
Tool makeSetEffectParameterTool(ProjectSession* session);
Tool makeSetEffectResourceTool(ProjectSession* session);
Tool makeEditCurvePointTool(ProjectSession* session);
Tool makeAddTransitionTool(ProjectSession* session);
Tool makeAddTextClipTool(ProjectSession* session);
Tool makeSetTextContentTool(ProjectSession* session);
Tool makeSetTextStyleTool(ProjectSession* session);
Tool makeAddCaptionCueTool(ProjectSession* session);
Tool makeSetCaptionTextTool(ProjectSession* session);
Tool makeRetimeCaptionCueTool(ProjectSession* session);
Tool makeRemoveCaptionCueTool(ProjectSession* session);
Tool makeGenerateTool(ProjectSession* session, Tool::Handler hook);
Tool makeListModelsTool(ProjectSession* session, Tool::Handler hook);
Tool makeTranscribeToCaptionsTool(ProjectSession* session, Tool::Handler hook);
Tool makeCaptureFrameTool(ProjectSession* session, Tool::Handler hook);
Tool makeExportTool(ProjectSession* session, Tool::Handler hook);
Tool makeProjectCreateTool(ProjectSession* session, Tool::Handler hook);
Tool makeProjectOpenTool(ProjectSession* session, Tool::Handler hook);
Tool makeProjectSaveTool(ProjectSession* session, Tool::Handler hook);
Tool makeProjectInfoTool(ProjectSession* session, Tool::Handler hook);
Tool makeSetProjectSettingsTool(ProjectSession* session);
Tool makeMediaImportTool(ProjectSession* session, ToolRegistryHooks::MediaImportHook hook);
Tool makeMediaListTool(ProjectSession* session, Tool::Handler hook);
Tool makeMediaSetTagsTool(ProjectSession* session);
Tool makeAddTrackTool(ProjectSession* session);
Tool makeRemoveTrackTool(ProjectSession* session);
Tool makeSetTrackMutedTool(ProjectSession* session);
Tool makeAddMarkerTool(ProjectSession* session);
Tool makeRemoveMarkerTool(ProjectSession* session);
Tool makeUndoTool(ProjectSession* session);
Tool makeRedoTool(ProjectSession* session);

}  // namespace palmier::services::detail

#endif  // PALMIER_SERVICES_TOOLREGISTRYINTERNAL_HPP
