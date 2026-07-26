// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolSchema.cpp — implementation of the single argument specification
// (task 3.1; Requirements 9.3, 9.9, 9.12).
//
// The whole point of this file is that every constraint is expressed once, by a
// helper that BOTH `toJsonSchema()` and `validate()` consult:
//
//   jsonKindName()  -> the "type" keyword and the type predicate (kindMatches)
//   minimumJson()   -> the "minimum" keyword and the lower-bound comparison
//   maximumJson()   -> the "maximum" keyword and the upper-bound comparison
//   lengthKeywords()-> "minLength"/"maxLength" vs "minItems"/"maxItems" and the
//                      measured length (string characters / array items)
//   enumApplies()   -> the "enum" keyword and the membership check
//   uuidApplies()   -> "format":"uuid" and the canonical-UUID parse
//
// A constraint that a helper reports as inapplicable to an argument's kind is
// neither rendered nor enforced, so the published schema and the validator accept
// exactly the same argument objects (Requirement 9.12).

#include "services/ToolSchema.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "core/Error.hpp"
#include "core/Uuid.hpp"

namespace palmier::services {
namespace {

// --- Shared constraint helpers ---------------------------------------------

/// True iff `value`'s JSON type satisfies `kind`. An `Integer` argument requires
/// an exact integer payload (a fractional number is rejected); a `Number`
/// argument accepts an integer or a double payload.
[[nodiscard]] bool kindMatches(const Json& value, JsonKind kind) noexcept {
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

/// The rendered/enforced lower bound, or nullopt when the argument declares none
/// applicable to its kind. An integer argument prefers `minInt` and falls back to
/// `minNum`; a number argument prefers `minNum` and falls back to `minInt`, so at
/// most one "minimum" keyword is ever produced.
[[nodiscard]] std::optional<Json> minimumJson(const ArgSpec& spec) {
    if (spec.kind == JsonKind::Integer) {
        if (spec.minInt) return Json(*spec.minInt);
        if (spec.minNum) return Json(*spec.minNum);
    } else if (spec.kind == JsonKind::Number) {
        if (spec.minNum) return Json(*spec.minNum);
        if (spec.minInt) return Json(static_cast<double>(*spec.minInt));
    }
    return std::nullopt;
}

/// The rendered/enforced upper bound; mirrors `minimumJson`.
[[nodiscard]] std::optional<Json> maximumJson(const ArgSpec& spec) {
    if (spec.kind == JsonKind::Integer) {
        if (spec.maxInt) return Json(*spec.maxInt);
        if (spec.maxNum) return Json(*spec.maxNum);
    } else if (spec.kind == JsonKind::Number) {
        if (spec.maxNum) return Json(*spec.maxNum);
        if (spec.maxInt) return Json(static_cast<double>(*spec.maxInt));
    }
    return std::nullopt;
}

/// The length keyword names for `kind`, or nullptr when length is inapplicable.
struct LengthKeywords {
    const char* minKeyword;
    const char* maxKeyword;
    const char* unit;  ///< for the rejection message: "characters" / "items"
};

[[nodiscard]] std::optional<LengthKeywords> lengthKeywords(JsonKind kind) noexcept {
    if (kind == JsonKind::String) return LengthKeywords{"minLength", "maxLength", "characters"};
    if (kind == JsonKind::Array)  return LengthKeywords{"minItems", "maxItems", "items"};
    return std::nullopt;
}

/// The measured length of `value` for the length constraint.
[[nodiscard]] std::size_t measuredLength(const Json& value, JsonKind kind) noexcept {
    if (kind == JsonKind::String) return value.asString().size();
    if (kind == JsonKind::Array)  return value.asArray().size();
    return 0;
}

/// True iff the closed value set applies to this argument (string kind only).
[[nodiscard]] bool enumApplies(const ArgSpec& spec) noexcept {
    return spec.kind == JsonKind::String && !spec.enumValues.empty();
}

/// True iff the canonical-UUID format applies to this argument (string kind only).
[[nodiscard]] bool uuidApplies(const ArgSpec& spec) noexcept {
    return spec.kind == JsonKind::String && spec.uuid;
}

/// "a, b, c" — the accepted values, for a rejection message.
[[nodiscard]] std::string joinValues(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ", ";
        out += values[i];
    }
    return out;
}

/// A rendered numeric bound as text, for a rejection message (an integer bound
/// reads "16", a fractional bound reads as its shortest round-trip form).
[[nodiscard]] std::string boundText(const Json& bound) { return bound.dump(); }

}  // namespace

// ---------------------------------------------------------------------------
// JsonKind
// ---------------------------------------------------------------------------

std::string_view jsonKindName(JsonKind kind) noexcept {
    switch (kind) {
        case JsonKind::Object:  return "object";
        case JsonKind::Array:   return "array";
        case JsonKind::String:  return "string";
        case JsonKind::Integer: return "integer";
        case JsonKind::Number:  return "number";
        case JsonKind::Bool:    return "boolean";
    }
    return "string";
}

// ---------------------------------------------------------------------------
// ToolSchema
// ---------------------------------------------------------------------------

ToolSchema& ToolSchema::arg(ArgSpec spec) {
    for (ArgSpec& existing : args_) {
        if (existing.name == spec.name) {
            existing = std::move(spec);
            return *this;
        }
    }
    args_.push_back(std::move(spec));
    return *this;
}

const ArgSpec* ToolSchema::find(std::string_view name) const noexcept {
    for (const ArgSpec& spec : args_) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

Json ToolSchema::toJsonSchema() const {
    Json properties = Json::object();
    Json required = Json::array();

    for (const ArgSpec& spec : args_) {
        Json property = Json::object();
        property.set("type", std::string(jsonKindName(spec.kind)));
        if (!spec.description.empty()) {
            property.set("description", spec.description);
        }
        if (enumApplies(spec)) {
            Json values = Json::array();
            for (const std::string& value : spec.enumValues) values.push_back(Json(value));
            property.set("enum", std::move(values));
        }
        if (uuidApplies(spec)) {
            property.set("format", "uuid");
        }
        if (std::optional<Json> minimum = minimumJson(spec); minimum.has_value()) {
            property.set("minimum", std::move(*minimum));
        }
        if (std::optional<Json> maximum = maximumJson(spec); maximum.has_value()) {
            property.set("maximum", std::move(*maximum));
        }
        if (std::optional<LengthKeywords> keywords = lengthKeywords(spec.kind);
            keywords.has_value()) {
            if (spec.minLength) {
                property.set(keywords->minKeyword,
                             Json(static_cast<std::int64_t>(*spec.minLength)));
            }
            if (spec.maxLength) {
                property.set(keywords->maxKeyword,
                             Json(static_cast<std::int64_t>(*spec.maxLength)));
            }
        }

        properties.set(spec.name, std::move(property));
        if (spec.required) required.push_back(Json(spec.name));
    }

    Json schema = Json::object();
    schema.set("type", "object");
    schema.set("properties", std::move(properties));
    schema.set("required", std::move(required));
    schema.set("additionalProperties", false);
    return schema;
}

Result<void> ToolSchema::validate(const Json& input) const {
    // The published schema is an object schema, so the input must be an object.
    if (!input.isObject()) {
        return err(invalidArgument("tool input must be a JSON object"));
    }

    // "required" — every required argument must be present.
    for (const ArgSpec& spec : args_) {
        if (spec.required && !input.contains(spec.name)) {
            return err(invalidArgument("missing required field '" + spec.name + "'"));
        }
    }

    // "additionalProperties": false — no undeclared member is accepted.
    for (const Json::Member& member : input.asObject()) {
        if (find(member.first) == nullptr) {
            return err(invalidArgument("unknown field '" + member.first +
                                       "' is not accepted by this tool"));
        }
    }

    // Per-argument constraints, in declaration order.
    for (const ArgSpec& spec : args_) {
        const Json* value = input.find(spec.name);
        if (value == nullptr) {
            continue;  // absence of an optional argument is accepted
        }

        // "type"
        if (!kindMatches(*value, spec.kind)) {
            return err(invalidArgument("field '" + spec.name + "' must be of type " +
                                       std::string(jsonKindName(spec.kind))));
        }

        // "enum"
        if (enumApplies(spec)) {
            const bool member = std::find(spec.enumValues.begin(), spec.enumValues.end(),
                                          value->asString()) != spec.enumValues.end();
            if (!member) {
                return err(invalidArgument("field '" + spec.name + "' must be one of: " +
                                           joinValues(spec.enumValues)));
            }
        }

        // "format": "uuid"
        if (uuidApplies(spec) && !Uuid::parse(value->asString()).has_value()) {
            return err(invalidArgument("field '" + spec.name + "' is not a valid UUID"));
        }

        // "minimum" / "maximum"
        if (std::optional<Json> minimum = minimumJson(spec);
            minimum.has_value() && value->asDouble() < minimum->asDouble()) {
            return err(outOfRange("field '" + spec.name + "' must be >= " +
                                  boundText(*minimum)));
        }
        if (std::optional<Json> maximum = maximumJson(spec);
            maximum.has_value() && value->asDouble() > maximum->asDouble()) {
            return err(outOfRange("field '" + spec.name + "' must be <= " +
                                  boundText(*maximum)));
        }

        // "minLength"/"maxLength" (strings) or "minItems"/"maxItems" (arrays)
        if (std::optional<LengthKeywords> keywords = lengthKeywords(spec.kind);
            keywords.has_value()) {
            const std::size_t length = measuredLength(*value, spec.kind);
            if (spec.minLength && length < *spec.minLength) {
                return err(outOfRange("field '" + spec.name + "' must have at least " +
                                      std::to_string(*spec.minLength) + " " +
                                      keywords->unit));
            }
            if (spec.maxLength && length > *spec.maxLength) {
                return err(outOfRange("field '" + spec.name + "' must have at most " +
                                      std::to_string(*spec.maxLength) + " " +
                                      keywords->unit));
            }
        }
    }

    return ok();
}

}  // namespace palmier::services
