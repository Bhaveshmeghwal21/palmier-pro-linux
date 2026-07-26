// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_session_roundtrip_property_test.cpp — the persistence
// properties of services::ProjectSession (task 2.3).
//
// Three properties live here, all three about what the session guarantees around
// a save:
//
//   Property 16 — saving to a file and opening it again yields the same project.
//   Property 17 — saving a project that was itself loaded is idempotent.
//   Property 19 — a successful save leaves the session unmodified, and the very
//                 next tool-applied edit makes it modified again.
//
// How a generated project reaches the session
// -------------------------------------------
// The session deliberately offers no "install this project value" entry point:
// its only two commit paths are createProject() and openProject() (design.md D1).
// A generated project therefore enters through openProject() of a seed document,
// which is exactly the load path the Tool_Surface's `project.open` uses; the
// generated shapes stay inside the bounds Requirement 4.7 states for projects
// "constructible through the Tool_Surface" (1-20 tracks, 0-200 clips per track,
// 0-10 effects per clip, 0-50 transitions, 0-200 registered assets, names of
// 0-255 characters). Every generated project is a LEGAL project: a supported
// schema version, a positive frame rate and canvas, unique non-nil asset ids,
// clips whose assetRef resolves in the asset table, and each track's clips
// ordered by timelineStart and overlapping only inside an incoming clip's
// explicit transition region — the exact conditions openProject() enforces
// through validateProject() and TimelineEngine::reset().
//
// Property 19's "editing tool invocation" is issued as the concrete EditCommand
// the corresponding tool builds, applied through ProjectSession::engine(), which
// is the single path every GUI, MCP and agent tool invocation funnels into (the
// tool surface is retargeted at the session in task 3.4). What the property is
// about is the session's dirty-flag bookkeeping, and that bookkeeping is driven
// by the engine's ChangeSet broadcast, so this is the behaviour under test.
//
// Cost control: the stated bounds allow ~4000 clips in one project, which no
// 100-case run needs to pay for on every case. The generator therefore draws a
// shape profile: most cases are small, while a deliberate minority hit the
// extremes the design calls for — 20 tracks, a 200-clip track, 200 registered
// assets, 50 transitions covering all five transition kinds, 10-effect clips,
// and empty / 255-character / non-ASCII names.
//
// _Requirements: 4.6, 4.7, 4.8_

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/EditCommand.hpp"
#include "core/EditCommands.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scratch files. Each generated case writes its documents under the OS temp
// directory and removes them when the case ends, so a 100-case run leaves
// nothing behind.
// ---------------------------------------------------------------------------

class ScratchPaths {
public:
    ~ScratchPaths() {
        for (const fs::path& path : paths_) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }

    [[nodiscard]] const fs::path& next(const char* tag) {
        static std::atomic<std::uint64_t> counter{0};
        paths_.push_back(fs::temp_directory_path() /
                         ("palmier_session_prop_" + std::string(tag) + "_" +
                          std::to_string(counter.fetch_add(1)) + ".palmier"));
        return paths_.back();
    }

private:
    std::vector<fs::path> paths_;
};

// ---------------------------------------------------------------------------
// Generators. Imperative style (RapidCheck's `*gen` draw operator) so each
// generated project shrinks and replays deterministically.
// ---------------------------------------------------------------------------

/// The shape envelope one generated project is drawn inside. `clipBudget` caps
/// the TOTAL clip count so a case that asks for a 200-clip track does not also
/// pay for twenty of them.
struct Shape {
    int maxTracks = 20;          ///< Requirement 4.7 bound: 1-20.
    int maxClipsPerTrack = 200;  ///< Requirement 4.7 bound: 0-200.
    int clipBudget = 300;        ///< Total clips across all tracks.
    int maxAssets = 200;         ///< Requirement 4.7 bound: 0-200.
    int maxTransitions = 50;     ///< Requirement 4.7 bound: 0-50.
    int maxEffectsPerClip = 10;  ///< Requirement 4.7 bound: 0-10.
};

/// Draw the profile of this case: mostly small projects, with a deliberate
/// minority at the upper end of each stated bound so the extremes are covered.
Shape drawShape() {
    Shape shape;
    switch (*rc::gen::inRange(0, 6)) {
        case 0: // Maximal track count.
            shape.maxTracks = 20;
            shape.maxClipsPerTrack = 4;
            shape.clipBudget = 80;
            break;
        case 1: // A single maximal-length track.
            shape.maxTracks = 2;
            shape.maxClipsPerTrack = 200;
            shape.clipBudget = 220;
            break;
        case 2: // Maximal asset table.
            shape.maxTracks = 3;
            shape.maxClipsPerTrack = 8;
            shape.clipBudget = 24;
            shape.maxAssets = 200;
            break;
        default: // The common, small case.
            shape.maxTracks = 5;
            shape.maxClipsPerTrack = 12;
            shape.clipBudget = 40;
            shape.maxAssets = 6;
            break;
    }
    return shape;
}

/// A UUID that is never nil (the version/variant bits are forced), so generated
/// asset ids are always importable into the session's media library.
Uuid drawUuid() {
    Uuid::Bytes bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(*rc::gen::inRange<int>(0, 256));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    return Uuid{bytes};
}

std::string drawAsciiText(int length) {
    static const std::string alphabet = "abcdeXYZ 0189_-.'\"\\/";
    std::string text;
    text.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        text.push_back(
            alphabet[*rc::gen::inRange<std::size_t>(0, alphabet.size())]);
    }
    return text;
}

/// Project, track and asset-path names across the whole 0-255 character band,
/// including the empty name, a maximal-length name and non-ASCII text.
std::string drawName() {
    switch (*rc::gen::inRange(0, 5)) {
        case 0:
            return std::string{}; // 0 characters
        case 1:
            return drawAsciiText(*rc::gen::inRange(1, 41));
        case 2:
            return drawAsciiText(255); // maximal length
        case 3: {
            // Non-ASCII, including multi-byte code points and an emoji.
            static const std::vector<std::string> pieces = {
                "é", "ü", "日本語", "Привет", "العربية", "🎬", "ñ", "λ"};
            std::string text;
            const int pieceCount = *rc::gen::inRange(1, 6);
            for (int i = 0; i < pieceCount; ++i) {
                text += pieces[*rc::gen::inRange<std::size_t>(0, pieces.size())];
            }
            return text;
        }
        default:
            return drawAsciiText(*rc::gen::inRange(0, 256));
    }
}

MediaAssetRef drawAssetRef() {
    MediaAssetRef ref;
    ref.assetId = drawUuid();
    ref.sourcePath = "/media/" + drawName();
    return ref;
}

Effect drawEffect() {
    Effect effect;
    effect.id = drawUuid();
    effect.type = *rc::gen::element<EffectType>(
        EffectType::Brightness, EffectType::Contrast, EffectType::Blur,
        EffectType::CropTransform, EffectType::ColorGrade, EffectType::InvertColors,
        EffectType::Custom);
    static const std::vector<std::string> paramNames = {"amount", "radius", "gamma",
                                                        "lift", "é-scale"};
    const int paramCount = *rc::gen::inRange(0, 3);
    for (int i = 0; i < paramCount; ++i) {
        const std::string& name = paramNames[*rc::gen::inRange<std::size_t>(0, paramNames.size())];
        // Built from an integer draw so the double is finite and recovers exactly
        // through the store's shortest-form numeric encoding.
        effect.parameters[name] =
            static_cast<double>(*rc::gen::inRange(-100'000, 100'001)) / 1'000.0;
    }
    return effect;
}

/// The number of effects on one clip, covering both stated extremes: a
/// zero-effect clip and a maximal (10-effect) clip.
int drawEffectCount(int maxEffects) {
    switch (*rc::gen::inRange(0, 4)) {
        case 0:
            return 0;
        case 1:
            return maxEffects;
        default:
            return *rc::gen::inRange(0, maxEffects + 1);
    }
}

/// All five transition kinds, cycled from a drawn offset so a case carrying five
/// or more transitions exhibits every kind.
TransitionKind transitionKindAt(int index, int offset) {
    static const TransitionKind kinds[] = {TransitionKind::Crossfade, TransitionKind::DipToColor,
                                           TransitionKind::Wipe, TransitionKind::Slide,
                                           TransitionKind::Fade};
    return kinds[static_cast<std::size_t>((index + offset) % 5)];
}

/// Draw one project. Clips are laid out along each track so the result satisfies
/// the timeline invariants: ordered by timelineStart, and overlapping the
/// preceding clip only within the incoming clip's transition region.
Project drawProject(const Shape& shape) {
    Project project;
    project.id = drawUuid();
    project.name = drawName();
    project.version = SchemaVersion::current();

    // A valid, positive rational frame rate inside the tool surface's 1-240 band.
    const std::int64_t den = *rc::gen::inRange<std::int64_t>(1, 1'002);
    const std::int64_t num = *rc::gen::inRange<std::int64_t>(1, 240 * den + 1);
    project.timelineFps = FrameRate{num, den};

    project.canvas = Resolution{static_cast<std::uint32_t>(*rc::gen::inRange(16, 7'681)),
                                static_cast<std::uint32_t>(*rc::gen::inRange(16, 4'321))};

    project.colorSpace = *rc::gen::element<ColorSpace>(
        ColorSpace::Srgb, ColorSpace::Rec709, ColorSpace::Rec2020, ColorSpace::Rec2100Pq,
        ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3, ColorSpace::LinearSrgb);

    const int assetCount = *rc::gen::inRange(1, shape.maxAssets + 1);
    project.assets.reserve(static_cast<std::size_t>(assetCount));
    for (int i = 0; i < assetCount; ++i) {
        project.assets.push_back(drawAssetRef());
    }

    int transitionsRemaining = *rc::gen::inRange(0, shape.maxTransitions + 1);
    const int transitionKindOffset = *rc::gen::inRange(0, 5);
    int transitionIndex = 0;
    int clipsRemaining = shape.clipBudget;

    const int trackCount = *rc::gen::inRange(1, shape.maxTracks + 1);
    project.tracks.reserve(static_cast<std::size_t>(trackCount));
    for (int t = 0; t < trackCount; ++t) {
        Track track;
        track.id = drawUuid();
        track.kind = *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);
        track.name = drawName();
        track.muted = *rc::gen::arbitrary<bool>();
        track.locked = *rc::gen::arbitrary<bool>();

        const int wanted = *rc::gen::inRange(0, shape.maxClipsPerTrack + 1);
        const int clipCount = wanted < clipsRemaining ? wanted : (clipsRemaining > 0 ? clipsRemaining : 0);
        clipsRemaining -= clipCount;

        Duration cursor = Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 5'001));
        track.clips.reserve(static_cast<std::size_t>(clipCount));
        for (int c = 0; c < clipCount; ++c) {
            Clip clip;
            clip.id = drawUuid();
            clip.assetRef = project.assets[*rc::gen::inRange<std::size_t>(0, project.assets.size())];

            const std::int64_t sourceInMs = *rc::gen::inRange<std::int64_t>(0, 60'001);
            const std::int64_t lengthMs = *rc::gen::inRange<std::int64_t>(20, 10'001);
            clip.sourceIn = Duration::fromMilliseconds(sourceInMs);
            clip.sourceOut = Duration::fromMilliseconds(sourceInMs + lengthMs);

            if (transitionsRemaining > 0 && *rc::gen::arbitrary<bool>()) {
                --transitionsRemaining;
                Transition transition;
                transition.id = drawUuid();
                transition.kind = transitionKindAt(transitionIndex++, transitionKindOffset);
                transition.duration =
                    Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 2'001));
                clip.transitionIn = transition;
            }

            // Position: after the previous clip, optionally overlapping it inside
            // the incoming transition region (the one legal overlap).
            Duration start = cursor + Duration::fromMilliseconds(
                                          *rc::gen::inRange<std::int64_t>(0, 1'001));
            if (c > 0 && clip.transitionIn.has_value() && *rc::gen::arbitrary<bool>()) {
                const Duration previous = track.clips.back().duration();
                std::int64_t overlapMs = clip.transitionIn->duration.milliseconds();
                const std::int64_t previousMs = previous.milliseconds();
                const std::int64_t currentMs = lengthMs;
                if (overlapMs > previousMs - 1) {
                    overlapMs = previousMs - 1;
                }
                if (overlapMs > currentMs - 1) {
                    overlapMs = currentMs - 1;
                }
                if (overlapMs > 0) {
                    start = cursor - Duration::fromMilliseconds(overlapMs);
                }
            }
            clip.timelineStart = start;
            cursor = clip.timelineEnd();

            const int effectCount = drawEffectCount(shape.maxEffectsPerClip);
            clip.effects.reserve(static_cast<std::size_t>(effectCount));
            for (int e = 0; e < effectCount; ++e) {
                clip.effects.push_back(drawEffect());
            }

            // gain >= 0 and opacity in [0, 1], both from integer draws so the
            // resulting doubles round-trip exactly.
            clip.gain = static_cast<double>(*rc::gen::inRange(0, 4'001)) / 1'000.0;
            clip.opacity = static_cast<double>(*rc::gen::inRange(0, 1'001)) / 1'000.0;

            track.clips.push_back(std::move(clip));
        }

        project.tracks.push_back(std::move(track));
    }

    return project;
}

// ---------------------------------------------------------------------------
// Equality: exactly the aspects Requirement 4.7 enumerates — identifier, name,
// frame rate, canvas, colour space, track order, clip order, clip source ranges,
// effects, transitions and asset references.
// ---------------------------------------------------------------------------

void assertSameClip(const Clip& a, const Clip& b) {
    RC_ASSERT(a.id == b.id);
    RC_ASSERT(a.assetRef.assetId == b.assetRef.assetId);
    RC_ASSERT(a.assetRef.sourcePath == b.assetRef.sourcePath);
    RC_ASSERT(a.timelineStart.ticks() == b.timelineStart.ticks());
    RC_ASSERT(a.sourceIn.ticks() == b.sourceIn.ticks());
    RC_ASSERT(a.sourceOut.ticks() == b.sourceOut.ticks());
    RC_ASSERT(a.gain == b.gain);
    RC_ASSERT(a.opacity == b.opacity);

    RC_ASSERT(a.effects.size() == b.effects.size());
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        RC_ASSERT(a.effects[i].id == b.effects[i].id);
        RC_ASSERT(a.effects[i].type == b.effects[i].type);
        RC_ASSERT(a.effects[i].parameters == b.effects[i].parameters);
    }

    RC_ASSERT(a.transitionIn.has_value() == b.transitionIn.has_value());
    if (a.transitionIn.has_value()) {
        RC_ASSERT(a.transitionIn->id == b.transitionIn->id);
        RC_ASSERT(a.transitionIn->kind == b.transitionIn->kind);
        RC_ASSERT(a.transitionIn->duration.ticks() == b.transitionIn->duration.ticks());
    }
}

void assertSameProject(const Project& a, const Project& b) {
    RC_ASSERT(a.id == b.id);
    RC_ASSERT(a.name == b.name);
    RC_ASSERT(a.timelineFps == b.timelineFps);
    RC_ASSERT(a.canvas == b.canvas);
    RC_ASSERT(a.colorSpace == b.colorSpace);

    RC_ASSERT(a.assets.size() == b.assets.size());
    for (std::size_t i = 0; i < a.assets.size(); ++i) {
        RC_ASSERT(a.assets[i].assetId == b.assets[i].assetId);
        RC_ASSERT(a.assets[i].sourcePath == b.assets[i].sourcePath);
    }

    RC_ASSERT(a.tracks.size() == b.tracks.size());
    for (std::size_t t = 0; t < a.tracks.size(); ++t) {
        const Track& ta = a.tracks[t];
        const Track& tb = b.tracks[t];
        RC_ASSERT(ta.id == tb.id);
        RC_ASSERT(ta.kind == tb.kind);
        RC_ASSERT(ta.name == tb.name);
        RC_ASSERT(ta.muted == tb.muted);
        RC_ASSERT(ta.locked == tb.locked);
        RC_ASSERT(ta.clips.size() == tb.clips.size());
        for (std::size_t c = 0; c < ta.clips.size(); ++c) {
            assertSameClip(ta.clips[c], tb.clips[c]);
        }
    }
}

// ---------------------------------------------------------------------------
// Session helpers.
// ---------------------------------------------------------------------------

/// Make `project` the session's current project the way the Tool_Surface does:
/// through a `.palmier` document and ProjectSession::openProject.
bool openGeneratedProject(ProjectSession& session, const Project& project,
                          const fs::path& seedPath) {
    if (saveProjectToFile(project, seedPath).isError()) {
        return false;
    }
    return session.openProject(seedPath).isOk();
}

/// Save the session's current project to `path` and apply the completion on this
/// thread. Returns the delivered completion, or std::nullopt if none arrived.
std::optional<ProjectSession::SaveCompletionInfo> saveAndSettle(ProjectSession& session,
                                                               const fs::path& path) {
    std::optional<ProjectSession::SaveCompletionInfo> info;
    if (session
            .requestSave(path,
                         [&info](const ProjectSession::SaveCompletionInfo& i) { info = i; })
            .isError()) {
        return std::nullopt;
    }
    (void)session.awaitSaveCompletions();
    return info;
}

/// One editing tool invocation, expressed as the core EditCommand the
/// corresponding tool builds. Only invocations that are valid for the given
/// project are offered, so a drawn invocation always succeeds.
struct EditInvocation {
    std::string                  toolName;
    std::unique_ptr<EditCommand> command;
};

/// Every (track index, clip index) pair in the project, so a clip-targeting
/// invocation can pick a real clip.
std::vector<std::pair<std::size_t, std::size_t>> clipAddresses(const Project& project) {
    std::vector<std::pair<std::size_t, std::size_t>> addresses;
    for (std::size_t t = 0; t < project.tracks.size(); ++t) {
        for (std::size_t c = 0; c < project.tracks[t].clips.size(); ++c) {
            addresses.emplace_back(t, c);
        }
    }
    return addresses;
}

Duration trackEnd(const Track& track) {
    Duration end = Duration::zero();
    for (const Clip& clip : track.clips) {
        if (clip.timelineEnd() > end) {
            end = clip.timelineEnd();
        }
    }
    return end;
}

EditInvocation drawEditInvocation(const Project& project) {
    const std::vector<std::pair<std::size_t, std::size_t>> clips = clipAddresses(project);

    // `timeline.add_track` and `timeline.add_clip` need no existing clip; the
    // rest target one, so they only join the menu when the project has clips.
    enum Kind { AddTrack, AddClip, AddEffect, AddTransition, DeleteClip, MoveClip, SplitClip };
    std::vector<Kind> menu = {AddTrack, AddClip};
    if (!clips.empty()) {
        menu.insert(menu.end(), {AddEffect, AddTransition, DeleteClip, MoveClip, SplitClip});
    }
    const Kind kind = menu[*rc::gen::inRange<std::size_t>(0, menu.size())];

    switch (kind) {
        case AddTrack: {
            const TrackKind trackKind =
                *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);
            return {"timeline.add_track", std::make_unique<AddTrackCommand>(trackKind)};
        }
        case AddClip: {
            const std::size_t t = *rc::gen::inRange<std::size_t>(0, project.tracks.size());
            const Track&      track = project.tracks[t];
            Clip              clip;
            clip.id = drawUuid();
            clip.assetRef = project.assets.front();
            // Placed clear of everything already on the track, so the placement
            // is always a legal one.
            clip.timelineStart = trackEnd(track) + Duration::fromMilliseconds(1'000);
            clip.sourceIn = Duration::zero();
            clip.sourceOut = Duration::fromMilliseconds(
                *rc::gen::inRange<std::int64_t>(100, 5'001));
            return {"timeline.add_clip",
                    std::make_unique<AddClipCommand>(track.id, std::move(clip))};
        }
        case AddEffect: {
            const auto [t, c] = clips[*rc::gen::inRange<std::size_t>(0, clips.size())];
            return {"timeline.add_effect",
                    std::make_unique<AddEffectCommand>(project.tracks[t].clips[c].id,
                                                       drawEffect())};
        }
        case AddTransition: {
            const auto [t, c] = clips[*rc::gen::inRange<std::size_t>(0, clips.size())];
            const Clip& target = project.tracks[t].clips[c];
            Transition  transition;
            transition.id = drawUuid();
            transition.kind = transitionKindAt(*rc::gen::inRange(0, 5), 0);
            // At least as long as any transition already there: widening the
            // permitted overlap can never invalidate the track.
            const Duration existing = target.transitionIn.has_value()
                                          ? target.transitionIn->duration
                                          : Duration::zero();
            transition.duration =
                existing + Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 501));
            return {"timeline.add_transition",
                    std::make_unique<SetTransitionCommand>(target.id, transition)};
        }
        case DeleteClip: {
            const auto [t, c] = clips[*rc::gen::inRange<std::size_t>(0, clips.size())];
            return {"timeline.delete_clip",
                    std::make_unique<DeleteClipCommand>(project.tracks[t].clips[c].id)};
        }
        case MoveClip: {
            const auto [t, c] = clips[*rc::gen::inRange<std::size_t>(0, clips.size())];
            const Track& track = project.tracks[t];
            // Moved clear of every other clip on its own track.
            const Duration destination = trackEnd(track) + Duration::fromMilliseconds(1'000);
            return {"timeline.move_clip",
                    std::make_unique<MoveClipCommand>(track.clips[c].id, destination)};
        }
        case SplitClip:
        default: {
            // A split needs an interior playhead, so pick a clip long enough to
            // have one; every generated clip is at least 20 ms long.
            const auto [t, c] = clips[*rc::gen::inRange<std::size_t>(0, clips.size())];
            const Clip&    target = project.tracks[t].clips[c];
            const Duration playhead =
                target.timelineStart + Duration::fromNanoseconds(target.duration().ticks() / 2);
            return {"timeline.split_clip",
                    std::make_unique<SplitClipCommand>(target.id, playhead)};
        }
    }
}

// ---------------------------------------------------------------------------
// Feature: end-to-end-editor-integration, Property 16: Save/open round-trip
// preserves the project — for all projects constructible through the
// Tool_Surface with 1-20 tracks, 0-200 clips per track, 0-10 effects per clip,
// 0-50 transitions, 0-200 registered assets and names of 0-255 characters,
// saving to a file and then opening it yields a project equal to the original in
// identifier, name, frame rate, canvas, colour space, track order, clip order,
// clip source ranges, effects, transitions and asset references.
//
// Requirement 4.7: "FOR ALL projects that can be constructed through the
// Tool_Surface with 1 to 20 tracks, 0 to 200 clips per track, 0 to 10 effects
// per clip, 0 to 50 transitions, 0 to 200 registered assets and names of 0 to
// 255 characters, saving then opening SHALL produce a project equal to the
// original in identifier, name, frame rate, canvas, color space, track order,
// clip order, clip source ranges, effects, transitions and asset references
// (round-trip property)."
//
// **Validates: Requirements 4.7**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ProjectSessionPersistenceProperties,
              SaveThenOpenYieldsAnEqualProject,
              ()) {
    const Project original = drawProject(drawShape());

    ScratchPaths   paths;
    const fs::path seedPath = paths.next("seed");
    const fs::path savedPath = paths.next("saved");

    ProjectSession session;
    RC_ASSERT(openGeneratedProject(session, original, seedPath));
    // The project the session now holds is the generated one.
    assertSameProject(original, session.engine().snapshot());

    const std::optional<ProjectSession::SaveCompletionInfo> saved =
        saveAndSettle(session, savedPath);
    RC_ASSERT(saved.has_value());
    RC_ASSERT(saved->succeeded);

    // Opening the saved document in a fresh session yields the same project.
    ProjectSession reopened;
    const Result<ProjectSession::Status> opened = reopened.openProject(savedPath);
    RC_ASSERT(opened.isOk());
    assertSameProject(original, reopened.engine().snapshot());
}

// ---------------------------------------------------------------------------
// Feature: end-to-end-editor-integration, Property 17: Saving a loaded project
// is idempotent — for all projects within the Property-16 bounds, saving,
// opening, and saving again produces a document that deserialises to a project
// equal to the one produced by the first load.
//
// Requirement 4.8: "FOR ALL projects within the bounds stated in criterion 7,
// saving a loaded project SHALL produce a document that deserializes to the same
// project as the first save (idempotence property)."
//
// **Validates: Requirements 4.8**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ProjectSessionPersistenceProperties,
              SavingALoadedProjectIsIdempotent,
              ()) {
    const Project original = drawProject(drawShape());

    ScratchPaths   paths;
    const fs::path seedPath = paths.next("idem_seed");
    const fs::path firstSave = paths.next("idem_first");
    const fs::path secondSave = paths.next("idem_second");

    ProjectSession session;
    RC_ASSERT(openGeneratedProject(session, original, seedPath));

    // First save.
    const std::optional<ProjectSession::SaveCompletionInfo> first =
        saveAndSettle(session, firstSave);
    RC_ASSERT(first.has_value());
    RC_ASSERT(first->succeeded);

    // Open what was written; this load is the reference the second save is
    // measured against.
    ProjectSession loader;
    RC_ASSERT(loader.openProject(firstSave).isOk());
    const Project firstLoad = loader.engine().snapshot();

    // Save the loaded project again, without editing it.
    const std::optional<ProjectSession::SaveCompletionInfo> second =
        saveAndSettle(loader, secondSave);
    RC_ASSERT(second.has_value());
    RC_ASSERT(second->succeeded);

    const Result<Project> secondLoad = loadProjectFromFile(secondSave);
    RC_ASSERT(secondLoad.isOk());
    assertSameProject(firstLoad, secondLoad.value());
}

// ---------------------------------------------------------------------------
// Feature: end-to-end-editor-integration, Property 19: Unmodified until the
// next tool-applied edit — for any project and any subsequent editing tool
// invocation, the session reports unmodified immediately after a successful save
// and reports modified immediately after that invocation succeeds.
//
// Requirement 4.6: "WHEN a save completes successfully, THE Project_Session
// SHALL report the project as unmodified through its public accessor until the
// next edit applied through the Tool_Surface."
//
// **Validates: Requirements 4.6**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ProjectSessionPersistenceProperties,
              UnmodifiedAfterSaveUntilTheNextToolAppliedEdit,
              ()) {
    Shape shape;
    shape.maxTracks = 4;
    shape.maxClipsPerTrack = 8;
    shape.clipBudget = 16;
    shape.maxAssets = 4;
    shape.maxTransitions = 4;
    const Project original = drawProject(shape);

    ScratchPaths   paths;
    const fs::path seedPath = paths.next("dirty_seed");
    const fs::path savedPath = paths.next("dirty_saved");

    ProjectSession session;
    RC_ASSERT(openGeneratedProject(session, original, seedPath));

    // An edit before the save, so the save has something to clear.
    EditInvocation before = drawEditInvocation(session.engine().snapshot());
    RC_ASSERT(session.engine().apply(std::move(before.command)).changed());
    RC_ASSERT(session.modified());

    const std::optional<ProjectSession::SaveCompletionInfo> saved =
        saveAndSettle(session, savedPath);
    RC_ASSERT(saved.has_value());
    RC_ASSERT(saved->succeeded);

    // Immediately after a successful save: unmodified, with the written location
    // recorded.
    RC_ASSERT(!session.modified());
    RC_ASSERT(!session.status().modified);
    RC_ASSERT(session.documentPath().has_value());
    RC_ASSERT(*session.documentPath() == savedPath);

    // The next editing tool invocation makes it modified again — immediately,
    // and only because of that invocation.
    EditInvocation next = drawEditInvocation(session.engine().snapshot());
    RC_ASSERT(!next.toolName.empty());
    const CommandResult applied = session.engine().apply(std::move(next.command));
    RC_ASSERT(applied.changed());
    RC_ASSERT(session.modified());
    RC_ASSERT(session.status().modified);
    // The recorded location is untouched by the edit.
    RC_ASSERT(session.documentPath().has_value());
    RC_ASSERT(*session.documentPath() == savedPath);
}

} // namespace
} // namespace palmier::services
