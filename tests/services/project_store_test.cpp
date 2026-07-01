// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_store_test.cpp — example-based unit tests for the
// `.palmier` project store (task 5.1; Requirements 3.5, 3.6).
//
// These verify the serialize/deserialize mechanism directly: that the complete
// project state (tracks and their order, clips and their positions/source ranges,
// effects, transitions, referenced assets, timeline settings, and schema version)
// survives a round-trip, that the writer is idempotent, and that malformed or
// unsupported documents are rejected with the right ErrorCode. The universal
// round-trip PROPERTY (design P11) is covered separately by task 5.2.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "core/Clip.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"
#include "services/ProjectStore.hpp"

namespace palmier::services {
namespace {

// A representative project exercising every field the store must persist:
// two tracks (video + audio) in a specific order, multiple clips with distinct
// positions/source ranges, effects with named parameters, an optional transition,
// referenced assets, and non-default timeline settings.
Project makeSampleProject() {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "Sample \"quoted\"/slashed\tproject — ☃";
    p.timelineFps = FrameRate::fps23_976();
    p.canvas = Resolution::uhd4k();
    p.colorSpace = ColorSpace::Rec2020;
    p.version = SchemaVersion::current();

    MediaAssetRef a1{Uuid::generateV4(), "/media/a.mp4"};
    MediaAssetRef a2{Uuid::generateV4(), "/media/b.png"};
    p.assets = {a1, a2};

    Track video;
    video.id = Uuid::generateV4();
    video.kind = TrackKind::Video;
    video.muted = false;
    video.locked = true;

    Clip c1;
    c1.id = Uuid::generateV4();
    c1.assetRef = a1;
    c1.timelineStart = Duration::fromNanoseconds(1'234'567'890'123LL);
    c1.sourceIn = Duration::fromMilliseconds(500);
    c1.sourceOut = Duration::fromMilliseconds(5000);
    c1.gain = 0.75;
    c1.opacity = 0.5;
    c1.effects.push_back(Effect::brightness(0.1));
    c1.effects.push_back(Effect{Uuid::generateV4(), EffectType::ColorGrade,
                                {{"lift", -0.25}, {"gamma", 1.25}}});
    c1.transitionIn = Transition{Uuid::generateV4(), TransitionKind::Crossfade,
                                 Duration::fromMilliseconds(250)};
    video.clips.push_back(c1);

    Clip c2;
    c2.id = Uuid::generateV4();
    c2.assetRef = a2;
    c2.timelineStart = Duration::fromMilliseconds(5000);
    c2.sourceIn = Duration::zero();
    c2.sourceOut = Duration::fromMilliseconds(2000);
    video.clips.push_back(c2);

    Track audio;
    audio.id = Uuid::generateV4();
    audio.kind = TrackKind::Audio;
    audio.muted = true;

    p.tracks = {video, audio};
    return p;
}

void expectSameClip(const Clip& a, const Clip& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.assetRef.assetId, b.assetRef.assetId);
    EXPECT_EQ(a.assetRef.sourcePath, b.assetRef.sourcePath);
    EXPECT_EQ(a.timelineStart.ticks(), b.timelineStart.ticks());
    EXPECT_EQ(a.sourceIn.ticks(), b.sourceIn.ticks());
    EXPECT_EQ(a.sourceOut.ticks(), b.sourceOut.ticks());
    EXPECT_DOUBLE_EQ(a.gain, b.gain);
    EXPECT_DOUBLE_EQ(a.opacity, b.opacity);

    ASSERT_EQ(a.effects.size(), b.effects.size());
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        EXPECT_EQ(a.effects[i].id, b.effects[i].id);
        EXPECT_EQ(a.effects[i].type, b.effects[i].type);
        EXPECT_EQ(a.effects[i].parameters, b.effects[i].parameters);
    }

    ASSERT_EQ(a.transitionIn.has_value(), b.transitionIn.has_value());
    if (a.transitionIn.has_value()) {
        EXPECT_EQ(a.transitionIn->id, b.transitionIn->id);
        EXPECT_EQ(a.transitionIn->kind, b.transitionIn->kind);
        EXPECT_EQ(a.transitionIn->duration.ticks(), b.transitionIn->duration.ticks());
    }
}

void expectSameProject(const Project& a, const Project& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.name, b.name);
    EXPECT_EQ(a.timelineFps, b.timelineFps);
    EXPECT_EQ(a.canvas, b.canvas);
    EXPECT_EQ(a.colorSpace, b.colorSpace);
    EXPECT_EQ(a.version, b.version);

    ASSERT_EQ(a.assets.size(), b.assets.size());
    for (std::size_t i = 0; i < a.assets.size(); ++i) {
        EXPECT_EQ(a.assets[i].assetId, b.assets[i].assetId);
        EXPECT_EQ(a.assets[i].sourcePath, b.assets[i].sourcePath);
    }

    ASSERT_EQ(a.tracks.size(), b.tracks.size());
    for (std::size_t t = 0; t < a.tracks.size(); ++t) {
        EXPECT_EQ(a.tracks[t].id, b.tracks[t].id);
        EXPECT_EQ(a.tracks[t].kind, b.tracks[t].kind);
        EXPECT_EQ(a.tracks[t].muted, b.tracks[t].muted);
        EXPECT_EQ(a.tracks[t].locked, b.tracks[t].locked);
        ASSERT_EQ(a.tracks[t].clips.size(), b.tracks[t].clips.size());
        for (std::size_t c = 0; c < a.tracks[t].clips.size(); ++c) {
            expectSameClip(a.tracks[t].clips[c], b.tracks[t].clips[c]);
        }
    }
}

TEST(ProjectStore, RoundTripPreservesCompleteState) {
    const Project original = makeSampleProject();
    const std::string text = serializeProject(original);

    Result<Project> loaded = deserializeProject(text);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    expectSameProject(original, loaded.value());
}

TEST(ProjectStore, SerializationIsIdempotent) {
    const Project original = makeSampleProject();
    const std::string first = serializeProject(original);
    Result<Project> loaded = deserializeProject(first);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    // Re-serializing a deserialized project reproduces byte-identical text.
    EXPECT_EQ(first, serializeProject(loaded.value()));
}

TEST(ProjectStore, EmptyProjectRoundTrips) {
    Project p;
    p.id = Uuid::generateV4();
    p.name = "empty";
    p.timelineFps = FrameRate::fps30();
    p.canvas = Resolution::hd1080();
    // No tracks, no assets.

    Result<Project> loaded = deserializeProject(serializeProject(p));
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    expectSameProject(p, loaded.value());
    EXPECT_TRUE(loaded.value().tracks.empty());
    EXPECT_TRUE(loaded.value().assets.empty());
}

TEST(ProjectStore, RejectsMalformedJson) {
    Result<Project> r = deserializeProject("{ this is not valid json ");
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectStore, RejectsWrongFormatMagic) {
    Result<Project> r = deserializeProject(R"({"format":"something-else","version":"1.0"})");
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectStore, RejectsUnsupportedSchemaVersion) {
    // A future major version this build cannot load must be rejected as Unsupported.
    Project p = makeSampleProject();
    std::string text = serializeProject(p);
    const auto pos = text.find("\"1.0\"");
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, 5, "\"2.0\"");

    Result<Project> r = deserializeProject(text);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::Unsupported);
}

TEST(ProjectStore, RejectsMissingRequiredField) {
    // A document missing the project's canvas is structurally invalid.
    const char* text = R"({
        "format": "palmier-project",
        "version": "1.0",
        "project": {
            "id": "00000000-0000-4000-8000-000000000001",
            "name": "x",
            "timelineFps": {"num": 30, "den": 1},
            "colorSpace": "rec709",
            "tracks": [],
            "assets": []
        }
    })";
    Result<Project> r = deserializeProject(text);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectStore, FileRoundTrip) {
    const Project original = makeSampleProject();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "palmier_project_store_test.palmier";
    std::filesystem::remove(path);

    Result<void> saved = saveProjectToFile(original, path);
    ASSERT_TRUE(saved.isOk()) << saved.error().toString();

    Result<Project> loaded = loadProjectFromFile(path);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    expectSameProject(original, loaded.value());

    std::filesystem::remove(path);
}

TEST(ProjectStore, LoadFromMissingFileIsIoError) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "palmier_does_not_exist_12345.palmier";
    std::filesystem::remove(path);
    Result<Project> r = loadProjectFromFile(path);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::Io);
}

} // namespace
} // namespace palmier::services
