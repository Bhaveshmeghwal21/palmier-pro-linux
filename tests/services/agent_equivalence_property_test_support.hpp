// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared fixtures and helpers for the agent_equivalence_property_test cases, split out so that no single file
// exceeds this repository's 1000-line ceiling.
//
// The support namespace opens INSIDE the original enclosing namespace, because these
// helpers name the types they use unqualified. `inline` and named rather than an
// anonymous namespace: an anonymous namespace in a header gives every translation unit
// its own copy, and a helper only some partitions use would warn as unused.

#ifndef PALMIER_TESTS_SERVICES_AGENT_EQUIVALENCE_PROPERTY_TEST_SUPPORT_HPP_
#define PALMIER_TESTS_SERVICES_AGENT_EQUIVALENCE_PROPERTY_TEST_SUPPORT_HPP_

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
namespace testsupport_agentequivalencepropertytest {

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
                ("palmier_agent_equiv_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
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
    [[nodiscard]] fs::path unwritablePath(std::string_view tag, std::string_view extension) const {
        return root_ / "no-such-directory" / (std::string(tag) + std::string(extension));
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// The UUID canonicalizer (see the file comment).
// ---------------------------------------------------------------------------

/// True iff `text` holds a canonical lowercase UUID at `at` (8-4-4-4-12 hex).
[[nodiscard]] inline bool uuidAt(const std::string& text, std::size_t at) {
    static constexpr std::size_t kLength = 36;
    if (at + kLength > text.size()) return false;
    static constexpr std::size_t kDashes[] = {8, 13, 18, 23};
    for (std::size_t i = 0; i < kLength; ++i) {
        const char c = text[at + i];
        const bool isDashPosition =
            (i == kDashes[0] || i == kDashes[1] || i == kDashes[2] || i == kDashes[3]);
        if (isDashPosition) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/// Every canonical UUID appearing in `text`, in order of first appearance.
[[nodiscard]] inline std::vector<std::string> uuidsIn(const std::string& text) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i + 36 <= text.size();) {
        if (uuidAt(text, i)) {
            const std::string uuid = text.substr(i, 36);
            bool seen = false;
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
[[nodiscard]] inline std::string canonicalizeFreshIds(const std::string& text,
                                                      const std::vector<std::string>& known) {
    std::map<std::string, std::string> placeholders;
    std::size_t next = 0;
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
            const auto it = placeholders.find(uuid);
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
    BogusSelection,     ///< the selected clip is not in the project
    PlayheadOutside,    ///< the playhead is past the selected clip's end
    BogusTracks,        ///< track ordinals resolve to identifiers no track has
    MissingImportFile,  ///< `import <path>` names a file that does not exist
    UnwritableExport,   ///< `export as mp4 to <path>` names a missing directory
    UnwritableSave,     ///< the document path's parent directory does not exist
};

struct World {
    ScratchDir scratch;
    Project project;
    std::vector<Uuid> trackIds;  ///< in project order; index i is ordinal i+1
    Uuid clipId;                 ///< a clip with room on either side to split
    std::int64_t clipStartNs = 0;
    std::int64_t clipEndNs = 0;

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
    Uuid absentClipId = Uuid::generateV4();
    std::vector<Uuid> absentTrackIds = {Uuid::generateV4(), Uuid::generateV4(), Uuid::generateV4()};

    /// The editor context both interpreters read. Identical on both sides by
    /// construction — it is a property of the world, not of a session — and stable
    /// across reads.
    [[nodiscard]] EditorContext context() const {
        EditorContext context;
        context.selectedClipId = (sabotage == Sabotage::BogusSelection)
                                     ? absentClipId  // no clip has this id
                                     : clipId;
        context.playheadNs = (sabotage == Sabotage::PlayheadOutside)
                                 ? clipEndNs + 1'000'000'000
                                 : clipStartNs + (clipEndNs - clipStartNs) / 2;
        context.trackIds = (sabotage == Sabotage::BogusTracks) ? absentTrackIds : trackIds;
        context.documentPath = documentPath;
        return context;
    }
};

/// Build the case's world: a project of two video tracks and one audio track,
/// with two clips on the first video track so ordinals 1..3 all resolve and the
/// selected clip has an interior playhead.
///
/// Every identifier comes from `Uuid::generateV4()`.
[[nodiscard]] inline std::unique_ptr<World> makeWorld(Sabotage sabotage) {
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
                return err<ImportedAsset>(
                    makeError(ErrorCode::NotFound, "media.import: no such file: " + path.string()));
            }
            std::map<std::string, Uuid>& ids = *world_.importIds;
            const auto it = ids.find(path.string());
            const Uuid assetId = (it == ids.end())
                                     ? ids.emplace(path.string(), Uuid::generateV4()).first->second
                                     : it->second;

            const MediaAssetRef asset(assetId, path.string());
            if (Result<void> added = session_.mediaLibrary().importAsset(asset); added.isError()) {
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
                return err<Json>(
                    makeError(ErrorCode::Io, "timeline.export: cannot write " + outputPath));
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

    [[nodiscard]] ProjectSession& session() noexcept { return session_; }
    [[nodiscard]] const ToolRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] McpToolExecutor& executor() noexcept { return *executor_; }

    /// The interpreter this stack's agent uses: the real offline interpreter over
    /// the world's context, with the network seam armed to fail the test.
    [[nodiscard]] OfflineIntentInterpreter interpreter() const {
        OfflineIntentInterpreter::Options options;
        const World* world = &world_;
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
    const World& world_;
    ProjectSession session_;
    ToolRegistry registry_;
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
    Sabotage sabotage;
};

inline const std::vector<FailableTool>& failableTools() {
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
[[nodiscard]] inline std::vector<PhrasePattern> patternsFor(std::string_view toolName) {
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
    PhrasePattern pattern;
    std::string utterance;
    bool forcedToFail = false;
};

[[nodiscard]] inline DrawnCase drawCase() {
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
inline void primeHistory(Stack& stack, const World& world, int priming) {
    if (priming == 0) return;
    Json arguments = Json::object();
    arguments.set("trackId", world.trackIds[0].toString());
    arguments.set("muted", true);
    if (!stack.executor().executeTool("timeline.set_track_muted", arguments).isOk()) return;
    if (priming == 2) {
        (void)stack.executor().executeTool("edit.undo", Json::object());
    }
}

}  // namespace testsupport_agentequivalencepropertytest
}  // namespace palmier::services

#endif  // PALMIER_TESTS_SERVICES_AGENT_EQUIVALENCE_PROPERTY_TEST_SUPPORT_HPP_
