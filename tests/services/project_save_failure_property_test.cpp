// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_save_failure_property_test.cpp — the universally
// quantified save-failure property for services::ProjectSession (task 2.4).
//
// Design Property 18 (design.md "Correctness Properties"):
//
//     For any project, any pre-existing document bytes at the destination, and
//     any injected write failure (insufficient space, insufficient permissions,
//     inaccessible location), the pre-existing file is byte-for-byte unchanged,
//     the in-memory project is byte-identical, the modified flag remains true,
//     and the error names the destination path.
//
// This is the universal counterpart to the single example in
// project_session_test.cpp (`AFailedSavePreservesTheProjectTheFlagAndAnyPrevious-
// File`), which pins one project, one previous-file spelling and one failure
// kind. Here all three axes are generated:
//
//   * the project — an arbitrary VALID project (supported schema version, positive
//     frame rate and canvas, resolvable asset references, ordered non-overlapping
//     clips with effects, transitions, gain and opacity), seeded into a real
//     ProjectSession through a real `.palmier` document and openProject, then
//     genuinely modified through the engine so the "modified flag remains true"
//     clause has something to preserve;
//   * the pre-existing destination bytes — empty, arbitrary short byte strings
//     (including quotes, control bytes and non-ASCII), and large multi-kilobyte
//     payloads, so "byte-for-byte unchanged" is checked against a real file whose
//     size ranges from 0 to hundreds of kilobytes;
//   * the failure kind — insufficient space, insufficient permissions and an
//     inaccessible location, each injected deterministically through the
//     `services::RawFileWriter` seam (no test needs a full volume, a read-only
//     mount or elevated privileges).
//
// "The in-memory project is byte-identical" is asserted on the project's own
// serialized rendering: the `.palmier` bytes of the session snapshot taken before
// the save request must equal the bytes of the snapshot taken after the failed
// completion has been applied. That is a total, field-complete comparison of the
// project value rather than a hand-listed subset of its fields.
//
// Every case runs the real off-thread save path: requestSave starts a worker, and
// the completion is applied on this (owning) thread by awaitSaveCompletions(),
// exactly as the shell does.
//
// _Requirements: 4.4, 14.7_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
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
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectSaveService.hpp"
#include "services/ProjectSession.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scratch filesystem: one unique directory per generated case, removed however
// the case ends (RapidCheck reports a failure by throwing out of the property
// body, so cleanup must be RAII rather than trailing statements).
// ---------------------------------------------------------------------------

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<std::uint64_t> counter{0};
        path_ = fs::temp_directory_path() /
                ("palmier_save_failure_prop_" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] fs::path file(std::string_view name) const { return path_ / name; }

private:
    fs::path path_;
};

void writeBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// ---------------------------------------------------------------------------
// Generators (imperative, drawn with RapidCheck's `*gen` operator so values
// shrink and replay deterministically).
// ---------------------------------------------------------------------------

// Identities are freshly generated rather than drawn byte-wise: the session only
// accepts a project whose asset identities are non-nil and pairwise distinct (an
// open catalogues them into a MediaManager), and a byte-wise draw shrinks straight
// to the nil UUID and to duplicates. Property 18 quantifies over project CONTENT,
// not over identity collisions, which are a malformed-document concern covered by
// the open tests in project_session_test.cpp.
Uuid drawUuid() { return Uuid::generateV4(); }

MediaAssetRef drawAssetRef() {
    MediaAssetRef ref;
    ref.assetId = drawUuid();
    ref.sourcePath = "/media/" + *rc::gen::arbitrary<std::string>() + ".mp4";
    return ref;
}

Effect drawEffect() {
    Effect fx;
    fx.id = drawUuid();
    fx.type = *rc::gen::element<EffectType>(EffectType::Brightness, EffectType::Contrast,
                                           EffectType::Blur, EffectType::ColorGrade);
    const int paramCount = *rc::gen::inRange<int>(0, 3);
    for (int i = 0; i < paramCount; ++i) {
        fx.parameters[*rc::gen::arbitrary<std::string>()] =
            static_cast<double>(*rc::gen::inRange<int>(-1'000, 1'001)) / 100.0;
    }
    return fx;
}

// A clip laid out strictly after `cursor` so every generated track satisfies the
// engine's ordering / non-overlap invariants (and so openProject accepts the
// document without needing to re-derive a legal layout).
Clip drawClipAfter(const std::vector<MediaAssetRef>& assets, Duration& cursor) {
    Clip clip;
    clip.id = drawUuid();
    clip.assetRef = assets[static_cast<std::size_t>(
        *rc::gen::inRange<int>(0, static_cast<int>(assets.size())))];

    const std::int64_t gapMs = *rc::gen::inRange<std::int64_t>(0, 5'001);
    const std::int64_t inMs = *rc::gen::inRange<std::int64_t>(0, 60'001);
    const std::int64_t lengthMs = *rc::gen::inRange<std::int64_t>(1, 30'001);

    clip.timelineStart = cursor + Duration::fromMilliseconds(gapMs);
    clip.sourceIn = Duration::fromMilliseconds(inMs);
    clip.sourceOut = Duration::fromMilliseconds(inMs + lengthMs);
    cursor = clip.timelineEnd();

    const int effectCount = *rc::gen::inRange<int>(0, 3);
    for (int i = 0; i < effectCount; ++i) {
        clip.effects.push_back(drawEffect());
    }

    if (*rc::gen::arbitrary<bool>()) {
        Transition transition;
        transition.id = drawUuid();
        transition.kind = *rc::gen::element<TransitionKind>(
            TransitionKind::Crossfade, TransitionKind::DipToColor, TransitionKind::Wipe,
            TransitionKind::Slide, TransitionKind::Fade);
        transition.duration = Duration::fromMilliseconds(*rc::gen::inRange<std::int64_t>(0, 2'001));
        clip.transitionIn = transition;
    }

    clip.gain = static_cast<double>(*rc::gen::inRange<int>(0, 4'001)) / 1'000.0;
    clip.opacity = static_cast<double>(*rc::gen::inRange<int>(0, 1'001)) / 1'000.0;
    return clip;
}

Track drawTrack(const std::vector<MediaAssetRef>& assets) {
    Track track;
    track.id = drawUuid();
    track.kind = *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio);
    track.name = *rc::gen::arbitrary<std::string>();
    track.muted = *rc::gen::arbitrary<bool>();
    track.locked = *rc::gen::arbitrary<bool>();

    Duration  cursor = Duration::zero();
    const int clipCount = *rc::gen::inRange<int>(0, 5);
    for (int i = 0; i < clipCount; ++i) {
        track.clips.push_back(drawClipAfter(assets, cursor));
    }
    return track;
}

// An arbitrary VALID project: a supported schema version, a positive rational
// frame rate inside the session's accepted band, a positive canvas, a non-empty
// asset table every clip resolves into, and legal per-track clip layouts.
Project drawProject() {
    Project p;
    p.id = drawUuid();
    p.name = *rc::gen::arbitrary<std::string>();
    p.timelineFps = FrameRate{*rc::gen::inRange<std::int64_t>(1, 241),
                              *rc::gen::inRange<std::int64_t>(1, 4)};
    p.canvas = Resolution{static_cast<std::uint32_t>(*rc::gen::inRange<int>(16, 7'681)),
                          static_cast<std::uint32_t>(*rc::gen::inRange<int>(16, 4'321))};
    p.colorSpace = *rc::gen::element<ColorSpace>(ColorSpace::Srgb, ColorSpace::Rec709,
                                                ColorSpace::Rec2020, ColorSpace::Rec2100Pq,
                                                ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3,
                                                ColorSpace::LinearSrgb);
    p.version = SchemaVersion::current();

    const int assetCount = *rc::gen::inRange<int>(1, 4);
    for (int i = 0; i < assetCount; ++i) {
        p.assets.push_back(drawAssetRef());
    }

    const int trackCount = *rc::gen::inRange<int>(0, 4);
    for (int i = 0; i < trackCount; ++i) {
        p.tracks.push_back(drawTrack(p.assets));
    }
    return p;
}

// Pre-existing destination bytes: empty, arbitrary short byte strings, and large
// multi-kilobyte payloads. All three shapes must survive a failed save untouched.
std::string drawPreExistingBytes() {
    switch (*rc::gen::inRange<int>(0, 3)) {
        case 0:
            return {}; // an existing but empty document
        case 1:
            return *rc::gen::arbitrary<std::string>();
        default: {
            const std::size_t size =
                static_cast<std::size_t>(*rc::gen::inRange<int>(64 * 1024, 256 * 1024));
            const char filler = static_cast<char>(*rc::gen::inRange<int>(1, 256));
            std::string large(size, filler);
            // A distinguishable head and tail, so a truncating or partially
            // overwriting save cannot pass by accident on a uniform buffer.
            large.front() = 'H';
            large.back() = 'T';
            return large;
        }
    }
}

// The three failure kinds Requirement 4.4 / 14.7 enumerate, each injected through
// the RawFileWriter seam. Every one of these writers refuses BEFORE touching the
// filesystem, so the destination is never opened, let alone truncated.
enum class FailureKind { InsufficientSpace, InsufficientPermissions, InaccessibleLocation };

struct InjectedFailure {
    FailureKind kind = FailureKind::InsufficientSpace;
    ErrorCode   code = ErrorCode::Io;
    std::string message;
};

InjectedFailure drawFailure() {
    const FailureKind kind = *rc::gen::element<FailureKind>(
        FailureKind::InsufficientSpace, FailureKind::InsufficientPermissions,
        FailureKind::InaccessibleLocation);
    switch (kind) {
        case FailureKind::InsufficientSpace:
            return {kind, ErrorCode::Io, "no space left on device"};
        case FailureKind::InsufficientPermissions:
            return {kind, ErrorCode::PermissionDenied, "permission denied"};
        case FailureKind::InaccessibleLocation:
            break;
    }
    return {kind, ErrorCode::Io, "save location is inaccessible"};
}

RawFileWriter writerFor(const InjectedFailure& failure) {
    return [failure](const fs::path& path, std::string_view) -> Result<void> {
        return makeError(failure.code, failure.message + " while writing '" + path.string() + "'");
    };
}

// ---------------------------------------------------------------------------
// The property.
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 18: A failed save preserves
// the file and the modified state — for any project, any pre-existing document
// bytes at the destination, and any injected write failure (insufficient space,
// insufficient permissions, inaccessible location), the pre-existing file is
// byte-for-byte unchanged, the in-memory project is byte-identical, the modified
// flag remains true, and the error names the destination path.
// Validates: Requirements 4.4, 14.7
RC_GTEST_PROP(ProjectSaveFailureProperties,
              AFailedSavePreservesTheFileAndTheModifiedState,
              ()) {
    const Project         project = drawProject();
    const std::string     previousBytes = drawPreExistingBytes();
    const InjectedFailure failure = drawFailure();

    const ScratchDir scratch;
    const fs::path   sourcePath = scratch.file("source.palmier");
    const fs::path   destination = scratch.file("destination.palmier");

    // Seed the session with the generated project through the real document path,
    // then modify it through the engine so there is a dirty state to preserve.
    RC_ASSERT(saveProjectToFile(project, sourcePath).isOk());
    ProjectSession session(writerFor(failure));
    RC_ASSERT(session.openProject(sourcePath).isOk());
    RC_ASSERT(session.engine().apply(std::make_unique<AddTrackCommand>(TrackKind::Video)).changed());
    RC_ASSERT(session.modified());

    // The destination already holds a previously saved document.
    writeBytes(destination, previousBytes);

    const std::string   projectBefore = serializeProject(session.engine().snapshot());
    const std::uint64_t revisionBefore = session.revision();
    const auto          documentPathBefore = session.documentPath();

    std::optional<ProjectSession::SaveCompletionInfo> info;
    RC_ASSERT(session
                  .requestSave(destination,
                               [&info](const ProjectSession::SaveCompletionInfo& i) { info = i; })
                  .isOk());
    RC_ASSERT(session.awaitSaveCompletions() == 1u);
    RC_ASSERT(info.has_value());

    // The save did not complete, and the reported error names the destination.
    RC_ASSERT(!info->succeeded);
    RC_ASSERT(info->error.code() == failure.code);
    RC_ASSERT(info->error.message().find(destination.string()) != std::string::npos);

    // The pre-existing file is byte-for-byte unchanged.
    RC_ASSERT(fs::exists(destination));
    RC_ASSERT(readBytes(destination) == previousBytes);

    // The in-memory project is byte-identical, and its dirty state is intact.
    RC_ASSERT(serializeProject(session.engine().snapshot()) == projectBefore);
    RC_ASSERT(session.revision() == revisionBefore);
    RC_ASSERT(session.modified());
    RC_ASSERT(info->stillModified);
    RC_ASSERT(session.documentPath() == documentPathBefore);
}

} // namespace
} // namespace palmier::services
