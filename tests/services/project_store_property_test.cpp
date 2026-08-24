// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/project_store_property_test.cpp — property-based test for the
// `.palmier` project persistence round-trip (task 5.2).
//
// Design property P11 (design.md "Correctness Properties"):
//
//     For any valid project, serializing it to the `.palmier` store and then
//     deserializing it yields an equivalent project (all clips, tracks, edits,
//     and media references preserved).
//
// This is the universal counterpart to the example-based round-trip tests in
// project_store_test.cpp (task 5.1) and to Requirement 3.5 ("persist all clips,
// tracks, edits, and imported media references such that reopening the project
// restores the prior editing state, including clip positions, track order, and
// the selected version of each clip"). The serializer/deserializer under test is
// implemented in services/ProjectStore.cpp; this file adds the dedicated
// RapidCheck property that exercises it across arbitrary valid projects.
//
// Strategy: an imperative RapidCheck generator (using the `*gen` draw operator)
// builds an arbitrary VALID project — a supported schema version, a positive
// timeline frame rate and canvas, a non-empty asset table, and tracks of clips
// whose assetRefs resolve into that table, with non-negative timeline positions,
// a positive source range (sourceOut > sourceIn), ordered effects carrying named
// scalar parameters (including full-range finite doubles to stress the numeric
// text encoding), an optional incoming transition, gain >= 0, and opacity in
// [0, 1]. Strings (project name, source paths, effect-parameter names) are drawn
// arbitrarily to exercise the JSON string escaping/round-tripping, including
// quotes, control characters, and non-ASCII bytes. Each generated project is
// serialized then deserialized, and the reconstructed project is asserted to be
// structurally equivalent to the original, field by field.
//
// _Requirements: 3.5_

#include "services/ProjectStore.hpp"

#include <cmath>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/ClipGroup.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/SchemaVersion.hpp"
#include "core/TextStyle.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {
namespace {

// ---------------------------------------------------------------------------
// Imperative generators (drawn with RapidCheck's `*gen` operator). Each helper
// draws fresh sub-values so generated values shrink and replay deterministically.
// ---------------------------------------------------------------------------

// A duration in whole nanoseconds within `[0, maxNs]`. Kept well inside int64
// range so downstream sums (e.g. sourceIn + length) never overflow, while the
// tick count still round-trips exactly through the store's integer encoding.
Duration drawDuration(std::int64_t maxNs) {
    return Duration::fromNanoseconds(*rc::gen::inRange<std::int64_t>(0, maxNs + 1));
}

// An arbitrary FINITE double. NaN/Inf are excluded because the `.palmier` numeric
// encoding (std::to_chars/std::from_chars shortest form) is defined for finite
// values; any finite double round-trips exactly through that encoding.
double drawFiniteDouble() {
    const double d = *rc::gen::arbitrary<double>();
    return std::isfinite(d) ? d : 0.0;
}

// A UUID built from 16 arbitrary bytes (so identities vary and shrink).
Uuid drawUuid() {
    Uuid::Bytes bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(*rc::gen::inRange<int>(0, 256));
    }
    return Uuid{bytes};
}

MediaAssetRef drawAssetRef() {
    MediaAssetRef ref;
    ref.assetId = drawUuid();
    ref.sourcePath = *rc::gen::arbitrary<std::string>();
    // Schema 1.4 addition (usable-editor tasks.md task 15): an arbitrary tag
    // list, including the empty default, must survive.
    const int tagCount = *rc::gen::inRange(0, 4);
    for (int i = 0; i < tagCount; ++i) {
        ref.tags.push_back(*rc::gen::arbitrary<std::string>());
    }
    return ref;
}

Effect drawEffect() {
    Effect fx;
    fx.id = drawUuid();
    fx.type = *rc::gen::element<EffectType>(
        EffectType::Brightness, EffectType::Contrast, EffectType::Blur,
        EffectType::CropTransform, EffectType::ColorGrade, EffectType::Custom);
    const int paramCount = *rc::gen::inRange<int>(0, 4);
    for (int i = 0; i < paramCount; ++i) {
        // Duplicate names simply collapse in the map; the round-trip holds either
        // way. Values are full-range finite doubles to stress numeric encoding.
        const std::string name = *rc::gen::arbitrary<std::string>();
        fx.parameters[name] = drawFiniteDouble();
    }
    return fx;
}

Transition drawTransition() {
    Transition t;
    t.id = drawUuid();
    t.kind = *rc::gen::element<TransitionKind>(
        TransitionKind::Crossfade, TransitionKind::DipToColor, TransitionKind::Wipe,
        TransitionKind::Slide, TransitionKind::Fade);
    // Transition regions are non-negative in a valid project.
    t.duration = drawDuration(60LL * Duration::kTicksPerSecond); // up to 60 s
    return t;
}

// Draw a clip whose assetRef resolves into the project's asset table (so the
// generated project satisfies the "every Clip.assetRef resolves in assets" rule).
Clip drawClip(const std::vector<MediaAssetRef>& assets) {
    Clip clip;
    clip.id = drawUuid();
    clip.assetRef =
        assets[static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(assets.size())))];

    // Non-negative timeline position and a positive source range
    // (sourceOut > sourceIn), matching the clip validation rules.
    constexpr std::int64_t kMaxPosNs = 100'000LL * Duration::kTicksPerSecond; // ~27.7 h
    constexpr std::int64_t kMaxLenNs = 3'600LL * Duration::kTicksPerSecond;   // up to 1 h
    const std::int64_t inNs = *rc::gen::inRange<std::int64_t>(0, kMaxPosNs + 1);
    const std::int64_t lenNs = *rc::gen::inRange<std::int64_t>(1, kMaxLenNs + 1);
    clip.timelineStart = drawDuration(kMaxPosNs);
    clip.sourceIn = Duration::fromNanoseconds(inNs);
    clip.sourceOut = Duration::fromNanoseconds(inNs + lenNs);

    const int effectCount = *rc::gen::inRange<int>(0, 4);
    for (int i = 0; i < effectCount; ++i) {
        clip.effects.push_back(drawEffect());
    }

    if (*rc::gen::arbitrary<bool>()) {
        clip.transitionIn = drawTransition();
    }

    // gain >= 0 and opacity in [0, 1]. Built from integer draws so the resulting
    // double is finite and in range; whatever double results round-trips exactly.
    clip.gain = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000.0;
    clip.opacity = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000'000.0;
    return clip;
}

// Draw a text clip (usable-editor task 12; Requirement 9): no assetRef at all
// (left at its default nil/"invalid" value, mirroring how AddClipCommand's own
// asset-registration step is written to treat exactly that), the same
// timeline-position/duration shape as drawClip, and a fully-populated
// TextStyle whose every field is drawn within TextStyle::isValid()'s own
// bounds, so the generated project satisfies ProjectValidation's text-clip
// rules alongside the ordinary media-clip ones drawClip already satisfies.
Clip drawTextClip() {
    Clip clip;
    clip.id = drawUuid();

    constexpr std::int64_t kMaxPosNs = 100'000LL * Duration::kTicksPerSecond;
    constexpr std::int64_t kMaxLenNs = 3'600LL * Duration::kTicksPerSecond;
    const std::int64_t lenNs = *rc::gen::inRange<std::int64_t>(1, kMaxLenNs + 1);
    clip.timelineStart = drawDuration(kMaxPosNs);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromNanoseconds(lenNs);

    clip.gain = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000.0;
    clip.opacity = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000'000.0;

    TextStyle style;
    style.content = *rc::gen::arbitrary<std::string>();
    style.fontFamily = *rc::gen::arbitrary<std::string>();
    // Point size strictly > 0 (TextStyle::isValid()); built from an integer draw
    // so the resulting double is finite and round-trips exactly, matching
    // gain/opacity's own construction above.
    style.pointSize = static_cast<double>(*rc::gen::inRange<int>(1, 1'000'001)) / 1'000.0;
    const auto drawUnit = [] {
        return static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000'000.0;
    };
    style.colorR = drawUnit();
    style.colorG = drawUnit();
    style.colorB = drawUnit();
    style.colorA = drawUnit();
    style.alignment = *rc::gen::element<TextAlignment>(
        TextAlignment::Left, TextAlignment::Center, TextAlignment::Right);
    style.x = drawUnit();
    style.y = drawUnit();
    clip.textStyle = std::move(style);
    return clip;
}

// Draw a caption cue (usable-editor task 13; Requirement 10): no assetRef, the
// same timeline-position/duration shape as drawClip/drawTextClip, and a plain
// non-empty captionText (ProjectValidation rejects an empty one) — mirroring
// drawTextClip exactly minus the style, since Requirement 10 asks for none.
Clip drawCaptionCue() {
    Clip clip;
    clip.id = drawUuid();

    constexpr std::int64_t kMaxPosNs = 100'000LL * Duration::kTicksPerSecond;
    constexpr std::int64_t kMaxLenNs = 3'600LL * Duration::kTicksPerSecond;
    const std::int64_t lenNs = *rc::gen::inRange<std::int64_t>(1, kMaxLenNs + 1);
    clip.timelineStart = drawDuration(kMaxPosNs);
    clip.sourceIn = Duration::zero();
    clip.sourceOut = Duration::fromNanoseconds(lenNs);

    clip.gain = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000.0;
    clip.opacity = static_cast<double>(*rc::gen::inRange<int>(0, 1'000'001)) / 1'000'000.0;

    // Non-empty: rc::gen::arbitrary<std::string>() can draw "", which
    // ProjectValidation rejects for captionText, so a non-empty suffix is
    // prepended to guarantee it — the string's OWN arbitrary content still
    // varies freely, including embedded newlines/unicode/etc.
    clip.captionText = "c" + *rc::gen::arbitrary<std::string>();
    return clip;
}

Track drawTrack(const std::vector<MediaAssetRef>& assets) {
    Track track;
    track.id = drawUuid();
    // Schema 1.3: "caption" joins "video"/"audio"/"text" (usable-editor task
    // 13; Requirement 10).
    track.kind = *rc::gen::element<TrackKind>(TrackKind::Video, TrackKind::Audio,
                                              TrackKind::Text, TrackKind::Caption);
    // Schema 1.1: an arbitrary label (including the empty default) must survive.
    track.name = *rc::gen::arbitrary<std::string>();
    track.muted = *rc::gen::arbitrary<bool>();
    track.locked = *rc::gen::arbitrary<bool>();
    const int clipCount = *rc::gen::inRange<int>(0, 6);
    for (int i = 0; i < clipCount; ++i) {
        // A text track's clips must all carry a TextStyle, a caption track's
        // clips must all carry captionText (and no other track's clips may
        // carry either); ProjectValidation enforces this both ways.
        if (track.kind == TrackKind::Text) {
            track.clips.push_back(drawTextClip());
        } else if (track.kind == TrackKind::Caption) {
            track.clips.push_back(drawCaptionCue());
        } else {
            track.clips.push_back(drawClip(assets));
        }
    }
    return track;
}

Project drawProject() {
    Project p;
    p.id = drawUuid();
    p.name = *rc::gen::arbitrary<std::string>();

    // A valid, positive rational frame rate. FrameRate normalizes (reduces) on
    // construction, so the reduced form is what serializes and round-trips.
    const std::int64_t num = *rc::gen::inRange<std::int64_t>(1, 240'001);
    const std::int64_t den = *rc::gen::inRange<std::int64_t>(1, 1'002);
    p.timelineFps = FrameRate{num, den};

    // A valid, strictly-positive canvas.
    p.canvas = Resolution{static_cast<std::uint32_t>(*rc::gen::inRange<int>(1, 8'193)),
                          static_cast<std::uint32_t>(*rc::gen::inRange<int>(1, 8'193))};

    p.colorSpace = *rc::gen::element<ColorSpace>(
        ColorSpace::Unknown, ColorSpace::Srgb, ColorSpace::Rec709, ColorSpace::Rec2020,
        ColorSpace::Rec2100Pq, ColorSpace::Rec2100Hlg, ColorSpace::DisplayP3,
        ColorSpace::LinearSrgb);

    // Schema version stays at the current, supported version (the only version
    // this build loads); reopening restores exactly what was written.
    p.version = SchemaVersion::current();

    // A non-empty asset table so clips always have a resolvable asset to point at.
    const int assetCount = *rc::gen::inRange<int>(1, 5);
    for (int i = 0; i < assetCount; ++i) {
        p.assets.push_back(drawAssetRef());
    }

    const int trackCount = *rc::gen::inRange<int>(0, 5);
    for (int i = 0; i < trackCount; ++i) {
        p.tracks.push_back(drawTrack(p.assets));
    }

    // Schema 1.1 reserves clipGroups. Nothing interprets it, so any set of group
    // identities and clip identities is a legal document; the store must return it
    // exactly as written.
    const int groupCount = *rc::gen::inRange<int>(0, 4);
    for (int g = 0; g < groupCount; ++g) {
        ClipGroup group;
        group.id = drawUuid();
        const int memberCount = *rc::gen::inRange<int>(0, 5);
        for (int m = 0; m < memberCount; ++m) {
            group.clipIds.push_back(drawUuid());
        }
        p.clipGroups.push_back(group);
    }
    return p;
}

// ---------------------------------------------------------------------------
// Structural equivalence checks (all clips, tracks, edits, media references).
// Finite doubles compare with exact equality: the shortest-form to_chars/
// from_chars round-trip recovers the identical double.
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

    // Schema 1.2 (usable-editor task 12; Requirement 9).
    RC_ASSERT(a.textStyle.has_value() == b.textStyle.has_value());
    if (a.textStyle.has_value()) {
        RC_ASSERT(a.textStyle->content == b.textStyle->content);
        RC_ASSERT(a.textStyle->fontFamily == b.textStyle->fontFamily);
        RC_ASSERT(a.textStyle->pointSize == b.textStyle->pointSize);
        RC_ASSERT(a.textStyle->colorR == b.textStyle->colorR);
        RC_ASSERT(a.textStyle->colorG == b.textStyle->colorG);
        RC_ASSERT(a.textStyle->colorB == b.textStyle->colorB);
        RC_ASSERT(a.textStyle->colorA == b.textStyle->colorA);
        RC_ASSERT(a.textStyle->alignment == b.textStyle->alignment);
        RC_ASSERT(a.textStyle->x == b.textStyle->x);
        RC_ASSERT(a.textStyle->y == b.textStyle->y);
    }
}

void assertSameProject(const Project& a, const Project& b) {
    RC_ASSERT(a.id == b.id);
    RC_ASSERT(a.name == b.name);
    RC_ASSERT(a.timelineFps == b.timelineFps);
    RC_ASSERT(a.canvas == b.canvas);
    RC_ASSERT(a.colorSpace == b.colorSpace);
    RC_ASSERT(a.version == b.version);

    RC_ASSERT(a.assets.size() == b.assets.size());
    for (std::size_t i = 0; i < a.assets.size(); ++i) {
        RC_ASSERT(a.assets[i].assetId == b.assets[i].assetId);
        RC_ASSERT(a.assets[i].sourcePath == b.assets[i].sourcePath);
    }

    RC_ASSERT(a.clipGroups.size() == b.clipGroups.size());
    for (std::size_t g = 0; g < a.clipGroups.size(); ++g) {
        RC_ASSERT(a.clipGroups[g].id == b.clipGroups[g].id);
        RC_ASSERT(a.clipGroups[g].clipIds == b.clipGroups[g].clipIds);
    }

    RC_ASSERT(a.tracks.size() == b.tracks.size());
    for (std::size_t t = 0; t < a.tracks.size(); ++t) {
        RC_ASSERT(a.tracks[t].id == b.tracks[t].id);
        RC_ASSERT(a.tracks[t].kind == b.tracks[t].kind);
        RC_ASSERT(a.tracks[t].name == b.tracks[t].name);
        RC_ASSERT(a.tracks[t].muted == b.tracks[t].muted);
        RC_ASSERT(a.tracks[t].locked == b.tracks[t].locked);
        RC_ASSERT(a.tracks[t].clips.size() == b.tracks[t].clips.size());
        for (std::size_t c = 0; c < a.tracks[t].clips.size(); ++c) {
            assertSameClip(a.tracks[t].clips[c], b.tracks[t].clips[c]);
        }
    }
}

// Feature: palmier-pro-linux, Property 11: Project persistence round-trip —
// serialize then deserialize yields an equivalent project (all clips, tracks,
// edits, and media references preserved).
// Validates: Requirements 3.5
RC_GTEST_PROP(ProjectStoreProperties,
              SerializeThenDeserializeYieldsEquivalentProject,
              ()) {
    const Project original = drawProject();

    const std::string text = serializeProject(original);
    Result<Project> loaded = deserializeProject(text);

    RC_ASSERT(loaded.isOk());
    assertSameProject(original, loaded.value());
}

}  // namespace
}  // namespace palmier::services
