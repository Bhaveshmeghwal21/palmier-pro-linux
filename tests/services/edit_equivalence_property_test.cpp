// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/edit_equivalence_property_test.cpp — property test for the
// UI / MCP / agent edit equivalence guarantee (task 15.4).
//
// Design property P4 (design.md "Correctness Properties"):
//
//     For any EditCommand, issuing it through the UI, an MCP tool call, or the
//     in-app agent produces the same resulting project state.
//
// This is the "single editing path" guarantee of Requirements 7.4 (an invoked
// MCP tool executes the corresponding operation on the current project), 8.1
// (the Agent_Chat operates on the current project using the SAME tools exposed
// by the MCP server), and 8.4 (an agent-directed reference resolves against the
// current project). The mechanism that makes P4 true is reuse, not
// re-implementation: all three callers converge on the same concrete
// EditCommand applied through the one TimelineEngine::apply path (design.md
// "All mutations flow through a Command object"):
//
//   * "UI" path    — construct the concrete EditCommand directly and apply it
//                    through TimelineEngine::apply (core/EditCommands.*,
//                    core/TimelineEngine.*). This is what the Qt views do.
//   * "MCP" path    — McpToolExecutor::executeTool(name, args) over the shared
//                    buildDefaultToolRegistry(session) surface
//                    (services/McpToolExecutor.*, services/ToolRegistry.*).
//   * "agent" path  — AgentOrchestrator::sendMessage(msg) with an interpreter
//                    that maps the message to the identical tool call, driven
//                    through the very same McpToolExecutor + ToolRegistry
//                    (services/AgentOrchestrator.*).
//
// Strategy. Build ONE valid seed project (a few tracks each seeded with a clip
// and a shared media asset) and copy it into THREE independent TimelineEngines —
// one per path — so all three start byte-for-byte identical. Then, for an
// arbitrary-length sequence of arbitrary generated editing operations
// (add/delete/move/trim/split/reorder/add_effect), generate ONE path-neutral
// operation per step against the (identical) current state and issue the SAME
// logical edit down all three paths. After each step, assert the three resulting
// project snapshots are equal (modulo the id renaming described below).
//
// Nondeterministic ids — the documented approach. Some edits mint fresh ids the
// three paths cannot be forced to agree on, and a naive "same clip id" scheme
// would break a multi-step sequence:
//   * add_clip — the clip id CAN be fixed, so we pre-generate it once and pass it
//     explicitly to every path (the concrete Clip.id and the tool's optional
//     "clipId" argument). The added clip is therefore identical across paths and
//     safe to target later.
//   * split_clip — SplitClipCommand mints the right-half's id internally (there
//     is no argument for it), so the three engines end up with three DIFFERENT
//     right-half ids for the same logical clip.
//   * add_effect — the add_effect tool mints the Effect id internally, so the
//     three paths produce three DIFFERENT effect ids.
// Two techniques together make the equivalence exact and robust across steps:
//   (1) Structural targeting. An operation never hard-codes a clip id from one
//       engine. It records the target as a flat clip index into the (identical)
//       structure, and each path resolves that index against its OWN engine
//       snapshot to obtain that engine's actual clip id. So even after a split
//       leaves each engine with a different right-half id, a later edit still
//       addresses "the same logical clip" in every engine.
//   (2) Comparison modulo a consistent id renaming. Each project is canonicalized
//       by walking it in a fixed order and replacing every UUID with a sequential
//       token assigned at first occurrence. Two projects are equal iff there is a
//       consistent bijection between their ids preserving the entire structure
//       and every non-id attribute (track kind/flags, clip timing/source range/
//       gain/opacity, effect type+parameters, transition kind+duration, order).
//       The freshly minted split/effect ids are absorbed by the renaming; all
//       shared ids (tracks, assets, seed clips, the pre-generated add id) line up
//       positionally.

#include "services/AgentOrchestrator.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/CommandResult.hpp"
#include "core/Duration.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/McpToolExecutor.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolRegistry.hpp"

namespace palmier::services {
namespace {

constexpr Duration ms(std::int64_t v) { return Duration::fromMilliseconds(v); }

// ---------------------------------------------------------------------------
// Canonicalization — "equal up to a consistent id renaming".
// ---------------------------------------------------------------------------
std::string canonicalize(const Project& p) {
    std::unordered_map<std::string, int> idMap;
    auto tok = [&idMap](const Uuid& u) -> int {
        const std::string key = u.toString();
        auto it = idMap.find(key);
        if (it != idMap.end()) return it->second;
        const int next = static_cast<int>(idMap.size());
        idMap.emplace(key, next);
        return next;
    };

    std::ostringstream os;
    os << "P<fps=" << p.timelineFps.numerator() << '/' << p.timelineFps.denominator()
       << ";canvas=" << p.canvas.width << 'x' << p.canvas.height
       << ";tracks=" << p.tracks.size() << '>';
    for (const Track& t : p.tracks) {
        os << "\n T#" << tok(t.id) << "[kind=" << static_cast<int>(t.kind)
           << ";muted=" << (t.muted ? 1 : 0) << ";locked=" << (t.locked ? 1 : 0)
           << ";clips=" << t.clips.size() << ']';
        for (const Clip& c : t.clips) {
            os << "\n  C#" << tok(c.id) << "{asset=" << tok(c.assetRef.assetId)
               << ";start=" << c.timelineStart.nanoseconds()
               << ";in=" << c.sourceIn.nanoseconds()
               << ";out=" << c.sourceOut.nanoseconds()
               << ";gain=" << c.gain << ";opacity=" << c.opacity
               << ";effects=" << c.effects.size() << '}';
            for (const Effect& e : c.effects) {
                os << "\n   E#" << tok(e.id) << "(type=" << static_cast<int>(e.type) << ";";
                for (const auto& [k, v] : e.parameters) os << k << '=' << v << ',';
                os << ')';
            }
            if (c.transitionIn.has_value()) {
                os << "\n   X#" << tok(c.transitionIn->id)
                   << "(kind=" << static_cast<int>(c.transitionIn->kind)
                   << ";dur=" << c.transitionIn->duration.nanoseconds() << ')';
            }
        }
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// Snapshot helpers.
// ---------------------------------------------------------------------------
struct ClipLocator {
    Uuid   trackId;
    ClipId clipId;
    Clip   clip;
};

std::vector<ClipLocator> allClips(const Project& p) {
    std::vector<ClipLocator> out;
    for (const Track& t : p.tracks) {
        for (const Clip& c : t.clips) out.push_back(ClipLocator{t.id, c.id, c});
    }
    return out;
}

Duration trackEnd(const Project& p, const Uuid& trackId) {
    Duration end = Duration::zero();
    for (const Track& t : p.tracks) {
        if (t.id != trackId) continue;
        for (const Clip& c : t.clips) {
            if (c.timelineEnd() > end) end = c.timelineEnd();
        }
    }
    return end;
}

// A valid seed project: `videoTracks` video lanes + one audio lane, each seeded
// with a single clip referencing a shared media asset (so add_clip has a
// resolvable asset to point at).
Project makeSeedProject(int videoTracks) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "edit-equivalence";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    MediaAssetRef asset(Uuid::generateV4(), "mem://asset");
    project.assets.push_back(asset);

    const int total = videoTracks + 1;  // + one audio lane
    for (int i = 0; i < total; ++i) {
        Track track;
        track.id = Uuid::generateV4();
        track.kind = (i < videoTracks) ? TrackKind::Video : TrackKind::Audio;
        Clip clip;
        clip.id = Uuid::generateV4();
        clip.assetRef = asset;
        clip.timelineStart = ms(0);
        clip.sourceIn = ms(0);
        clip.sourceOut = ms(1000);
        track.clips.push_back(std::move(clip));
        project.tracks.push_back(std::move(track));
    }
    return project;
}

// ---------------------------------------------------------------------------
// A path-neutral description of one generated edit. Absolute geometry (already
// resolved from the identical shared state) is stored directly; the target clip
// is identified by its flat index so each path resolves it against its OWN
// engine snapshot — which is what makes a split's per-engine right-half id, and
// any later edit of it, line up across all three paths.
// ---------------------------------------------------------------------------
struct OpSpec {
    int          kind = 0;
    int          trackIndex = 0;        // add_clip / reorder_clips
    std::int64_t startNs = 0;           // add_clip
    std::int64_t lenNs = 0;             // add_clip
    Uuid         addClipId;             // add_clip (pre-generated, shared)
    int          clipFlat = 0;          // delete/move/trim/split/add_effect target
    std::int64_t boundaryNs = 0;        // trim_clip
    std::int64_t sourceDurationNs = 0;  // trim_clip
    std::int64_t newStartNs = 0;        // move_clip
    std::int64_t playheadNs = 0;        // split_clip
    double       amount = 0.0;          // add_effect
    bool         splitAsEffect = false; // split fallback when the clip is < 2 frames
};

// Generate the next operation against the (identical) shared state `s`.
OpSpec generateSpec(const Project& s, const FrameRate fps) {
    const std::vector<ClipLocator> clips = allClips(s);
    OpSpec spec;
    spec.sourceDurationNs = ms(600000).nanoseconds();  // 10-minute virtual source
    // When there are no clips only add_clip applies.
    spec.kind = clips.empty() ? 0 : *rc::gen::inRange(0, 7);

    switch (spec.kind) {
        case 0: {  // add_clip — appended past the track end so it never overlaps.
            spec.trackIndex = *rc::gen::inRange(0, static_cast<int>(s.tracks.size()));
            const Uuid trackId = s.tracks[static_cast<std::size_t>(spec.trackIndex)].id;
            const Duration gap = ms(*rc::gen::inRange(0, 500));
            spec.startNs = (trackEnd(s, trackId) + gap).nanoseconds();
            spec.lenNs = fps.durationForFrames(*rc::gen::inRange(1, 60)).nanoseconds();
            spec.addClipId = Uuid::generateV4();  // fixed + shared across all paths
            break;
        }
        case 1:      // delete_clip
        case 2:      // move_clip
        case 6: {    // add_effect
            spec.clipFlat = *rc::gen::inRange(0, static_cast<int>(clips.size()));
            if (spec.kind == 2) {
                const Duration gap = ms(*rc::gen::inRange(0, 500));
                spec.newStartNs =
                    (trackEnd(s, clips[static_cast<std::size_t>(spec.clipFlat)].trackId) + gap)
                        .nanoseconds();
            } else if (spec.kind == 6) {
                spec.amount = static_cast<double>(*rc::gen::inRange(0, 100)) / 100.0;
            }
            break;
        }
        case 3: {  // trim_clip (End) — shrink to an interior boundary (never overlaps).
            spec.clipFlat = *rc::gen::inRange(0, static_cast<int>(clips.size()));
            const Clip& c = clips[static_cast<std::size_t>(spec.clipFlat)].clip;
            const std::int64_t curFrames = fps.framesForDuration(c.duration());
            const std::int64_t hi = curFrames < 1 ? 1 : curFrames;
            const std::int64_t frames =
                *rc::gen::inRange(static_cast<int>(1), static_cast<int>(hi) + 1);
            spec.boundaryNs = (c.sourceIn + fps.durationForFrames(frames)).nanoseconds();
            break;
        }
        case 4: {  // split_clip — at an interior playhead (mints a fresh right id).
            spec.clipFlat = *rc::gen::inRange(0, static_cast<int>(clips.size()));
            const Clip& c = clips[static_cast<std::size_t>(spec.clipFlat)].clip;
            const std::int64_t durFrames = fps.framesForDuration(c.duration());
            if (durFrames >= 2) {
                const std::int64_t at =
                    *rc::gen::inRange(static_cast<int>(1), static_cast<int>(durFrames));
                spec.playheadNs = (c.timelineStart + fps.durationForFrames(at)).nanoseconds();
            } else {
                // Fall back to a guaranteed-applicable add_effect.
                spec.splitAsEffect = true;
                spec.amount = static_cast<double>(*rc::gen::inRange(0, 100)) / 100.0;
            }
            break;
        }
        default: {  // 5 — reorder_clips (reverse a track's clip order).
            spec.trackIndex = *rc::gen::inRange(0, static_cast<int>(s.tracks.size()));
            break;
        }
    }
    return spec;
}

std::string toolNameFor(const OpSpec& spec) {
    switch (spec.kind) {
        case 0:  return "timeline.add_clip";
        case 1:  return "timeline.delete_clip";
        case 2:  return "timeline.move_clip";
        case 3:  return "timeline.trim_clip";
        case 4:  return spec.splitAsEffect ? "timeline.add_effect" : "timeline.split_clip";
        case 5:  return "timeline.reorder_clips";
        default: return "timeline.add_effect";
    }
}

// The concrete EditCommand for the UI path, resolved against `snap` (the UI
// engine's own snapshot, so the target clip id is that engine's id).
std::unique_ptr<EditCommand> buildCommand(const OpSpec& spec, const Project& snap,
                                          const FrameRate fps) {
    const std::vector<ClipLocator> clips = allClips(snap);
    const auto target = [&] { return clips[static_cast<std::size_t>(spec.clipFlat)].clipId; };
    switch (spec.kind) {
        case 0: {
            Clip clip;
            clip.id = spec.addClipId;
            clip.assetRef = MediaAssetRef(snap.assets.front().assetId, "");
            clip.timelineStart = Duration::fromNanoseconds(spec.startNs);
            clip.sourceIn = ms(0);
            clip.sourceOut = Duration::fromNanoseconds(spec.lenNs);
            return std::make_unique<AddClipCommand>(
                snap.tracks[static_cast<std::size_t>(spec.trackIndex)].id, std::move(clip));
        }
        case 1:
            return std::make_unique<DeleteClipCommand>(target());
        case 2:
            return std::make_unique<MoveClipCommand>(
                target(), Duration::fromNanoseconds(spec.newStartNs));
        case 3:
            return std::make_unique<TrimClipCommand>(
                target(), TrimClipCommand::Edge::End,
                Duration::fromNanoseconds(spec.boundaryNs), fps,
                Duration::fromNanoseconds(spec.sourceDurationNs));
        case 4:
            if (spec.splitAsEffect) {
                return std::make_unique<AddEffectCommand>(
                    target(),
                    Effect{Uuid::generateV4(), EffectType::Brightness, {{"amount", spec.amount}}});
            }
            return std::make_unique<SplitClipCommand>(
                target(), Duration::fromNanoseconds(spec.playheadNs));
        case 5: {
            const Track& track = snap.tracks[static_cast<std::size_t>(spec.trackIndex)];
            std::vector<ClipId> order;
            for (const Clip& c : track.clips) order.push_back(c.id);
            std::reverse(order.begin(), order.end());
            return std::make_unique<ReorderClipsCommand>(track.id, std::move(order));
        }
        default:
            return std::make_unique<AddEffectCommand>(
                target(),
                Effect{Uuid::generateV4(), EffectType::Brightness, {{"amount", spec.amount}}});
    }
}

// The equivalent MCP/agent tool arguments, resolved against `snap` (the calling
// path's own engine snapshot, so the target clip id is that engine's id).
Json buildArgs(const OpSpec& spec, const Project& snap) {
    const std::vector<ClipLocator> clips = allClips(snap);
    const auto targetStr = [&] {
        return clips[static_cast<std::size_t>(spec.clipFlat)].clipId.toString();
    };
    Json args = Json::object();
    switch (spec.kind) {
        case 0:
            args.set("trackId", snap.tracks[static_cast<std::size_t>(spec.trackIndex)].id.toString());
            args.set("assetId", snap.assets.front().assetId.toString());
            args.set("clipId", spec.addClipId.toString());
            args.set("timelineStartNs", spec.startNs);
            args.set("sourceInNs", static_cast<std::int64_t>(0));
            args.set("sourceOutNs", spec.lenNs);
            break;
        case 1:
            args.set("clipId", targetStr());
            break;
        case 2:
            args.set("clipId", targetStr());
            args.set("timelineStartNs", spec.newStartNs);
            break;
        case 3:
            args.set("clipId", targetStr());
            args.set("edge", "end");
            args.set("boundaryNs", spec.boundaryNs);
            args.set("sourceDurationNs", spec.sourceDurationNs);
            break;
        case 4:
            if (spec.splitAsEffect) {
                args.set("clipId", targetStr());
                args.set("type", "brightness");
                Json params = Json::object();
                params.set("amount", spec.amount);
                args.set("parameters", std::move(params));
            } else {
                args.set("clipId", targetStr());
                args.set("playheadNs", spec.playheadNs);
            }
            break;
        case 5: {
            const Track& track = snap.tracks[static_cast<std::size_t>(spec.trackIndex)];
            std::vector<ClipId> order;
            for (const Clip& c : track.clips) order.push_back(c.id);
            std::reverse(order.begin(), order.end());
            Json orderJson = Json::array();
            for (const ClipId& id : order) orderJson.push_back(Json(id.toString()));
            args.set("trackId", track.id.toString());
            args.set("order", std::move(orderJson));
            break;
        }
        default:
            args.set("clipId", targetStr());
            args.set("type", "brightness");
            Json params = Json::object();
            params.set("amount", spec.amount);
            args.set("parameters", std::move(params));
            break;
    }
    return args;
}

// A gate that always authorizes: Req 8.5's auth gating is covered by the
// orchestrator's own unit tests; here we exercise the shared *edit* path, so
// every send is authorized.
class AlwaysAllowGate : public IAgentAuthGate {
public:
    [[nodiscard]] Result<void> authorize() const override { return ok(); }
};

// ---------------------------------------------------------------------------
// Property P4 — UI / MCP / agent edit equivalence.
// ---------------------------------------------------------------------------

// Feature: palmier-pro-linux, Property 4: UI / MCP / agent edit equivalence —
// issuing any EditCommand via the UI, an MCP tool call, or the in-app agent
// produces the same resulting project state.
// Validates: Requirements 7.4, 8.1, 8.4
RC_GTEST_PROP(EditEquivalenceProperties,
              UiMcpAgentProduceIdenticalState,
              ()) {
    const int videoTracks = *rc::gen::inRange(1, 4);   // 1..3 video lanes
    const int numCommands = *rc::gen::inRange(1, 25);  // arbitrary edit sequence
    const FrameRate fps = FrameRate::fps30();

    // One seed project, copied into three independent engines — one per path. The
    // two tool-driven paths reach their engine through a ProjectSession, which is
    // how the surface is wired since task 3.4 (design.md D1); the UI path still
    // applies EditCommands to a bare engine, which is exactly the comparison.
    const Project seed = makeSeedProject(videoTracks);
    TimelineEngine uiEngine(seed);
    ProjectSession mcpSession;
    (void)mcpSession.engine().reset(seed);
    TimelineEngine& mcpEngine = mcpSession.engine();
    ProjectSession agentSession;
    (void)agentSession.engine().reset(seed);
    TimelineEngine& agentEngine = agentSession.engine();

    // "MCP" path: the shared executor over the default tool surface.
    ToolRegistry mcpRegistry = buildDefaultToolRegistry(mcpSession);
    McpToolExecutor mcpExecutor(mcpRegistry, &mcpSession);

    // "agent" path: the SAME executor + registry, driven by the orchestrator.
    ToolRegistry agentRegistry = buildDefaultToolRegistry(agentSession);
    McpToolExecutor agentExecutor(agentRegistry, &agentSession);
    AlwaysAllowGate gate;
    auto intentSlot = std::make_shared<AgentIntent>();
    IntentInterpreter interpreter =
        [intentSlot](std::string_view) -> Result<AgentIntent> { return *intentSlot; };
    AgentOrchestrator agent(agentExecutor, gate, interpreter);

    // All three engines start identical.
    RC_ASSERT(canonicalize(uiEngine.snapshot()) == canonicalize(mcpEngine.snapshot()));
    RC_ASSERT(canonicalize(uiEngine.snapshot()) == canonicalize(agentEngine.snapshot()));

    for (int step = 0; step < numCommands; ++step) {
        // Generate one path-neutral operation from the (identical) current state.
        const OpSpec spec = generateSpec(uiEngine.snapshot(), fps);
        const std::string toolName = toolNameFor(spec);

        // UI path: apply the concrete EditCommand directly.
        (void)uiEngine.apply(buildCommand(spec, uiEngine.snapshot(), fps));

        // MCP path: run the equivalent tool call through the executor, resolving
        // the target against the MCP engine's own snapshot.
        (void)mcpExecutor.executeTool(toolName, buildArgs(spec, mcpEngine.snapshot()));

        // Agent path: route the identical tool call through the orchestrator,
        // which drives the same shared executor + registry, resolving the target
        // against the agent engine's own snapshot.
        *intentSlot = AgentIntent{toolName, buildArgs(spec, agentEngine.snapshot())};
        (void)agent.sendMessage("apply");

        // P4: regardless of whether the edit applied, was a no-op, or failed and
        // was rolled back, all three paths must land on the SAME project state
        // (modulo a consistent id renaming for freshly minted split/effect ids).
        const std::string uiState = canonicalize(uiEngine.snapshot());
        const std::string mcpState = canonicalize(mcpEngine.snapshot());
        const std::string agentState = canonicalize(agentEngine.snapshot());
        RC_ASSERT(uiState == mcpState);
        RC_ASSERT(uiState == agentState);
    }
}

}  // namespace
}  // namespace palmier::services
