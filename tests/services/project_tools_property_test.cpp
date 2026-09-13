// SPDX-License-Identifier: GPL-3.0-or-later
//
// The project_tools_property_test cases that stayed in this file after it was split.
//
// Suites here: ProjectToolsProperties.
// Compiled into the same target as its siblings, so every case keeps the name CTest
// already knew it by.

#include "project_tools_property_test_support.hpp"

namespace palmier::services {
namespace {

using namespace palmier::services::testsupport_projecttoolspropertytest;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 8: project.create carries
// exactly the requested settings — for any project name of 1-128 characters,
// frame rate in 1-240 fps, canvas within 16x16 … 7680x4320 and colour space from
// the domain core's set, `project.create` produces a current project carrying
// exactly those settings, reported unmodified with no document path, and returns
// an identifier distinct from every identifier previously returned in the session.
//
// Requirement 3.2: "WHEN `project.create` is invoked with a project name of 1 to
// 128 characters, a frame rate between 1 and 240 frames per second, a canvas
// resolution between 16x16 and 7680x4320 pixels, and a color space drawn from the
// set the domain core exposes, THE Project_Session SHALL create a project carrying
// exactly those settings, make it the current project, report it as unmodified
// with no known on-disk location, and return a project identifier unique within
// the session."
//
// **Validates: Requirements 3.2**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, CreateCarriesExactlyTheRequestedSettings, ()) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    std::set<std::string> identifiers;
    const int             creations = *rc::gen::inRange(1, 5);

    for (int i = 0; i < creations; ++i) {
        const CreateRequest request = drawCreateRequest();

        const Result<Json> created = registry.invoke("project.create", request.args());
        RC_ASSERT(created.isOk());
        const Json& result = created.value();

        // The created project is the CURRENT project.
        const Project project = session.engine().snapshot();
        RC_ASSERT(result.stringOr("projectId") == project.id.toString());

        // ... carrying exactly the requested settings.
        RC_ASSERT(project.name == request.name);
        RC_ASSERT(project.canvas.width == static_cast<std::uint32_t>(request.width));
        RC_ASSERT(project.canvas.height == static_cast<std::uint32_t>(request.height));
        RC_ASSERT(project.colorSpace == request.expectedColorSpace());
        RC_ASSERT(project.timelineFps.isValid());
        // The frame rate is stored as an exact rational, so the check is that the
        // rational denotes the requested rate (and, for a whole rate, that it is
        // the exact n/1 rational rather than an approximation of it).
        RC_ASSERT(std::abs(project.timelineFps.toDouble() - request.fps) < 1e-3);
        if (std::abs(request.fps - std::round(request.fps)) < 1e-9) {
            RC_ASSERT(project.timelineFps.numerator() ==
                      static_cast<std::int64_t>(std::llround(request.fps)));
            RC_ASSERT(project.timelineFps.denominator() == 1);
        }

        // ... and reported by the tool exactly as the project holds it.
        const Json* fps = result.find("fps");
        RC_ASSERT(fps != nullptr);
        RC_ASSERT(fps->intOr("numerator") == project.timelineFps.numerator());
        RC_ASSERT(fps->intOr("denominator") == project.timelineFps.denominator());
        const Json* canvas = result.find("canvas");
        RC_ASSERT(canvas != nullptr);
        RC_ASSERT(canvas->intOr("width") == request.width);
        RC_ASSERT(canvas->intOr("height") == request.height);
        RC_ASSERT(result.stringOr("name") == request.name);
        RC_ASSERT(result.stringOr("colorSpace") ==
                  std::string(toStringView(request.expectedColorSpace())));

        // Unmodified, with no known on-disk location.
        RC_ASSERT(!session.modified());
        RC_ASSERT(!session.documentPath().has_value());
        RC_ASSERT(!result.boolOr("modified"));
        const Json* documentPath = result.find("documentPath");
        RC_ASSERT(documentPath != nullptr);
        RC_ASSERT(documentPath->isNull());

        // The identifier is unique within the session.
        RC_ASSERT(identifiers.insert(project.id.toString()).second);

        // `project.info` agrees with the create result on every setting.
        const Result<Json> info = registry.invoke("project.info", Json::object());
        RC_ASSERT(info.isOk());
        RC_ASSERT(info.value().stringOr("projectId") == project.id.toString());
        RC_ASSERT(info.value().stringOr("name") == request.name);
        RC_ASSERT(info.value().intOr("trackCount") == 0);
        RC_ASSERT(info.value().intOr("clipCount") == 0);
        RC_ASSERT(!info.value().boolOr("modified"));
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 10: project.open reports the
// loaded project accurately — for any project within the persistence bounds,
// saving it then opening it via `project.open` makes it current, reports it
// unmodified with its document path known, and returns identifier, track count
// and clip count equal to the loaded project's actual values.
//
// Requirement 3.4: "WHEN `project.open` is invoked with the path of a `.palmier`
// document that deserializes successfully, THE Project_Session SHALL replace the
// current project with the loaded project, make it the current project, report it
// as unmodified with its on-disk location known, and return the loaded project's
// identifier, track count and clip count."
//
// **Validates: Requirements 3.4**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, OpenReportsTheLoadedProjectAccurately, ()) {
    const Project original = drawProject(drawShape());

    ScratchDir     scratch;
    const fs::path seedPath = scratch.file("open_seed");
    const fs::path savedPath = scratch.file("open_saved");

    // The document is produced by the tool surface itself: seed a session with the
    // generated project and write it out through `project.save`.
    ProjectSession     writer;
    const ToolRegistry writerRegistry = buildDefaultToolRegistry(writer);
    RC_ASSERT(seedSession(writer, original, seedPath));

    Json saveArgs = Json::object();
    saveArgs.set("path", Json(savedPath.string()));
    const Result<Json> saved = writerRegistry.invoke("project.save", saveArgs);
    RC_ASSERT(saved.isOk());
    RC_ASSERT(fs::exists(savedPath));

    // A session holding an unrelated project, so "replace the current project"
    // is observable.
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    Json               createArgs = Json::object();
    createArgs.set("name", Json(std::string("Displaced")));
    createArgs.set("fps", Json(30.0));
    createArgs.set("width", Json(static_cast<std::int64_t>(640)));
    createArgs.set("height", Json(static_cast<std::int64_t>(480)));
    RC_ASSERT(registry.invoke("project.create", createArgs).isOk());
    RC_ASSERT(session.engine().snapshot().id != original.id);

    Json openArgs = Json::object();
    openArgs.set("path", Json(savedPath.string()));
    const Result<Json> opened = registry.invoke("project.open", openArgs);
    RC_ASSERT(opened.isOk());
    const Json& result = opened.value();

    // The loaded project is now the current project.
    const Project current = session.engine().snapshot();
    RC_ASSERT(current.id == original.id);
    RC_ASSERT(current.name == original.name);
    RC_ASSERT(current.tracks.size() == original.tracks.size());
    RC_ASSERT(clipCount(current) == clipCount(original));

    // The returned identifier, track count and clip count equal the loaded
    // project's ACTUAL values.
    RC_ASSERT(result.stringOr("projectId") == current.id.toString());
    RC_ASSERT(result.intOr("trackCount") == static_cast<std::int64_t>(current.tracks.size()));
    RC_ASSERT(result.intOr("clipCount") == static_cast<std::int64_t>(clipCount(current)));

    // Unmodified, with its on-disk location known.
    RC_ASSERT(!result.boolOr("modified"));
    RC_ASSERT(!session.modified());
    RC_ASSERT(session.documentPath().has_value());
    RC_ASSERT(*session.documentPath() == savedPath);
    RC_ASSERT(result.stringOr("documentPath") == savedPath.string());

    // `project.info` reports the same counts as the open result.
    const Result<Json> info = registry.invoke("project.info", Json::object());
    RC_ASSERT(info.isOk());
    RC_ASSERT(info.value().intOr("trackCount") == result.intOr("trackCount"));
    RC_ASSERT(info.value().intOr("clipCount") == result.intOr("clipCount"));
    RC_ASSERT(info.value().stringOr("documentPath") == savedPath.string());
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 11: No project open blocks
// every other tool — for any tool other than `project.create` / `project.open`,
// and any arguments, invoking it while no project is current leaves all state
// unchanged, creates no edit command, and returns an error stating that no
// project is open.
//
// Requirement 3.5: "IF any tool other than `project.create` and `project.open` is
// invoked while no project is current, THEN THE Project_Session SHALL leave all
// state unchanged, create no edit command, and return an error stating that no
// project is open."
//
// The tool set is taken from the registry, not from a hard-coded list, and the
// arguments are generated from each tool's own `ToolSchema` (valid and invalid),
// so the refusal is shown to be independent of both.
//
// **Validates: Requirements 3.5**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, NoProjectOpenBlocksEveryOtherTool, ()) {
    ScratchDir scratch;

    // Hooks for every tool that has one. None may run: the refusal precedes
    // argument parsing, command construction and the hooks themselves, so a hook
    // that ran would mean state could have changed.
    bool              hookRan = false;
    ToolRegistryHooks hooks;
    const auto        recordingHandler = [&hookRan](const Json&) -> Result<Json> {
        hookRan = true;
        return Json::object();
    };
    hooks.generate = recordingHandler;
    hooks.exportTimeline = recordingHandler;
    hooks.saveProject = recordingHandler;
    hooks.projectInfo = recordingHandler;
    hooks.listMedia = recordingHandler;
    hooks.importMedia = [&hookRan](const fs::path&) -> Result<ImportedAsset> {
        hookRan = true;
        return ImportedAsset{};
    };

    // A null session is the "no project is current" state.
    const ToolRegistry registry = buildDefaultToolRegistry(nullptr, std::move(hooks));
    RC_ASSERT(registry.size() > 2);

    const ArgMode mode = *rc::gen::element<ArgMode>(ArgMode::Empty, ArgMode::Valid,
                                                    ArgMode::WrongType, ArgMode::OutOfBounds,
                                                    ArgMode::UnknownKey, ArgMode::Garbage);

    std::size_t blocked = 0;
    for (const Tool& tool : registry.tools()) {
        if (tool.name == "project.create" || tool.name == "project.open") continue;

        const Json         args = drawArgs(tool.schema, mode, scratch);
        const Result<Json> invoked = registry.invoke(tool.name, args);

        // An error stating that no project is open, naming the tool that was
        // refused — whatever the arguments were.
        RC_ASSERT(invoked.isError());
        RC_ASSERT(invoked.error().code() == ErrorCode::FailedPrecondition);
        RC_ASSERT(contains(invoked.error().message(), "no project is open"));
        RC_ASSERT(contains(invoked.error().message(), tool.name));
        ++blocked;
    }

    // Every tool but the two exempt ones was refused, no hook ran, and nothing
    // was written anywhere a tool was pointed at.
    RC_ASSERT(blocked == registry.size() - 2);
    RC_ASSERT(!hookRan);
    RC_ASSERT(scratch.empty());
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 12: Track and clip counts
// equal successful call counts — for all sequences of at most 64 successful
// `timeline.add_track` calls and at most 500 successful `timeline.add_clip` calls
// containing no removal calls, `project.info` reports a track count equal to the
// successful `add_track` count and a clip count equal to the successful
// `add_clip` count.
//
// Requirement 3.7: "FOR ALL sequences of at most 64 successful
// `timeline.add_track` calls and at most 500 successful `timeline.add_clip` calls
// containing no removal calls, `project.info` SHALL report a track count equal to
// the number of successful `timeline.add_track` calls and a clip count equal to
// the number of successful `timeline.add_clip` calls (invariant property)."
//
// The generated sequences deliberately INCLUDE calls that are expected to fail —
// a clip at a position already occupied, a clip on an absent track, a track of an
// unknown kind — so the counts must be taken from the results rather than from
// the number of calls issued.
//
// **Validates: Requirements 3.7**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, CountsEqualSuccessfulCallCounts, ()) {
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    Json createArgs = Json::object();
    createArgs.set("name", Json(std::string("Counting")));
    createArgs.set("fps", Json(30.0));
    createArgs.set("width", Json(static_cast<std::int64_t>(1920)));
    createArgs.set("height", Json(static_cast<std::int64_t>(1080)));
    RC_ASSERT(registry.invoke("project.create", createArgs).isOk());

    // A clip must reference a registered asset, and asset registration is not one
    // of the two tools under test, so the asset table is installed directly. A
    // reset is not an edit: it leaves the project with no tracks and no clips, so
    // both counters still start at zero.
    Project withAssets = session.engine().snapshot();
    const int assetCount = *rc::gen::inRange(1, 4);
    for (int i = 0; i < assetCount; ++i) {
        withAssets.assets.emplace_back(Uuid::generateV4(), "/media/asset.mp4");
    }
    RC_ASSERT(session.engine().reset(withAssets).isOk());

    enum class Op { AddTrack, AddTrackBadKind, AddClip, AddClipDuplicatePosition, AddClipAbsentTrack };

    struct TrackState {
        std::string  id;
        std::int64_t nextFreeNs = 0;
        std::vector<std::int64_t> starts;
    };
    std::vector<TrackState> tracks;

    std::size_t trackSuccesses = 0;
    std::size_t clipSuccesses = 0;

    const int operations = *rc::gen::inRange(4, 41);
    for (int i = 0; i < operations; ++i) {
        std::vector<Op> menu = {Op::AddTrack, Op::AddTrackBadKind, Op::AddClipAbsentTrack};
        if (!tracks.empty()) {
            menu.push_back(Op::AddClip);
            menu.push_back(Op::AddClip);  // weighted: keep clip successes plentiful
            for (const TrackState& track : tracks) {
                if (!track.starts.empty()) {
                    menu.push_back(Op::AddClipDuplicatePosition);
                    break;
                }
            }
        }
        const Op op = menu[drawIndex(menu.size())];

        switch (op) {
            case Op::AddTrack: {
                Json args = Json::object();
                args.set("kind", Json(*rc::gen::element<std::string>("video", "audio")));
                const Result<Json> result = registry.invoke("timeline.add_track", args);
                RC_ASSERT(result.isOk());
                ++trackSuccesses;
                tracks.push_back(TrackState{result.value().stringOr("trackId"), 0, {}});
                break;
            }
            case Op::AddTrackBadKind: {
                // Expected to fail: 'kind' is a closed set of 'video' and 'audio'.
                Json args = Json::object();
                args.set("kind", Json(*rc::gen::element<std::string>("midi", "", "Video",
                                                                    "subtitle")));
                RC_ASSERT(registry.invoke("timeline.add_track", args).isError());
                break;
            }
            case Op::AddClip: {
                TrackState&        track = tracks[drawIndex(tracks.size())];
                const std::int64_t lengthNs =
                    *rc::gen::inRange<std::int64_t>(100'000'000, 2'000'000'000);
                Json args = Json::object();
                args.set("trackId", Json(track.id));
                args.set("assetId", Json(withAssets.assets[drawIndex(withAssets.assets.size())]
                                             .assetId.toString()));
                args.set("timelineStartNs", Json(track.nextFreeNs));
                args.set("sourceOutNs", Json(lengthNs));
                const Result<Json> result = registry.invoke("timeline.add_clip", args);
                RC_ASSERT(result.isOk());
                ++clipSuccesses;
                track.starts.push_back(track.nextFreeNs);
                track.nextFreeNs += lengthNs + 1'000'000;
                break;
            }
            case Op::AddClipDuplicatePosition: {
                // Expected to fail: a second clip starting where one already does
                // overlaps it outside any transition region.
                std::vector<std::size_t> occupied;
                for (std::size_t t = 0; t < tracks.size(); ++t) {
                    if (!tracks[t].starts.empty()) occupied.push_back(t);
                }
                RC_ASSERT(!occupied.empty());
                const TrackState& track = tracks[occupied[drawIndex(occupied.size())]];
                Json args = Json::object();
                args.set("trackId", Json(track.id));
                args.set("assetId", Json(withAssets.assets[0].assetId.toString()));
                args.set("timelineStartNs", Json(track.starts[drawIndex(track.starts.size())]));
                args.set("sourceOutNs", Json(static_cast<std::int64_t>(500'000'000)));
                RC_ASSERT(registry.invoke("timeline.add_clip", args).isError());
                break;
            }
            case Op::AddClipAbsentTrack: {
                // Expected to fail: the track id names no track in the project.
                Json args = Json::object();
                args.set("trackId", Json(Uuid::generateV4().toString()));
                args.set("assetId", Json(withAssets.assets[0].assetId.toString()));
                args.set("sourceOutNs", Json(static_cast<std::int64_t>(500'000'000)));
                RC_ASSERT(registry.invoke("timeline.add_clip", args).isError());
                break;
            }
        }
    }

    RC_ASSERT(trackSuccesses <= 64);
    RC_ASSERT(clipSuccesses <= 500);

    const Result<Json> info = registry.invoke("project.info", Json::object());
    RC_ASSERT(info.isOk());
    RC_ASSERT(info.value().intOr("trackCount") == static_cast<std::int64_t>(trackSuccesses));
    RC_ASSERT(info.value().intOr("clipCount") == static_cast<std::int64_t>(clipSuccesses));
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 13: Out-of-range arguments
// are rejected and named — for any argument object violating exactly one declared
// bound of `project.create`, `timeline.add_track` or `timeline.remove_track`
// (name length, frame rate, canvas width/height, colour space, track kind, the
// 64-track-per-kind cap, an absent track id), the project is left byte-identical,
// no edit command is created, and the error names the rejected argument.
//
// Requirement 3.8: "IF `project.create`, `timeline.add_track` or
// `timeline.remove_track` is invoked with an argument outside its accepted
// values … THEN THE Project_Session SHALL leave the project unchanged, create no
// edit command, and return an error naming the rejected argument."
//
// **Validates: Requirements 3.8**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, OutOfRangeArgumentsAreRejectedAndNamed, ()) {
    enum class Violation {
        NameTooShort,
        NameTooLong,
        FpsBelow,
        FpsAbove,
        WidthBelow,
        WidthAbove,
        HeightBelow,
        HeightAbove,
        ColorSpaceOutsideTheSet,
        TrackKindOutsideTheSet,
        TrackKindCapExceeded,
        AbsentTrackId,
    };
    const Violation violation = *rc::gen::element<Violation>(
        Violation::NameTooShort, Violation::NameTooLong, Violation::FpsBelow,
        Violation::FpsAbove, Violation::WidthBelow, Violation::WidthAbove,
        Violation::HeightBelow, Violation::HeightAbove, Violation::ColorSpaceOutsideTheSet,
        Violation::TrackKindOutsideTheSet, Violation::TrackKindCapExceeded,
        Violation::AbsentTrackId);

    ScratchDir     scratch;
    const fs::path seedPath = scratch.file("bounds_seed");

    // A non-trivial starting project with some undo history, so "unchanged" and
    // "no edit command" are both observable.
    ProjectSession     session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    RC_ASSERT(seedSession(session, drawProject(Shape{4, 6, 16, 3}), seedPath));

    if (violation == Violation::TrackKindCapExceeded) {
        // The cap counts the project's existing tracks, so the fixture installs a
        // full kind directly rather than paying for 64 tool calls.
        Project full = session.engine().snapshot();
        const TrackKind kind =
            *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);
        full.tracks.clear();
        for (std::size_t i = 0; i < 64; ++i) {
            Track track;
            track.id = Uuid::generateV4();
            track.kind = kind;
            full.tracks.push_back(std::move(track));
        }
        RC_ASSERT(session.engine().reset(full).isOk());
    } else if (*rc::gen::arbitrary<bool>()) {
        Json args = Json::object();
        args.set("kind", Json(*rc::gen::element<std::string>("video", "audio")));
        RC_ASSERT(registry.invoke("timeline.add_track", args).isOk());
    }

    // A valid base tuple, perturbed on EXACTLY one field.
    const CreateRequest base = drawCreateRequest();
    std::string         toolName = "project.create";
    std::string         rejectedArgument;
    Json                args = base.args();

    switch (violation) {
        case Violation::NameTooShort:
            args.set("name", Json(std::string{}));  // 0 characters
            rejectedArgument = "name";
            break;
        case Violation::NameTooLong:
            args.set("name", Json(drawAsciiText(kMaxProjectNameLength +
                                                1 + drawIndex(64))));
            rejectedArgument = "name";
            break;
        case Violation::FpsBelow:
            args.set("fps", Json(*rc::gen::element<double>(0.0, 0.5, -1.0, -1'000.0)));
            rejectedArgument = "fps";
            break;
        case Violation::FpsAbove:
            args.set("fps", Json(*rc::gen::element<double>(
                                 static_cast<double>(kMaxFramesPerSecond) + 1.0, 1'000.0,
                                 100'000.0)));
            rejectedArgument = "fps";
            break;
        case Violation::WidthBelow:
            args.set("width", Json(static_cast<std::int64_t>(kMinCanvasWidth) - 1 -
                                   static_cast<std::int64_t>(drawIndex(16))));
            rejectedArgument = "width";
            break;
        case Violation::WidthAbove:
            args.set("width", Json(static_cast<std::int64_t>(kMaxCanvasWidth) + 1 +
                                   static_cast<std::int64_t>(drawIndex(1'000))));
            rejectedArgument = "width";
            break;
        case Violation::HeightBelow:
            args.set("height", Json(static_cast<std::int64_t>(kMinCanvasHeight) - 1 -
                                    static_cast<std::int64_t>(drawIndex(16))));
            rejectedArgument = "height";
            break;
        case Violation::HeightAbove:
            args.set("height", Json(static_cast<std::int64_t>(kMaxCanvasHeight) + 1 +
                                    static_cast<std::int64_t>(drawIndex(1'000))));
            rejectedArgument = "height";
            break;
        case Violation::ColorSpaceOutsideTheSet:
            args.set("colorSpace", Json(*rc::gen::element<std::string>(
                                        "Unknown", "cmyk", "rec709", "", "ACEScg")));
            rejectedArgument = "colorSpace";
            break;
        case Violation::TrackKindOutsideTheSet:
            toolName = "timeline.add_track";
            args = Json::object();
            args.set("kind", Json(*rc::gen::element<std::string>("midi", "subtitle", "",
                                                                "VIDEO")));
            rejectedArgument = "kind";
            break;
        case Violation::TrackKindCapExceeded: {
            toolName = "timeline.add_track";
            args = Json::object();
            // The kind the fixture filled to the cap.
            args.set("kind", Json(std::string(session.engine().snapshot().tracks.front().kind ==
                                                      TrackKind::Audio
                                                  ? "audio"
                                                  : "video")));
            rejectedArgument = "kind";
            break;
        }
        case Violation::AbsentTrackId: {
            toolName = "timeline.remove_track";
            args = Json::object();
            const Uuid absent = Uuid::generateV4();
            args.set("trackId", Json(absent.toString()));
            rejectedArgument = absent.toString();
            break;
        }
    }

    const SessionFingerprint before = fingerprint(session);

    const Result<Json> invoked = registry.invoke(toolName, args);

    // Rejected, naming the argument that was rejected.
    RC_ASSERT(invoked.isError());
    const std::string message = invoked.error().message();
    if (violation == Violation::FpsBelow || violation == Violation::FpsAbove) {
        // The tool argument is `fps`; the session names the same bound as
        // `frameRate`, so either spelling identifies the rejected argument.
        RC_ASSERT(contains(message, "fps") || contains(message, "frameRate"));
    } else {
        RC_ASSERT(contains(message, rejectedArgument));
    }

    // The project is byte-identical, its document path and modified flag are
    // untouched, and no edit command was created (the undo depth did not move).
    RC_ASSERT(fingerprint(session) == before);

    // Where the violated bound is declared in the schema, the published schema
    // rejects the same object and names the same argument, so a caller that
    // validates first sees an identical verdict (Requirement 9.12's mechanism).
    const Tool* tool = registry.find(toolName);
    RC_ASSERT(tool != nullptr);
    const bool schemaExpressible = violation != Violation::TrackKindCapExceeded &&
                                   violation != Violation::AbsentTrackId;
    if (schemaExpressible) {
        const Result<void> validated = tool->schema.validate(args);
        RC_ASSERT(validated.isError());
        RC_ASSERT(contains(validated.error().message(), rejectedArgument));
    }
}
// ===========================================================================
// Feature: end-to-end-editor-integration, Property 14: A failed open preserves
// the previous session exactly — for any prior project (modified or not, with or
// without a document path) and any open target that is missing, unreadable, not a
// valid `.palmier` document, or written with an unsupported schema version, the
// project, its document path, its modified flag and its undo history are
// unchanged, and the error names the path and the failure reason.
//
// Requirement 3.9: "IF `project.open` is invoked with a path that is missing,
// unreadable, not a valid `.palmier` document, or written with an unsupported
// schema version, THEN THE Project_Session SHALL keep the project that was
// current before the call unchanged, including its modified state, and return an
// error naming the path and the failure reason."
// Requirement 4.10: "IF a File > Open target is missing, unreadable, not a valid
// `.palmier` document, or declares an unsupported schema version, THEN THE
// Project_Session SHALL keep the current project, its document path, its modified
// state and all four refreshed panels unchanged, and display an error naming the
// selected file and the failure reason."
//
// **Validates: Requirements 3.9, 4.10**
// ===========================================================================
RC_GTEST_PROP(ProjectToolsProperties, AFailedOpenPreservesThePreviousSession, ()) {
    // --- The prior session: with or without a document path, modified or not,
    //     with or without undo history.
    enum class Prior { FreshDefault, Created, Opened, OpenedThenEdited, CreatedThenEdited };
    const Prior prior = *rc::gen::element<Prior>(Prior::FreshDefault, Prior::Created,
                                                 Prior::Opened, Prior::OpenedThenEdited,
                                                 Prior::CreatedThenEdited);

    ScratchDir     scratch;
    ProjectSession session;
    const ToolRegistry registry = buildDefaultToolRegistry(session);

    const auto createProject = [&]() {
        RC_ASSERT(registry.invoke("project.create", drawCreateRequest().args()).isOk());
    };
    const auto openSeed = [&]() {
        const fs::path seedPath = scratch.file("prior_seed");
        RC_ASSERT(seedSession(session, drawProject(Shape{4, 8, 20, 3}), seedPath));
    };
    const auto edit = [&]() {
        Json args = Json::object();
        args.set("kind", Json(*rc::gen::element<std::string>("video", "audio")));
        RC_ASSERT(registry.invoke("timeline.add_track", args).isOk());
    };

    switch (prior) {
        case Prior::FreshDefault:
            break;  // the empty default project: no path, unmodified
        case Prior::Created:
            createProject();
            break;
        case Prior::Opened:
            openSeed();
            break;
        case Prior::OpenedThenEdited:
            openSeed();
            edit();
            break;
        case Prior::CreatedThenEdited:
            createProject();
            edit();
            break;
    }

    // --- The open target: one of the four failure classes.
    enum class Failure { Missing, Unreadable, NotAPalmierDocument, UnsupportedSchemaVersion };
    const Failure failure = *rc::gen::element<Failure>(
        Failure::Missing, Failure::Unreadable, Failure::NotAPalmierDocument,
        Failure::UnsupportedSchemaVersion);

    // A well-formed document to derive the invalid variants from.
    const Project      sample = drawProject(Shape{2, 4, 8, 2});
    const std::string  validDocument = serializeProject(sample);

    const auto writeBytes = [](const fs::path& path, std::string_view bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return static_cast<bool>(out);
    };

    fs::path target;
    switch (failure) {
        case Failure::Missing:
            // Never created.
            target = scratch.file("absent");
            RC_ASSERT(!fs::exists(target));
            break;

        case Failure::Unreadable: {
            // A path whose parent component is a regular file: opening it for
            // reading cannot succeed (ENOTDIR), whatever the effective user is —
            // permission bits alone would not stop a root-owned CI runner.
            const fs::path blocker = scratch.file("blocker");
            RC_ASSERT(writeBytes(blocker, validDocument));
            target = blocker / "inside_a_file.palmier";
            break;
        }

        case Failure::NotAPalmierDocument: {
            target = scratch.file("invalid");
            // Truncated, corrupted, wrong-magic and empty variants — each one is
            // unconditionally unparsable or carries the wrong format magic, so no
            // generated variant can accidentally be a loadable document.
            std::string bytes;
            switch (*rc::gen::inRange(0, 5)) {
                case 0:  // truncated: a strict prefix is never a complete object
                    bytes = validDocument.substr(
                        0, drawIndex(std::max<std::size_t>(validDocument.size(), 1)));
                    break;
                case 1: {  // corrupted: every ':' becomes ',', so no key/value pairs
                    bytes = validDocument;
                    std::replace(bytes.begin(), bytes.end(), ':', ',');
                    break;
                }
                case 2:  // corrupted: binary noise (13 bytes, one of them NUL)
                    bytes = std::string("\x01\x02\x00\xff\xfe garbage", 13);
                    break;
                case 3:  // wrong magic: valid JSON, but not a `.palmier` document
                    bytes = R"({"format":"not-palmier","version":"1.1","project":{}})";
                    break;
                default:  // empty file
                    bytes.clear();
                    break;
            }
            RC_ASSERT(writeBytes(target, bytes));
            break;
        }

        case Failure::UnsupportedSchemaVersion: {
            target = scratch.file("future_version");
            // A future MAJOR version: same document, version replaced.
            std::string bytes = validDocument;
            const std::string versionKey = "\"version\":";
            const std::size_t keyAt = bytes.find(versionKey);
            RC_ASSERT(keyAt != std::string::npos);
            const std::size_t openQuote = bytes.find('"', keyAt + versionKey.size());
            RC_ASSERT(openQuote != std::string::npos);
            const std::size_t closeQuote = bytes.find('"', openQuote + 1);
            RC_ASSERT(closeQuote != std::string::npos);
            const std::string future =
                std::to_string(SchemaVersion::current().major +
                               static_cast<std::uint32_t>(1 + drawIndex(3))) +
                "." + std::to_string(drawIndex(3));
            bytes.replace(openQuote + 1, closeQuote - openQuote - 1, future);
            RC_ASSERT(writeBytes(target, bytes));
            break;
        }
    }

    const SessionFingerprint before = fingerprint(session);

    Json args = Json::object();
    args.set("path", Json(target.string()));
    const Result<Json> opened = registry.invoke("project.open", args);

    // The open failed, naming the path and the failure reason.
    RC_ASSERT(opened.isError());
    const std::string message = opened.error().message();
    RC_ASSERT(contains(message, target.string()));
    const std::string prefix = "could not open project '" + target.string() + "': ";
    RC_ASSERT(message.rfind(prefix, 0) == 0);
    RC_ASSERT(message.size() > prefix.size());  // a stated reason follows
    if (failure == Failure::Missing || failure == Failure::Unreadable) {
        RC_ASSERT(opened.error().code() == ErrorCode::Io);
    } else if (failure == Failure::UnsupportedSchemaVersion) {
        RC_ASSERT(opened.error().code() == ErrorCode::Unsupported);
        RC_ASSERT(contains(message, "schema version"));
    }

    // The project, its document path, its modified flag and its undo history are
    // all exactly as they were.
    RC_ASSERT(fingerprint(session) == before);
}

}  // namespace
}  // namespace palmier::services
