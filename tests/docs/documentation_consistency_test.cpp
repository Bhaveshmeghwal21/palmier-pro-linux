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
namespace {

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

[[nodiscard]] std::string docsPath(std::string_view name) {
    return std::string{PALMIER_DOCS_DIR} + "/" + std::string{name};
}

[[nodiscard]] std::string sourcePath(std::string_view relative) {
    return std::string{PALMIER_SOURCE_DIR} + "/" + std::string{relative};
}

// ===========================================================================
// The live tool surface, read from the `tools/list` payload
// ===========================================================================

/// Exactly what an MCP client sees: `ToolRegistry::describe()` is the `tools/list`
/// result, and each entry's `inputSchema` is the rendered `ToolSchema`. Reading the
/// live side out of the published payload (rather than out of `ToolSchema::args()`)
/// is what makes this Requirement 16.7's comparison and not a weaker one.
[[nodiscard]] std::vector<LiveTool> liveToolsFrom(const services::Json& described) {
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

[[nodiscard]] std::vector<LiveSetting> liveSettings() {
    std::vector<LiveSetting> settings;
    for (const std::string_view key : app::AppSettings::recognizedKeys()) {
        settings.push_back(
            LiveSetting{std::string(key),
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
        hooks.importMedia = [](const std::filesystem::path& path)
            -> Result<services::ImportedAsset> {
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
                    entry.set("minDurationTicks",
                             static_cast<std::int64_t>(minDuration.ticks()));
                    entry.set("maxDurationTicks",
                             static_cast<std::int64_t>(maxDuration.ticks()));
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
        registry_ = services::buildDefaultToolRegistry(session_, std::move(hooks));
    }

    [[nodiscard]] const std::vector<ObservedResult>& observed() const noexcept {
        return observed_;
    }

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
            project.assets.push_back(MediaAssetRef(id, "/media/source" + std::to_string(i) + ".mp4"));
            ids.push_back(id);
        }
        EXPECT_TRUE(session_.engine().reset(project).isOk());
        return ids;
    }

    /// The clip identifiers currently on `trackId`, read through `timeline.read`.
    [[nodiscard]] std::vector<std::string> clipIdsOf(const std::string& trackId) {
        std::vector<std::string> ids;
        Result<services::Json> timeline = registry_.invoke("timeline.read", services::Json::object());
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
    services::ToolRegistry   registry_;
    std::vector<ObservedResult> observed_;
};

[[nodiscard]] services::Json object() { return services::Json::object(); }

[[nodiscard]] services::Json with(std::string key, services::Json value) {
    services::Json args = object();
    args.set(std::move(key), std::move(value));
    return args;
}

/// Run the whole scenario and hand back what every payload contained.
[[nodiscard]] std::vector<ObservedResult> observeRealResults(
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

    services::Json mute = object();
    mute.set("trackId", trackId);
    mute.set("muted", true);
    observer.run("timeline.set_track_muted", mute);

    services::Json move = object();
    move.set("clipId", secondClip);
    move.set("timelineStartNs", 6 * kSecond);
    observer.run("timeline.move_clip", move);

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
    observer.run("timeline.split_clip", split);

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
    observer.run("timeline.add_effect", effect);

    services::Json transition = object();
    transition.set("clipId", secondClip);
    transition.set("kind", std::string{"crossfade"});
    transition.set("durationNs", kSecond / 2);
    observer.run("timeline.add_transition", transition);

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
    std::vector<DocumentedTool>   tools;
    std::vector<DocumentedSetting> settings;
    std::vector<LiveOption> liveOptions;
    std::vector<LiveTool>   liveTools;
    std::vector<ObservedResult> observed;
};

void append(std::vector<DocDefect>& into, const std::vector<DocDefect>& more) {
    into.insert(into.end(), more.begin(), more.end());
}

/// Read the documents, read the system, compare. Performed once per process and
/// cached: the scenario applies real edit commands and writes one project file, and
/// there is no reason to repeat it per test case.
const CheckRun& checkRun() {
    static const CheckRun run = [] {
        CheckRun out;

        const std::string buildDoc = testsupport::readWholeFile(docsPath("BUILD.md"));
        const std::string toolsDoc = testsupport::readWholeFile(docsPath("TOOLS.md"));
        const std::string remoteDoc = testsupport::readWholeFile(docsPath("REMOTE_ACCESS.md"));
        const std::string manifest = testsupport::readWholeFile(PALMIER_OPTIONS_MANIFEST);

        // An unreadable input is a failure, never a pass: a checker that cannot
        // find its input would otherwise agree with everything.
        for (const auto& [name, content] :
             std::vector<std::pair<std::string, const std::string*>>{
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
            std::filesystem::temp_directory_path()
            / ("palmier-docs-check-" + std::to_string(static_cast<long>(::getpid())));
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
            {"timeline.export", {"src/services/ExportCoordinator.cpp", "Json exportOutcomeToJson("}},
            {"generation.generate", {"src/app/ApplicationComposition.cpp", "makeGenerateHook("}}};
        for (const auto& [toolName, where] : hookOwned) {
            const testsupport::FunctionFieldScan scan =
                testsupport::scanJsonSetFields(testsupport::readWholeFile(sourcePath(where.first)),
                                               where.second);
            if (!scan.found) {
                out.defects.push_back(DocDefect{DocDefectKind::InputUnreadable, where.second,
                                                where.first, toolName,
                                                "the result-rendering function was not found"});
                continue;
            }
            const auto documented =
                std::find_if(out.tools.begin(), out.tools.end(),
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

[[nodiscard]] std::vector<DocDefect> ofKind(const std::vector<DocDefect>& defects,
                                            DocDefectKind kind) {
    std::vector<DocDefect> matching;
    for (const DocDefect& defect : defects) {
        if (defect.kind == kind) {
            matching.push_back(defect);
        }
    }
    return matching;
}

// ===========================================================================
// The checked-in documentation agrees with the running system
// ===========================================================================

TEST(DocumentationConsistency, EveryDocumentAndTheRunningSystemAgreeOnEveryName) {
    const CheckRun& run = checkRun();
    EXPECT_TRUE(run.defects.empty())
        << run.defects.size() << " documentation inconsistency(ies):\n"
        << testsupport::toString(run.defects);
}

TEST(DocumentationConsistency, TheDocumentedCMakeOptionsAreTheOptionsTheTreeDefines) {
    const CheckRun& run = checkRun();
    ASSERT_FALSE(run.liveOptions.empty()) << "palmier_options.txt carried no PALMIER_* records";
    ASSERT_FALSE(run.options.empty()) << "docs/BUILD.md carried no option rows";
    EXPECT_EQ(run.options.size(), run.liveOptions.size());
    const std::vector<DocDefect> defects = testsupport::checkOptions(run.options, run.liveOptions);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}

TEST(DocumentationConsistency, TheDocumentedToolSurfaceIsWhatToolsListPublishes) {
    const CheckRun& run = checkRun();
    ASSERT_FALSE(run.liveTools.empty()) << "the tool registry advertised nothing";
    EXPECT_EQ(run.tools.size(), run.liveTools.size());
    const std::vector<DocDefect> defects = testsupport::checkTools(run.tools, run.liveTools);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}

TEST(DocumentationConsistency, TheDocumentedConfigurationInputsAreTheRecognisedKeys) {
    const CheckRun& run = checkRun();
    const std::vector<LiveSetting> live = liveSettings();
    ASSERT_FALSE(live.empty()) << "AppSettings recognised no keys";
    const std::vector<DocDefect> defects = testsupport::checkSettings(run.settings, live);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}

TEST(DocumentationConsistency, TheDocumentedResultFieldsAreTheFieldsTheHandlersReturn) {
    const CheckRun& run = checkRun();
    const std::vector<DocDefect> defects =
        testsupport::checkResultFields(run.tools, run.observed);
    EXPECT_TRUE(defects.empty()) << testsupport::toString(defects);
}

// Non-vacuity: the scenario must really have exercised the surface. A silently
// empty observation set would make the result check above pass by checking nothing.
TEST(DocumentationConsistency, EveryToolWithARegistryOwnedResultWasActuallyInvoked) {
    const CheckRun& run = checkRun();
    std::vector<std::string> invoked;
    for (const ObservedResult& result : run.observed) {
        if (std::find(invoked.begin(), invoked.end(), result.toolName) == invoked.end()) {
            invoked.push_back(result.toolName);
        }
    }

    // The only tools allowed to be missing are the two whose payload the registry
    // does not render; they are checked against their rendering function instead.
    std::vector<std::string> missing;
    for (const LiveTool& tool : run.liveTools) {
        if (std::find(invoked.begin(), invoked.end(), tool.name) == invoked.end()) {
            missing.push_back(tool.name);
        }
    }
    const std::vector<std::string> expectedMissing{"generation.generate", "timeline.export"};
    std::sort(missing.begin(), missing.end());
    EXPECT_EQ(missing, expectedMissing)
        << "the result scenario must invoke every tool whose success payload the registry itself "
           "renders";
}

// Requirement 16.8's third clause, asserted rather than assumed.
TEST(DocumentationConsistency, LeavesTheDocumentationUnmodified) {
    const std::vector<std::string> documents{docsPath("BUILD.md"), docsPath("TOOLS.md"),
                                             docsPath("REMOTE_ACCESS.md")};
    std::vector<std::string> before;
    for (const std::string& path : documents) {
        before.push_back(testsupport::readWholeFile(path));
        ASSERT_FALSE(before.back().empty()) << path;
    }

    (void)checkRun();

    for (std::size_t i = 0; i < documents.size(); ++i) {
        EXPECT_EQ(testsupport::readWholeFile(documents[i]), before[i])
            << documents[i] << " was modified by the check";
    }
}

// ===========================================================================
// The checker is falsifiable — each fault it must catch, injected
// ===========================================================================

TEST(DocumentationChecker, ReportsAnOptionTheTreeDefinesAndTheDocumentationOmits) {
    const std::vector<DocumentedOption> documented{{"PALMIER_BUILD_UI", "BOOL", "User-settable"}};
    const std::vector<LiveOption> live{{"PALMIER_BUILD_UI", "BOOL"}, {"PALMIER_WERROR", "BOOL"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "PALMIER_WERROR");
    EXPECT_EQ(defects.front().document, "docs/BUILD.md");
}

TEST(DocumentationChecker, ReportsAnOptionTheDocumentationInventsAndReportsItsSection) {
    const std::vector<DocumentedOption> documented{
        {"PALMIER_ENABLE_TELEPATHY", "BOOL", "User-settable options"}};
    const std::vector<LiveOption> live{{"PALMIER_BUILD_UI", "BOOL"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    const std::vector<DocDefect> unknown =
        ofKind(defects, DocDefectKind::UnknownDocumentedName);
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown.front().name, "PALMIER_ENABLE_TELEPATHY");
    // Requirement 16.8: the report names the section the name appears in.
    EXPECT_EQ(unknown.front().section, "User-settable options");
}

TEST(DocumentationChecker, ReportsAnOptionDocumentedWithTheWrongType) {
    const std::vector<DocumentedOption> documented{{"PALMIER_PBT_MIN_SUCCESS", "BOOL", "table"}};
    const std::vector<LiveOption> live{{"PALMIER_PBT_MIN_SUCCESS", "STRING"}};

    const std::vector<DocDefect> defects = testsupport::checkOptions(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::TypeMismatch);
}

TEST(DocumentationChecker, ReportsAMissingOptionsRegionRatherThanFindingNothing) {
    std::vector<DocDefect> defects;
    const std::vector<DocumentedOption> options = testsupport::extractDocumentedOptions(
        "# Build\n\n| Option | Type |\n|---|---|\n| `PALMIER_BUILD_UI` | BOOL |\n", defects);
    EXPECT_TRUE(options.empty());
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::SectionMissing);
}

TEST(DocumentationChecker, ReportsAnEmptyOptionsManifestRatherThanAgreeingWithEverything) {
    std::vector<DocDefect> defects;
    const std::vector<LiveOption> options =
        testsupport::parseOptionsManifest("# only a comment\n", defects);
    EXPECT_TRUE(options.empty());
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::InputUnreadable);
}

TEST(DocumentationChecker, ReportsARenamedArgumentInBothDirections) {
    const std::string markdown = R"(# Tools

## `timeline.move_clip`

Move a clip.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string *uuid* | **yes** |
| `startNs` | integer | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    ASSERT_TRUE(extraction.empty()) << testsupport::toString(extraction);
    ASSERT_EQ(documented.size(), 1u);

    const std::vector<LiveTool> live{
        {"timeline.move_clip",
         {{"clipId", "string", true, true}, {"timelineStartNs", "integer", true, false}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::UndocumentedName).size(), 1u)
        << testsupport::toString(defects);
    EXPECT_EQ(ofKind(defects, DocDefectKind::UnknownDocumentedName).size(), 1u)
        << testsupport::toString(defects);
}

TEST(DocumentationChecker, ReportsAFlippedRequiredMarkingAndAChangedJsonType) {
    const std::string markdown = R"(# Tools

## `timeline.add_clip`

Add a clip.

| Argument | Type | Required |
|---|---|---|
| `trackId` | string *uuid* | **yes** |
| `opacity` | integer | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    ASSERT_TRUE(extraction.empty());

    const std::vector<LiveTool> live{
        {"timeline.add_clip",
         {{"trackId", "string", true, true}, {"opacity", "number", false, false}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::TypeMismatch).size(), 1u)
        << testsupport::toString(defects);
    EXPECT_EQ(ofKind(defects, DocDefectKind::RequiredMismatch).size(), 1u)
        << testsupport::toString(defects);
}

TEST(DocumentationChecker, ReportsAMissingUuidMarker) {
    const std::string markdown = R"(# Tools

## `timeline.delete_clip`

Delete a clip.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string | **yes** |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> extraction;
    const std::vector<DocumentedTool> documented =
        testsupport::extractDocumentedTools(markdown, extraction);
    const std::vector<LiveTool> live{
        {"timeline.delete_clip", {{"clipId", "string", true, /*uuid=*/true}}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::TypeMismatch);
    EXPECT_EQ(defects.front().section, "timeline.delete_clip");
}

TEST(DocumentationChecker, ReportsADocumentedToolOrderThatIsNotThePublishedOrder) {
    const std::vector<DocumentedTool> documented{{"edit.redo"}, {"edit.undo"}};
    const std::vector<LiveTool> live{{"edit.undo", {}}, {"edit.redo", {}}};

    // Both sections say "No arguments." in the real document; here they do not,
    // so filter to the ordering finding this case is about.
    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    EXPECT_EQ(ofKind(defects, DocDefectKind::OrderMismatch).size(), 1u)
        << testsupport::toString(defects);
}

TEST(DocumentationChecker, ReportsAToolThatTakesNoArgumentsWithoutSayingSo) {
    const std::vector<DocumentedTool> documented{{"project.info"}};  // no "No arguments."
    const std::vector<LiveTool> live{{"project.info", {}}};

    const std::vector<DocDefect> defects = testsupport::checkTools(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
}

TEST(DocumentationChecker, ReportsAResultFieldTheHandlerReturnsAndTheDocumentationOmits) {
    DocumentedTool documented;
    documented.name = "project.info";
    documented.resultFields = {"projectId"};

    const std::vector<ObservedResult> observed{{"project.info", {"projectId", "undoDepth"}}};

    const std::vector<DocDefect> defects = testsupport::checkResultFields({documented}, observed);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "undoDepth");
}

TEST(DocumentationChecker, ReportsAnUnconditionalResultFieldNoInvocationEverReturns) {
    DocumentedTool documented;
    documented.name = "project.info";
    documented.resultFields = {"projectId", "phaseOfTheMoon"};

    const std::vector<ObservedResult> observed{{"project.info", {"projectId"}}};

    const std::vector<DocDefect> defects = testsupport::checkResultFields({documented}, observed);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UnknownDocumentedName);
    EXPECT_EQ(defects.front().name, "phaseOfTheMoon");
}

TEST(DocumentationChecker, AcceptsAConditionalResultFieldThatNoScenarioProduced) {
    DocumentedTool documented;
    documented.name = "media.import";
    documented.resultFields = {"assetId"};
    documented.conditionalResultFields = {"width"};

    const std::vector<ObservedResult> observed{{"media.import", {"assetId"}}};

    EXPECT_TRUE(testsupport::checkResultFields({documented}, observed).empty());
}

TEST(DocumentationChecker, ReportsAWrongEnvironmentVariableOrFlagForAKnownKey) {
    const std::vector<DocumentedSetting> documented{
        {"remote.port", "PALMIER_REMOTE_PORTS", "--remote-port", "Every configuration input"}};
    const std::vector<LiveSetting> live{{"remote.port", "PALMIER_REMOTE_PORT", "--remote-port"}};

    const std::vector<DocDefect> defects = testsupport::checkSettings(documented, live);
    ASSERT_EQ(defects.size(), 1u) << testsupport::toString(defects);
    EXPECT_EQ(defects.front().name, "PALMIER_REMOTE_PORTS");
    EXPECT_EQ(defects.front().section, "Every configuration input");
}

TEST(DocumentationChecker, ReportsASettingsKeyTheDocumentationDoesNotMention) {
    const std::vector<LiveSetting> live{{"agent.interpreter", "PALMIER_AGENT_INTERPRETER",
                                        "--agent-interpreter"}};
    const std::vector<DocDefect> defects = testsupport::checkSettings({}, live);
    ASSERT_EQ(defects.size(), 1u);
    EXPECT_EQ(defects.front().kind, DocDefectKind::UndocumentedName);
    EXPECT_EQ(defects.front().name, "agent.interpreter");
}

TEST(DocumentationChecker, ReportsAMissingResultRenderingFunctionInsteadOfAnEmptySet) {
    const testsupport::FunctionFieldScan scan = testsupport::scanJsonSetFields(
        "Json somethingElse() { Json out; out.set(\"a\", 1); return out; }", "Json renamedAway(");
    EXPECT_FALSE(scan.found);
    EXPECT_TRUE(scan.fields.empty());
}

TEST(DocumentationChecker, ScansTheFieldNamesARenderingFunctionSets) {
    const testsupport::FunctionFieldScan scan = testsupport::scanJsonSetFields(
        R"(Json render(const Outcome& o) {
    Json out = Json::object();
    out.set("outputPath", o.path);
    if (!o.reason.empty()) out.set("fallbackReason", o.reason);
    return out;
}
Json other() { Json x; x.set("notThisOne", 1); return x; })",
        "Json render(");
    ASSERT_TRUE(scan.found);
    EXPECT_EQ(scan.fields, (std::vector<std::string>{"outputPath", "fallbackReason"}));
}

TEST(DocumentationChecker, ExtractsArgumentsResultFieldsAndConditionalFieldsFromASection) {
    const std::string markdown = R"(# Tools

## `media.import`

Import a file.

| Argument | Type | Required | Accepted values |
|---|---|---|---|
| `path` | string | **yes** | 1-4096 characters |

Result: `assetId` (string), `duplicate` (bool - already registered).

`width` (integer), `height` (integer) and `fps` (number) are present only for a video asset.

## `timeline.read`

Read it.

No arguments.

Result - the whole project:

| Field | Type | Notes |
|---|---|---|
| `id` | string | project UUID |
| `extra` | string | present only when something |
)";
    std::vector<DocDefect> defects;
    const std::vector<DocumentedTool> tools =
        testsupport::extractDocumentedTools(markdown, defects);
    ASSERT_TRUE(defects.empty()) << testsupport::toString(defects);
    ASSERT_EQ(tools.size(), 2u);

    EXPECT_EQ(tools[0].name, "media.import");
    ASSERT_EQ(tools[0].arguments.size(), 1u);
    EXPECT_EQ(tools[0].arguments.front().name, "path");
    EXPECT_TRUE(tools[0].arguments.front().required);
    EXPECT_EQ(tools[0].resultFields, (std::vector<std::string>{"assetId", "duplicate"}));
    EXPECT_EQ(tools[0].conditionalResultFields,
              (std::vector<std::string>{"width", "height", "fps"}));

    EXPECT_EQ(tools[1].name, "timeline.read");
    EXPECT_TRUE(tools[1].declaresNoArguments);
    EXPECT_TRUE(tools[1].arguments.empty());
    EXPECT_EQ(tools[1].resultFields, (std::vector<std::string>{"id"}));
    EXPECT_EQ(tools[1].conditionalResultFields, (std::vector<std::string>{"extra"}));
}

TEST(DocumentationChecker, TreatsTheCommandResultMarkerAsTheStatusOrNoOpTrio) {
    const std::string markdown = R"(# Tools

## `timeline.add_clip`

Add it.

| Argument | Type | Required |
|---|---|---|
| `clipId` | string *uuid* | no |

Result: `clipId` (string), *(command result)*.
)";
    std::vector<DocDefect> defects;
    const std::vector<DocumentedTool> tools =
        testsupport::extractDocumentedTools(markdown, defects);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_TRUE(tools.front().commandResult);
    for (const char* field : {"status", "noOp", "indication"}) {
        EXPECT_NE(std::find(tools.front().conditionalResultFields.begin(),
                            tools.front().conditionalResultFields.end(), field),
                  tools.front().conditionalResultFields.end())
            << field;
    }
}

}  // namespace
}  // namespace palmier
