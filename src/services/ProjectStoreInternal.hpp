// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStoreInternal.hpp - the internal contract shared by the units that
// implement the `.palmier` project store.
//
// The store is a single UTF-8 JSON document. To avoid adding a third-party JSON
// dependency (design.md Dependencies lists no JSON library), the store carries a
// small, self-contained JSON writer and a recursive-descent parser. They implement
// exactly the subset the `.palmier` format needs: objects, arrays, strings (with the
// standard escapes and \uXXXX), numbers (integers preserved losslessly by keeping the
// raw lexeme), booleans and null.
//
// Numbers that represent Duration ticks are int64 nanosecond counts; parsing them
// through the raw lexeme rather than a double keeps full 64-bit precision. Fractional
// values (gain/opacity) round-trip via std::to_chars/std::from_chars shortest form.
//
// These entities were an anonymous namespace inside a single 1082-line
// services/ProjectStore.cpp. Splitting that file by subject means they must cross
// translation units, and internal linkage cannot, so they now live in
// palmier::services::detail. Nothing here is part of the public surface: that remains
// services/ProjectStore.hpp, unchanged.

#ifndef PALMIER_SERVICES_PROJECTSTOREINTERNAL_HPP
#define PALMIER_SERVICES_PROJECTSTOREINTERNAL_HPP

#include "services/ProjectStoreJson.hpp"

#include "services/ProjectStore.hpp"
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "core/Clip.hpp"
#include "core/ClipGroup.hpp"
#include "core/ColorSpace.hpp"
#include "core/Duration.hpp"
#include "core/Effect.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Resolution.hpp"
#include "core/SchemaVersion.hpp"
#include "core/Track.hpp"
#include "core/Transition.hpp"
#include "core/Uuid.hpp"

namespace palmier::services::detail {

inline constexpr std::string_view kFormatMagic = "palmier-project";

std::string_view colorSpaceKey(ColorSpace cs);

std::optional<ColorSpace> colorSpaceFromKey(std::string_view k);

std::string_view trackKindKey(TrackKind k);

std::optional<TrackKind> trackKindFromKey(std::string_view k);

std::string_view effectTypeKey(EffectType t);

std::optional<EffectType> effectTypeFromKey(std::string_view k);

std::string_view transitionKindKey(TransitionKind k);

std::optional<TransitionKind> transitionKindFromKey(std::string_view k);

void writeUuid(JsonWriter& w, std::string_view field, const Uuid& id);

void writeDuration(JsonWriter& w, std::string_view field, Duration d);

void writeMediaAssetRef(JsonWriter& w, const MediaAssetRef& ref);

void writeEffect(JsonWriter& w, const Effect& fx);

void writeTransition(JsonWriter& w, const Transition& t);

std::string_view textAlignmentKey(TextAlignment a);

std::optional<TextAlignment> textAlignmentFromKey(std::string_view k);

void writeTextStyle(JsonWriter& w, const TextStyle& style);

void writeClip(JsonWriter& w, const Clip& clip);

void writeTrack(JsonWriter& w, const Track& track);

void writeClipGroup(JsonWriter& w, const ClipGroup& group);

void writeProject(JsonWriter& w, const Project& p);

const JsonValue& requireMember(const JsonValue& obj, std::string_view key);

const JsonValue& requireObject(const JsonValue& v, std::string_view what);

const JsonValue& requireArray(const JsonValue& v, std::string_view what);

std::string requireString(const JsonValue& obj, std::string_view key);

bool requireBool(const JsonValue& obj, std::string_view key);

std::int64_t parseInt64(const JsonValue& v, std::string_view key);

std::int64_t requireInt64(const JsonValue& obj, std::string_view key);

double parseDouble(const JsonValue& v, std::string_view what);

double requireDouble(const JsonValue& obj, std::string_view key);

Uuid requireUuid(const JsonValue& obj, std::string_view key);

Duration readDuration(const JsonValue& obj, std::string_view key);

std::string optionalString(const JsonValue& obj, std::string_view key, std::string fallback);

Uuid parseUuidString(const JsonValue& v, std::string_view what);

ClipGroup readClipGroup(const JsonValue& v);

std::vector<ClipGroup> readClipGroups(const JsonValue& projectObj);

MediaAssetRef readMediaAssetRef(const JsonValue& v);

Effect readEffect(const JsonValue& v);

Transition readTransition(const JsonValue& v);

TextStyle readTextStyle(const JsonValue& v);

Clip readClip(const JsonValue& v);

Track readTrack(const JsonValue& v);

Project readProject(const JsonValue& v);

}  // namespace palmier::services::detail

#endif  // PALMIER_SERVICES_PROJECTSTOREINTERNAL_HPP
