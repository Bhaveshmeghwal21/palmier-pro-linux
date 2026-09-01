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

namespace palmier::services {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Tool names (the surface `buildDefaultToolRegistry` publishes)
// ---------------------------------------------------------------------------

constexpr std::string_view kReadTimeline  = "timeline.read";
constexpr std::string_view kProjectCreate = "project.create";
constexpr std::string_view kProjectOpen   = "project.open";
constexpr std::string_view kProjectSave   = "project.save";
constexpr std::string_view kProjectInfo   = "project.info";
constexpr std::string_view kSetProjectSettings = "project.set_settings";
constexpr std::string_view kMediaImport   = "media.import";
constexpr std::string_view kMediaList     = "media.list";
constexpr std::string_view kMediaSetTags  = "media.set_tags";
constexpr std::string_view kAddTrack      = "timeline.add_track";
constexpr std::string_view kRemoveTrack   = "timeline.remove_track";
constexpr std::string_view kSetTrackMuted = "timeline.set_track_muted";
constexpr std::string_view kUndo          = "edit.undo";
constexpr std::string_view kRedo          = "edit.redo";
constexpr std::string_view kAddClip       = "timeline.add_clip";
constexpr std::string_view kDeleteClip    = "timeline.delete_clip";
constexpr std::string_view kMoveClip      = "timeline.move_clip";
constexpr std::string_view kTrimClip      = "timeline.trim_clip";
constexpr std::string_view kSplitClip     = "timeline.split_clip";
constexpr std::string_view kReorderClips  = "timeline.reorder_clips";
constexpr std::string_view kRippleDelete  = "timeline.ripple_delete";
constexpr std::string_view kRippleTrim    = "timeline.ripple_trim";
constexpr std::string_view kCloseGap      = "timeline.close_gap";
constexpr std::string_view kAddEffect     = "timeline.add_effect";
constexpr std::string_view kRemoveEffect       = "timeline.remove_effect";
constexpr std::string_view kReorderEffects     = "timeline.reorder_effects";
constexpr std::string_view kSetEffectParameter = "timeline.set_effect_parameter";
constexpr std::string_view kEditCurvePoint = "timeline.edit_curve_point";
constexpr std::string_view kAddTransition = "timeline.add_transition";
// Text and titles (usable-editor task 12; Requirement 9).
constexpr std::string_view kAddTextClip    = "timeline.add_text_clip";
constexpr std::string_view kSetTextContent = "timeline.set_text_content";
constexpr std::string_view kSetTextStyle   = "timeline.set_text_style";
// Captions and transcription (usable-editor task 13; Requirement 10).
constexpr std::string_view kAddCaptionCue        = "timeline.add_caption_cue";
constexpr std::string_view kSetCaptionText       = "timeline.set_caption_text";
constexpr std::string_view kRetimeCaptionCue     = "timeline.retime_caption_cue";
constexpr std::string_view kRemoveCaptionCue     = "timeline.remove_caption_cue";
constexpr std::string_view kTranscribeToCaptions = "timeline.transcribe_to_captions";
constexpr std::string_view kCaptureFrame  = "timeline.capture_frame";
constexpr std::string_view kGenerate      = "generation.generate";
constexpr std::string_view kListModels    = "generation.list_models";
constexpr std::string_view kExport        = "timeline.export";

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
[[nodiscard]] bool isHookBacked(std::string_view tool) {
    return tool == kGenerate || tool == kListModels || tool == kExport ||
           tool == kMediaImport || tool == kTranscribeToCaptions || tool == kCaptureFrame;
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
                ("palmier_mcp_protocol_prop_" +
                 std::to_string(static_cast<long long>(::getpid())) + "_" +
                 std::to_string(counter.fetch_add(1)));
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
        return root_ / (std::string(tag) + "_" + std::to_string(counter.fetch_add(1)) +
                        ".palmier");
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

/// Text from an alphabet that needs no JSON escaping, so a drawn value survives
/// dump/parse unchanged and a failing case reads cleanly.
[[nodiscard]] std::string drawAsciiText(std::size_t length) {
    static const std::string alphabet = "abcdeXYZ 0189_-.";
    std::string              text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) text.push_back(alphabet[drawIndex(alphabet.size())]);
    return text;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Project generator
// ---------------------------------------------------------------------------

constexpr std::int64_t kClipLengthMs = 1000;  ///< long enough for a frame-safe trim/split
constexpr std::int64_t kClipGapMs    = 500;   ///< so an added clip has somewhere to land

/// A legal project: a supported schema version, a valid frame rate and canvas,
/// unique non-nil identities, and per-track clips ordered by timelineStart with no
/// overlap. The first track always holds at least two clips so the clip-targeted
/// tools always have a subject, and every clip starts at source zero so a
/// start-edge trim can only shift a clip LATER (never onto its predecessor).
[[nodiscard]] Project drawSeedProject() {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "MCP protocol properties";
    project.version = SchemaVersion::current();
    project.timelineFps = *rc::gen::element<FrameRate>(FrameRate::fps24(), FrameRate::fps25(),
                                                       FrameRate::fps30());
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

        const int   clips = t == 0 ? *rc::gen::inRange(2, 5) : *rc::gen::inRange(0, 4);
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

[[nodiscard]] std::size_t clipCount(const Project& project) {
    std::size_t count = 0;
    for (const Track& track : project.tracks) count += track.clips.size();
    return count;
}

/// The end of the last clip anywhere in the project, in nanoseconds.
[[nodiscard]] std::int64_t projectEndNs(const Project& project) {
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
    std::string              bytes;
    std::size_t              trackCount = 0;
    std::size_t              clipCount = 0;
    std::size_t              undoDepth = 0;
    bool                     canUndo = false;
    std::vector<std::string> assetRefs;
    std::vector<std::string> clipFacts;  ///< id, source range, effects, transition

    [[nodiscard]] bool operator==(const ProjectFingerprint& other) const = default;
};

[[nodiscard]] ProjectFingerprint fingerprint(const ProjectSession& session) {
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
                fact += effect.id.toString() + ":" +
                        std::to_string(static_cast<int>(effect.type)) + ",";
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
    [[nodiscard]] ProjectSession&     session() { return *session_; }
    [[nodiscard]] TimelineEngine&     engine() { return session_->engine(); }

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
    std::unique_ptr<ProjectSession>     session_;
    ToolRegistry                        registry_;
    McpSessionRegistry                  sessions_;
    std::unique_ptr<McpToolExecutor>    executor_;
    std::unique_ptr<McpProtocolHandler> handler_;
};

/// One handled request plus how long the handler took (Requirement 9.1's
/// 1000-millisecond bound).
struct Timed {
    McpReply                  reply;
    std::chrono::milliseconds elapsed{0};
};

[[nodiscard]] Timed handleTimed(McpProtocolHandler& handler, const McpRequestContext& context,
                               std::string_view body) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    McpReply reply = handler.handle(context, body);
    const std::chrono::milliseconds elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return Timed{std::move(reply), elapsed};
}

// ---------------------------------------------------------------------------
// Envelope helpers
// ---------------------------------------------------------------------------

/// A JSON-RPC request object. `id` absent ⇒ a notification.
[[nodiscard]] Json envelope(std::string_view method, const std::optional<Json>& id,
                           std::optional<Json> params = std::nullopt) {
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    if (id.has_value()) request.set("id", *id);
    request.set("method", Json(std::string(method)));
    if (params.has_value()) request.set("params", std::move(*params));
    return request;
}

[[nodiscard]] Json callParams(std::string_view tool, Json arguments) {
    Json params = Json::object();
    params.set("name", Json(std::string(tool)));
    params.set("arguments", std::move(arguments));
    return params;
}

/// The parsed response body, or std::nullopt when it is not JSON at all.
[[nodiscard]] std::optional<Json> parseBody(const McpReply& reply) {
    Result<Json> parsed = Json::parse(reply.body);
    if (parsed.isError()) return std::nullopt;
    return std::move(parsed).value();
}

/// `"jsonrpc":"2.0"` plus exactly one of `result` / `error` (Requirement 9.1).
[[nodiscard]] bool isJsonRpcEnvelope(const Json& body) {
    if (!body.isObject()) return false;
    if (body.stringOr("jsonrpc") != "2.0") return false;
    return body.contains("result") != body.contains("error");
}

[[nodiscard]] std::int64_t errorCode(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->intOr("code") : 0;
}

[[nodiscard]] std::string errorMessage(const Json& body) {
    const Json* error = body.find("error");
    return error != nullptr ? error->stringOr("message") : std::string{};
}

/// The text of a tool result's first `content` entry, or std::nullopt when the
/// result does not carry the shape Requirement 9.4 specifies.
[[nodiscard]] std::optional<std::string> firstTextContent(const Json& result) {
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
[[nodiscard]] Json validValue(const ArgSpec& spec, const ScratchDir& scratch) {
    switch (spec.kind) {
        case JsonKind::Object: {
            Json object = Json::object();
            object.set("amount", Json(0.5));
            return object;
        }
        case JsonKind::Array: {
            Json              items = Json::array();
            const std::size_t count = std::max<std::size_t>(spec.minLength.value_or(0),
                                                            drawIndex(3));
            for (std::size_t i = 0; i < count; ++i) {
                items.push_back(Json(Uuid::generateV4().toString()));
            }
            return items;
        }
        case JsonKind::String: {
            if (!spec.enumValues.empty()) {
                return Json(spec.enumValues[drawIndex(spec.enumValues.size())]);
            }
            if (spec.uuid) return Json(Uuid::generateV4().toString());
            if (spec.name == "path" || spec.name == "outputPath") {
                return Json(scratch.file("generated").string());
            }
            const std::size_t low = std::max<std::size_t>(spec.minLength.value_or(1), 1);
            const std::size_t high = std::min<std::size_t>(spec.maxLength.value_or(low + 8),
                                                           low + 8);
            return Json(drawAsciiText(low + drawIndex(high - low + 1)));
        }
        case JsonKind::Integer: {
            const std::int64_t low = spec.minInt.value_or(0);
            const std::int64_t high = spec.maxInt.value_or(low + 1'000'000);
            return Json(low >= high ? low : *rc::gen::inRange<std::int64_t>(low, high));
        }
        case JsonKind::Number: {
            const double low = spec.minNum.value_or(static_cast<double>(spec.minInt.value_or(0)));
            const double high =
                spec.maxNum.value_or(static_cast<double>(spec.maxInt.value_or(0)) + 100.0);
            const double span = high > low ? high - low : 1.0;
            return Json(low + span * (static_cast<double>(drawIndex(101)) / 100.0));
        }
        case JsonKind::Bool:
            return Json(*rc::gen::arbitrary<bool>());
    }
    return Json(nullptr);
}

/// A schema-valid argument object for `tool`: every required argument present,
/// each optional one included at random.
[[nodiscard]] Json schemaValidArgs(const Tool& tool, const ScratchDir& scratch) {
    Json args = Json::object();
    for (const ArgSpec& spec : tool.schema.args()) {
        if (!spec.required && !*rc::gen::arbitrary<bool>()) continue;
        args.set(spec.name, validValue(spec, scratch));
    }
    return args;
}

/// A value of a type the argument does not accept.
[[nodiscard]] Json wrongTypedValue(JsonKind kind) {
    if (kind == JsonKind::String) return Json(static_cast<std::int64_t>(7));
    if (kind == JsonKind::Bool) return Json("true");
    if (kind == JsonKind::Array || kind == JsonKind::Object) return Json("not-a-composite");
    return Json("not-a-number");
}

/// True iff `spec` declares a bound a value can fall outside of.
[[nodiscard]] bool hasBound(const ArgSpec& spec) {
    return spec.uuid || !spec.enumValues.empty() || spec.minInt.has_value() ||
           spec.maxInt.has_value() || spec.minNum.has_value() || spec.maxNum.has_value() ||
           spec.minLength.has_value() || spec.maxLength.has_value();
}

/// A value violating exactly one declared bound of `spec`.
[[nodiscard]] Json outOfBoundsValue(const ArgSpec& spec) {
    if (!spec.enumValues.empty()) return Json("not-a-member-of-the-enum");
    if (spec.uuid) return Json("not-a-uuid");
    if (spec.maxInt.has_value()) return Json(*spec.maxInt + 1);
    if (spec.minInt.has_value()) return Json(*spec.minInt - 1);
    if (spec.maxNum.has_value()) return Json(*spec.maxNum + 1.0);
    if (spec.minNum.has_value()) return Json(*spec.minNum - 1.0);
    if (spec.maxLength.has_value()) {
        return spec.kind == JsonKind::Array ? Json::array() : Json(drawAsciiText(*spec.maxLength + 1));
    }
    // A minLength bound: an empty string / empty array is one short of it.
    return spec.kind == JsonKind::Array ? Json::array() : Json(std::string{});
}

/// The enum value set a tool declares for `argument` (empty when it declares none).
[[nodiscard]] std::string drawEnumValue(const ToolRegistry& registry, std::string_view tool,
                                       std::string_view argument) {
    const Tool* entry = registry.find(tool);
    if (entry == nullptr) return {};
    const ArgSpec* spec = entry->schema.find(argument);
    if (spec == nullptr || spec->enumValues.empty()) return {};
    return spec->enumValues[drawIndex(spec->enumValues.size())];
}

// ---------------------------------------------------------------------------
// Property 46's invocation generator: a real tool with arguments that both
// validate against its advertised schema AND are semantically applicable to the
// current project, drawn from live project state.
// ---------------------------------------------------------------------------

struct Invocation {
    std::string tool;
    Json        arguments;
};

/// A track holding at least one clip, or nullptr when the project has none.
[[nodiscard]] const Track* drawTrackWithClips(const Project& project) {
    std::vector<const Track*> candidates;
    for (const Track& track : project.tracks) {
        if (!track.clips.empty()) candidates.push_back(&track);
    }
    if (candidates.empty()) return nullptr;
    return candidates[drawIndex(candidates.size())];
}

[[nodiscard]] Invocation drawValidInvocation(const ToolRegistry& registry,
                                            const ProjectSession& session,
                                            const ScratchDir& scratch,
                                            const fs::path& document) {
    const Tool&       tool = registry.tools()[drawIndex(registry.size())];
    const std::string name = tool.name;
    const Project     project = session.engine().snapshot();

    Json args = Json::object();

    // The arg-less tools, and the hook-backed tools, take their arguments straight
    // from the schema. `edit.undo` / `edit.redo` (task 10.1) belong here: they take
    // no arguments and an empty history is the engine's documented successful
    // no-op, never an error, so they are applicable to every generated project.
    if (name == kReadTimeline || name == kProjectInfo || name == kMediaList ||
        name == kUndo || name == kRedo || isHookBacked(name)) {
        return Invocation{name, schemaValidArgs(tool, scratch)};
    }

    // media.set_tags (usable-editor tasks.md task 15; no dedicated
    // Requirement) needs a REAL asset id from the project, exactly like
    // timeline.add_clip needs a real trackId/assetId — the seed project
    // always carries at least one asset (see drawSeedProject()).
    if (name == kMediaSetTags) {
        args.set("assetId", Json(project.assets[drawIndex(project.assets.size())].assetId.toString()));
        Json tags = Json::array();
        const int tagCount = *rc::gen::inRange(0, 4);
        for (int i = 0; i < tagCount; ++i) {
            tags.push_back(Json(drawAsciiText(1 + drawIndex(12))));
        }
        args.set("tags", std::move(tags));
        return Invocation{name, std::move(args)};
    }

    if (name == kProjectCreate) {
        args.set("name", Json(drawAsciiText(1 + drawIndex(24))));
        args.set("fps", Json(static_cast<double>(*rc::gen::inRange(1, 241))));
        args.set("width", Json(static_cast<std::int64_t>(*rc::gen::inRange(16, 7681))));
        args.set("height", Json(static_cast<std::int64_t>(*rc::gen::inRange(16, 4321))));
        if (*rc::gen::arbitrary<bool>()) {
            args.set("colorSpace", Json(drawEnumValue(registry, kProjectCreate, "colorSpace")));
        }
        return Invocation{name, std::move(args)};
    }
    if (name == kProjectOpen) {
        args.set("path", Json(document.string()));
        return Invocation{name, std::move(args)};
    }
    if (name == kProjectSave) {
        args.set("path", Json(scratch.file("saved").string()));
        return Invocation{name, std::move(args)};
    }
    // Mutable project settings (task 10; Requirement 7): at least one field must
    // be given, so the generic schemaValidArgs() fallback (which may supply none)
    // would not be applicable here. A distinct fps from the seed's own rate proves
    // this changes the setting rather than reporting a no-op-shaped success.
    if (name == kSetProjectSettings) {
        const double newFps = project.timelineFps.toDouble() >= 60.0 ? 24.0 : 60.0;
        args.set("fps", Json(newFps));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddTrack) {
        args.set("kind", Json(drawEnumValue(registry, kAddTrack, "kind")));
        return Invocation{name, std::move(args)};
    }
    if (name == kRemoveTrack) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        return Invocation{name, std::move(args)};
    }
    // Task 10.1: like every other track-addressed edit, applicable exactly when the
    // identifier names a track of the current project. Either flag value applies.
    if (name == kSetTrackMuted) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        args.set("muted", Json(*rc::gen::arbitrary<bool>()));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddClip) {
        const Track& track = project.tracks[drawIndex(project.tracks.size())];
        args.set("trackId", Json(track.id.toString()));
        args.set("assetId",
                 Json(project.assets[drawIndex(project.assets.size())].assetId.toString()));
        // Beyond everything already on the timeline, so the placement cannot
        // overlap an existing clip.
        args.set("timelineStartNs",
                 Json(projectEndNs(project) + Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("sourceInNs", Json(static_cast<std::int64_t>(0)));
        args.set("sourceOutNs",
                 Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        if (*rc::gen::arbitrary<bool>()) {
            args.set("opacity", Json(static_cast<double>(drawIndex(101)) / 100.0));
        }
        return Invocation{name, std::move(args)};
    }

    // Everything below needs a clip. The seed project always has at least two on
    // its first track, so this is reachable for every generated project.
    const Track* track = drawTrackWithClips(project);
    RC_ASSERT(track != nullptr);

    if (name == kReorderClips) {
        std::vector<std::size_t> order(track->clips.size());
        std::iota(order.begin(), order.end(), 0u);
        for (std::size_t i = order.size(); i > 1; --i) {  // Fisher-Yates
            std::swap(order[i - 1], order[drawIndex(i)]);
        }
        Json entries = Json::array();
        for (const std::size_t index : order) {
            entries.push_back(Json(track->clips[index].id.toString()));
        }
        args.set("trackId", Json(track->id.toString()));
        args.set("order", std::move(entries));
        return Invocation{name, std::move(args)};
    }

    // A start-edge trim shifts the clip later on its track, so trimming targets
    // the LAST clip of the track, where no successor can be displaced.
    const Clip& lastClip = track->clips.back();
    const Clip& anyClip = track->clips[drawIndex(track->clips.size())];

    if (name == kTrimClip) {
        args.set("clipId", Json(lastClip.id.toString()));
        args.set("edge", Json(drawEnumValue(registry, kTrimClip, "edge")));
        // Deliberately unclamped: TrimClipCommand clamps the boundary into the
        // legal range rather than rejecting it, so any value is applicable.
        args.set("boundaryNs",
                 Json(Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(
                          -kClipLengthMs, 3 * kClipLengthMs))
                          .nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kDeleteClip) {
        args.set("clipId", Json(anyClip.id.toString()));
        return Invocation{name, std::move(args)};
    }
    // Task 8.2 — ripple editing. Every seed clip is a full second long and is
    // separated from its neighbour by a gap, which is what makes these applicable.
    if (name == kRippleDelete) {
        // Closing a one-second hole only ever moves later clips EARLIER by one
        // second, and each already begins a gap after its predecessor's end, so no
        // choice of clip can overlap or cross zero.
        args.set("clipId", Json(anyClip.id.toString()));
        return Invocation{name, std::move(args)};
    }
    if (name == kRippleTrim) {
        // Applicable for either edge and any boundary, because the command clamps:
        // `sourceDurationNs` defaults to the clip's out-point, so an end-edge trim
        // can only SHORTEN the clip (its followers move earlier by the same amount,
        // preserving every gap) and a start-edge trim leaves the trailing edge fixed
        // (so no follower moves at all). The seed project declares no clip groups,
        // so no cross-track propagation is in play here.
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("edge", Json(drawEnumValue(registry, kRippleTrim, "edge")));
        args.set("boundaryNs",
                 Json(Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(
                          -kClipLengthMs, 3 * kClipLengthMs))
                          .nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kCloseGap) {
        // A gap can only be closed after a clip that HAS a successor, so this
        // deliberately targets the first track, which the seed project always fills
        // with at least two clips, and never its last clip.
        const Track* twoOrMore = nullptr;
        for (const Track& candidate : project.tracks) {
            if (candidate.clips.size() >= 2) {
                twoOrMore = &candidate;
                break;
            }
        }
        RC_ASSERT(twoOrMore != nullptr);
        const Clip& withSuccessor = twoOrMore->clips[drawIndex(twoOrMore->clips.size() - 1)];
        args.set("clipId", Json(withSuccessor.id.toString()));
        return Invocation{name, std::move(args)};
    }
    if (name == kMoveClip) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("timelineStartNs",
                 Json(projectEndNs(project) +
                      Duration::fromMilliseconds(2 * kClipGapMs).nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    if (name == kSplitClip) {
        // Strictly interior: the clip is a full second long, so the midpoint is
        // several frames from either edge at every generated frame rate.
        const std::int64_t interior =
            anyClip.timelineStart.nanoseconds() +
            (anyClip.timelineEnd().nanoseconds() - anyClip.timelineStart.nanoseconds()) / 2;
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("playheadNs", Json(interior));
        return Invocation{name, std::move(args)};
    }
    if (name == kAddEffect) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("type", Json(drawEnumValue(registry, kAddEffect, "type")));
        if (*rc::gen::arbitrary<bool>()) {
            Json parameters = Json::object();
            parameters.set("amount", Json(static_cast<double>(drawIndex(101)) / 100.0));
            args.set("parameters", std::move(parameters));
        }
        return Invocation{name, std::move(args)};
    }
    if (name == kAddTransition) {
        args.set("clipId", Json(anyClip.id.toString()));
        args.set("kind", Json(drawEnumValue(registry, kAddTransition, "kind")));
        args.set("durationNs",
                 Json(Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 200))
                          .nanoseconds()));
        return Invocation{name, std::move(args)};
    }
    // Effect lifecycle (task 9.2; Requirement 6). drawSeedProject() guarantees the
    // first clip of track 0 carries exactly one effect, which is what these three
    // target: track 0 always exists and always has at least one clip, so this does
    // not depend on which track/clip drawEnumValue or anyClip happened to pick.
    if (name == kRemoveEffect || name == kReorderEffects || name == kSetEffectParameter) {
        const Clip& seeded = project.tracks[0].clips[0];
        const Uuid  effectId = seeded.effects.front().id;
        args.set("clipId", Json(seeded.id.toString()));
        if (name == kRemoveEffect) {
            args.set("effectId", Json(effectId.toString()));
        } else if (name == kReorderEffects) {
            // A permutation of exactly the clip's one effect id is the only
            // permutation there is, and it is trivially valid.
            Json order = Json::array();
            order.push_back(Json(effectId.toString()));
            args.set("order", std::move(order));
        } else {
            args.set("effectId", Json(effectId.toString()));
            args.set("parameter", Json(std::string{"amount"}));
            args.set("value", Json(static_cast<double>(drawIndex(201)) / 100.0));
        }
        return Invocation{name, std::move(args)};
    }

    // Tone-curve control points (monitoring-and-grading Requirement 5.7). Targets the
    // same guaranteed seeded effect. The effect's TYPE does not matter: a control point
    // is a parameter like any other, and the command deliberately does not police which
    // effect kind carries one -- exactly as set_effect_parameter does not.
    if (name == kEditCurvePoint) {
        const Clip& seeded = project.tracks[0].clips[0];
        args.set("clipId", Json(seeded.id.toString()));
        args.set("effectId", Json(seeded.effects.front().id.toString()));
        args.set("channel", Json(drawEnumValue(registry, kEditCurvePoint, "channel")));
        // "add" only: move and remove need a point that already exists, and this draws
        // one invocation against a fresh project every case. The undo/redo and
        // renumbering behaviour of the other two is pinned in the core command's own
        // tests, where a curve can be built up first.
        args.set("operation", Json(std::string{"add"}));
        args.set("x", Json(static_cast<double>(drawIndex(101)) / 100.0));
        args.set("y", Json(static_cast<double>(drawIndex(101)) / 100.0));
        return Invocation{name, std::move(args)};
    }

    // Text and titles (usable-editor task 12; Requirement 9). The seed project's
    // guaranteed text track/clip (see drawSeedProject()) is the stable target,
    // exactly like the effect-lifecycle tools above target track 0's guaranteed
    // effect: these three need a REAL text track/clip, which a random
    // schema-valid uuid from the generic fallback below would essentially never
    // draw, and this property's own premise (every non-hook-backed invocation
    // succeeds) needs that.
    const Track* textTrack = nullptr;
    for (const Track& candidate : project.tracks) {
        if (candidate.kind == TrackKind::Text) {
            textTrack = &candidate;
            break;
        }
    }
    RC_ASSERT(textTrack != nullptr);

    if (name == kAddTextClip) {
        args.set("trackId", Json(textTrack->id.toString()));
        args.set("timelineStartNs",
                 Json(projectEndNs(project) +
                      Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("durationNs",
                 Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        args.set("content", Json(drawAsciiText(1 + drawIndex(24))));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetTextContent || name == kSetTextStyle) {
        const Clip& textClip = textTrack->clips.front();
        args.set("clipId", Json(textClip.id.toString()));
        if (name == kSetTextContent) {
            args.set("content", Json(drawAsciiText(1 + drawIndex(24))));
        } else {
            args.set("pointSize", Json(static_cast<double>(1 + drawIndex(200))));
        }
        return Invocation{name, std::move(args)};
    }

    // Captions (usable-editor task 13; Requirement 10.2). The seed project's
    // guaranteed caption track/cue (see drawSeedProject()) is the stable
    // target, for the identical reason the text track/clip above is: these
    // four need a REAL caption track/cue, which a random schema-valid uuid
    // from the generic fallback would essentially never draw.
    // timeline.transcribe_to_captions is NOT handled here — it is
    // isHookBacked() (see that function's own doc comment): even in a real
    // build no recognizer backend is bundled, so it always reports
    // Unsupported by name regardless of its arguments, exactly like
    // generation.generate/list_models/timeline.export/media.import, and
    // ToolsCallSuccessShape's isHookBacked() branch is what actually checks
    // its result shape.
    const Track* captionTrack = nullptr;
    for (const Track& candidate : project.tracks) {
        if (candidate.kind == TrackKind::Caption) {
            captionTrack = &candidate;
            break;
        }
    }
    RC_ASSERT(captionTrack != nullptr);

    if (name == kAddCaptionCue) {
        args.set("trackId", Json(captionTrack->id.toString()));
        args.set("timelineStartNs",
                 Json(projectEndNs(project) +
                      Duration::fromMilliseconds(kClipGapMs).nanoseconds()));
        args.set("durationNs",
                 Json(Duration::fromMilliseconds(kClipLengthMs).nanoseconds()));
        args.set("text", Json(drawAsciiText(1 + drawIndex(24))));
        return Invocation{name, std::move(args)};
    }
    if (name == kSetCaptionText || name == kRetimeCaptionCue || name == kRemoveCaptionCue) {
        const Clip& captionCue = captionTrack->clips.front();
        args.set("clipId", Json(captionCue.id.toString()));
        if (name == kSetCaptionText) {
            args.set("text", Json(drawAsciiText(1 + drawIndex(24))));
        } else if (name == kRetimeCaptionCue) {
            // At least one of timelineStartNs/durationNs is required; supply
            // durationNs only, mirroring setTextStyle's own "supply one field"
            // pattern above.
            args.set("durationNs",
                     Json(Duration::fromMilliseconds(kClipLengthMs / 2).nanoseconds()));
        }
        // kRemoveCaptionCue needs only clipId, already set above.
        return Invocation{name, std::move(args)};
    }

    // A tool added later without a case here still exercises the property through
    // its own schema.
    return Invocation{name, schemaValidArgs(tool, scratch)};
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 43: JSON-RPC envelope
// round-trip — for all well-formed JSON-RPC 2.0 requests carrying an `id`, the
// response parses as JSON, carries `"jsonrpc":"2.0"`, echoes the `id` unchanged in
// both type and value, carries exactly one of `result` or `error`, and is produced
// within 1000 milliseconds; and for all well-formed notifications, which carry no
// `id`, the response body is zero bytes.
//
// Requirement 9.1: "WHEN the MCP_Endpoint receives a POST request on `/mcp` whose
// body is a JSON-RPC 2.0 request of at most 1 MiB, THE MCP_Protocol_Handler SHALL
// respond within 1000 milliseconds with a JSON-RPC 2.0 response object that
// carries `"jsonrpc":"2.0"`, carries the request `id` unchanged, and carries
// exactly one of `result` or `error`."
// Requirement 9.13: "FOR ALL well-formed JSON-RPC requests that carry an `id`, THE
// MCP_Protocol_Handler SHALL produce a response that parses as JSON and echoes the
// request `id` unchanged in both type and value; FOR ALL well-formed
// notifications, which carry no `id`, THE MCP_Protocol_Handler SHALL produce no
// response body (round-trip property)."
//
// The request arm draws over the three *request* methods; `notifications/initialized`
// is the protocol's one notification, and carrying an `id` on it is not well formed
// (Requirement 9.10 fixes its answer as a zero-byte body regardless), so it is the
// subject of the notification arm instead.
//
// **Validates: Requirements 9.1, 9.13**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, JsonRpcEnvelopeRoundTrip, ()) {
    ScratchDir scratch;

    McpSessionRegistry::Options options;
    options.maxSessions = 32;  // so a case may draw several initializes
    Stack             stack(drawSeedProject(), {}, options);
    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const int requests = *rc::gen::inRange(1, 5);
    for (int i = 0; i < requests; ++i) {
        // --- the notification arm ------------------------------------------
        if (*rc::gen::inRange(0, 4) == 0) {
            std::optional<Json> params;
            if (*rc::gen::arbitrary<bool>()) params = Json::object();
            const std::string body =
                envelope("notifications/initialized", std::nullopt, std::move(params)).dump();

            const Timed answered = handleTimed(stack.handler(), Stack::context(session), body);
            RC_ASSERT(answered.reply.body.empty());
            RC_ASSERT(answered.reply.body.size() == 0u);
            RC_ASSERT(answered.reply.httpStatus == 202);
            RC_ASSERT(answered.elapsed < 1000ms);
            continue;
        }

        // --- the request arm ------------------------------------------------
        Json id(nullptr);
        switch (*rc::gen::inRange(0, 3)) {
            case 0:
                id = Json(drawAsciiText(1 + drawIndex(12)));
                break;
            case 1:
                id = Json(*rc::gen::inRange<std::int64_t>(-4096, 1'000'001));
                break;
            default:
                id = Json(nullptr);
                break;
        }

        std::string        method;
        std::optional<Json> params;
        switch (*rc::gen::inRange(0, 3)) {
            case 0: {
                method = "initialize";
                if (*rc::gen::arbitrary<bool>()) {
                    Json p = Json::object();
                    p.set("protocolVersion", Json(drawAsciiText(1 + drawIndex(10))));
                    params = std::move(p);
                }
                break;
            }
            case 1:
                method = "tools/list";
                if (*rc::gen::arbitrary<bool>()) params = Json::object();
                break;
            default: {
                method = "tools/call";
                const Tool& tool = stack.registry().tools()[drawIndex(stack.registry().size())];
                switch (*rc::gen::inRange(0, 3)) {
                    case 0:  // a schema-valid argument object
                        params = callParams(tool.name, schemaValidArgs(tool, scratch));
                        break;
                    case 1:  // an unregistered tool name
                        params = callParams("timeline." + drawAsciiText(6), Json::object());
                        break;
                    default:  // arguments the schema rejects
                        params = callParams(tool.name, Json::object());
                        break;
                }
                break;
            }
        }

        const std::string body = envelope(method, id, std::move(params)).dump();
        const Timed       answered = handleTimed(stack.handler(), Stack::context(session), body);

        // Produced within the stated bound.
        RC_ASSERT(answered.elapsed < 1000ms);

        // Parses as JSON, and is a JSON-RPC 2.0 response with exactly one of
        // result / error.
        const std::optional<Json> parsedBody = parseBody(answered.reply);
        RC_ASSERT(parsedBody.has_value());
        RC_ASSERT(isJsonRpcEnvelope(*parsedBody));

        // Echoes the id unchanged in both type and value.
        const Json* echoed = parsedBody->find("id");
        RC_ASSERT(echoed != nullptr);
        RC_ASSERT(echoed->type() == id.type());
        RC_ASSERT(*echoed == id);
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 44: initialize negotiates a
// supported protocol version — for any client-requested protocol version string,
// the `initialize` result carries a negotiated version equal to the request when
// that version is supported and equal to the highest supported version otherwise,
// together with a non-empty server name, a server version, and a capabilities
// object declaring the `tools` capability.
//
// Requirement 9.2: "WHEN the MCP_Protocol_Handler receives the `initialize`
// method, THE MCP_Protocol_Handler SHALL return a negotiated protocol version
// equal to the client-requested version when that version is one the
// MCP_Protocol_Handler supports and otherwise equal to the highest version it
// supports, a non-empty server name, a server version, and a capabilities object
// that declares the `tools` capability."
//
// **Validates: Requirements 9.2**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, InitializeNegotiatesASupportedProtocolVersion, ()) {
    McpSessionRegistry::Options options;
    options.maxSessions = 32;
    Stack stack(drawSeedProject(), {}, options);

    const int attempts = *rc::gen::inRange(1, 6);
    for (int i = 0; i < attempts; ++i) {
        // Supported versions, unsupported date-shaped strings, the empty string,
        // non-date garbage, and the omitted-parameter case.
        std::optional<std::string> requested;
        switch (*rc::gen::inRange(0, 5)) {
            case 0:
                requested = std::string(
                    McpProtocolHandler::kSupportedProtocolVersions[drawIndex(
                        std::size(McpProtocolHandler::kSupportedProtocolVersions))]);
                break;
            case 1: {
                char buffer[16] = {};
                std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d",
                              *rc::gen::inRange(1990, 2101), *rc::gen::inRange(1, 13),
                              *rc::gen::inRange(1, 29));
                requested = std::string(buffer);
                break;
            }
            case 2:
                requested = std::string{};
                break;
            case 3:
                requested = drawAsciiText(1 + drawIndex(20));
                break;
            default:
                requested = std::nullopt;  // no protocolVersion supplied
                break;
        }

        std::optional<Json> params;
        if (requested.has_value()) {
            Json p = Json::object();
            p.set("protocolVersion", Json(*requested));
            params = std::move(p);
        }

        const std::string body =
            envelope("initialize", Json(static_cast<std::int64_t>(i + 1)), std::move(params))
                .dump();
        const McpReply reply = stack.handler().handle(Stack::context(std::nullopt), body);

        const std::optional<Json> parsedBody = parseBody(reply);
        RC_ASSERT(parsedBody.has_value());
        RC_ASSERT(isJsonRpcEnvelope(*parsedBody));
        const Json* result = parsedBody->find("result");
        RC_ASSERT(result != nullptr);

        // The requested version when supported; the highest supported otherwise.
        const std::string expected =
            requested.has_value() && McpProtocolHandler::isSupportedProtocolVersion(*requested)
                ? *requested
                : std::string(McpProtocolHandler::latestProtocolVersion());
        RC_ASSERT(result->stringOr("protocolVersion") == expected);
        RC_ASSERT(McpProtocolHandler::isSupportedProtocolVersion(
            result->stringOr("protocolVersion")));

        // A non-empty server name and a server version.
        const Json* serverInfo = result->find("serverInfo");
        RC_ASSERT(serverInfo != nullptr);
        RC_ASSERT(!serverInfo->stringOr("name").empty());
        RC_ASSERT(!serverInfo->stringOr("version").empty());

        // A capabilities object declaring `tools`.
        const Json* capabilities = result->find("capabilities");
        RC_ASSERT(capabilities != nullptr);
        RC_ASSERT(capabilities->isObject());
        const Json* tools = capabilities->find("tools");
        RC_ASSERT(tools != nullptr);
        RC_ASSERT(tools->isObject());
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 45: tools/list describes every
// registered tool — for any registry contents, the `tools/list` result contains
// exactly one entry per registered tool, and every entry carries a non-empty
// `name` of at most 64 characters, a non-empty `description`, and an `inputSchema`
// object of type `object` naming each accepted argument and listing the required
// arguments.
//
// Requirement 9.3: "WHEN the MCP_Protocol_Handler receives the `tools/list`
// method, THE MCP_Protocol_Handler SHALL return one entry per tool registered in
// the Tool_Surface — so that the entry count equals the Tool_Surface tool count —
// with each entry containing a non-empty `name` of at most 64 characters, a
// non-empty `description`, and an `inputSchema` object that is a JSON Schema of
// type object naming each accepted argument and listing the required arguments."
//
// **Validates: Requirements 9.3**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ToolsListDescribesEveryRegisteredTool, ()) {
    // A generated subset, in a generated order, of the full advertised surface.
    // The null-session overload advertises the identical surface, so it can be
    // consulted for the tool count before the session under test exists.
    const ToolRegistry advertised =
        buildDefaultToolRegistry(static_cast<ProjectSession*>(nullptr));
    const std::size_t total = advertised.size();
    RC_ASSERT(total > 0u);

    std::vector<std::size_t> order(total);
    std::iota(order.begin(), order.end(), 0u);
    for (std::size_t i = total; i > 1; --i) {  // Fisher-Yates
        std::swap(order[i - 1], order[drawIndex(i)]);
    }
    order.resize(*rc::gen::inRange<std::size_t>(0, total + 1));

    Stack stack(drawSeedProject(), [order](ProjectSession& session) {
        const ToolRegistry full = buildDefaultToolRegistry(session);
        ToolRegistry       subset;
        for (const std::size_t index : order) subset.add(full.tools()[index]);
        return subset;
    });
    RC_ASSERT(stack.registry().size() == order.size());

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const McpReply reply = stack.handler().handle(
        Stack::context(session),
        envelope("tools/list", Json(static_cast<std::int64_t>(7))).dump());

    const std::optional<Json> body = parseBody(reply);
    RC_ASSERT(body.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*body));
    const Json* result = body->find("result");
    RC_ASSERT(result != nullptr);
    const Json* entries = result->find("tools");
    RC_ASSERT(entries != nullptr);
    RC_ASSERT(entries->isArray());

    // Exactly one entry per registered tool.
    RC_ASSERT(entries->asArray().size() == stack.registry().size());

    std::set<std::string> described;
    for (const Json& entry : entries->asArray()) {
        RC_ASSERT(entry.isObject());

        const std::string name = entry.stringOr("name");
        RC_ASSERT(!name.empty());
        RC_ASSERT(name.size() <= 64u);
        RC_ASSERT(described.insert(name).second);  // one entry, not several
        RC_ASSERT(!entry.stringOr("description").empty());

        const Tool* tool = stack.registry().find(name);
        RC_ASSERT(tool != nullptr);

        const Json* schema = entry.find("inputSchema");
        RC_ASSERT(schema != nullptr);
        RC_ASSERT(schema->isObject());
        RC_ASSERT(schema->stringOr("type") == "object");

        // Naming each accepted argument ...
        const Json* properties = schema->find("properties");
        RC_ASSERT(properties != nullptr);
        RC_ASSERT(properties->isObject());
        RC_ASSERT(properties->asObject().size() == tool->schema.size());
        for (const ArgSpec& spec : tool->schema.args()) {
            RC_ASSERT(properties->contains(spec.name));
        }

        // ... and listing the required ones, and only those.
        std::set<std::string> requiredBySchema;
        if (const Json* required = schema->find("required"); required != nullptr) {
            RC_ASSERT(required->isArray());
            for (const Json& item : required->asArray()) {
                RC_ASSERT(item.isString());
                RC_ASSERT(requiredBySchema.insert(item.asString()).second);
            }
        }
        std::set<std::string> requiredByTool;
        for (const ArgSpec& spec : tool->schema.args()) {
            if (spec.required) requiredByTool.insert(spec.name);
        }
        RC_ASSERT(requiredBySchema == requiredByTool);
    }
    RC_ASSERT(described.size() == stack.registry().size());
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 46: tools/call success shape —
// for any registered tool and any argument object that validates against its
// advertised `inputSchema`, the `tools/call` result carries a `content` array
// whose first entry has `"type":"text"` and carries `isError` set to false.
//
// Requirement 9.4: "WHEN the MCP_Protocol_Handler receives the `tools/call` method
// with a tool name registered in the Tool_Surface and an arguments object that
// validates against that tool's advertised `inputSchema`, THE MCP_Protocol_Handler
// SHALL execute the tool through the same execution policy used for GUI and
// in-app agent invocations — argument validation, rollback on failure, and
// undo-history recording — and return a result object containing a `content` array
// whose first entry has `"type":"text"`, plus `isError` set to false."
//
// `generation.generate`, `generation.list_models`, `timeline.export` and
// `media.import` are hook-backed and no hook is wired in a test binary, so their
// capability is absent from this build:
// they are required to answer in the identical result shape with `isError` true
// naming the tool, which is what "the capability is not configured" looks like
// through the protocol rather than a protocol fault.
//
// **Validates: Requirements 9.4**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ToolsCallSuccessShape, ()) {
    ScratchDir    scratch;
    const Project seed = drawSeedProject();

    // A readable document for `project.open`, written through the project store.
    const fs::path document = scratch.file("document");
    RC_ASSERT(saveProjectToFile(seed, document).isOk());

    Stack             stack(seed);
    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const Invocation invocation =
        drawValidInvocation(stack.registry(), stack.session(), scratch, document);
    const Tool* tool = stack.registry().find(invocation.tool);
    RC_ASSERT(tool != nullptr);

    // The arguments validate against the very schema `tools/list` advertises.
    RC_ASSERT(tool->schema.validate(invocation.arguments).isOk());

    const std::string body =
        envelope("tools/call", Json(static_cast<std::int64_t>(11)),
                 callParams(invocation.tool, invocation.arguments))
            .dump();
    const McpReply reply = stack.handler().handle(Stack::context(session), body);

    const std::optional<Json> parsedBody = parseBody(reply);
    RC_ASSERT(parsedBody.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*parsedBody));

    // The call reached the tool: the answer is a result, never a protocol error.
    const Json* result = parsedBody->find("result");
    RC_ASSERT(result != nullptr);

    // A `content` array whose first entry is a text entry.
    const std::optional<std::string> text = firstTextContent(*result);
    RC_ASSERT(text.has_value());
    RC_ASSERT(!text->empty());

    const Json* isError = result->find("isError");
    RC_ASSERT(isError != nullptr);
    RC_ASSERT(isError->isBool());

    if (isHookBacked(invocation.tool)) {
        // The capability is not configured in this build: reported as a tool
        // failure naming the tool, in the same result shape.
        RC_ASSERT(isError->asBool());
        RC_ASSERT(contains(*text, invocation.tool));
    } else {
        RC_ASSERT(!isError->asBool());
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 47: A failing tool leaves the
// project and history untouched — for any project and any tool invocation forced to
// fail, the `tools/call` result carries `isError` set to true with a `content`
// entry naming the failing tool and the failure reason, and the project's track
// count, clip set, clip source ranges, effects, transitions, asset references and
// undo history compare equal to their pre-invocation values.
//
// Requirement 9.5: "IF a tool invoked through `tools/call` fails, THEN THE
// MCP_Protocol_Handler SHALL return a JSON-RPC result with `isError` set to true
// and a `content` entry naming the failing tool and the failure reason, and SHALL
// leave the project — its track count, clip set, clip source ranges, effects,
// transitions, asset references and undo history — in its pre-invocation state."
//
// The failure is injected at three points: before any command is created, after
// exactly one applied command, and after several applied commands — so the
// rollback obligation is exercised from "nothing to undo" through "undo a whole
// batch". `InvalidArgument` and `NotFound` failures keep their −32602
// classification instead of becoming an `isError` result, so they are Property 48's
// subject rather than this one's.
//
// **Validates: Requirements 9.5**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, FailingToolLeavesProjectAndHistoryUntouched, ()) {
    const Project seed = drawSeedProject();

    // 0 = fail before any command, 1 = fail after one, 2 = fail after several.
    const int         appliedBeforeFailure = *rc::gen::inRange(0, 3);
    const std::string reason = "forced failure " + drawAsciiText(1 + drawIndex(12));
    const std::string failingToolName = "test.forced_failure";

    Stack stack(seed, [appliedBeforeFailure, reason,
                       failingToolName](ProjectSession& session) {
        ToolRegistry registry = buildDefaultToolRegistry(session);

        Tool failing;
        failing.name = failingToolName;
        failing.description = "Applies a generated number of edits and then fails.";
        failing.handler = [&session, appliedBeforeFailure,
                           reason](const Json&) -> Result<Json> {
            TimelineEngine& engine = session.engine();
            for (int i = 0; i < appliedBeforeFailure; ++i) {
                const Project project = engine.snapshot();
                if (project.tracks.empty()) break;

                Clip clip;
                clip.id = Uuid::generateV4();
                clip.assetRef = project.assets.empty()
                                    ? MediaAssetRef(Uuid::generateV4(), "/media/forced.mp4")
                                    : project.assets.front();
                clip.timelineStart = Duration::fromNanoseconds(
                    projectEndNs(project) +
                    Duration::fromMilliseconds(kClipGapMs).nanoseconds());
                clip.sourceIn = Duration::zero();
                clip.sourceOut = Duration::fromMilliseconds(kClipLengthMs);
                const CommandResult applied = engine.apply(
                    std::make_unique<AddClipCommand>(project.tracks.front().id,
                                                     std::move(clip)));
                if (applied.isError()) break;
            }
            // A FailedPrecondition (rather than InvalidArgument / NotFound) is what
            // Requirement 9.5 classifies as a tool failure.
            return err<Json>(failedPrecondition(reason));
        };
        registry.add(std::move(failing));
        return registry;
    });

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const ProjectFingerprint before = fingerprint(stack.session());

    const McpReply reply = stack.handler().handle(
        Stack::context(session),
        envelope("tools/call", Json(static_cast<std::int64_t>(13)),
                 callParams(failingToolName, Json::object()))
            .dump());

    const std::optional<Json> body = parseBody(reply);
    RC_ASSERT(body.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*body));
    const Json* result = body->find("result");
    RC_ASSERT(result != nullptr);

    // isError true, with a content entry naming the tool and the reason.
    RC_ASSERT(result->boolOr("isError", false));
    const std::optional<std::string> text = firstTextContent(*result);
    RC_ASSERT(text.has_value());
    RC_ASSERT(contains(*text, failingToolName));
    RC_ASSERT(contains(*text, reason));

    // The project and the undo history are exactly as they were.
    const ProjectFingerprint after = fingerprint(stack.session());
    RC_ASSERT(after.trackCount == before.trackCount);
    RC_ASSERT(after.clipCount == before.clipCount);
    RC_ASSERT(after.clipFacts == before.clipFacts);
    RC_ASSERT(after.assetRefs == before.assetRefs);
    RC_ASSERT(after.undoDepth == before.undoDepth);
    RC_ASSERT(after.canUndo == before.canUndo);
    RC_ASSERT(after.bytes == before.bytes);
    RC_ASSERT(after == before);
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 48: Protocol faults map to
// their assigned codes and create no edit command — for any request violating
// exactly one protocol rule, the response carries the code that rule is assigned —
// −32700 for an unparsable or over-1-MiB body, −32600 for a missing
// `"jsonrpc":"2.0"`, a missing `method`, or a `method`/`id` of a disallowed JSON
// type, −32601 for an unsupported method (with the method named), −32602 for an
// unknown tool, a missing required argument, a wrong-typed argument or an
// out-of-bounds argument value (with the tool or argument named) — and no edit
// command is created and the project is byte-identical.
//
// Requirement 9.6: "IF a request body is not parsable JSON, or exceeds 1 MiB, THEN
// THE MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32700 and SHALL
// create no edit command."
// Requirement 9.7: "IF a request is not a valid JSON-RPC 2.0 request object —
// because it omits `"jsonrpc":"2.0"`, omits `method`, or supplies `method` or `id`
// with a JSON type other than string (or, for `id`, number or null) — THEN THE
// MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32600."
// Requirement 9.8: "IF a request names a method other than `initialize`,
// `notifications/initialized`, `tools/list` or `tools/call`, THEN THE
// MCP_Protocol_Handler SHALL respond with JSON-RPC error code -32601 and an error
// message naming the unsupported method."
// Requirement 9.9: "IF a `tools/call` request names a tool absent from the
// Tool_Surface, omits a required argument, supplies an argument of the wrong JSON
// type, or supplies an argument value outside the bounds declared by the tool's
// `inputSchema`, THEN THE MCP_Protocol_Handler SHALL respond with JSON-RPC error
// code -32602 naming the offending tool or argument, SHALL create no edit command,
// and SHALL leave the project state unchanged."
//
// **Validates: Requirements 9.6, 9.7, 9.8, 9.9**
// ===========================================================================
RC_GTEST_PROP(McpProtocolProperties, ProtocolFaultsMapToTheirAssignedCodes, ()) {
    ScratchDir scratch;
    Stack      stack(drawSeedProject());

    const std::string session = stack.openSession();
    RC_ASSERT(!session.empty());

    const ProjectFingerprint before = fingerprint(stack.session());

    enum class Fault {
        Unparsable,
        OversizeDeclared,
        OversizeActual,
        NotAnObject,
        MissingJsonrpc,
        WrongJsonrpc,
        MissingMethod,
        NonStringMethod,
        DisallowedIdType,
        UnsupportedMethod,
        UnknownTool,
        MissingRequiredArgument,
        WrongTypedArgument,
        OutOfBoundsArgument,
    };
    static constexpr Fault kFaults[] = {
        Fault::Unparsable,         Fault::OversizeDeclared,   Fault::OversizeActual,
        Fault::NotAnObject,        Fault::MissingJsonrpc,     Fault::WrongJsonrpc,
        Fault::MissingMethod,      Fault::NonStringMethod,    Fault::DisallowedIdType,
        Fault::UnsupportedMethod,  Fault::UnknownTool,        Fault::MissingRequiredArgument,
        Fault::WrongTypedArgument, Fault::OutOfBoundsArgument};
    const Fault fault = kFaults[drawIndex(std::size(kFaults))];

    McpRequestContext context = Stack::context(session);
    std::string       body;
    std::int64_t      expectedCode = 0;
    std::string       mustName;      ///< the method, tool or argument the message must name
    std::string       alsoMustName;  ///< a second name (the tool, for argument faults)

    // A well-formed `tools/call` on a real tool is the base every argument fault
    // perturbs in exactly one place.
    const Tool& tool = stack.registry().tools()[drawIndex(stack.registry().size())];

    switch (fault) {
        case Fault::Unparsable: {
            static const char* const unparsable[] = {
                "{", "[", "}", "{\"jsonrpc\"", "{\"jsonrpc\":\"2.0\",", "not json at all",
                "\"unterminated", "{,}", "{\"a\":}", "tru"};
            body = unparsable[drawIndex(std::size(unparsable))];
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::OversizeDeclared: {
            body = envelope("tools/list", Json(static_cast<std::int64_t>(1))).dump();
            context.bodyBytes = McpProtocolHandler::kDefaultMaxBodyBytes +
                                static_cast<std::size_t>(*rc::gen::inRange(1, 4096));
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::OversizeActual: {
            Json padded = Json::object();
            padded.set("name", Json(std::string(kReadTimeline)));
            padded.set("padding",
                       Json(std::string(McpProtocolHandler::kDefaultMaxBodyBytes + 64, 'p')));
            body = envelope("tools/call", Json(static_cast<std::int64_t>(1)),
                            std::move(padded))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorParse;
            break;
        }
        case Fault::NotAnObject: {
            switch (*rc::gen::inRange(0, 3)) {
                case 0:
                    body = R"(["jsonrpc","2.0"])";
                    break;
                case 1:
                    body = "42";
                    break;
                default:
                    body = "\"tools/list\"";
                    break;
            }
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::MissingJsonrpc: {
            Json request = Json::object();
            request.set("id", Json(static_cast<std::int64_t>(3)));
            request.set("method", Json("tools/list"));
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::WrongJsonrpc: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(3)));
            request.set("jsonrpc", *rc::gen::arbitrary<bool>()
                                       ? Json("1.0")
                                       : Json(2.0));  // wrong value / wrong type
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::MissingMethod: {
            Json request = Json::object();
            request.set("jsonrpc", Json("2.0"));
            request.set("id", Json(static_cast<std::int64_t>(4)));
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::NonStringMethod: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(4)));
            switch (*rc::gen::inRange(0, 3)) {
                case 0:
                    request.set("method", Json(static_cast<std::int64_t>(42)));
                    break;
                case 1:
                    request.set("method", Json::array());
                    break;
                default:
                    request.set("method", Json(true));
                    break;
            }
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::DisallowedIdType: {
            Json request = envelope("tools/list", Json(static_cast<std::int64_t>(5)));
            switch (*rc::gen::inRange(0, 3)) {
                case 0: {
                    Json object = Json::object();
                    object.set("a", Json(static_cast<std::int64_t>(1)));
                    request.set("id", std::move(object));
                    break;
                }
                case 1: {
                    Json array = Json::array();
                    array.push_back(Json(static_cast<std::int64_t>(1)));
                    request.set("id", std::move(array));
                    break;
                }
                default:
                    request.set("id", Json(*rc::gen::arbitrary<bool>()));
                    break;
            }
            body = request.dump();
            expectedCode = McpProtocolHandler::kErrorInvalidRequest;
            break;
        }
        case Fault::UnsupportedMethod: {
            static const char* const prefixes[] = {"resources/", "prompts/", "completion/",
                                                   "logging/",   "tools/",   ""};
            mustName = std::string(prefixes[drawIndex(std::size(prefixes))]) +
                       drawAsciiText(1 + drawIndex(10));
            RC_PRE(mustName != "initialize" && mustName != "tools/list" &&
                   mustName != "tools/call" && mustName != "notifications/initialized");
            body = envelope(mustName, Json(static_cast<std::int64_t>(6))).dump();
            expectedCode = McpProtocolHandler::kErrorMethodNotFound;
            break;
        }
        case Fault::UnknownTool: {
            mustName = "timeline." + drawAsciiText(1 + drawIndex(10));
            RC_PRE(!stack.registry().has(mustName));
            body = envelope("tools/call", Json(static_cast<std::int64_t>(7)),
                            callParams(mustName, Json::object()))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::MissingRequiredArgument: {
            std::vector<const ArgSpec*> required;
            for (const ArgSpec& spec : tool.schema.args()) {
                if (spec.required) required.push_back(&spec);
            }
            RC_PRE(!required.empty());
            const ArgSpec& dropped = *required[drawIndex(required.size())];

            Json args = schemaValidArgs(tool, scratch);
            for (const ArgSpec& spec : tool.schema.args()) {  // ensure every other one is present
                if (spec.name != dropped.name && !args.contains(spec.name)) {
                    args.set(spec.name, validValue(spec, scratch));
                }
            }
            Json trimmed = Json::object();
            for (const auto& [key, value] : args.asObject()) {
                if (key != dropped.name) trimmed.set(key, value);
            }
            mustName = dropped.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(8)),
                            callParams(tool.name, std::move(trimmed)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::WrongTypedArgument: {
            RC_PRE(tool.schema.size() > 0u);
            const ArgSpec& spec = tool.schema.args()[drawIndex(tool.schema.size())];
            Json           args = schemaValidArgs(tool, scratch);
            args.set(spec.name, wrongTypedValue(spec.kind));
            mustName = spec.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(9)),
                            callParams(tool.name, std::move(args)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
        case Fault::OutOfBoundsArgument: {
            std::vector<const ArgSpec*> bounded;
            for (const ArgSpec& spec : tool.schema.args()) {
                if (hasBound(spec)) bounded.push_back(&spec);
            }
            RC_PRE(!bounded.empty());
            const ArgSpec& spec = *bounded[drawIndex(bounded.size())];
            Json           args = schemaValidArgs(tool, scratch);
            args.set(spec.name, outOfBoundsValue(spec));
            mustName = spec.name;
            alsoMustName = tool.name;
            body = envelope("tools/call", Json(static_cast<std::int64_t>(10)),
                            callParams(tool.name, std::move(args)))
                       .dump();
            expectedCode = McpProtocolHandler::kErrorInvalidParams;
            break;
        }
    }

    const McpReply            reply = stack.handler().handle(context, body);
    const std::optional<Json> parsedBody = parseBody(reply);
    RC_ASSERT(parsedBody.has_value());
    RC_ASSERT(isJsonRpcEnvelope(*parsedBody));
    RC_ASSERT(parsedBody->contains("error"));

    // The code that rule is assigned.
    RC_ASSERT(errorCode(*parsedBody) == expectedCode);

    // Naming the method, the tool or the argument.
    const std::string message = errorMessage(*parsedBody);
    if (!mustName.empty()) RC_ASSERT(contains(message, mustName));
    if (!alsoMustName.empty()) RC_ASSERT(contains(message, alsoMustName));

    // No edit command was created, and the project is byte-identical.
    const ProjectFingerprint after = fingerprint(stack.session());
    RC_ASSERT(after.bytes == before.bytes);
    RC_ASSERT(after.undoDepth == before.undoDepth);
    RC_ASSERT(!stack.engine().canUndo());
    RC_ASSERT(after == before);
}

}  // namespace
}  // namespace palmier::services
