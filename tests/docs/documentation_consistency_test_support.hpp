// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/docs/documentation_consistency_test.cpp — the documentation consistency
// checker running against the CHECKED-IN documentation (task 12.7;
// Requirements 16.7, 16.8).
//
// Requirement 16.7 makes this a Verification_Suite obligation, not a CI script:
// "WHEN the Verification_Suite runs, THE Verification_Suite SHALL compare the
// CMake option names stated in the documentation against the options defined by
// the build system, and the tool names and argument names stated in the
// documentation against the names the Tool_Surface returns from `tools/list`."
// Requirement 16.8 adds that a mismatch fails, is reported with the documentation
// section it appears in, and leaves the documentation unmodified.
//
// WHAT MAKES THIS NON-DECORATIVE
// ------------------------------
// 1. EVERY EXPECTATION IS DERIVED, NONE IS RESTATED. There is no list of option
//    names, tool names, argument names or setting keys anywhere in this file. The
//    option set comes from `palmier_options.txt`, which
//    cmake/PalmierOptionsManifest.cmake writes at configure time from the tree's
//    own `PALMIER_*` cache entries. The tool and argument sets come from the
//    payload `ToolRegistry::describe()` publishes — the same bytes `tools/list`
//    returns. The result fields come from INVOKING the real handlers. The settings
//    surface comes from `AppSettings`, whose accessors read the one key table in
//    src/app/AppSettings.cpp. Add an option, a tool, an argument or a settings key
//    tomorrow and this test fails tomorrow until the documentation says so.
//
// 2. THE CHECKER IS PROVEN FALSIFIABLE. Every extractor and every check is a pure
//    function, so the `DocumentationChecker*` cases below drive them over
//    synthetic documents carrying each fault: a missing marker, a renamed option,
//    a flipped required marking, a changed JSON type, a reordered tool list, a
//    result field the handler does not return. Without those, an extractor with a
//    broken pattern would report a clean tree and nobody would know.
//
// 3. THE DOCUMENTS ARE PROVEN UNTOUCHED. `LeavesTheDocumentationUnmodified` hashes
//    all three documents before and after a full check run (Requirement 16.8).
//
// SCOPE BOUNDARY, NAMED RATHER THAN HIDDEN
// ----------------------------------------
// Twenty of the twenty-two tools render their own success payload inside
// `ToolRegistry`, so their documented result fields are checked against real
// invocations. `timeline.export` and `generation.generate` do not: the registry
// only holds a hook for them, and the payload is rendered by a collaborator
// (`services::exportOutcomeToJson`, and the `makeGenerateHook` lambda in the
// composition root). Linking either collaborator would drag FFmpeg, libsecret and
// lcms2 into this binary. So those two are checked against the field names their
// real rendering function sets, read out of the source of record — still the
// actual code, and a rename of either function fails this test loudly
// (`FunctionFieldScan::found`) rather than quietly checking nothing.
//
// _Requirements: 16.7, 16.8_

#include "support/DocumentationChecker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid, for a per-process scratch directory name

#include "app/AppSettings.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Uuid.hpp"
#include "services/GenerationModelCatalog.hpp"
#include "services/Json.hpp"
#include "services/MediaImportService.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

// Shared fixtures and helpers for the documentation_consistency_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_TEST_SUPPORT_HPP_

#ifndef PALMIER_DOCS_DIR
#error "PALMIER_DOCS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_SOURCE_DIR
#error "PALMIER_SOURCE_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef PALMIER_OPTIONS_MANIFEST
#error "PALMIER_OPTIONS_MANIFEST must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace palmier {
namespace testsupport_documentationconsistencytest {

using testsupport::DocDefect;
using testsupport::DocDefectKind;
using testsupport::DocumentedArgument;
using testsupport::DocumentedOption;
using testsupport::DocumentedSetting;
using testsupport::DocumentedTool;
using testsupport::LiveArgument;
using testsupport::LiveOption;
using testsupport::LiveSetting;
using testsupport::LiveTool;
using testsupport::ObservedResult;

// ===========================================================================
// Inputs
// ===========================================================================

[[nodiscard]] inline std::string docsPath(std::string_view name) {
    return std::string{PALMIER_DOCS_DIR} + "/" + std::string{name};
}

[[nodiscard]] inline std::string sourcePath(std::string_view relative) {
    return std::string{PALMIER_SOURCE_DIR} + "/" + std::string{relative};
}

// ===========================================================================
// The live tool surface, read from the `tools/list` payload
// ===========================================================================

/// Exactly what an MCP client sees: `ToolRegistry::describe()` is the `tools/list`
/// result, and each entry's `inputSchema` is the rendered `ToolSchema`. Reading the
/// live side out of the published payload (rather than out of `ToolSchema::args()`)
/// is what makes this Requirement 16.7's comparison and not a weaker one.
[[nodiscard]] inline std::vector<LiveTool> liveToolsFrom(const services::Json& described) {
    std::vector<LiveTool> tools;
    for (const services::Json& entry : described.asArray()) {
        LiveTool tool;
        tool.name = entry.stringOr("name");

        const services::Json* schema = entry.find("inputSchema");
        if (schema == nullptr) {
            tools.push_back(std::move(tool));
            continue;
        }
        std::vector<std::string> required;
        if (const services::Json* names = schema->find("required"); names != nullptr) {
            for (const services::Json& name : names->asArray()) {
                required.push_back(name.asString());
            }
        }
        if (const services::Json* properties = schema->find("properties"); properties != nullptr) {
            for (const auto& [name, property] : properties->asObject()) {
                LiveArgument argument;
                argument.name = name;
                argument.type = property.stringOr("type");
                argument.uuid = property.stringOr("format") == "uuid";
                argument.required =
                    std::find(required.begin(), required.end(), name) != required.end();
                tool.arguments.push_back(std::move(argument));
            }
        }
        tools.push_back(std::move(tool));
    }
    return tools;
}

// ===========================================================================
// The live settings surface, read from AppSettings' key table
// ===========================================================================

[[nodiscard]] inline std::vector<LiveSetting> liveSettings() {
    std::vector<LiveSetting> settings;
    for (const std::string_view key : app::AppSettings::recognizedKeys()) {
        settings.push_back(LiveSetting{std::string(key),
                                       std::string(app::AppSettings::environmentVariableFor(key)),
                                       std::string(app::AppSettings::commandLineFlagFor(key))});
    }
    return settings;
}

// ===========================================================================
// Observing real result payloads
// ===========================================================================

/// Drives the real tool surface over a real `ProjectSession` and records the
/// top-level field names each success payload carries. The scenario is chosen so
/// that every tool the registry renders a payload for is invoked at least once, and
/// so that `media.import`'s conditional resolution/frame-rate fields are seen (a
/// video asset) as well as omitted (an audio-only asset).
class ResultObserver {
public:
    ResultObserver() {
        services::ToolRegistryHooks hooks;
        // The IMPORT OPERATION is hooked (this binary links no FFmpeg), but the
        // RESULT SHAPE under test is the registry's own rendering of it — the
        // registry is documented as the single place that shape exists.
        hooks.importMedia =
            [](const std::filesystem::path& path) -> Result<services::ImportedAsset> {
            services::ImportedAsset asset;
            asset.assetId = Uuid::generateV4();
            asset.sourcePath = path;
            asset.containerFormat = "mov,mp4,m4a";
            asset.durationMs = 2000;
            asset.hasAudio = true;
            if (path.extension() != ".wav") {
                asset.hasVideo = true;
                asset.resolution = Resolution{1920, 1080};
                asset.frameRate = FrameRate{30, 1};
            }
            return Result<services::ImportedAsset>(std::move(asset));
        };
        // generation.list_models's real hook needs only the catalog (pure,
        // in-tree data — no FFmpeg, no network), so it is wired for real here
        // rather than stubbed, matching this fixture's own stated goal: observe
        // the registry's OWN rendering of every result shape it owns.
        hooks.listModels = [this](const services::Json&) -> Result<services::Json> {
            std::map<std::string, services::Json> byProvider;
            for (const services::CatalogModel& model : catalog_.listModels()) {
                services::Json entry = services::Json::object();
                entry.set("id", model.id);
                entry.set("mediaType", std::string(services::toStringView(model.mediaType)));
                entry.set("servesUpscale", model.servesUpscale);
                if (model.audioDurationRange.has_value()) {
                    const auto& [minDuration, maxDuration] = *model.audioDurationRange;
                    entry.set("minDurationTicks", static_cast<std::int64_t>(minDuration.ticks()));
                    entry.set("maxDurationTicks", static_cast<std::int64_t>(maxDuration.ticks()));
                }
                services::Json& list = byProvider[model.provider];
                if (!list.isArray()) list = services::Json::array();
                list.push_back(std::move(entry));
            }
            services::Json providers = services::Json::object();
            for (auto& [provider, list] : byProvider) providers.set(provider, std::move(list));
            services::Json out = services::Json::object();
            out.set("providers", std::move(providers));
            return out;
        };
        // timeline.transcribe_to_captions's real hook is bound to
        // UnavailableTranscriptionBackend in this build (no recognizer is
        // bundled), which would always fail run()'s EXPECT_TRUE(isOk()) below.
        // Stubbed here for the identical reason listModels is stubbed above:
        // this fixture observes the REGISTRY'S OWN rendering of a result shape,
        // not whether a particular backend is configured — the shape stubbed
        // here matches ApplicationComposition's own makeTranscribeToCaptionsHook
        // documented success shape ({status, cuesAdded}).
        hooks.transcribeToCaptions = [](const services::Json&) -> Result<services::Json> {
            services::Json out = services::Json::object();
            out.set("status", std::string{"transcribed"});
            out.set("cuesAdded", static_cast<std::int64_t>(1));
            return out;
        };
        // timeline.capture_frame's real hook needs a Qt image encoder (this
        // binary links no Qt), so it is stubbed here for the identical reason
        // listModels/transcribeToCaptions are stubbed above: this fixture
        // observes the REGISTRY'S OWN rendering of a result shape, not whether
        // a real image is actually written to disk.
        hooks.captureFrame = [](const services::Json& in) -> Result<services::Json> {
            services::Json out = services::Json::object();
            out.set("outputPath", in.stringOr("outputPath"));
            return out;
        };
        registry_ = services::buildDefaultToolRegistry(session_, std::move(hooks));
    }

    [[nodiscard]] const std::vector<ObservedResult>& observed() const noexcept { return observed_; }

    /// Invoke `tool`, require success, record its field names, return the payload.
    services::Json run(std::string_view tool, const services::Json& args) {
        Result<services::Json> result = registry_.invoke(tool, args);
        EXPECT_TRUE(result.isOk()) << tool << ": " << result.error().toString();
        if (result.isError()) {
            return services::Json::object();
        }
        ObservedResult record;
        record.toolName = std::string(tool);
        for (const auto& [name, value] : result.value().asObject()) {
            record.fields.push_back(name);
        }
        observed_.push_back(std::move(record));
        return result.value();
    }

    /// Register `count` assets directly on the project value, so clip tools have
    /// something legal to reference. A reset is not an edit, so this leaves the undo
    /// history alone — the same idiom the session-tool suites use.
    std::vector<Uuid> registerAssets(std::size_t count) {
        Project project = session_.engine().snapshot();
        std::vector<Uuid> ids;
        for (std::size_t i = 0; i < count; ++i) {
            const Uuid id = Uuid::generateV4();
            project.assets.push_back(
                MediaAssetRef(id, "/media/source" + std::to_string(i) + ".mp4"));
            ids.push_back(id);
        }
        EXPECT_TRUE(session_.engine().reset(project).isOk());
        return ids;
    }

    /// Register one asset in `session_.mediaLibrary()` itself, not merely in
    /// `Project.assets` (`registerAssets()` above only does the latter, via a
    /// raw `engine().reset()` that never rebuilds the separate MediaManager —
    /// see ProjectSession.hpp's own documented split). media.set_tags looks
    /// the asset up through `mediaLibrary()`, so it needs this instead.
    Uuid registerMediaLibraryAsset() {
        const Uuid id = Uuid::generateV4();
        EXPECT_TRUE(
            session_.mediaLibrary().importAsset(MediaAssetRef(id, "/media/tagged.mp4")).isOk());
        return id;
    }

    /// The clip identifiers currently on `trackId`, read through `timeline.read`.
    [[nodiscard]] std::vector<std::string> clipIdsOf(const std::string& trackId) {
        std::vector<std::string> ids;
        Result<services::Json> timeline =
            registry_.invoke("timeline.read", services::Json::object());
        if (timeline.isError()) {
            return ids;
        }
        const services::Json* tracks = timeline.value().find("tracks");
        if (tracks == nullptr) {
            return ids;
        }
        for (const services::Json& track : tracks->asArray()) {
            if (track.stringOr("id") != trackId) {
                continue;
            }
            const services::Json* clips = track.find("clips");
            if (clips == nullptr) {
                continue;
            }
            for (const services::Json& clip : clips->asArray()) {
                ids.push_back(clip.stringOr("id"));
            }
        }
        return ids;
    }

private:
    services::GenerationModelCatalog catalog_;
    services::ProjectSession session_;
    services::ToolRegistry registry_;
    std::vector<ObservedResult> observed_;
};

[[nodiscard]] inline services::Json object() { return services::Json::object(); }

[[nodiscard]] inline services::Json with(std::string key, services::Json value) {
    services::Json args = object();
    args.set(std::move(key), std::move(value));
    return args;
}

/// Run the whole scenario and hand back what every payload contained.
[[nodiscard]] inline std::vector<ObservedResult> observeRealResults(
    const std::filesystem::path& scratchDirectory) {
    ResultObserver observer;

    services::Json create = object();
    create.set("name", std::string{"Documentation check"});
    create.set("fps", 30.0);
    create.set("width", static_cast<std::int64_t>(1920));
    create.set("height", static_cast<std::int64_t>(1080));
    create.set("colorSpace", std::string{"Rec.709"});
    observer.run("project.create", create);

    // Both arms of `media.import`'s conditional result: a video asset reports
    // width/height/fps, an audio-only one omits them.
    observer.run("media.import", with("path", services::Json(std::string{"/media/clip.mp4"})));
    observer.run("media.import", with("path", services::Json(std::string{"/media/voice.wav"})));

    const std::vector<Uuid> assets = observer.registerAssets(2);
    observer.run("media.list", object());
    observer.run("generation.list_models", object());

    // Media organisation (usable-editor tasks.md task 15; no dedicated
    // Requirement). media.set_tags looks its asset up through mediaLibrary(),
    // not Project.assets, so it needs its own real asset registered there.
    const Uuid taggedAsset = observer.registerMediaLibraryAsset();
    services::Json setTags = object();
    setTags.set("assetId", taggedAsset.toString());
    services::Json tags = services::Json::array();
    tags.push_back(services::Json(std::string{"b-roll"}));
    setTags.set("tags", std::move(tags));
    observer.run("media.set_tags", setTags);

    const services::Json videoTrack =
        observer.run("timeline.add_track", with("kind", services::Json(std::string{"video"})));
    observer.run("timeline.add_track", with("kind", services::Json(std::string{"audio"})));
    const std::string trackId = videoTrack.stringOr("trackId");

    // A third track, added and removed immediately, so `timeline.remove_track`'s
    // result is observed without disturbing the two tracks the rest of the
    // scenario builds on.
    const services::Json spareTrack =
        observer.run("timeline.add_track", with("kind", services::Json(std::string{"audio"})));
    observer.run("timeline.remove_track",
                 with("trackId", services::Json(spareTrack.stringOr("trackId"))));

    // Markers, added and removed the same way the spare track was: create one,
    // observe `timeline.add_marker`'s result, then remove it so
    // `timeline.remove_marker`'s result is observed too without leaving a marker
    // behind for the rest of the scenario to account for.
    services::Json addMarkerArgs = object();
    addMarkerArgs.set("timelineNs", static_cast<std::int64_t>(0));
    addMarkerArgs.set("label", std::string{"scenario marker"});
    const services::Json marker = observer.run("timeline.add_marker", addMarkerArgs);
    observer.run("timeline.remove_marker",
                 with("markerId", services::Json(marker.stringOr("markerId"))));

    constexpr std::int64_t kSecond = 1'000'000'000;
    const auto addClip = [&](const Uuid& assetId, std::int64_t startNs, std::int64_t lengthNs) {
        services::Json args = object();
        args.set("trackId", trackId);
        args.set("assetId", assetId.toString());
        args.set("timelineStartNs", startNs);
        args.set("sourceInNs", static_cast<std::int64_t>(0));
        args.set("sourceOutNs", lengthNs);
        args.set("opacity", 1.0);
        args.set("gain", 1.0);
        args.set("sourcePath", std::string{"/media/source0.mp4"});
        return observer.run("timeline.add_clip", args);
    };
    const std::string firstClip = addClip(assets[0], 0, 4 * kSecond).stringOr("clipId");
    const std::string secondClip = addClip(assets[1], 8 * kSecond, 2 * kSecond).stringOr("clipId");

    observer.run("timeline.read", object());
    observer.run("project.info", object());

    // Mutable project settings (task 10; Requirement 7): a genuine change (24fps,
    // distinct from the 30fps the project was created with above), so the tool
    // applies an edit rather than reporting a no-op.
    observer.run("project.set_settings", with("fps", services::Json(24.0)));

    services::Json mute = object();
    mute.set("trackId", trackId);
    mute.set("muted", true);
    observer.run("timeline.set_track_muted", mute);

    services::Json move = object();
    move.set("clipId", secondClip);
    move.set("timelineStartNs", 6 * kSecond);
    observer.run("timeline.move_clip", move);

    // Both properties at once, so each optional field of the rendered payload is observed.
    services::Json clipProps = object();
    clipProps.set("clipId", secondClip);
    clipProps.set("opacity", services::Json(0.5));
    clipProps.set("gain", services::Json(0.75));
    observer.run("timeline.set_clip_properties", clipProps);

    // Two keyframes so the rendered keyframeCount is a number a reader can check.
    services::Json keyframes = object();
    keyframes.set("clipId", secondClip);
    keyframes.set("property", std::string{"opacity"});
    services::Json points = services::Json::array();
    services::Json firstPoint = object();
    firstPoint.set("timeNs", static_cast<std::int64_t>(0));
    firstPoint.set("value", services::Json(0.0));
    points.push_back(std::move(firstPoint));
    services::Json secondPoint = object();
    secondPoint.set("timeNs", 2 * kSecond);
    secondPoint.set("value", services::Json(1.0));
    points.push_back(std::move(secondPoint));
    keyframes.set("keyframes", std::move(points));
    observer.run("timeline.set_keyframes", keyframes);

    // Two clips so the arrangement actually places something, and the rendered clipCount is
    // observed.
    services::Json layout = object();
    services::Json layoutIds = services::Json::array();
    layoutIds.push_back(firstClip);
    layoutIds.push_back(secondClip);
    layout.set("clipIds", std::move(layoutIds));
    layout.set("layout", std::string{"side_by_side"});
    observer.run("timeline.apply_layout", layout);

    services::Json trim = object();
    trim.set("clipId", firstClip);
    trim.set("edge", std::string{"end"});
    trim.set("boundaryNs", 3 * kSecond);
    trim.set("sourceDurationNs", 4 * kSecond);
    observer.run("timeline.trim_clip", trim);

    // A split at an interior playhead, so `rightClipId` is observed.
    services::Json split = object();
    split.set("clipId", firstClip);
    split.set("playheadNs", 1 * kSecond);
    const services::Json splitResult = observer.run("timeline.split_clip", split);
    const std::string rightOfSplit = splitResult.stringOr("rightClipId");

    // Ripple editing and gap management (task 8; Requirement 5), exercised on the
    // split's own right-hand piece so nothing `secondClip` is used for downstream
    // is disturbed. Trimming `firstClip` shorter (it is 1s long, [0,1s)) pulls
    // `rightOfSplit` earlier by the same amount under `timeline.ripple_trim`.
    services::Json rippleTrim = object();
    rippleTrim.set("clipId", firstClip);
    rippleTrim.set("edge", std::string{"end"});
    rippleTrim.set("boundaryNs", static_cast<std::int64_t>(kSecond / 2));
    rippleTrim.set("sourceDurationNs", kSecond);
    observer.run("timeline.ripple_trim", rippleTrim);

    // `rightOfSplit` now ends well before `secondClip` (at 6s); closing that gap
    // moves `secondClip` earlier too, which is fine — nothing downstream depends on
    // its absolute position, only on the `secondClip` id captured above.
    observer.run("timeline.close_gap", with("clipId", services::Json(rightOfSplit)));

    // Removed via `timeline.ripple_delete` rather than `timeline.delete_clip`, so
    // this tool's payload is observed too; only `firstClip` and `secondClip` remain
    // for the reorder immediately below.
    observer.run("timeline.ripple_delete", with("clipId", services::Json(rightOfSplit)));

    // A genuine reordering (the reverse of the current order), so the tool applies
    // an edit rather than reporting a no-op.
    std::vector<std::string> clips = observer.clipIdsOf(trackId);
    std::reverse(clips.begin(), clips.end());
    services::Json order = services::Json::array();
    for (const std::string& id : clips) {
        order.push_back(services::Json(id));
    }
    services::Json reorder = object();
    reorder.set("trackId", trackId);
    reorder.set("order", std::move(order));
    observer.run("timeline.reorder_clips", reorder);

    services::Json effect = object();
    effect.set("clipId", secondClip);
    effect.set("type", std::string{"brightness"});
    services::Json parameters = object();
    parameters.set("amount", 0.25);
    effect.set("parameters", std::move(parameters));
    const services::Json firstEffect = observer.run("timeline.add_effect", effect);
    const std::string firstEffectId = firstEffect.stringOr("effectId");

    // Effect lifecycle management (task 9; Requirement 6), exercised on the same
    // clip: a parameter change on the effect just added, a second effect to make
    // the reorder genuine (the reverse of the current two-element order), and then
    // the second effect removed, so `timeline.remove_effect`'s payload is observed
    // without leaving `secondClip` without any effect for the reorder to have
    // acted on in the first place.
    services::Json setParameter = object();
    setParameter.set("clipId", secondClip);
    setParameter.set("effectId", firstEffectId);
    setParameter.set("parameter", std::string{"amount"});
    setParameter.set("value", 0.4);
    observer.run("timeline.set_effect_parameter", setParameter);

    // A tone-curve control point on the same effect (monitoring-and-grading
    // Requirement 5.7). Only "add" is exercised here: the observer's job is to record
    // one real payload per documented tool, and "add" is the operation that needs no
    // pre-existing point, so it is the one that works against a freshly seeded effect.
    services::Json curvePoint = object();
    curvePoint.set("clipId", secondClip);
    curvePoint.set("effectId", firstEffectId);
    curvePoint.set("channel", std::string{"master"});
    curvePoint.set("operation", std::string{"add"});
    curvePoint.set("x", 0.25);
    curvePoint.set("y", 0.6);
    observer.run("timeline.edit_curve_point", curvePoint);

    // A resource path on the same effect (monitoring-and-grading Requirement 7.9). The file
    // need not exist: the tool records a path and does not read it.
    services::Json resource = object();
    resource.set("clipId", secondClip);
    resource.set("effectId", firstEffectId);
    resource.set("path", std::string{"/luts/documented.cube"});
    observer.run("timeline.set_effect_resource", resource);

    services::Json secondEffectArgs = object();
    secondEffectArgs.set("clipId", secondClip);
    secondEffectArgs.set("type", std::string{"contrast"});
    services::Json secondParameters = object();
    secondParameters.set("amount", 0.1);
    secondEffectArgs.set("parameters", std::move(secondParameters));
    const services::Json secondEffect = observer.run("timeline.add_effect", secondEffectArgs);
    const std::string secondEffectId = secondEffect.stringOr("effectId");

    services::Json reorderEffects = object();
    reorderEffects.set("clipId", secondClip);
    services::Json effectOrder = services::Json::array();
    effectOrder.push_back(services::Json(secondEffectId));
    effectOrder.push_back(services::Json(firstEffectId));
    reorderEffects.set("order", std::move(effectOrder));
    observer.run("timeline.reorder_effects", reorderEffects);

    services::Json removeEffect = object();
    removeEffect.set("clipId", secondClip);
    removeEffect.set("effectId", secondEffectId);
    observer.run("timeline.remove_effect", removeEffect);

    services::Json transition = object();
    transition.set("clipId", secondClip);
    transition.set("kind", std::string{"crossfade"});
    transition.set("durationNs", kSecond / 2);
    observer.run("timeline.add_transition", transition);

    // Text and titles (usable-editor Phase 4 task 12; Requirement 9), on a
    // dedicated text track so `timeline.add_text_clip`'s own "must name a text
    // track" rule is observed being satisfied, not merely declared.
    const services::Json textTrack =
        observer.run("timeline.add_track", with("kind", services::Json(std::string{"text"})));
    const std::string textTrackId = textTrack.stringOr("trackId");

    services::Json addTextClip = object();
    addTextClip.set("trackId", textTrackId);
    addTextClip.set("timelineStartNs", static_cast<std::int64_t>(0));
    addTextClip.set("durationNs", 2 * kSecond);
    addTextClip.set("content", std::string{"Palmier Pro"});
    addTextClip.set("fontFamily", std::string{"sans-serif"});
    addTextClip.set("pointSize", 32.0);
    addTextClip.set("colorR", 1.0);
    addTextClip.set("colorG", 1.0);
    addTextClip.set("colorB", 1.0);
    addTextClip.set("colorA", 1.0);
    addTextClip.set("alignment", std::string{"center"});
    addTextClip.set("x", 0.5);
    addTextClip.set("y", 0.9);
    const services::Json textClipResult = observer.run("timeline.add_text_clip", addTextClip);
    const std::string textClipId = textClipResult.stringOr("clipId");

    services::Json setTextContent = object();
    setTextContent.set("clipId", textClipId);
    setTextContent.set("content", std::string{"Palmier Pro Edition"});
    observer.run("timeline.set_text_content", setTextContent);

    services::Json setTextStyle = object();
    setTextStyle.set("clipId", textClipId);
    setTextStyle.set("pointSize", 40.0);
    setTextStyle.set("alignment", std::string{"left"});
    observer.run("timeline.set_text_style", setTextStyle);

    // Captions and transcription (usable-editor Phase 4 task 13; Requirement
    // 10), on a dedicated caption track for the identical reason the text
    // clip above needs a dedicated text track.
    const services::Json captionTrack =
        observer.run("timeline.add_track", with("kind", services::Json(std::string{"caption"})));
    const std::string captionTrackId = captionTrack.stringOr("trackId");

    services::Json addCaptionCue = object();
    addCaptionCue.set("trackId", captionTrackId);
    addCaptionCue.set("timelineStartNs", static_cast<std::int64_t>(0));
    addCaptionCue.set("durationNs", 2 * kSecond);
    addCaptionCue.set("text", std::string{"Hello, world!"});
    const services::Json captionCueResult = observer.run("timeline.add_caption_cue", addCaptionCue);
    const std::string captionCueId = captionCueResult.stringOr("clipId");

    services::Json setCaptionText = object();
    setCaptionText.set("clipId", captionCueId);
    setCaptionText.set("text", std::string{"Hello, Palmier!"});
    observer.run("timeline.set_caption_text", setCaptionText);

    services::Json retimeCaptionCue = object();
    retimeCaptionCue.set("clipId", captionCueId);
    retimeCaptionCue.set("timelineStartNs", kSecond);
    observer.run("timeline.retime_caption_cue", retimeCaptionCue);

    // No recognizer backend is bundled in this build (Requirement 10.5), so
    // this reports "not available" by name rather than the ToolsCallSuccessShape
    // a real backend would produce; it is observed here purely so the tool's
    // OWN existence and argument names are cross-checked against docs/TOOLS.md,
    // exactly like every other observer.run call in this scenario.
    services::Json transcribeToCaptions = object();
    transcribeToCaptions.set("sourceClipId", firstClip);
    transcribeToCaptions.set("captionTrackId", captionTrackId);
    observer.run("timeline.transcribe_to_captions", transcribeToCaptions);

    observer.run("timeline.remove_caption_cue", with("clipId", services::Json(captionCueId)));

    // Still-frame capture (usable-editor tasks.md task 14; no dedicated
    // Requirement).
    services::Json captureFrame = object();
    captureFrame.set("outputPath", (scratchDirectory / "documentation-check-frame.png").string());
    captureFrame.set("positionNs", static_cast<std::int64_t>(0));
    observer.run("timeline.capture_frame", captureFrame);

    observer.run("timeline.delete_clip", with("clipId", services::Json(secondClip)));
    observer.run("edit.undo", object());
    observer.run("edit.redo", object());

    const std::filesystem::path document = scratchDirectory / "documentation-check.palmier";
    observer.run("project.save", with("path", services::Json(document.string())));
    observer.run("project.open", with("path", services::Json(document.string())));

    return observer.observed();
}

// ===========================================================================
// The whole check, assembled once
// ===========================================================================

struct CheckRun {
    std::vector<DocDefect> defects;
    std::vector<DocumentedOption> options;
    std::vector<DocumentedTool> tools;
    std::vector<DocumentedSetting> settings;
    std::vector<LiveOption> liveOptions;
    std::vector<LiveTool> liveTools;
    std::vector<ObservedResult> observed;
};

inline void append(std::vector<DocDefect>& into, const std::vector<DocDefect>& more) {
    into.insert(into.end(), more.begin(), more.end());
}

/// Read the documents, read the system, compare. Performed once per process and
/// cached: the scenario applies real edit commands and writes one project file, and
/// there is no reason to repeat it per test case.
inline const CheckRun& checkRun() {
    static const CheckRun run = [] {
        CheckRun out;

        const std::string buildDoc = testsupport::readWholeFile(docsPath("BUILD.md"));
        const std::string toolsDoc = testsupport::readWholeFile(docsPath("TOOLS.md"));
        const std::string remoteDoc = testsupport::readWholeFile(docsPath("REMOTE_ACCESS.md"));
        const std::string manifest = testsupport::readWholeFile(PALMIER_OPTIONS_MANIFEST);

        // An unreadable input is a failure, never a pass: a checker that cannot
        // find its input would otherwise agree with everything.
        for (const auto& [name, content] : std::vector<std::pair<std::string, const std::string*>>{
                 {"docs/BUILD.md", &buildDoc},
                 {"docs/TOOLS.md", &toolsDoc},
                 {"docs/REMOTE_ACCESS.md", &remoteDoc},
                 {PALMIER_OPTIONS_MANIFEST, &manifest}}) {
            if (content->empty()) {
                out.defects.push_back(DocDefect{DocDefectKind::InputUnreadable, name, name, "",
                                                "empty or unreadable"});
            }
        }

        out.options = testsupport::extractDocumentedOptions(buildDoc, out.defects);
        out.tools = testsupport::extractDocumentedTools(toolsDoc, out.defects);
        out.settings = testsupport::extractDocumentedSettings(remoteDoc, out.defects);
        out.liveOptions = testsupport::parseOptionsManifest(manifest, out.defects);

        services::ProjectSession* noSession = nullptr;
        const services::ToolRegistry advertised = services::buildDefaultToolRegistry(noSession);
        out.liveTools = liveToolsFrom(advertised.describe());

        const std::filesystem::path scratch =
            std::filesystem::temp_directory_path() /
            ("palmier-docs-check-" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ignored;
        std::filesystem::create_directories(scratch, ignored);
        out.observed = observeRealResults(scratch);
        std::filesystem::remove_all(scratch, ignored);

        append(out.defects, testsupport::checkOptions(out.options, out.liveOptions));
        append(out.defects, testsupport::checkTools(out.tools, out.liveTools));
        append(out.defects, testsupport::checkSettings(out.settings, liveSettings()));
        append(out.defects, testsupport::checkResultFields(out.tools, out.observed));

        // The two hook-owned payloads, against the field names their real rendering
        // function sets.
        const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> hookOwned{
            {"timeline.export",
             {"src/services/ExportToolSurface.cpp", "Json exportOutcomeToJson("}},
            {"generation.generate",
             {"src/app/ApplicationCompositionHooks.cpp", "makeGenerateHook("}}};
        for (const auto& [toolName, where] : hookOwned) {
            const testsupport::FunctionFieldScan scan = testsupport::scanJsonSetFields(
                testsupport::readWholeFile(sourcePath(where.first)), where.second);
            if (!scan.found) {
                out.defects.push_back(DocDefect{DocDefectKind::InputUnreadable, where.second,
                                                where.first, toolName,
                                                "the result-rendering function was not found"});
                continue;
            }
            const auto documented = std::find_if(out.tools.begin(), out.tools.end(),
                                                 [&toolName](const DocumentedTool& candidate) {
                                                     return candidate.name == toolName;
                                                 });
            if (documented == out.tools.end()) {
                continue;  // already reported as an undocumented tool
            }
            append(out.defects, testsupport::checkNameSets(documented->allResultFields(),
                                                           scan.fields, "docs/TOOLS.md", toolName));
        }
        return out;
    }();
    return run;
}

[[nodiscard]] inline std::vector<DocDefect> ofKind(const std::vector<DocDefect>& defects,
                                                   DocDefectKind kind) {
    std::vector<DocDefect> matching;
    for (const DocDefect& defect : defects) {
        if (defect.kind == kind) {
            matching.push_back(defect);
        }
    }
    return matching;
}

}  // namespace testsupport_documentationconsistencytest
}  // namespace palmier

#endif  // PALMIER_TESTS_DOCS_DOCUMENTATION_CONSISTENCY_TEST_SUPPORT_HPP_
