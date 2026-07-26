// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ToolSchema.hpp — one argument specification per tool, rendered as the
// JSON Schema `tools/list` publishes *and* enforced as the validator that runs
// before a tool executes (task 3.1; Requirements 9.3, 9.9, 9.12).
//
// design.md decision D3, "Schema/handler agreement (Requirement 9.12)": the
// advertised schema and the runtime validator must accept exactly the same
// argument objects. The mechanism is that they are not written twice. A tool
// declares each argument once as an `ArgSpec`; `ToolSchema::toJsonSchema()`
// renders the draft-07 object schema from that list and `ToolSchema::validate()`
// enforces the identical constraint set from the same list, so the two cannot
// drift. Requirement 9.12 is the property `validate(args).isOk() ==
// handlerAccepts(args)` over generated inputs; the enforcement mechanism is that
// any acceptance rule a handler needs must first become expressible here, in the
// `ArgSpec` vocabulary, rather than living privately inside a handler.
//
// The constraint vocabulary, and how each part appears on both sides:
//
//   kind                 -> "type": object|array|string|integer|number|boolean;
//                           an integer argument rejects a fractional JSON number,
//                           a number argument accepts either numeric payload.
//   required             -> membership of the schema's "required" array.
//   description          -> "description" (omitted when empty).
//   minInt/maxInt        -> "minimum"/"maximum" for an integer argument.
//   minNum/maxNum        -> "minimum"/"maximum" for a number argument (they also
//                           serve an integer argument that declares no integer
//                           bound, and vice versa, so exactly one "minimum" and
//                           one "maximum" key is ever emitted).
//   minLength/maxLength  -> "minLength"/"maxLength" for a string argument;
//                           "minItems"/"maxItems" for an array argument.
//   enumValues           -> "enum" for a string argument (a closed value set).
//   uuid                 -> "format": "uuid"; the value must parse as a canonical
//                           UUID string (core::Uuid::parse).
//
// Constraints that do not apply to an argument's kind are inert on both sides:
// they are neither rendered nor enforced, so the two remain in agreement.
//
// The rendered object schema always carries `"additionalProperties": false` and
// `validate()` correspondingly rejects any member the argument list does not
// declare, closing the silent-unknown-key hole Requirement 9.9 relies on.
//
// Transport-agnostic and dependency-light: this depends only on core (Result,
// Error, Uuid) and the service-layer `Json` value, so the tool surface, the MCP
// protocol handler and the in-app agent all share one definition.

#ifndef PALMIER_SERVICES_TOOLSCHEMA_HPP
#define PALMIER_SERVICES_TOOLSCHEMA_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "services/Json.hpp"

namespace palmier::services {

// ---------------------------------------------------------------------------
// JsonKind
// ---------------------------------------------------------------------------

/// The JSON type an argument accepts. `Integer` and `Number` are distinguished
/// because timeline positions are exact nanosecond tick counts: an integer
/// argument rejects a fractional payload, while a number argument accepts both an
/// integer and a double payload.
enum class JsonKind { Object, Array, String, Integer, Number, Bool };

/// The draft-07 `"type"` keyword name for `kind` ("boolean" for `Bool`).
[[nodiscard]] std::string_view jsonKindName(JsonKind kind) noexcept;

// ---------------------------------------------------------------------------
// ArgSpec
// ---------------------------------------------------------------------------

/// One tool argument, declared exactly once. Both the advertised schema and the
/// pre-execution validator are generated from this value.
struct ArgSpec {
    std::string  name;
    JsonKind     kind = JsonKind::String;
    bool         required = false;
    std::string  description;
    std::optional<std::int64_t> minInt, maxInt;
    std::optional<double>       minNum, maxNum;
    std::optional<std::size_t>  minLength, maxLength;
    std::vector<std::string>    enumValues;   ///< closed value set (string kind)
    bool         uuid = false;                ///< canonical UUID string
};

// ---------------------------------------------------------------------------
// ToolSchema
// ---------------------------------------------------------------------------

/// An ordered list of `ArgSpec`s with two renderings of the same constraints:
/// the published JSON Schema and the runtime validator.
class ToolSchema {
public:
    ToolSchema() = default;

    /// Declare an argument. A later declaration replaces an earlier one with the
    /// same name, keeping the original position so rendering stays stable.
    ToolSchema& arg(ArgSpec spec);

    /// The draft-07 object schema published by `tools/list` (Requirement 9.3):
    /// `{"type":"object","properties":{...},"required":[...],
    ///   "additionalProperties":false}`, with properties in declaration order and
    /// `required` listing the required arguments in declaration order.
    [[nodiscard]] Json toJsonSchema() const;

    /// Enforce exactly the constraint set `toJsonSchema()` publishes
    /// (Requirement 9.9). Checked in order: the input is an object; every
    /// required argument is present; no undeclared member is present; then, in
    /// declaration order, each present argument's type, bounds, length, enum
    /// membership and UUID format.
    [[nodiscard]] Result<void> validate(const Json& input) const;

    /// The declared arguments in declaration order.
    [[nodiscard]] const std::vector<ArgSpec>& args() const noexcept { return args_; }

    /// The `ArgSpec` named `name`, or nullptr when undeclared.
    [[nodiscard]] const ArgSpec* find(std::string_view name) const noexcept;

    /// Number of declared arguments.
    [[nodiscard]] std::size_t size() const noexcept { return args_.size(); }

private:
    std::vector<ArgSpec> args_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_TOOLSCHEMA_HPP
