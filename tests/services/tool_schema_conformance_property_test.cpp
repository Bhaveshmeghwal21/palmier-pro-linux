// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/tool_schema_conformance_property_test.cpp — Property 50, the
// schema/handler agreement property (task 3.3; Requirements 9.12).
//
// Requirement 9.12: "FOR ALL tools in the Tool_Surface, the `tools/list` entry
// SHALL declare a schema that accepts the arguments the tool's own handler accepts
// and rejects arguments the handler rejects (schema conformance property)."
//
// design.md decision D3 ("Schema/handler agreement") makes this checkable rather
// than aspirational: every tool declares its arguments exactly once as a
// `ToolSchema`, `toJsonSchema()` renders what `tools/list` publishes and
// `validate()` enforces the identical constraint set before a handler ever runs.
// This property is the test that the two really do line up over generated
// argument objects, and it iterates whatever tools `buildDefaultToolRegistry`
// currently registers, so tools added later are covered without editing it.
//
// ---------------------------------------------------------------------------
// What "agree" can mean, and where it cannot
// ---------------------------------------------------------------------------
//
// Task 3.2 converted every tool and recorded, next to each declaration, the rules
// that genuinely cannot be expressed in the `ArgSpec` vocabulary. A property that
// asserted a naive `validate(args).isOk() == handlerAccepts(args)` would fail on
// those by construction and would say nothing about the rules the two sides do
// share. So this property compares like with like, in two directions, and every
// exclusion below is a *named* class with a reason rather than a silent skip:
//
//   Direction A (the direction that matters for an advertised surface):
//     whenever `validate()` ACCEPTS an argument object, the handler must not
//     reject it for an argument-shape reason. A rejection that depends on project
//     state (NotFound: no such clip/track; FailedPrecondition: overlap, no
//     interior playhead, sub-frame trim; OutOfRange: an engine invariant) is out
//     of scope — the schema cannot see the project — and so is `Unsupported` from
//     a tool whose backend hook is not wired in this build.
//
//   Direction B (the schema must not be laxer than the handler on shared rules):
//     whenever an argument object violates a rule BOTH sides express — a missing
//     required argument, a required argument of the wrong JSON type, a required
//     string outside its declared closed value set, a required id that is not a
//     canonical UUID — `validate()` must reject it AND the handler must reject it
//     too.
//
// The five documented gap classes, excluded from Direction A by name
// (`documentedGap` below), each with the reason the vocabulary cannot express it:
//
//   1. Cross-field relations. `timeline.add_clip`'s `sourceOutNs > sourceInNs`
//      and `generation.generate`'s `sourceOutTicks > sourceInTicks` relate two
//      arguments; `ArgSpec` constrains one argument at a time.
//   2. Array item shape. `timeline.reorder_clips.order` can be declared an array
//      (with an item count) but not "an array of canonical UUID strings", so the
//      handler still checks the items.
//   3. Object member value types. `timeline.add_effect.parameters` and
//      `generation.generate.params` are open maps; the vocabulary cannot
//      constrain an object's member VALUES.
//   4. State-dependent rules. "the id exists in the project", trim's one-frame
//      minimum, split's interior playhead, move's overlap rejection and reorder's
//      permutation check are all functions of the current project, which the
//      schema never sees. Note reorder's permutation check is the one
//      state-dependent rejection carrying `InvalidArgument`; it is recognised by
//      the core command name its message carries.
//   5. Open, backend-defined sets. `generation.generate.model` names a model the
//      configured backend serves; that set is not closed at compile time.
//
// Two places where the handler is currently MORE permissive than the declaration
// are likewise excluded from Direction B, because there the schema is the stricter
// of the two and the executor validates before dispatching, so the permissiveness
// is unobservable:
//
//   * `requireInt`/`Json::intOr` accept a fractional number and truncate it,
//     where a declared `JsonKind::Integer` rejects it;
//   * optional arguments read through `stringOr`/`intOr`/`doubleOr` silently
//     ignore a wrong-typed value, where the schema rejects it. Direction B is
//     therefore asserted over REQUIRED arguments only.
//
// A disagreement outside those classes is a real bug in the tool surface, and the
// property fails loudly with the tool name, the argument object and the handler's
// error rather than widening the exclusion list.
//
// Generator: for each registered tool, an argument object built from its own
// `ArgSpec` list (valid values, ids drawn from the fixture project so handlers
// reach their commands), then one perturbation drawn from: omit a required key,
// wrong JSON type, a value just inside / just outside every declared numeric or
// length bound, an out-of-enum string, a malformed UUID, an extra unknown key, a
// fractional payload for an integer argument, garbage array items, non-numeric
// object members.
//
// _Requirements: 9.12_

#include "services/ToolRegistry.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "core/Clip.hpp"
#include "core/Duration.hpp"
#include "core/Error.hpp"
#include "core/FrameRate.hpp"
#include "core/MediaAssetRef.hpp"
#include "core/Project.hpp"
#include "core/Resolution.hpp"
#include "core/Result.hpp"
#include "core/TimelineEngine.hpp"
#include "core/Track.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ProjectSession.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

// The tools whose documented gaps are named below. Any other tool — including one
// a later task registers — is held to the property with no exclusions.
constexpr const char* kAddClip  = "timeline.add_clip";
constexpr const char* kReorder  = "timeline.reorder_clips";
constexpr const char* kReorderEffects = "timeline.reorder_effects";
constexpr const char* kGenerate = "generation.generate";
constexpr const char* kSetProjectSettings = "project.set_settings";

// ---------------------------------------------------------------------------
// The `ArgSpec` type predicate, mirrored
//
// `ToolSchema::validate` decides a type match with exactly this rule (an Integer
// argument demands an exact integer payload; a Number argument takes either
// numeric payload). The property needs the same predicate to build wrong-typed
// values and to classify shared-rule violations, and stating it here keeps the
// mirroring visible: if the vocabulary's rule changes, this must change with it.
// ---------------------------------------------------------------------------

[[nodiscard]] bool matchesKind(const Json& value, JsonKind kind) {
    switch (kind) {
        case JsonKind::Object:  return value.isObject();
        case JsonKind::Array:   return value.isArray();
        case JsonKind::String:  return value.isString();
        case JsonKind::Integer: return value.isInt();
        case JsonKind::Number:  return value.isNumber();
        case JsonKind::Bool:    return value.isBool();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fixture project
//
// A project with two tracks, two non-overlapping clips and one asset, so a
// generated id sometimes names a real entity and the handlers get past their
// argument parsing into their commands (where a rejection is state-dependent and
// out of scope). The ids are pooled so the UUID generator can draw either a real
// one or a fresh one.
// ---------------------------------------------------------------------------

Project makeFixtureProject(std::vector<Uuid>& pool) {
    Project project;
    project.id = Uuid::generateV4();
    project.name = "Tool Schema Conformance";
    project.timelineFps = FrameRate::fps30();
    project.canvas = Resolution::hd1080();

    MediaAssetRef asset(Uuid::generateV4(), "/media/fixture.mp4");
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
        pool.push_back(clip.id);
        video.clips.push_back(std::move(clip));
    }

    Track audio;
    audio.id = Uuid::generateV4();
    audio.kind = TrackKind::Audio;

    pool.push_back(video.id);
    pool.push_back(audio.id);
    pool.push_back(asset.assetId);

    project.tracks.push_back(std::move(video));
    project.tracks.push_back(std::move(audio));
    return project;
}

// ---------------------------------------------------------------------------
// Generators (imperative, drawn with RapidCheck's `*gen` operator so cases shrink
// and replay deterministically).
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t drawIndex(std::size_t count) {
    return *rc::gen::inRange<std::size_t>(0, count);
}

[[nodiscard]] std::string drawUuidString(const std::vector<Uuid>& pool) {
    // Half the time a real entity of the fixture project, so the handler reaches
    // its command instead of stopping at NotFound.
    if (*rc::gen::arbitrary<bool>() && !pool.empty()) {
        return pool[drawIndex(pool.size())].toString();
    }
    return Uuid::generateV4().toString();
}

/// Text of a length inside the declared `[minLength, maxLength]` window (capped so
/// a 2000-character upper bound does not produce 2000-character cases).
[[nodiscard]] std::string drawText(const ArgSpec& spec) {
    const std::size_t low = spec.minLength.value_or(1);
    const std::size_t high = std::min<std::size_t>(spec.maxLength.value_or(low + 8), low + 8);
    const std::size_t length = low >= high ? low : *rc::gen::inRange<std::size_t>(low, high + 1);
    return std::string(length, 'x');
}

/// The declared numeric bounds of `spec` as doubles, honouring the same
/// integer/number fallback `ToolSchema` renders and enforces.
[[nodiscard]] std::optional<double> lowerBound(const ArgSpec& spec) {
    if (spec.kind == JsonKind::Integer || spec.kind == JsonKind::Number) {
        if (spec.minInt) return static_cast<double>(*spec.minInt);
        if (spec.minNum) return *spec.minNum;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> upperBound(const ArgSpec& spec) {
    if (spec.kind == JsonKind::Integer || spec.kind == JsonKind::Number) {
        if (spec.maxInt) return static_cast<double>(*spec.maxInt);
        if (spec.maxNum) return *spec.maxNum;
    }
    return std::nullopt;
}

/// A value satisfying every constraint `spec` declares.
[[nodiscard]] Json validValue(const ArgSpec& spec, const std::vector<Uuid>& pool) {
    switch (spec.kind) {
        case JsonKind::Object: {
            Json object = Json::object();
            const std::size_t count = *rc::gen::inRange<std::size_t>(0, 3);
            for (std::size_t i = 0; i < count; ++i) {
                object.set("p" + std::to_string(i),
                           Json(static_cast<double>(*rc::gen::inRange<int>(-100, 101)) / 10.0));
            }
            return object;
        }
        case JsonKind::Array: {
            Json items = Json::array();
            const std::size_t count = *rc::gen::inRange<std::size_t>(
                spec.minLength.value_or(0), std::min<std::size_t>(spec.maxLength.value_or(3), 3) + 1);
            for (std::size_t i = 0; i < count; ++i) items.push_back(Json(drawUuidString(pool)));
            return items;
        }
        case JsonKind::String: {
            if (!spec.enumValues.empty()) {
                return Json(spec.enumValues[drawIndex(spec.enumValues.size())]);
            }
            if (spec.uuid) return Json(drawUuidString(pool));
            return Json(drawText(spec));
        }
        case JsonKind::Integer: {
            const std::int64_t low = spec.minInt.value_or(
                lowerBound(spec) ? static_cast<std::int64_t>(std::ceil(*lowerBound(spec))) : -1'000);
            const std::int64_t high = spec.maxInt.value_or(
                upperBound(spec) ? static_cast<std::int64_t>(std::floor(*upperBound(spec)))
                                 : low + 2'000'000'000);
            return Json(low >= high ? low : *rc::gen::inRange<std::int64_t>(low, high));
        }
        case JsonKind::Number: {
            const double low = lowerBound(spec).value_or(-1'000.0);
            const double high = upperBound(spec).value_or(low + 1'000.0);
            const int steps = *rc::gen::inRange<int>(0, 101);
            return Json(low + (high - low) * (static_cast<double>(steps) / 100.0));
        }
        case JsonKind::Bool:
            return Json(*rc::gen::arbitrary<bool>());
    }
    return Json(nullptr);
}

/// An argument object carrying every required argument and a random subset of the
/// optional ones, all values valid: the base each perturbation starts from.
[[nodiscard]] Json drawValidArgs(const ToolSchema& schema, const std::vector<Uuid>& pool) {
    Json args = Json::object();
    for (const ArgSpec& spec : schema.args()) {
        if (!spec.required && !*rc::gen::arbitrary<bool>()) continue;
        args.set(spec.name, validValue(spec, pool));
    }
    return args;
}

/// A value that does NOT satisfy `kind` (including a fractional number for an
/// Integer argument, and JSON null, which satisfies no declared kind).
[[nodiscard]] Json drawWrongTypedValue(JsonKind kind) {
    const Json candidates[] = {Json(static_cast<std::int64_t>(7)), Json(1.5),  Json("7"),
                               Json(true),                        Json::array(), Json::object(),
                               Json(nullptr)};
    const std::size_t count = sizeof(candidates) / sizeof(candidates[0]);
    for (std::size_t attempt = 0; attempt < count; ++attempt) {
        const Json& candidate = candidates[(drawIndex(count) + attempt) % count];
        if (!matchesKind(candidate, kind)) return candidate;
    }
    return Json(nullptr);
}

/// Indices of the declared arguments present in `args` that satisfy `predicate`.
template <typename Predicate>
[[nodiscard]] std::vector<const ArgSpec*> presentArgs(const ToolSchema& schema, const Json& args,
                                                      Predicate predicate) {
    std::vector<const ArgSpec*> matches;
    for (const ArgSpec& spec : schema.args()) {
        if (args.contains(spec.name) && predicate(spec)) matches.push_back(&spec);
    }
    return matches;
}

enum class Perturbation {
    None,
    OmitRequired,
    WrongType,
    JustInsideLowerBound,
    JustOutsideLowerBound,
    JustInsideUpperBound,
    JustOutsideUpperBound,
    LengthJustOutside,
    OutOfEnum,
    MalformedUuid,
    UnknownKey,
    FractionalInteger,
    GarbageArrayItems,
    NonNumericObjectMembers,
};

/// Apply one drawn perturbation to `args`. A perturbation that does not apply to
/// the chosen tool (it declares no bounded argument, no enum, no array, ...) is a
/// no-op, which simply leaves a valid case — no case is discarded.
[[nodiscard]] Json perturb(const ToolSchema& schema, Json args, Perturbation kind) {
    const auto anyArg = [](const ArgSpec&) { return true; };

    switch (kind) {
        case Perturbation::None:
            return args;

        case Perturbation::OmitRequired: {
            const std::vector<const ArgSpec*> required =
                presentArgs(schema, args, [](const ArgSpec& s) { return s.required; });
            if (required.empty()) return args;
            const std::string dropped = required[drawIndex(required.size())]->name;
            Json rebuilt = Json::object();
            for (const Json::Member& member : args.asObject()) {
                if (member.first != dropped) rebuilt.set(member.first, member.second);
            }
            return rebuilt;
        }

        case Perturbation::WrongType: {
            const std::vector<const ArgSpec*> present = presentArgs(schema, args, anyArg);
            if (present.empty()) return args;
            const ArgSpec& spec = *present[drawIndex(present.size())];
            args.set(spec.name, drawWrongTypedValue(spec.kind));
            return args;
        }

        case Perturbation::JustInsideLowerBound:
        case Perturbation::JustOutsideLowerBound:
        case Perturbation::JustInsideUpperBound:
        case Perturbation::JustOutsideUpperBound: {
            const bool lower = kind == Perturbation::JustInsideLowerBound ||
                               kind == Perturbation::JustOutsideLowerBound;
            const bool outside = kind == Perturbation::JustOutsideLowerBound ||
                                 kind == Perturbation::JustOutsideUpperBound;
            const std::vector<const ArgSpec*> bounded =
                presentArgs(schema, args, [&](const ArgSpec& s) {
                    return lower ? lowerBound(s).has_value() : upperBound(s).has_value();
                });
            if (bounded.empty()) return args;
            const ArgSpec& spec = *bounded[drawIndex(bounded.size())];
            const double bound = lower ? *lowerBound(spec) : *upperBound(spec);
            if (spec.kind == JsonKind::Integer) {
                const std::int64_t at = static_cast<std::int64_t>(bound);
                args.set(spec.name, Json(outside ? (lower ? at - 1 : at + 1) : at));
            } else {
                args.set(spec.name, Json(outside ? (lower ? bound - 0.5 : bound + 0.5) : bound));
            }
            return args;
        }

        case Perturbation::LengthJustOutside: {
            const std::vector<const ArgSpec*> sized =
                presentArgs(schema, args, [](const ArgSpec& s) {
                    return (s.kind == JsonKind::String || s.kind == JsonKind::Array) &&
                           (s.minLength.has_value() || s.maxLength.has_value());
                });
            if (sized.empty()) return args;
            const ArgSpec& spec = *sized[drawIndex(sized.size())];
            const bool below = spec.minLength.has_value() && *spec.minLength > 0 &&
                               (!spec.maxLength.has_value() || *rc::gen::arbitrary<bool>());
            const std::size_t length = below ? *spec.minLength - 1 : *spec.maxLength + 1;
            if (spec.kind == JsonKind::String) {
                args.set(spec.name, Json(std::string(length, 'x')));
            } else {
                Json items = Json::array();
                for (std::size_t i = 0; i < length; ++i) {
                    items.push_back(Json(Uuid::generateV4().toString()));
                }
                args.set(spec.name, std::move(items));
            }
            return args;
        }

        case Perturbation::OutOfEnum: {
            const std::vector<const ArgSpec*> closed =
                presentArgs(schema, args, [](const ArgSpec& s) {
                    return s.kind == JsonKind::String && !s.enumValues.empty();
                });
            if (closed.empty()) return args;
            args.set(closed[drawIndex(closed.size())]->name, Json("definitely-not-a-member"));
            return args;
        }

        case Perturbation::MalformedUuid: {
            const std::vector<const ArgSpec*> ids = presentArgs(
                schema, args, [](const ArgSpec& s) { return s.kind == JsonKind::String && s.uuid; });
            if (ids.empty()) return args;
            const char* malformed[] = {"not-a-uuid", "", "0123456789abcdef",
                                       "123e4567-e89b-12d3-a456-42665544000",
                                       "123e4567e89b12d3a45642665544000g"};
            args.set(ids[drawIndex(ids.size())]->name, Json(malformed[drawIndex(5)]));
            return args;
        }

        case Perturbation::UnknownKey:
            args.set("definitelyNotAnArgument", Json(*rc::gen::arbitrary<bool>()));
            return args;

        case Perturbation::FractionalInteger: {
            const std::vector<const ArgSpec*> integers = presentArgs(
                schema, args, [](const ArgSpec& s) { return s.kind == JsonKind::Integer; });
            if (integers.empty()) return args;
            const ArgSpec& spec = *integers[drawIndex(integers.size())];
            const double base = static_cast<double>(args.intOr(spec.name, 0));
            args.set(spec.name, Json(base + 0.5));
            return args;
        }

        case Perturbation::GarbageArrayItems: {
            const std::vector<const ArgSpec*> arrays = presentArgs(
                schema, args, [](const ArgSpec& s) { return s.kind == JsonKind::Array; });
            if (arrays.empty()) return args;
            Json items = Json::array();
            const std::size_t count = *rc::gen::inRange<std::size_t>(1, 4);
            for (std::size_t i = 0; i < count; ++i) {
                items.push_back(*rc::gen::arbitrary<bool>() ? Json("not-a-uuid")
                                                            : Json(static_cast<std::int64_t>(3)));
            }
            args.set(arrays[drawIndex(arrays.size())]->name, std::move(items));
            return args;
        }

        case Perturbation::NonNumericObjectMembers: {
            const std::vector<const ArgSpec*> objects = presentArgs(
                schema, args, [](const ArgSpec& s) { return s.kind == JsonKind::Object; });
            if (objects.empty()) return args;
            Json members = Json::object();
            members.set("text", Json("not-a-number"));
            members.set("flag", Json(true));
            args.set(objects[drawIndex(objects.size())]->name, std::move(members));
            return args;
        }
    }
    return args;
}

[[nodiscard]] Perturbation drawPerturbation() {
    return *rc::gen::element(
        Perturbation::None, Perturbation::OmitRequired, Perturbation::WrongType,
        Perturbation::JustInsideLowerBound, Perturbation::JustOutsideLowerBound,
        Perturbation::JustInsideUpperBound, Perturbation::JustOutsideUpperBound,
        Perturbation::LengthJustOutside, Perturbation::OutOfEnum, Perturbation::MalformedUuid,
        Perturbation::UnknownKey, Perturbation::FractionalInteger,
        Perturbation::GarbageArrayItems, Perturbation::NonNumericObjectMembers);
}

// ---------------------------------------------------------------------------
// Direction B: the rules BOTH sides express
//
// A violation of one of these must be rejected by `validate()` and by the
// handler. Only REQUIRED arguments are considered: an optional argument is read
// through `stringOr`/`intOr`/`doubleOr`, which ignore a wrong-typed value and
// substitute the default, so there the schema is deliberately the stricter of the
// two (documented permissiveness, unobservable because the executor validates
// first). A fractional payload for an Integer argument is likewise skipped:
// `requireInt` accepts and truncates it.
// ---------------------------------------------------------------------------

enum class SharedRule { None, MissingRequired, WrongTypedRequired, OutOfEnumRequired, BadUuidRequired };

[[nodiscard]] const char* sharedRuleName(SharedRule rule) {
    switch (rule) {
        case SharedRule::None:               return "none";
        case SharedRule::MissingRequired:    return "a required argument is missing";
        case SharedRule::WrongTypedRequired: return "a required argument has the wrong JSON type";
        case SharedRule::OutOfEnumRequired:  return "a required argument is outside its closed value set";
        case SharedRule::BadUuidRequired:    return "a required id is not a canonical UUID";
    }
    return "none";
}

[[nodiscard]] SharedRule sharedRuleViolation(const ToolSchema& schema, const Json& args) {
    for (const ArgSpec& spec : schema.args()) {
        if (!spec.required) continue;
        const Json* value = args.find(spec.name);
        if (value == nullptr) return SharedRule::MissingRequired;
        if (!matchesKind(*value, spec.kind)) {
            // `requireInt`/`intOr` take a fractional number and truncate it.
            if (spec.kind == JsonKind::Integer && value->isNumber()) continue;
            return SharedRule::WrongTypedRequired;
        }
        if (spec.kind == JsonKind::String && !spec.enumValues.empty()) {
            bool member = false;
            for (const std::string& accepted : spec.enumValues) {
                if (accepted == value->asString()) member = true;
            }
            if (!member) return SharedRule::OutOfEnumRequired;
        }
        if (spec.kind == JsonKind::String && spec.uuid &&
            !Uuid::parse(value->asString()).has_value()) {
            return SharedRule::BadUuidRequired;
        }
    }
    return SharedRule::None;
}

// ---------------------------------------------------------------------------
// Direction A: the documented gap classes
//
// Called only when `validate()` accepted the arguments and the handler
// nevertheless rejected them with InvalidArgument. Returns the reason when the
// disagreement is one of the five classes task 3.2 documented as inexpressible in
// the `ArgSpec` vocabulary, and nullptr when it is not — in which case the
// property fails, because an undocumented disagreement means the advertised
// schema is lying about what the tool accepts.
// ---------------------------------------------------------------------------

[[nodiscard]] bool namesACoreCommand(const std::string& message) {
    // Every core EditCommand prefixes its message with its own name; the only
    // state-dependent InvalidArgument in the surface is ReorderClipsCommand's
    // permutation check ("order must be a permutation of the track's clips").
    return message.find("Command:") != std::string::npos ||
           message.find("AddTransition:") != std::string::npos ||
           message.find("TimelineEngine") != std::string::npos;
}

[[nodiscard]] bool allItemsAreUuidStrings(const Json& value) {
    if (!value.isArray()) return false;
    for (const Json& item : value.asArray()) {
        if (!item.isString() || !Uuid::parse(item.asString()).has_value()) return false;
    }
    return true;
}

[[nodiscard]] const char* documentedGap(const Tool& tool, const ToolSchema& schema,
                                        const Json& args, const Error& error) {
    // Class 4 — a state-dependent rule enforced by the core command.
    if (namesACoreCommand(error.message())) {
        return "class 4: a state-dependent rule the schema cannot see (the core "
               "command's own check, e.g. reorder's permutation rule)";
    }

    // Class 1 — a relation between two arguments.
    if (tool.name == kAddClip && args.intOr("sourceOutNs", 0) <= args.intOr("sourceInNs", 0)) {
        return "class 1: cross-field relation sourceOutNs > sourceInNs";
    }
    if (tool.name == kGenerate && args.contains("sourceOutTicks") &&
        args.intOr("sourceOutTicks", 0) <= args.intOr("sourceInTicks", 0)) {
        return "class 1: cross-field relation sourceOutTicks > sourceInTicks";
    }
    if (tool.name == kSetProjectSettings) {
        // Every argument is individually optional, so an empty object is
        // schema-valid on its own — but the handler additionally requires at
        // least one of fps/width-height/colorSpace, and requires width and
        // height together or not at all. Neither rule names one argument's own
        // range; both relate the PRESENCE of several arguments to each other.
        if (!args.contains("fps") && !args.contains("width") && !args.contains("height") &&
            !args.contains("colorSpace")) {
            return "class 1: cross-field relation — at least one of fps/width+height/"
                   "colorSpace must be present";
        }
        if (args.contains("width") != args.contains("height")) {
            return "class 1: cross-field relation — width and height must be present "
                   "together";
        }
    }

    // Class 2 — an array's item shape (only its item count is expressible).
    if (tool.name == kReorder || tool.name == kReorderEffects) {
        const Json* order = args.find("order");
        if (order != nullptr && !allItemsAreUuidStrings(*order)) {
            return "class 2: array item shape — 'order' can only be declared an array, "
                   "not an array of canonical UUID strings";
        }
    }

    // Class 3 — an object's member VALUE types.
    for (const ArgSpec& spec : schema.args()) {
        if (spec.kind != JsonKind::Object) continue;
        const Json* value = args.find(spec.name);
        if (value == nullptr || !value->isObject()) continue;
        for (const Json::Member& member : value->asObject()) {
            if (!member.second.isNumber()) {
                return "class 3: object member value types are not expressible "
                       "(add_effect.parameters / generate.params)";
            }
        }
    }

    // Class 5 — an open, backend-defined value set.
    if (tool.name == kGenerate && error.message().find("model") != std::string::npos) {
        return "class 5: 'model' names an open, backend-defined set";
    }

    return nullptr;
}

[[nodiscard]] std::string describe(const Tool& tool, const Json& args, const Error& error) {
    return "tool '" + tool.name + "' args " + args.dump() + " -> " + error.toString();
}

// ---------------------------------------------------------------------------
// The property.
// ---------------------------------------------------------------------------

// Feature: end-to-end-editor-integration, Property 50: The advertised schema and
// the handler agree — for all tools in the Tool_Surface and all argument objects,
// the tool's advertised `inputSchema` accepts the object exactly when the tool's
// own handler accepts it, that is, `ToolSchema::validate(args)` succeeds if and
// only if the handler does not reject `args` as an invalid argument (compared over
// the rules both sides can express: the five gap classes task 3.2 documented as
// inexpressible in the ArgSpec vocabulary — cross-field relations, array item
// shape, object member value types, state-dependent rules and open
// backend-defined sets — are excluded by name, each with its reason).
// Validates: Requirements 9.12
RC_GTEST_PROP(ToolSchemaConformanceProperties,
              TheAdvertisedSchemaAndTheHandlerAgree,
              ()) {
    // A fresh session per case: the handlers apply real EditCommands, so each case
    // must start from the same fixture project.
    std::vector<Uuid> pool;
    ProjectSession    session;
    RC_ASSERT(session.engine().reset(makeFixtureProject(pool)).isOk());

    // No hooks: `generation.generate` and `timeline.export` are backed by injected
    // hooks the composition root wires, and this binary wires none, so those two
    // answer Unsupported — a configuration rejection, never an argument-shape one.
    const ToolRegistry registry = buildDefaultToolRegistry(session);
    RC_ASSERT(registry.size() > 0);

    // Iterate whatever the registry contains, so tools added later are covered.
    const Tool& tool = registry.tools()[drawIndex(registry.size())];
    const ToolSchema& schema = tool.schema;

    const Perturbation perturbation = drawPerturbation();
    const Json args = perturb(schema, drawValidArgs(schema, pool), perturbation);

    const Result<void> validated = schema.validate(args);
    const Result<Json> executed = registry.invoke(tool.name, args);

    // --- Direction A -------------------------------------------------------
    // The schema accepted it, so the handler must not reject it as a malformed
    // argument. A state-dependent rejection (NotFound / FailedPrecondition /
    // OutOfRange from the engine) or an Unsupported from an unwired hook is out of
    // scope: neither is a statement about argument shape.
    if (validated.isOk() && executed.isError() &&
        executed.error().code() == ErrorCode::InvalidArgument) {
        const char* gap = documentedGap(tool, schema, args, executed.error());
        if (gap == nullptr) {
            RC_FAIL("schema/handler disagreement outside every documented gap class: " +
                    describe(tool, args, executed.error()));
        }
    }

    // --- Direction B -------------------------------------------------------
    // A rule both sides express is broken: the schema must reject it, and so must
    // the handler (with whatever code it chooses — it never gets far enough to
    // touch the project).
    const SharedRule violated = sharedRuleViolation(schema, args);
    if (violated != SharedRule::None) {
        RC_ASSERT_FALSE(validated.isOk());
        if (executed.isOk()) {
            RC_FAIL(std::string("the handler accepted arguments the schema rejects (") +
                    sharedRuleName(violated) + "): tool '" + tool.name + "' args " +
                    args.dump());
        }
    }

    // A schema-valid argument object never trips a shared rule: the two renderings
    // of the vocabulary are the same constraint set.
    if (validated.isOk()) {
        RC_ASSERT(sharedRuleViolation(schema, args) == SharedRule::None);
    }
}

}  // namespace
}  // namespace palmier::services
