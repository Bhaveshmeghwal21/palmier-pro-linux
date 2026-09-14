// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/mcp_protocol_property_test.cpp — the universally quantified
// properties of the JSON-RPC 2.0 protocol layer (task 5.4).
//
// Six properties live here, all six about what `services::McpProtocolHandler`
// guarantees over every request the endpoint can receive:
//
//   Property 43 — the JSON-RPC envelope round-trip: a well-formed request
//                 carrying an `id` is answered, within 1000 ms, by a body that
//                 parses as JSON, carries `"jsonrpc":"2.0"`, echoes the `id`
//                 unchanged in both type and value and carries exactly one of
//                 `result` / `error`; a well-formed notification is answered with
//                 a zero-byte body (Requirements 9.1, 9.13).
//   Property 44 — `initialize` negotiates the requested protocol version when it
//                 is supported and the highest supported version otherwise, and
//                 always reports a non-empty server name, a server version and a
//                 capabilities object declaring `tools` (Requirement 9.2).
//   Property 45 — `tools/list` describes every registered tool and nothing else:
//                 one entry per tool, each with a non-empty `name` of at most 64
//                 characters, a non-empty `description` and an `inputSchema`
//                 object of type `object` naming each accepted argument and
//                 listing the required ones (Requirement 9.3).
//   Property 46 — a `tools/call` that reaches a tool answers in the specified
//                 shape: a `content` array whose first entry is a text entry,
//                 plus `isError` (Requirement 9.4).
//   Property 47 — an invocation forced to fail yields `isError` true naming the
//                 tool and the reason, and leaves the project — track count, clip
//                 set, clip source ranges, effects, transitions, asset references
//                 — and the undo history exactly as they were (Requirement 9.5).
//   Property 48 — a request violating exactly one protocol rule carries that
//                 rule's assigned code, creates no edit command and leaves the
//                 project byte-identical (Requirements 9.6, 9.7, 9.8, 9.9).
//
// Everything is driven through `McpProtocolHandler::handle` directly, with no
// socket: the HTTP-level conformance of the same surface is already covered by
// `tests/services/mcp_http_integration_test.cpp`, and driving the handler keeps a
// generated case to a function call so 100+ of them cost nothing. The stack under
// test is otherwise entirely real — a real `ProjectSession` holding a real
// `TimelineEngine`, the real default tool surface, the real `McpToolExecutor`
// execution policy and the real `McpSessionRegistry` — so a property that holds
// here holds of the production path.
//
// Two facts about a test binary shape the expectations, and are asserted rather
// than avoided:
//
//   * `generation.generate`, `generation.list_models`, `timeline.export` and
//     `media.import` are hook-backed (the generative backend, the model catalog,
//     the export engine and the media import service are wired by the composition
//     root). With no hook they answer `Unsupported`, so
//     Property 46 requires of them the same result *shape* plus `isError` true
//     naming the tool — the capability is absent from this build, which is not a
//     protocol fault.
//   * an `InvalidArgument` or `NotFound` failure keeps its −32602 classification
//     rather than degrading into an `isError` result, so Property 47's forced
//     failures use other error codes and the argument-stage faults live in
//     Property 48, where their assigned code is the subject.
//
// Pitfalls the generators avoid: identities are always drawn with
// `Uuid::generateV4()` rather than byte-wise (a shrink must never produce the nil
// UUID or a duplicate, both of which the domain core legitimately rejects); every
// path handed to a tool that writes is absolute and inside a per-case scratch
// directory whose name carries the process id, because `gtest_discover_tests` runs
// this binary once per case and ctest runs those processes in parallel.
//
// _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9, 9.13_

#include "services/McpProtocolHandler.hpp"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid, for a per-process scratch directory name

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/SchemaVersion.hpp"
#include "core/TextStyle.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpSessionRegistry.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/ToolRegistry.hpp"
#include "services/ToolSchema.hpp"

// Part 1 of 2 of the shared fixtures for mcp_protocol_property_test, split because the
// fixtures alone exceed the 1000-line ceiling this repository enforces. Cut only at
// points where the brace depth is zero, so no entity is divided.

#ifndef PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT1_HPP_
#define PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT1_HPP_

namespace palmier::services {
namespace testsupport_mcpprotocolpropertytest {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Tool names (the surface `buildDefaultToolRegistry` publishes)
// ---------------------------------------------------------------------------

constexpr std::string_view kReadTimeline = "timeline.read";
constexpr std::string_view kProjectCreate = "project.create";
constexpr std::string_view kProjectOpen = "project.open";
constexpr std::string_view kProjectSave = "project.save";
constexpr std::string_view kProjectInfo = "project.info";
constexpr std::string_view kSetProjectSettings = "project.set_settings";
constexpr std::string_view kMediaImport = "media.import";
constexpr std::string_view kMediaList = "media.list";
constexpr std::string_view kMediaSetTags = "media.set_tags";
constexpr std::string_view kAddTrack = "timeline.add_track";
constexpr std::string_view kRemoveTrack = "timeline.remove_track";
constexpr std::string_view kSetTrackMuted = "timeline.set_track_muted";
constexpr std::string_view kUndo = "edit.undo";
constexpr std::string_view kRedo = "edit.redo";
constexpr std::string_view kAddClip = "timeline.add_clip";
constexpr std::string_view kDeleteClip = "timeline.delete_clip";
constexpr std::string_view kMoveClip = "timeline.move_clip";
constexpr std::string_view kSetClipProperties = "timeline.set_clip_properties";
constexpr std::string_view kApplyLayout = "timeline.apply_layout";
constexpr std::string_view kSetKeyframes = "timeline.set_keyframes";
constexpr std::string_view kTrimClip = "timeline.trim_clip";
constexpr std::string_view kSplitClip = "timeline.split_clip";
constexpr std::string_view kReorderClips = "timeline.reorder_clips";
constexpr std::string_view kRippleDelete = "timeline.ripple_delete";
constexpr std::string_view kRippleTrim = "timeline.ripple_trim";
constexpr std::string_view kCloseGap = "timeline.close_gap";
constexpr std::string_view kAddEffect = "timeline.add_effect";
constexpr std::string_view kRemoveEffect = "timeline.remove_effect";
constexpr std::string_view kReorderEffects = "timeline.reorder_effects";
constexpr std::string_view kSetEffectParameter = "timeline.set_effect_parameter";
constexpr std::string_view kEditCurvePoint = "timeline.edit_curve_point";
constexpr std::string_view kSetEffectResource = "timeline.set_effect_resource";
constexpr std::string_view kAddTransition = "timeline.add_transition";
// Text and titles (usable-editor task 12; Requirement 9).
constexpr std::string_view kAddTextClip = "timeline.add_text_clip";
constexpr std::string_view kSetTextContent = "timeline.set_text_content";
constexpr std::string_view kSetTextStyle = "timeline.set_text_style";
// Captions and transcription (usable-editor task 13; Requirement 10).
constexpr std::string_view kAddCaptionCue = "timeline.add_caption_cue";
constexpr std::string_view kSetCaptionText = "timeline.set_caption_text";
constexpr std::string_view kRetimeCaptionCue = "timeline.retime_caption_cue";
constexpr std::string_view kRemoveCaptionCue = "timeline.remove_caption_cue";
constexpr std::string_view kTranscribeToCaptions = "timeline.transcribe_to_captions";
constexpr std::string_view kCaptureFrame = "timeline.capture_frame";
constexpr std::string_view kGenerate = "generation.generate";
constexpr std::string_view kListModels = "generation.list_models";
constexpr std::string_view kExport = "timeline.export";

/// The five tools whose capability is supplied by the composition root. In a test
/// binary no hook is wired, so they answer `Unsupported` — accounted for, not
/// treated as a failure. `generation.list_models` (usable-editor Phase 2 task 7;
/// PR 406) belongs here for the same reason as `generation.generate`: its handler
/// is a guarded hook, and the model catalog is supplied by `ApplicationComposition`,
/// so without that hook it reports that no catalog is configured.
/// `timeline.transcribe_to_captions` (usable-editor task 13; Requirement 10.4/
/// 10.5) belongs here for the identical reason: its handler is a guarded hook
/// too, and even in a REAL build the hook is bound to
/// `UnavailableTranscriptionBackend` (no recognizer is bundled), so it answers
/// `Unsupported` by name whether or not a hook is wired at all — a stronger
/// version of the same "capability not configured" shape the other four have.
/// `timeline.capture_frame` (usable-editor tasks.md task 14) belongs here too,
/// but for the ORIGINAL reason `generation.generate` does: in a real build its
/// hook IS wired to a working `ui::QtImageEncoder`, but this test binary's
/// `Stack` never supplies one, so it answers `Unsupported` for exactly the
/// "not configured in this build" reason the other four already have.
[[nodiscard]] inline bool isHookBacked(std::string_view tool) {
    return tool == kGenerate || tool == kListModels || tool == kExport || tool == kMediaImport ||
           tool == kTranscribeToCaptions || tool == kCaptureFrame;
}

// ---------------------------------------------------------------------------
// Scratch directory
// ---------------------------------------------------------------------------

/// A per-case directory under the OS temp directory, removed when the case ends.
/// The name carries the process id as well as a counter because CTest runs this
/// binary once per discovered case, in parallel processes.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        root_ = fs::temp_directory_path() /
                ("palmier_mcp_protocol_prop_" + std::to_string(static_cast<long long>(::getpid())) +
                 "_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

    /// A fresh ABSOLUTE path inside this directory. Every tool that writes is only
    /// ever given one of these, so no generated case can litter the ctest working
    /// directory.
    [[nodiscard]] fs::path file(std::string_view tag) const {
        static std::atomic<std::uint64_t> counter{0};
        return root_ / (std::string(tag) + "_" + std::to_string(counter.fetch_add(1)) + ".palmier");
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

/// Text from an alphabet that needs no JSON escaping, so a drawn value survives
/// dump/parse unchanged and a failing case reads cleanly.
[[nodiscard]] inline std::string drawAsciiText(std::size_t length) {
    static const std::string alphabet = "abcdeXYZ 0189_-.";
    std::string text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) text.push_back(alphabet[drawIndex(alphabet.size())]);
    return text;
}

[[nodiscard]] inline bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Project generator
// ---------------------------------------------------------------------------

constexpr std::int64_t kClipLengthMs = 1000;  ///< long enough for a frame-safe trim/split
constexpr std::int64_t kClipGapMs = 500;      ///< so an added clip has somewhere to land

/// A legal project: a supported schema version, a valid frame rate and canvas,
/// unique non-nil identities, and per-track clips ordered by timelineStart with no
/// overlap. The first track always holds at least two clips so the clip-targeted
/// tools always have a subject, and every clip starts at source zero so a
/// start-edge trim can only shift a clip LATER (never onto its predecessor).
[[nodiscard]] inline Project drawSeedProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP protocol properties";
    project.version = SchemaVersion::current();
    project.timelineFps =
        *rc::gen::element<FrameRate>(FrameRate::fps24(), FrameRate::fps25(), FrameRate::fps30());
    project.canvas = Resolution::hd1080();

    const int assetCount = *rc::gen::inRange(1, 4);
    for (int i = 0; i < assetCount; ++i) {
        project.assets.emplace_back(Uuid::generateV4(),
                                    "/media/source_" + std::to_string(i) + ".mp4");
    }

    const int trackCount = 1 + *rc::gen::inRange(0, 3);
    for (int t = 0; t < trackCount; ++t) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = t == 0 ? TrackKind::Video
                            : *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);

        const int clips = t == 0 ? *rc::gen::inRange(2, 5) : *rc::gen::inRange(0, 4);
        std::int64_t cursorMs = 0;
        for (int c = 0; c < clips; ++c) {
            Clip clip;
            clip.id = Uuid::generateV4();
            clip.assetRef = project.assets[drawIndex(project.assets.size())];
            clip.timelineStart = Duration::fromMilliseconds(cursorMs);
            clip.sourceIn = Duration::zero();
            clip.sourceOut = Duration::fromMilliseconds(kClipLengthMs);
            cursorMs += kClipLengthMs + kClipGapMs;
            // The first clip on track 0 always carries one effect, so tools that
            // change or remove an existing effect (task 9.2; Requirement 6) have a
            // stable, guaranteed target to draw a valid invocation against — every
            // other seed clip is left exactly as before, with none.
            if (t == 0 && c == 0) {
                clip.effects.push_back(Effect::brightness(0.1));
            }
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }

    // A guaranteed text track with a guaranteed text clip (usable-editor task 12;
    // Requirement 9), mirroring track 0's guaranteed effect above: tools that
    // target a text clip (timeline.add_text_clip needs a text TRACK;
    // timeline.set_text_content/set_text_style need an existing text CLIP) need a
    // stable target that does not depend on which of the randomly-kinded tracks
    // above happened to be drawn.
    {
        Track textTrack;
        textTrack.id = Uuid::generateV4();
        textTrack.kind = TrackKind::Text;
        Clip textClip;
        textClip.id = Uuid::generateV4();
        textClip.timelineStart = Duration::zero();
        textClip.sourceIn = Duration::zero();
        textClip.sourceOut = Duration::fromMilliseconds(kClipLengthMs);
        TextStyle style;
        style.content = "Seed title";
        textClip.textStyle = std::move(style);
        textTrack.clips.push_back(std::move(textClip));
        project.tracks.push_back(std::move(textTrack));
    }

    // A guaranteed caption track with a guaranteed caption cue (usable-editor
    // task 13; Requirement 10), mirroring the guaranteed text track above for
    // the identical reason: tools that target a caption cue
    // (timeline.add_caption_cue needs a caption TRACK;
    // timeline.set_caption_text/retime_caption_cue/remove_caption_cue need an
    // existing caption CUE; timeline.transcribe_to_captions needs both a
    // caption track to place cues on AND a source clip to transcribe from,
    // the latter already guaranteed by track 0's clips above) need a stable
    // target that does not depend on which of the randomly-kinded tracks
    // happened to be drawn.
    {
        Track captionTrack;
        captionTrack.id = Uuid::generateV4();
        captionTrack.kind = TrackKind::Caption;
        Clip captionCue;
        captionCue.id = Uuid::generateV4();
        captionCue.timelineStart = Duration::zero();
        captionCue.sourceIn = Duration::zero();
        captionCue.sourceOut = Duration::fromMilliseconds(kClipLengthMs);
        captionCue.captionText = "Seed caption";
        captionTrack.clips.push_back(std::move(captionCue));
        project.tracks.push_back(std::move(captionTrack));
    }
    return project;
}

[[nodiscard]] inline std::size_t clipCount(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

/// The end of the last clip anywhere in the project, in nanoseconds.
[[nodiscard]] inline std::int64_t projectEndNs(const Project& project) {
    std::int64_t end = 0;
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            end = std::max(end, clip.timelineEnd().nanoseconds());
        }
    }
    return end;
}

// ---------------------------------------------------------------------------
// Project fingerprint — everything Requirement 9.5 enumerates, in one comparable
// value. The serialized document bytes cover the whole project value; the
// explicit members spell out the specific facts the requirement names, so a
// failure report says which one moved.
// ---------------------------------------------------------------------------

struct ProjectFingerprint {
    std::string bytes;
    std::size_t trackCount = 0;
    std::size_t clipCount = 0;
    std::size_t undoDepth = 0;
    bool canUndo = false;
    std::vector<std::string> assetRefs;
    std::vector<std::string> clipFacts;  ///< id, source range, effects, transition

    [[nodiscard]] bool operator==(const ProjectFingerprint& other) const = default;
};

[[nodiscard]] inline ProjectFingerprint fingerprint(const ProjectSession& session) {
    const Project project = session.engine().snapshot();

    ProjectFingerprint print;
    print.bytes = serializeProject(project);
    print.trackCount = project.tracks.size();
    print.clipCount = clipCount(project);
    print.undoDepth = session.engine().undoDepth();
    print.canUndo = session.engine().canUndo();

    for (const MediaAssetRef& asset : project.assets) {
        print.assetRefs.push_back(asset.assetId.toString() + "@" + asset.sourcePath);
    }
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) {
            std::string fact = clip.id.toString() + "|" +
                               std::to_string(clip.timelineStart.nanoseconds()) + "|" +
                               std::to_string(clip.sourceIn.nanoseconds()) + "|" +
                               std::to_string(clip.sourceOut.nanoseconds()) + "|effects:";
            for (const Effect& effect : clip.effects) {
                fact += effect.id.toString() + ":" + std::to_string(static_cast<int>(effect.type)) +
                        ",";
            }
            fact += "|transition:";
            if (clip.transitionIn.has_value()) {
                fact += clip.transitionIn->id.toString() + ":" +
                        std::to_string(static_cast<int>(clip.transitionIn->kind)) + ":" +
                        std::to_string(clip.transitionIn->duration.nanoseconds());
            }
            print.clipFacts.push_back(std::move(fact));
        }
    }
    return print;
}

// ---------------------------------------------------------------------------
// The stack under test: session -> tool surface -> executor -> handler
// ---------------------------------------------------------------------------

using RegistryBuilder = std::function<ToolRegistry(ProjectSession&)>;

class Stack {
public:
    explicit Stack(Project project, RegistryBuilder builder = {},
                   McpSessionRegistry::Options sessionOptions = {})
        : session_(std::make_unique<ProjectSession>()), sessions_(std::move(sessionOptions)) {
        // media.set_tags (usable-editor tasks.md task 15) looks its asset up
        // through ProjectSession::mediaLibrary() — a separate MediaManager the
        // session normally rebuilds from the project on create/open (see
        // ProjectSession.hpp's own documented split) — not through
        // Project.assets directly. A raw engine().reset() below never
        // triggers that rebuild, so every asset the drawn project carries is
        // imported into the library here too, exactly as createProject and
        // openProject themselves would.
        for (const MediaAssetRef& asset : project.assets) {
            (void)session_->mediaLibrary().importAsset(asset);
        }
        (void)session_->engine().reset(std::move(project));
        registry_ = builder ? builder(*session_) : buildDefaultToolRegistry(*session_);
        executor_ = std::make_unique<McpToolExecutor>(registry_, session_.get());
        handler_ = std::make_unique<McpProtocolHandler>(registry_, *executor_, sessions_,
                                                        inlineMainThreadInvoker());
    }

    [[nodiscard]] McpProtocolHandler& handler() { return *handler_; }
    [[nodiscard]] const ToolRegistry& registry() const { return registry_; }
    [[nodiscard]] ProjectSession& session() { return *session_; }
    [[nodiscard]] TimelineEngine& engine() { return session_->engine(); }

    /// `initialize` + `notifications/initialized`, returning the session id.
    [[nodiscard]] std::string openSession() {
        Json params = Json::object();
        params.set("protocolVersion",
                   Json(std::string(McpProtocolHandler::latestProtocolVersion())));
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(static_cast<std::int64_t>(1)));
        request.set("method", Json("initialize"));
        request.set("params", std::move(params));

        const McpReply init = handler_->handle(context(std::nullopt), request.dump());
        if (!init.newSessionId.has_value()) return {};
        const std::string id = *init.newSessionId;

        Json notification = Json::object();
        notification.set("jsonrpc", Json("2.0"));
        notification.set("method", Json("notifications/initialized"));
        (void)handler_->handle(context(id), notification.dump());
        return id;
    }

    [[nodiscard]] static McpRequestContext context(std::optional<std::string> sessionId) {
        McpRequestContext ctx;
        ctx.sourceAddress = "127.0.0.1";
        ctx.sessionId = std::move(sessionId);
        return ctx;
    }

private:
    std::unique_ptr<ProjectSession> session_;
    ToolRegistry registry_;
    McpSessionRegistry sessions_;
    std::unique_ptr<McpToolExecutor> executor_;
    std::unique_ptr<McpProtocolHandler> handler_;
};

/// One handled request plus how long the handler took (Requirement 9.1's
/// 1000-millisecond bound).
struct Timed {
    McpReply reply;
    std::chrono::milliseconds elapsed{0};
};

[[nodiscard]] inline Timed handleTimed(McpProtocolHandler& handler,
                                       const McpRequestContext& context, std::string_view body) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    McpReply reply = handler.handle(context, body);
    const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return Timed{std::move(reply), elapsed};
}

// ---------------------------------------------------------------------------
// Envelope helpers
// ---------------------------------------------------------------------------

/// A JSON-RPC request object. `id` absent ⇒ a notification.
[[nodiscard]] inline Json envelope(std::string_view method, const std::optional<Json>& id,
                                   std::optional<Json> params = std::nullopt) {
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    if (id.has_value()) request.set("id", *id);
    request.set("method", Json(std::string(method)));
    if (params.has_value()) request.set("params", std::move(*params));
    return request;
}

[[nodiscard]] inline Json callParams(std::string_view tool, Json arguments) {
    Json params = Json::object();
    params.set("name", Json(std::string(tool)));
    params.set("arguments", std::move(arguments));
    return params;
}

/// The parsed response body, or std::nullopt when it is not JSON at all.
[[nodiscard]] inline std::optional<Json> parseBody(const McpReply& reply) {
    Result<Json> parsed = Json::parse(reply.body);
    if (parsed.isError()) return std::nullopt;
    return std::move(parsed).value();
}

/// `"jsonrpc":"2.0"` plus exactly one of `result` / `error` (Requirement 9.1).
[[nodiscard]] inline bool isJsonRpcEnvelope(const Json& body) {
    if (!body.isObject()) return false;
    if (body.stringOr("jsonrpc") != "2.0") return false;
    return body.contains("result") != body.contains("error");
}

[[nodiscard]] inline std::int64_t errorCode(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->intOr("code") : 0;
}

[[nodiscard]] inline std::string errorMessage(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->stringOr("message") : std::string{};
}

/// The text of a tool result's first `content` entry, or std::nullopt when the
/// result does not carry the shape Requirement 9.4 specifies.
[[nodiscard]] inline std::optional<std::string> firstTextContent(const Json& result) {
    const Json* content = result.find("content");
    if (content == nullptr || !content->isArray() || content->asArray().empty()) {
        return std::nullopt;
    }
    const Json& entry = content->asArray().front();
    if (!entry.isObject() || entry.stringOr("type") != "text") return std::nullopt;
    return entry.stringOr("text");
}

// ---------------------------------------------------------------------------
// Schema-derived argument construction
// ---------------------------------------------------------------------------

/// A value satisfying `spec`. Path-shaped arguments are pointed inside `scratch`,
/// so a tool that writes cannot escape the per-case directory.

}  // namespace testsupport_mcpprotocolpropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_MCP_PROTOCOL_PROPERTY_TEST_SUPPORT1_HPP_
