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
//
// Schema 1.1 (task 1.5; Requirements 3.9, 4.7, 4.10, 14.4) adds the backward- and
// forward-compatibility cases: a 1.0 document must still load, with each 1.1 field
// taking its documented default, and a document written at a major version this
// build does not know must be refused rather than partially loaded.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "core/Clip.hpp"
#include "core/ClipGroup.hpp"
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
    video.name = "A-cam";
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
    // Schema 1.1 effect kind (Requirement 14.4); parameterless.
    c1.effects.push_back(Effect::invertColors());
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
    // Left unnamed on purpose: "" is a legitimate value, not just a default.

    p.tracks = {video, audio};

    // Schema 1.1 reserves clipGroups; the store must persist it faithfully even
    // though no edit interprets it yet.
    p.clipGroups.push_back(ClipGroup{Uuid::generateV4(), {c1.id, c2.id}});
    p.clipGroups.push_back(ClipGroup{Uuid::generateV4(), {}});
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

    ASSERT_EQ(a.clipGroups.size(), b.clipGroups.size());
    for (std::size_t g = 0; g < a.clipGroups.size(); ++g) {
        EXPECT_EQ(a.clipGroups[g].id, b.clipGroups[g].id);
        EXPECT_EQ(a.clipGroups[g].clipIds, b.clipGroups[g].clipIds);
    }

    ASSERT_EQ(a.tracks.size(), b.tracks.size());
    for (std::size_t t = 0; t < a.tracks.size(); ++t) {
        EXPECT_EQ(a.tracks[t].id, b.tracks[t].id);
        EXPECT_EQ(a.tracks[t].kind, b.tracks[t].kind);
        EXPECT_EQ(a.tracks[t].name, b.tracks[t].name);
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

// Rewrite the declared version of an otherwise valid document.
std::string withDeclaredVersion(const Project& p, const std::string& version) {
    std::string text = serializeProject(p);
    const std::string current = '"' + SchemaVersion::current().toString() + '"';
    const auto pos = text.find(current);
    EXPECT_NE(pos, std::string::npos);
    text.replace(pos, current.size(), '"' + version + '"');
    return text;
}

TEST(ProjectStore, WritesTheCurrentSchemaVersion) {
    // Schema 1.2 is what this build writes (usable-editor task 12; Requirement 9).
    EXPECT_EQ(SchemaVersion::current(), SchemaVersion(1, 2));
    EXPECT_NE(serializeProject(makeSampleProject()).find("\"version\": \"1.2\""),
              std::string::npos);
}

TEST(ProjectStore, SchemaCompatibilityIsMinorForwardOnly) {
    // The rule the whole 1.1 migration rests on, stated directly: this build
    // (reader 1.1) reads 1.0 documents, and a 1.0-era build (reader 1.0) refuses a
    // 1.1 document rather than dropping the fields it does not know about. A
    // differing major is incompatible in either direction (Requirements 3.9, 4.7).
    constexpr SchemaVersion v10{1, 0};
    constexpr SchemaVersion v11{1, 1};

    EXPECT_TRUE(SchemaVersion::isCompatible(v11, v10));
    EXPECT_FALSE(SchemaVersion::isCompatible(v10, v11));
    EXPECT_TRUE(SchemaVersion::isCompatible(v11, v11));
    EXPECT_FALSE(SchemaVersion::isCompatible(v11, SchemaVersion(2, 0)));
    EXPECT_FALSE(SchemaVersion::isCompatible(SchemaVersion(2, 0), v11));

    // ...and therefore, for this build, 1.0 and 1.1 are both loadable.
    EXPECT_TRUE(v10.isSupported());
    EXPECT_TRUE(v11.isSupported());
}

TEST(ProjectStore, RejectsFutureMajorSchemaVersion) {
    // A future major version this build cannot load must be rejected as
    // Unsupported, and nothing of the document may be loaded (Requirements 3.9,
    // 4.10 — the caller keeps the project that was current).
    const std::string version =
        std::to_string(SchemaVersion::current().major + 1) + ".0";
    Result<Project> r = deserializeProject(withDeclaredVersion(makeSampleProject(), version));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(r.error().message().find(version), std::string::npos)
        << r.error().toString();
}

TEST(ProjectStore, RejectsNewerMinorSchemaVersion) {
    // The other direction of SchemaVersion::isCompatible: same major, but the
    // document is newer than the reader. This is exactly the case a 1.0-era build
    // faces when handed one of this build's 1.1 documents.
    const std::string version = std::to_string(SchemaVersion::current().major) + '.' +
                                std::to_string(SchemaVersion::current().minor + 1);
    Result<Project> r = deserializeProject(withDeclaredVersion(makeSampleProject(), version));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::Unsupported);
}

TEST(ProjectStore, ReadsSchema10DocumentApplyingDocumentedDefaults) {
    // A verbatim 1.0 document: no tracks[].name, no clipGroups. It must load, and
    // each schema-1.1 field must take its documented default (Requirement 4.7 —
    // documents written at any stage stay readable at every later stage).
    const char* text = R"({
        "format": "palmier-project",
        "version": "1.0",
        "project": {
            "id": "00000000-0000-4000-8000-000000000001",
            "name": "legacy",
            "timelineFps": {"num": 30, "den": 1},
            "canvas": {"width": 1920, "height": 1080},
            "colorSpace": "rec709",
            "tracks": [
                {
                    "id": "00000000-0000-4000-8000-000000000002",
                    "kind": "video",
                    "muted": false,
                    "locked": false,
                    "clips": [
                        {
                            "id": "00000000-0000-4000-8000-000000000003",
                            "assetRef": {
                                "assetId": "00000000-0000-4000-8000-000000000004",
                                "sourcePath": "/media/a.mp4"
                            },
                            "timelineStart": 0,
                            "sourceIn": 0,
                            "sourceOut": 1000000000,
                            "effects": [],
                            "transitionIn": null,
                            "gain": 1,
                            "opacity": 1
                        }
                    ]
                }
            ],
            "assets": [
                {
                    "assetId": "00000000-0000-4000-8000-000000000004",
                    "sourcePath": "/media/a.mp4"
                }
            ]
        }
    })";

    Result<Project> loaded = deserializeProject(text);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    const Project& p = loaded.value();

    // The stored version is preserved as read, not silently upgraded.
    EXPECT_EQ(p.version, SchemaVersion(1, 0));

    // The 1.1 defaults.
    ASSERT_EQ(p.tracks.size(), 1u);
    EXPECT_EQ(p.tracks[0].name, "");
    EXPECT_TRUE(p.clipGroups.empty());

    // Everything 1.0 did carry is unchanged.
    EXPECT_EQ(p.name, "legacy");
    EXPECT_EQ(p.timelineFps, FrameRate(30, 1));
    EXPECT_EQ(p.canvas, Resolution(1920, 1080));
    ASSERT_EQ(p.tracks[0].clips.size(), 1u);
    EXPECT_EQ(p.tracks[0].clips[0].sourceOut.ticks(), 1'000'000'000LL);
    ASSERT_EQ(p.assets.size(), 1u);
    EXPECT_EQ(p.assets[0].sourcePath, "/media/a.mp4");
}

TEST(ProjectStore, ExplicitNullSchema11FieldsTakeTheirDefaults) {
    Project p = makeSampleProject();
    std::string text = serializeProject(p);
    // Both 1.1 fields written as JSON null must read as their defaults rather
    // than as a malformed document.
    const auto namePos = text.find("\"name\": \"A-cam\"");
    ASSERT_NE(namePos, std::string::npos);
    text.replace(namePos, std::string_view{"\"name\": \"A-cam\""}.size(), "\"name\": null");

    Result<Project> loaded = deserializeProject(text);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    EXPECT_EQ(loaded.value().tracks[0].name, "");
}

TEST(ProjectStore, RejectsIllTypedSchema11Fields) {
    // Present-but-wrong-type is a hard error: a corrupt document must not be
    // silently downgraded to the absent-field defaults.
    Project p = makeSampleProject();
    std::string text = serializeProject(p);
    const auto namePos = text.find("\"name\": \"A-cam\"");
    ASSERT_NE(namePos, std::string::npos);
    text.replace(namePos, std::string_view{"\"name\": \"A-cam\""}.size(), "\"name\": 7");
    Result<Project> r = deserializeProject(text);
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);

    // Likewise for a clipGroups entry carrying a non-UUID clip id.
    const char* badGroups = R"({
        "format": "palmier-project",
        "version": "1.1",
        "project": {
            "id": "00000000-0000-4000-8000-000000000001",
            "name": "x",
            "timelineFps": {"num": 30, "den": 1},
            "canvas": {"width": 640, "height": 480},
            "colorSpace": "rec709",
            "tracks": [],
            "assets": [],
            "clipGroups": [
                {"id": "00000000-0000-4000-8000-000000000002", "clipIds": ["nope"]}
            ]
        }
    })";
    Result<Project> r2 = deserializeProject(badGroups);
    ASSERT_TRUE(r2.isError());
    EXPECT_EQ(r2.error().code(), ErrorCode::InvalidArgument);
}

TEST(ProjectStore, InvertColorsEffectUsesItsStableKey) {
    Project p = makeSampleProject();
    const std::string text = serializeProject(p);
    EXPECT_NE(text.find("\"invert_colors\""), std::string::npos);

    Result<Project> loaded = deserializeProject(text);
    ASSERT_TRUE(loaded.isOk()) << loaded.error().toString();
    const std::vector<Effect>& effects = loaded.value().tracks.at(0).clips.at(0).effects;
    ASSERT_EQ(effects.size(), 3u);
    EXPECT_EQ(effects[2].type, EffectType::InvertColors);
    EXPECT_TRUE(effects[2].parameters.empty());
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
