// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/agent_equivalence_property_test.cpp — the agent-equivalence
// property (task 10.3; design.md Property 62; Requirements 11.5, 11.10).
//
// What the property actually claims
// --------------------------------
// Requirement 11.10 is an equivalence statement between two ways of reaching the
// editor:
//
//   "FOR ALL utterances the offline interpreter maps successfully, executing the
//    produced invocation SHALL yield the same project state and the same undo
//    history depth as invoking that tool directly with the same arguments."
//
// and Requirement 11.5 says the agent's invocation runs "through the same
// execution policy the MCP_Protocol_Handler uses", validating arguments first,
// restoring the pre-invocation state on failure and recording exactly one undo
// entry on success.
//
// So the property needs TWO independent editors that started life identical, one
// driven the long way round and one driven directly, and a comparison strong
// enough that a difference cannot hide:
//
//   * agent path   — `OfflineIntentInterpreter` -> `AgentOrchestrator::sendMessage`
//                    -> `McpToolExecutor` -> `ToolRegistry` -> `TimelineEngine`.
//   * direct path  — `McpToolExecutor::executeTool(toolName, arguments,
//                    InvocationSource::Mcp)`, i.e. exactly what the MCP protocol
//                    handler does with a decoded `tools/call`.
//
// Both sides are the REAL surface: `buildDefaultToolRegistry` over a real
// `ProjectSession` with a real `TimelineEngine`, the real `McpToolExecutor`
// policy, and the real orchestrator with its real auth gate seam. Nothing about
// the edit path is stubbed.
//
// Why the two sessions are seeded from ONE project value
// -----------------------------------------------------
// "The same project state" is only a meaningful claim between editors that were
// identical to begin with. `World` builds one `Project` value — every identifier
// drawn from `Uuid::generateV4()`, never byte-wise, because `inRange` over UUID
// bytes shrinks towards the nil UUID and towards duplicates, which
// `MediaManager::importAsset` and the timeline invariants reject outright and
// which would make the failure the generator's rather than the product's — and
// resets BOTH sessions with a copy of it. The two editors therefore agree
// identifier for identifier, not merely in shape, which is what makes a
// byte-level comparison possible at all.
//
// How "byte-identical" is established, and the one thing that cannot be
// -------------------------------------------------------------------
// The comparison is over `serializeProject`, the canonical `.palmier` document
// text — the same serializer `project.save` writes and `project.open` reads — not
// over a hand-picked list of fields. A difference anywhere in the project (track
// order, clip ranges, mute flags, transitions, effects, asset references, project
// metadata) changes those bytes. The fingerprint appends the undo depth, the redo
// depth, the session revision, the modified flag and the media-library contents,
// so history depth and out-of-engine state are compared with the same rigour, and
// finally the tool's own JSON result payload.
//
// Two tools in the phrase table mint a FRESH identity as part of succeeding, and
// no two executions of them can ever agree on it:
//
//   * `timeline.add_track`  -> `AddTrackCommand` mints the new track's UUID;
//   * `timeline.split_clip` -> `SplitClipCommand` mints the right half's UUID.
//
// A random UUID is non-deterministic by definition, so demanding literal byte
// equality there would be demanding that `Uuid::generateV4()` stop being random.
// The property instead compares the two states MODULO fresh identity, by
// `canonicalizeFreshIds`: every UUID in the after-text that was not already in
// the before-text is rewritten to a positional placeholder (`<fresh-uuid-0>`,
// `<fresh-uuid-1>`, ...) in order of first appearance, and only then are the two
// texts compared byte for byte. That is deliberately weak in exactly one respect
// and in no other: it hides the VALUE of a newly minted identifier but preserves
// how many were minted, where each one appears, how often, and in what order — so
// a path that created a track in a different position, created a different number
// of objects, or referenced the wrong one still fails. The placeholder mapping is
// built per side from that side's own text, so the two sides are never allowed to
// borrow each other's identifiers.
//
// The third source of per-invocation non-determinism is the asset id `media.import`
// mints. That one is NOT canonicalized away: the import hook draws its ids from a
// per-case map shared by both stacks, so the same path imports as the same asset id
// on both sides and the comparison stays exact.
//
// Forcing half the invocations to fail (the second half of Property 62)
// --------------------------------------------------------------------
// "For any invocation that fails, both paths leave the project byte-identical and
// the undo depth unchanged." A failure is produced by sabotaging the EDITOR
// CONTEXT the interpreter reads — a selection that names no clip, a playhead past
// the clip's end, track ordinals resolving to identifiers no track has, an import
// of a path that does not exist, an export or a save into a directory that does
// not exist. Sabotaging the context rather than the tool surface is what keeps the
// two paths honest: the interpreter still produces ONE invocation, both paths
// receive exactly those arguments, and the failure arrives from the engine or the
// hook rather than from a divergence that was arranged.
//
// Nothing here sleeps, stubs a clock, needs FFmpeg, a GPU, a sound device or Qt.
// `media.import` and `timeline.export` are hook-backed in the real surface and are
// wired to minimal in-test hooks, because Property 62 is about two execution paths
// agreeing, not about whether an encoder emits valid H.264. Every write-performing
// tool is only ever handed an absolute path inside a per-case scratch directory
// whose name carries the process id.

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/MediaManager.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/OfflineIntentInterpreter.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scratch directory. Both stacks of one case share it, so the paths that end up
// inside tool arguments are identical on both sides; the name carries the process
// id because ctest runs this binary once per discovered case, in parallel
// processes.
// ---------------------------------------------------------------------------

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        root_ = fs::temp_directory_path() /
                ("palmier_agent_equiv_" + std::to_string(static_cast<long long>(::getpid())) +
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

    /// A fresh ABSOLUTE path inside this directory.
    [[nodiscard]] fs::path file(std::string_view tag, std::string_view extension) const {
        static std::atomic<std::uint64_t> counter{0};
        return root_ / (std::string(tag) + "_" + std::to_string(counter.fetch_add(1)) +
                        std::string(extension));
    }

    /// An existing readable file, so `media.import` has something real to register.
    [[nodiscard]] fs::path existingFile(std::string_view tag) const {
        const fs::path path = file(tag, ".mp4");
        std::ofstream out(path, std::ios::binary);
        out << "palmier scratch media";
        return path;
    }

    /// An absolute path whose PARENT does not exist, so a write into it fails for
    /// a reason that does not depend on the process's user (the suite runs as
    /// root, which bypasses permission bits).
    [[nodiscard]] fs::path unwritablePath(std::string_view tag,
                                          std::string_view extension) const {
        return root_ / "no-such-directory" /
               (std::string(tag) + std::string(extension));
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// The UUID canonicalizer (see the file comment).
// ---------------------------------------------------------------------------

/// True iff `text` holds a canonical lowercase UUID at `at` (8-4-4-4-12 hex).
[[nodiscard]] bool uuidAt(const std::string& text, std::size_t at) {
    static constexpr std::size_t kLength = 36;
    if (at + kLength > text.size()) return false;
    static constexpr std::size_t kDashes[] = {8, 13, 18, 23};
    for (std::size_t i = 0; i < kLength; ++i) {
        const char c = text[at + i];
        const bool isDashPosition = (i == kDashes[0] || i == kDashes[1] ||
                                     i == kDashes[2] || i == kDashes[3]);
        if (isDashPosition) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/// Every canonical UUID appearing in `text`, in order of first appearance.
[[nodiscard]] std::vector<std::string> uuidsIn(const std::string& text) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i + 36 <= text.size();) {
        if (uuidAt(text, i)) {
            const std::string uuid = text.substr(i, 36);
            bool              seen = false;
            for (const std::string& earlier : found) {
                if (earlier == uuid) {
                    seen = true;
                    break;
                }
            }
            if (!seen) found.push_back(uuid);
            i += 36;
        } else {
            ++i;
        }
    }
    return found;
}

/// Rewrite every UUID of `text` that is absent from `known` to a positional
/// placeholder, numbered by first appearance. UUIDs present in `known` — the
/// identifiers both sessions were seeded with — are left exactly as they are, so
/// they still have to match between the two paths.
///
/// This is the ONE concession to non-determinism in the whole property: the value
/// of an identifier that did not exist before the invocation. Count, position,
/// multiplicity and order of appearance are all preserved.
[[nodiscard]] std::string canonicalizeFreshIds(const std::string&              text,
                                               const std::vector<std::string>& known) {
    std::map<std::string, std::string> placeholders;
    std::size_t                        next = 0;
    for (const std::string& uuid : uuidsIn(text)) {
        bool isKnown = false;
        for (const std::string& k : known) {
            if (k == uuid) {
                isKnown = true;
                break;
            }
        }
        if (!isKnown) {
            placeholders.emplace(uuid, "<fresh-uuid-" + std::to_string(next++) + ">");
        }
    }

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (uuidAt(text, i)) {
            const std::string uuid = text.substr(i, 36);
            const auto        it = placeholders.find(uuid);
            out += (it == placeholders.end()) ? uuid : it->second;
            i += 36;
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The shared world of one case: the scratch directory, the ONE project value both
// sessions are seeded with, the argument paths both paths are handed, and the
// deterministic asset ids `media.import` mints.
// ---------------------------------------------------------------------------

/// How this case makes the invocation fail, if at all. Every mode perturbs the
/// EDITOR CONTEXT (never the tool surface), so both paths still receive identical
/// arguments and the failure comes from the engine or the hook.
enum class Sabotage {
    None,
    BogusSelection,    ///< the selected clip is not in the project
    PlayheadOutside,   ///< the playhead is past the selected clip's end
    BogusTracks,       ///< track ordinals resolve to identifiers no track has
    MissingImportFile, ///< `import <path>` names a file that does not exist
    UnwritableExport,  ///< `export as mp4 to <path>` names a missing directory
    UnwritableSave,    ///< the document path's parent directory does not exist
};

struct World {
    ScratchDir        scratch;
    Project           project;
    std::vector<Uuid> trackIds;      ///< in project order; index i is ordinal i+1
    Uuid              clipId;        ///< a clip with room on either side to split
    std::int64_t      clipStartNs = 0;
    std::int64_t      clipEndNs = 0;

    Sabotage sabotage = Sabotage::None;

    // The exact paths that end up in the tool arguments. Shared by both stacks.
    fs::path importPath;
    fs::path exportPath;
    fs::path documentPath;

    // Path -> asset id, so `media.import` of the same path yields the same asset
    // id on both sides. Every id from `Uuid::generateV4()`.
    std::shared_ptr<std::map<std::string, Uuid>> importIds =
        std::make_shared<std::map<std::string, Uuid>>();

    // The identifiers the sabotaged context reports. Drawn ONCE per case, because
    // `context()` is read once per interpretation and a freshly generated id per
    // read would hand the two paths different arguments — the property would then
    // be failing on the harness rather than on the product.
    Uuid              absentClipId = Uuid::generateV4();
    std::vector<Uuid> absentTrackIds = {Uuid::generateV4(), Uuid::generateV4(),
                                        Uuid::generateV4()};

    /// The editor context both interpreters read. Identical on both sides by
    /// construction — it is a property of the world, not of a session — and stable
    /// across reads.
    [[nodiscard]] EditorContext context() const {
        EditorContext context;
        context.selectedClipId =
            (sabotage == Sabotage::BogusSelection) ? absentClipId  // no clip has this id
                                                   : clipId;
        context.playheadNs = (sabotage == Sabotage::PlayheadOutside)
                                 ? clipEndNs + 1'000'000'000
                                 : clipStartNs + (clipEndNs - clipStartNs) / 2;
        context.trackIds =
            (sabotage == Sabotage::BogusTracks) ? absentTrackIds : trackIds;
        context.documentPath = documentPath;
        return context;
    }
};

/// Build the case's world: a project of two video tracks and one audio track,
/// with two clips on the first video track so ordinals 1..3 all resolve and the
/// selected clip has an interior playhead.
///
/// Every identifier comes from `Uuid::generateV4()`.
[[nodiscard]] std::unique_ptr<World> makeWorld(Sabotage sabotage) {
    auto world = std::make_unique<World>();
    world->sabotage = sabotage;

    Project& project = world->project;
    project.id = Uuid::generateV4();
    project.name = "Agent Equivalence";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    const MediaAssetRef asset(Uuid::generateV4(), "/media/agent-equivalence.mp4");
    project.assets.push_back(asset);

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;
    for (int i = 0; i < 2; ++i) {
        Clip clip;
        clip.id = Uuid::generateV4();
        clip.assetRef = asset;
        clip.timelineStart = Duration::fromMilliseconds(i * 10'000);
        clip.sourceIn = Duration::fromMilliseconds(0);
        clip.sourceOut = Duration::fromMilliseconds(5'000);
        if (i == 0) {
            world->clipId = clip.id;
            world->clipStartNs = clip.timelineStart.nanoseconds();
            world->clipEndNs = clip.timelineEnd().nanoseconds();
        }
        video.clips.push_back(std::move(clip));
    }

    Track secondVideo;
    secondVideo.id = Uuid::generateV4();
    secondVideo.kind = TrackKind::Video;

    Track audio;
    audio.id = Uuid::generateV4();
    audio.kind = TrackKind::Audio;

    world->trackIds = {video.id, secondVideo.id, audio.id};
    project.tracks.push_back(std::move(video));
    project.tracks.push_back(std::move(secondVideo));
    project.tracks.push_back(std::move(audio));

    // The argument paths. Both stacks are handed exactly these.
    world->importPath = (sabotage == Sabotage::MissingImportFile)
                            ? world->scratch.file("absent-import", ".mp4")
                            : world->scratch.existingFile("import");
    world->exportPath = (sabotage == Sabotage::UnwritableExport)
                            ? world->scratch.unwritablePath("export", ".mp4")
                            : world->scratch.file("export", ".mp4");
    world->documentPath = (sabotage == Sabotage::UnwritableSave)
                              ? world->scratch.unwritablePath("project", ".palmier")
                              : world->scratch.file("project", ".palmier");
    return world;
}

// ---------------------------------------------------------------------------
// One editor: a real ProjectSession, the real default tool surface, the real
// execution policy. Two of these per case, seeded from the same project value.
// ---------------------------------------------------------------------------

class Stack {
public:
    explicit Stack(const World& world) : world_(world) {
        [[maybe_unused]] const bool seeded = session_.engine().reset(world.project).isOk();

        ToolRegistryHooks hooks;
        // `media.import`: register the file as one asset of this session's REAL
        // media library, with an asset id drawn once per path and SHARED between
        // the two stacks — so the same import yields the same asset id on both
        // sides and the comparison stays exact rather than being canonicalized.
        hooks.importMedia = [this](const fs::path& path) -> Result<ImportedAsset> {
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                return err<ImportedAsset>(makeError(
                    ErrorCode::NotFound, "media.import: no such file: " + path.string()));
            }
            std::map<std::string, Uuid>& ids = *world_.importIds;
            const auto                   it = ids.find(path.string());
            const Uuid                   assetId =
                (it == ids.end()) ? ids.emplace(path.string(), Uuid::generateV4()).first->second
                                                : it->second;

            const MediaAssetRef asset(assetId, path.string());
            if (Result<void> added = session_.mediaLibrary().importAsset(asset);
                added.isError()) {
                return err<ImportedAsset>(std::move(added).error());
            }
            ImportedAsset imported;
            imported.assetId = assetId;
            imported.sourcePath = path;
            imported.containerFormat = "mp4";
            imported.durationMs = 5'000;
            imported.hasVideo = true;
            return imported;
        };
        // `timeline.export`: write the requested bytes to the requested path. The
        // encoder is stage 9's concern and its files are owned elsewhere.
        hooks.exportTimeline = [](const Json& in) -> Result<Json> {
            const std::string outputPath = in.stringOr("outputPath");
            if (outputPath.empty()) {
                return err<Json>(makeError(ErrorCode::InvalidArgument,
                                           "timeline.export: 'outputPath' is required"));
            }
            std::ofstream out(outputPath, std::ios::binary);
            if (!out) {
                return err<Json>(makeError(ErrorCode::Io,
                                           "timeline.export: cannot write " + outputPath));
            }
            out << "palmier export";
            Json result = Json::object();
            result.set("outputPath", outputPath);
            result.set("format", in.stringOr("format"));
            return result;
        };

        registry_ = buildDefaultToolRegistry(session_, std::move(hooks));
        executor_ = std::make_unique<McpToolExecutor>(registry_, &session_);
    }

    [[nodiscard]] ProjectSession&     session() noexcept { return session_; }
    [[nodiscard]] const ToolRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] McpToolExecutor&    executor() noexcept { return *executor_; }

    /// The interpreter this stack's agent uses: the real offline interpreter over
    /// the world's context, with the network seam armed to fail the test.
    [[nodiscard]] OfflineIntentInterpreter interpreter() const {
        OfflineIntentInterpreter::Options options;
        const World*                      world = &world_;
        options.context = [world] { return world->context(); };
        options.network = [](std::string_view endpoint) -> Result<void> {
            ADD_FAILURE() << "the offline interpreter reached for the network: " << endpoint;
            return err<void>(makeError(ErrorCode::Unsupported, "no network in Offline_Mode"));
        };
        return OfflineIntentInterpreter(std::move(options));
    }

    /// Everything Requirement 11.10 calls "the project state and the undo history
    /// depth", rendered as one text: the canonical `.palmier` serialization of the
    /// project (the same bytes `project.save` writes), both history depths, the
    /// session revision and modified flag, and the media library.
    [[nodiscard]] std::string fingerprint() {
        std::string out = serializeProject(session_.engine().snapshot());
        out += "\nundoDepth=" + std::to_string(session_.engine().undoDepth());
        out += "\nredoDepth=" + std::to_string(session_.engine().redoDepth());
        out += "\nrevision=" + std::to_string(session_.revision());
        out += "\nmodified=" + std::string(session_.modified() ? "1" : "0");
        out += "\nlibrary=";
        for (const MediaAssetRef& asset : session_.mediaLibrary().library()) {
            out += "\n  " + asset.assetId.toString() + " " + asset.sourcePath;
        }
        return out;
    }

    [[nodiscard]] std::size_t undoDepth() const { return session_.engine().undoDepth(); }
    [[nodiscard]] std::size_t redoDepth() const { return session_.engine().redoDepth(); }

private:
    const World&                     world_;
    ProjectSession                   session_;
    ToolRegistry                     registry_;
    std::unique_ptr<McpToolExecutor> executor_;
};

/// The auth gate of Requirement 8.5, always authorizing: this property is about
/// execution equivalence, and an unauthorized send never reaches a tool at all.
class AlwaysAuthorized : public IAgentAuthGate {
public:
    [[nodiscard]] Result<void> authorize() const override { return ok(); }
};

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

/// The phrases that a sabotaged context can actually make FAIL, paired with the
/// sabotage that does it. `add a video track`, `add an audio track`,
/// `show the timeline`, `undo` and `redo` are absent because no context can make
/// them fail — they take no context-derived argument (undo and redo on an empty
/// history are a documented no-op, not a failure).
struct FailableTool {
    const char* toolName;
    Sabotage    sabotage;
};

const std::vector<FailableTool>& failableTools() {
    static const std::vector<FailableTool> tools = {
        {"timeline.split_clip", Sabotage::PlayheadOutside},
        {"timeline.split_clip", Sabotage::BogusSelection},
        {"timeline.delete_clip", Sabotage::BogusSelection},
        {"timeline.set_track_muted", Sabotage::BogusTracks},
        {"media.import", Sabotage::MissingImportFile},
        {"timeline.export", Sabotage::UnwritableExport},
        {"project.save", Sabotage::UnwritableSave},
    };
    return tools;
}

/// The patterns of the documented table resolving to `toolName`.
[[nodiscard]] std::vector<PhrasePattern> patternsFor(std::string_view toolName) {
    std::vector<PhrasePattern> matching;
    for (const PhrasePattern& pattern : OfflineIntentInterpreter::patterns()) {
        if (pattern.toolName == toolName) matching.push_back(pattern);
    }
    return matching;
}

/// One drawn utterance plus the world it is drawn against. Half the cases draw a
/// (phrase, sabotage) pair from `failableTools()` so the invocation is forced to
/// fail; the other half draw freely from the whole documented table with an
/// untouched context.
struct DrawnCase {
    std::unique_ptr<World> world;
    PhrasePattern          pattern;
    std::string            utterance;
    bool                   forcedToFail = false;
};

[[nodiscard]] DrawnCase drawCase() {
    DrawnCase drawn;
    drawn.forcedToFail = *rc::gen::arbitrary<bool>();

    // `rc::gen::elementOf` is the uniform choice over a container; `inRange` is
    // biased towards its low end, which would over-sample the first few phrases of
    // the table and under-sample the rest.
    if (drawn.forcedToFail) {
        const FailableTool failable = *rc::gen::elementOf(failableTools());
        drawn.pattern = *rc::gen::elementOf(patternsFor(failable.toolName));
        drawn.world = makeWorld(failable.sabotage);
    } else {
        drawn.pattern = *rc::gen::elementOf(OfflineIntentInterpreter::patterns());
        drawn.world = makeWorld(Sabotage::None);
    }

    switch (drawn.pattern.match) {
        case PhraseMatch::Exact:
            drawn.utterance = drawn.pattern.canonicalUtterance();
            break;
        case PhraseMatch::Ordinal: {
            // Only ordinals the project can offer, so the refusal under test is the
            // execution one and not the interpreter's separate "track N does not
            // exist". Under Sabotage::BogusTracks the context still offers three
            // ordinals — they simply name identifiers no track has.
            const std::size_t ordinal = *rc::gen::element<std::size_t>(1, 2, 3);
            drawn.utterance = drawn.pattern.canonicalUtterance(ordinal);
            break;
        }
        case PhraseMatch::Suffix: {
            const fs::path path = drawn.pattern.toolName == "media.import"
                                      ? drawn.world->importPath
                                      : drawn.world->exportPath;
            drawn.utterance = drawn.pattern.canonicalUtterance(1, path.string());
            break;
        }
    }
    return drawn;
}

/// Prime BOTH stacks identically so `undo` and `redo` are exercised over a
/// non-empty history as well as an empty one. Applied through the tool surface,
/// so it introduces no fresh identifier of its own (`timeline.set_track_muted`
/// mints nothing) and leaves the two sides identical.
void primeHistory(Stack& stack, const World& world, int priming) {
    if (priming == 0) return;
    Json arguments = Json::object();
    arguments.set("trackId", world.trackIds[0].toString());
    arguments.set("muted", true);
    if (!stack.executor().executeTool("timeline.set_track_muted", arguments).isOk()) return;
    if (priming == 2) {
        (void)stack.executor().executeTool("edit.undo", Json::object());
    }
}

// ===========================================================================
// Feature: end-to-end-editor-integration, Property 62: The agent path equals a
// direct tool invocation — for all utterances the offline interpreter maps
// successfully, executing the produced invocation through the agent yields the
// same project state and the same undo-history depth as invoking that tool
// directly with the same arguments; and for any invocation that fails, both paths
// leave the project byte-identical and the undo depth unchanged.
//
// Requirement 11.10: "FOR ALL utterances the offline interpreter maps
// successfully, executing the produced invocation SHALL yield the same project
// state and the same undo history depth as invoking that tool directly with the
// same arguments (equivalence property)."
//
// Requirement 11.5: "WHEN the Agent_Interpreter produces a tool invocation, THE
// Tool_Surface SHALL execute it through the same execution policy the
// MCP_Protocol_Handler uses, validating the arguments before execution, restoring
// the pre-invocation project state if execution fails, and recording exactly one
// undo entry when execution succeeds."
//
// The generator is the documented phrase table x starting contexts, with half the
// cases drawing a (phrase, context sabotage) pair that forces the invocation to
// fail, and an independently drawn history priming so `undo` and `redo` are seen
// over both an empty and a non-empty history.
//
// The comparison is over `serializeProject` — the canonical `.palmier` document
// text — plus both history depths, the session revision, the modified flag, the
// media library and the tool's own result payload, compared byte for byte after
// identifiers that did not exist before the invocation have been replaced by
// positional placeholders (see the file comment: `timeline.add_track` and
// `timeline.split_clip` mint a random UUID as part of succeeding, and no two
// executions can agree on its value).
//
// **Validates: Requirements 11.5, 11.10**
// ===========================================================================
RC_GTEST_PROP(AgentEquivalenceProperties, TheAgentPathEqualsADirectToolInvocation, ()) {
    const DrawnCase drawn = drawCase();
    const World&    world = *drawn.world;
    // 0 = pristine history, 1 = one applied edit (so `undo` has something to do),
    // 2 = one applied edit then undone (so `redo` has something to do).
    const int priming = *rc::gen::element(0, 1, 2);

    // Two editors, seeded from ONE project value: identical identifier for
    // identifier, which is what makes a byte-level comparison meaningful.
    Stack agentStack(world);
    Stack directStack(world);
    primeHistory(agentStack, world, priming);
    primeHistory(directStack, world, priming);

    // They really did start identical.
    const std::string beforeAgent = agentStack.fingerprint();
    const std::string beforeDirect = directStack.fingerprint();
    RC_ASSERT(beforeAgent == beforeDirect);

    // The identifiers that existed BEFORE the invocation. Anything else in the
    // after-text was minted by the invocation.
    const std::vector<std::string> knownIds = uuidsIn(beforeAgent);

    // The one invocation both paths are given. Requirement 11.10 says "the same
    // arguments", so the arguments are produced once and handed to both sides; the
    // two interpreters are separately asserted to agree on them, which is why
    // sharing them is not hiding anything.
    const OfflineIntentInterpreter agentInterpreter = agentStack.interpreter();
    const Result<AgentIntent>      interpreted = agentInterpreter.interpret(drawn.utterance);
    RC_PRE(interpreted.isOk());  // Property 62 quantifies over utterances that MAP
    const AgentIntent& intent = interpreted.value();

    const Result<AgentIntent> interpretedAgain =
        directStack.interpreter().interpret(drawn.utterance);
    RC_ASSERT(interpretedAgain.isOk());
    RC_ASSERT(interpretedAgain.value().toolName == intent.toolName);
    RC_ASSERT(interpretedAgain.value().arguments.dump() == intent.arguments.dump());

    // --- The agent path: interpreter -> orchestrator -> executor -----------
    AlwaysAuthorized  gate;
    AgentOrchestrator agent(agentStack.executor(), gate, agentInterpreter.asInterpreter());
    const Result<AgentTurn> agentOutcome = agent.sendMessage(drawn.utterance);

    // --- The direct path: the same tool, the same arguments, the same policy the
    //     MCP protocol handler applies to a decoded `tools/call`.
    const Result<Json> directOutcome = directStack.executor().executeTool(
        intent.toolName, intent.arguments, InvocationSource::Mcp);

    RC_TAG(std::string(intent.toolName) + (directOutcome.isOk() ? " ok" : " failed"));

    // 1. The two paths agree on WHETHER the invocation succeeded.
    RC_ASSERT(agentOutcome.isOk() == directOutcome.isOk());

    // 2. And on why, when it failed.
    if (directOutcome.isError()) {
        RC_ASSERT(agentOutcome.error().code() == directOutcome.error().code());
        RC_ASSERT(agentOutcome.error().message() == directOutcome.error().message());
    } else {
        RC_ASSERT(agentOutcome.value().toolName == intent.toolName);
    }

    // 3. The undo and redo depths match exactly (Requirement 11.10's "the same
    //    undo history depth"), compared as numbers, not as text.
    RC_ASSERT(agentStack.undoDepth() == directStack.undoDepth());
    RC_ASSERT(agentStack.redoDepth() == directStack.redoDepth());

    // 4. And the project states are byte-identical, modulo identifiers that did
    //    not exist before the invocation. The tool's own result payload is folded
    //    into the compared text, so a divergence in what the two paths REPORTED
    //    fails here too.
    std::string afterAgent = agentStack.fingerprint();
    std::string afterDirect = directStack.fingerprint();
    if (agentOutcome.isOk()) {
        afterAgent += "\nresult=" + agentOutcome.value().result.dump();
        afterDirect += "\nresult=" + directOutcome.value().dump();
    }
    const std::string canonicalAgent = canonicalizeFreshIds(afterAgent, knownIds);
    const std::string canonicalDirect = canonicalizeFreshIds(afterDirect, knownIds);
    if (canonicalAgent != canonicalDirect) {
        RC_FAIL("the agent path and a direct invocation of '" + intent.toolName +
                "' with " + intent.arguments.dump() +
                " left different states.\nagent:\n" + canonicalAgent + "\ndirect:\n" +
                canonicalDirect);
    }

    // 5. Requirement 11.5's two halves, now that the paths are known to agree.
    if (directOutcome.isError()) {
        // "restoring the pre-invocation project state if execution fails" — each
        // side is byte-identical to ITS OWN pre-invocation fingerprint, so nothing
        // was left behind and no undo residue remains.
        RC_ASSERT(afterAgent == beforeAgent);
        RC_ASSERT(afterDirect == beforeDirect);
    } else if (intent.toolName != "timeline.read" && intent.toolName != "project.save" &&
               intent.toolName != "media.import" && intent.toolName != "timeline.export" &&
               intent.toolName != "edit.undo" && intent.toolName != "edit.redo") {
        // "recording exactly one undo entry when execution succeeds" — for the
        // tools that are edits. The excluded six are not edits: three are reads or
        // writes that apply no command, and undo/redo MOVE through the history
        // rather than extend it.
        const std::size_t beforeDepth = (priming == 0) ? 0u : (priming == 1 ? 1u : 0u);
        RC_ASSERT(agentStack.undoDepth() == beforeDepth + 1);
    }
}

// ---------------------------------------------------------------------------
// Unit tests: the specific examples the property quantifies over but does not pin
// by name, and the checks that keep the property from being vacuous.
// ---------------------------------------------------------------------------

// The property compares texts after canonicalizing fresh identifiers. If that
// canonicalization were too aggressive the whole property would be vacuous, so
// this pins exactly what it does and does not hide.
TEST(AgentEquivalenceCanonicalization, HidesFreshIdentifierValuesAndNothingElse) {
    const Uuid known = Uuid::generateV4();
    const Uuid freshA = Uuid::generateV4();
    const Uuid freshB = Uuid::generateV4();
    const std::vector<std::string> knownIds = {known.toString()};

    // A known id survives verbatim; a fresh one becomes a positional placeholder.
    EXPECT_EQ(canonicalizeFreshIds("track " + known.toString(), knownIds),
              "track " + known.toString());
    EXPECT_EQ(canonicalizeFreshIds("track " + freshA.toString(), knownIds),
              "track <fresh-uuid-0>");

    // Two different fresh ids get two different placeholders...
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " " + freshB.toString(), knownIds),
              "<fresh-uuid-0> <fresh-uuid-1>");
    // ...and the SAME fresh id repeated keeps one placeholder, so multiplicity and
    // cross-references are preserved.
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " " + freshA.toString(), knownIds),
              "<fresh-uuid-0> <fresh-uuid-0>");

    // Consistently relabelling two fresh ids IS invisible, and deliberately so:
    // which random value `Uuid::generateV4()` returned is not part of the project
    // state, so two paths that minted the same structure with different values are
    // equivalent.
    EXPECT_EQ(canonicalizeFreshIds(freshA.toString() + " x " + freshB.toString(), knownIds),
              canonicalizeFreshIds(freshB.toString() + " x " + freshA.toString(), knownIds));

    // What is NOT invisible is the pattern of repetition: a text in which the
    // first fresh id recurs does not canonicalize like one in which the second
    // does, so a path that cross-referenced the wrong object still fails.
    EXPECT_NE(canonicalizeFreshIds(
                  freshA.toString() + " " + freshB.toString() + " " + freshA.toString(),
                  knownIds),
              canonicalizeFreshIds(
                  freshA.toString() + " " + freshB.toString() + " " + freshB.toString(),
                  knownIds));

    // Nor is putting a fresh id where a pre-existing one belongs.
    EXPECT_NE(canonicalizeFreshIds("track " + known.toString(), knownIds),
              canonicalizeFreshIds("track " + freshA.toString(), knownIds));

    // A different NUMBER of fresh ids never canonicalizes alike.
    EXPECT_NE(canonicalizeFreshIds(freshA.toString(), knownIds),
              canonicalizeFreshIds(freshA.toString() + " " + freshB.toString(), knownIds));

    // Non-UUID text is untouched, including near-misses of the UUID shape.
    EXPECT_EQ(canonicalizeFreshIds("plain text, 1234-56, {}", knownIds),
              "plain text, 1234-56, {}");
}

// The fingerprint is the property's whole notion of "project state". A change the
// fingerprint cannot see is a change the property cannot catch, so this proves it
// sees an ordinary edit, an undo, a media import and a history depth.
TEST(AgentEquivalenceFingerprint, ChangesWithEveryPartOfTheStateItClaimsToCover) {
    const std::unique_ptr<World> world = makeWorld(Sabotage::None);
    Stack                        stack(*world);

    const std::string initial = stack.fingerprint();

    Json muteArgs = Json::object();
    muteArgs.set("trackId", world->trackIds[0].toString());
    muteArgs.set("muted", true);
    ASSERT_TRUE(stack.executor().executeTool("timeline.set_track_muted", muteArgs).isOk());
    const std::string afterEdit = stack.fingerprint();
    EXPECT_NE(afterEdit, initial) << "an applied edit must move the fingerprint";

    ASSERT_TRUE(stack.executor().executeTool("edit.undo", Json::object()).isOk());
    const std::string afterUndo = stack.fingerprint();
    EXPECT_NE(afterUndo, afterEdit);
    // The project value is back, but the history depths are not, so the
    // fingerprint must still differ from the pristine one.
    EXPECT_NE(afterUndo, initial) << "the fingerprint must carry the undo/redo depths";

    Json importArgs = Json::object();
    importArgs.set("path", world->importPath.string());
    ASSERT_TRUE(stack.executor().executeTool("media.import", importArgs).isOk());
    EXPECT_NE(stack.fingerprint(), afterUndo)
        << "the fingerprint must carry the media library";
}

// Every documented phrase, deterministically, on its own pair of stacks: the
// property draws from the phrase table and over 100 cases will very likely reach
// all twelve, but "very likely" is not "always".
TEST(AgentEquivalence, EveryDocumentedPhraseAgreesAcrossTheTwoPaths) {
    const std::vector<PhrasePattern>& table = OfflineIntentInterpreter::patterns();
    ASSERT_EQ(table.size(), 12u);

    for (const PhrasePattern& pattern : table) {
        const std::unique_ptr<World> world = makeWorld(Sabotage::None);
        Stack                        agentStack(*world);
        Stack                        directStack(*world);

        std::string utterance;
        switch (pattern.match) {
            case PhraseMatch::Exact:
                utterance = pattern.canonicalUtterance();
                break;
            case PhraseMatch::Ordinal:
                utterance = pattern.canonicalUtterance(2);
                break;
            case PhraseMatch::Suffix:
                utterance = pattern.canonicalUtterance(
                    1, (pattern.toolName == "media.import" ? world->importPath
                                                           : world->exportPath)
                           .string());
                break;
        }

        const std::string              before = agentStack.fingerprint();
        const std::vector<std::string> knownIds = uuidsIn(before);
        ASSERT_EQ(before, directStack.fingerprint()) << utterance;

        const OfflineIntentInterpreter interpreter = agentStack.interpreter();
        const Result<AgentIntent>      intent = interpreter.interpret(utterance);
        ASSERT_TRUE(intent.isOk()) << utterance << ": " << intent.error().toString();

        AlwaysAuthorized  gate;
        AgentOrchestrator agent(agentStack.executor(), gate, interpreter.asInterpreter());
        const Result<AgentTurn> viaAgent = agent.sendMessage(utterance);
        const Result<Json>      viaDirect = directStack.executor().executeTool(
            intent.value().toolName, intent.value().arguments, InvocationSource::Mcp);

        ASSERT_TRUE(viaDirect.isOk())
            << utterance << ": " << viaDirect.error().toString();
        ASSERT_TRUE(viaAgent.isOk()) << utterance << ": " << viaAgent.error().toString();

        EXPECT_EQ(agentStack.undoDepth(), directStack.undoDepth()) << utterance;
        EXPECT_EQ(agentStack.redoDepth(), directStack.redoDepth()) << utterance;
        EXPECT_EQ(canonicalizeFreshIds(agentStack.fingerprint(), knownIds),
                  canonicalizeFreshIds(directStack.fingerprint(), knownIds))
            << utterance;
    }
}

// Both halves of Property 62 are reachable by construction: every sabotage mode
// really does make its phrase fail on BOTH paths, and really does leave both
// projects exactly as they were. Without this the failure arm could be silently
// empty and the property would only ever test the success arm.
TEST(AgentEquivalence, EverySabotageModeFailsBothPathsAndRollsBothBack) {
    for (const FailableTool& failable : failableTools()) {
        const std::unique_ptr<World> world = makeWorld(failable.sabotage);
        Stack                        agentStack(*world);
        Stack                        directStack(*world);

        std::vector<PhrasePattern> candidates;
        for (const PhrasePattern& pattern : OfflineIntentInterpreter::patterns()) {
            if (pattern.toolName == failable.toolName) candidates.push_back(pattern);
        }
        ASSERT_FALSE(candidates.empty()) << failable.toolName;
        const PhrasePattern& pattern = candidates.front();

        std::string utterance;
        switch (pattern.match) {
            case PhraseMatch::Exact:
                utterance = pattern.canonicalUtterance();
                break;
            case PhraseMatch::Ordinal:
                utterance = pattern.canonicalUtterance(1);
                break;
            case PhraseMatch::Suffix:
                utterance = pattern.canonicalUtterance(
                    1, (pattern.toolName == "media.import" ? world->importPath
                                                           : world->exportPath)
                           .string());
                break;
        }

        const std::string before = agentStack.fingerprint();
        ASSERT_EQ(before, directStack.fingerprint()) << utterance;

        const OfflineIntentInterpreter interpreter = agentStack.interpreter();
        const Result<AgentIntent>      intent = interpreter.interpret(utterance);
        ASSERT_TRUE(intent.isOk())
            << utterance << " (sabotage must fail at EXECUTION, not at "
               "interpretation): "
            << intent.error().toString();

        AlwaysAuthorized  gate;
        AgentOrchestrator agent(agentStack.executor(), gate, interpreter.asInterpreter());
        const Result<AgentTurn> viaAgent = agent.sendMessage(utterance);
        const Result<Json>      viaDirect = directStack.executor().executeTool(
            intent.value().toolName, intent.value().arguments, InvocationSource::Mcp);

        EXPECT_TRUE(viaAgent.isError()) << utterance;
        EXPECT_TRUE(viaDirect.isError()) << utterance;
        ASSERT_TRUE(viaAgent.isError() && viaDirect.isError()) << utterance;
        EXPECT_EQ(viaAgent.error().code(), viaDirect.error().code()) << utterance;

        // Requirement 11.5: the pre-invocation state is restored on both paths.
        EXPECT_EQ(agentStack.fingerprint(), before) << utterance;
        EXPECT_EQ(directStack.fingerprint(), before) << utterance;
    }
}

// Requirement 11.5 calls the agent path and the MCP path "the same execution
// policy". The `InvocationSource` tag is documented as affecting logging only;
// this pins that, because if it did affect the outcome the property's direct arm
// (which uses `InvocationSource::Mcp`) would not be a fair comparator.
TEST(AgentEquivalence, TheInvocationSourceTagDoesNotChangeTheOutcome) {
    const InvocationSource sources[] = {InvocationSource::Gui, InvocationSource::Mcp,
                                        InvocationSource::Agent};
    // ONE world for all three sources: a fresh world would seed different project
    // identifiers and the three texts would differ for a reason that has nothing to
    // do with the source tag.
    const std::unique_ptr<World> world = makeWorld(Sabotage::None);
    std::string                  reference;
    for (const InvocationSource source : sources) {
        Stack                          stack(*world);
        const std::vector<std::string> knownIds = uuidsIn(stack.fingerprint());

        Json arguments = Json::object();
        arguments.set("kind", "video");
        const Result<Json> executed =
            stack.executor().executeTool("timeline.add_track", arguments, source);
        ASSERT_TRUE(executed.isOk()) << invocationSourceName(source);

        const std::string canonical = canonicalizeFreshIds(stack.fingerprint(), knownIds);
        if (reference.empty()) {
            reference = canonical;
        } else {
            EXPECT_EQ(canonical, reference) << invocationSourceName(source);
        }
    }
}

}  // namespace
}  // namespace palmier::services
