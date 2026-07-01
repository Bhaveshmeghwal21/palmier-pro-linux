// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/Json.hpp — a small, dependency-light JSON value for the service layer.
//
// The MCP server (design.md "Component 2: MCP Server") advertises editor tools
// with a JSON input schema and exchanges JSON tool arguments/results over HTTP,
// and the in-app agent (Component 6) drives the *same* tool handlers. To keep the
// tool surface transport-agnostic and buildable without pulling in Qt, FFmpeg, or
// a third-party JSON dependency into the domain-facing layer, this header defines
// the minimal JSON model those handlers need:
//
//   * a value that is one of null / bool / integer / double / string / array /
//     object (object members preserve insertion order so emitted schemas and
//     serialized timeline state read deterministically);
//   * exact 64-bit integers kept distinct from doubles, because timeline
//     positions are nanosecond tick counts (core::Duration) that can exceed the
//     53-bit exact range of a double;
//   * a permissive recursive-descent `parse` and a compact `dump` for the HTTP
//     transport (task 15.2) and for building JSON-Schema objects (task 15.1);
//   * small typed accessors (`at`, `find`, `stringOr`, `intOr`, ...) so tool
//     handlers can read arguments without hand-rolling type checks.
//
// This is intentionally not a full JSON Schema validator or a spec-perfect JSON
// parser — it is the least machinery required to express the tool surface and
// carry tool calls. It depends only on the standard library (and, for parse
// results, core/Result), so it compiles and unit-tests standalone on any
// platform.

#ifndef PALMIER_SERVICES_JSON_HPP
#define PALMIER_SERVICES_JSON_HPP

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Result.hpp"

namespace palmier::services {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    using Array  = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;  // insertion-ordered

    // --- Construction ------------------------------------------------------

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Int), int_(v) {}
    Json(std::int64_t v) : type_(Type::Int), int_(v) {}
    Json(double v) : type_(Type::Double), double_(v) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), string_(s) {}

    /// Named factories for the composite kinds.
    [[nodiscard]] static Json array() { Json j; j.type_ = Type::Array; return j; }
    [[nodiscard]] static Json array(Array items) {
        Json j; j.type_ = Type::Array; j.array_ = std::move(items); return j;
    }
    [[nodiscard]] static Json object() { Json j; j.type_ = Type::Object; return j; }
    [[nodiscard]] static Json object(std::initializer_list<Member> members) {
        Json j; j.type_ = Type::Object; j.object_.assign(members.begin(), members.end());
        return j;
    }

    // --- Type inspection ---------------------------------------------------

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isInt() const noexcept { return type_ == Type::Int; }
    [[nodiscard]] bool isDouble() const noexcept { return type_ == Type::Double; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Int || type_ == Type::Double; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    // --- Scalar access (unchecked; callers guard with the is* predicates) --

    [[nodiscard]] bool asBool() const noexcept { return bool_; }
    [[nodiscard]] std::int64_t asInt() const noexcept { return int_; }
    /// Numeric value as a double (promotes an integer payload).
    [[nodiscard]] double asDouble() const noexcept {
        return type_ == Type::Int ? static_cast<double>(int_) : double_;
    }
    [[nodiscard]] const std::string& asString() const noexcept { return string_; }

    [[nodiscard]] const Array& asArray() const noexcept { return array_; }
    [[nodiscard]] Array& asArray() noexcept { return array_; }
    [[nodiscard]] const Object& asObject() const noexcept { return object_; }
    [[nodiscard]] Object& asObject() noexcept { return object_; }

    // --- Array building ----------------------------------------------------

    /// Append `value` to an array (converts a Null value into an empty array).
    void push_back(Json value) {
        if (type_ != Type::Array) { type_ = Type::Array; array_.clear(); }
        array_.push_back(std::move(value));
    }

    // --- Object building / lookup -----------------------------------------

    /// Set object member `key` to `value` (converts a Null value into an empty
    /// object). Replaces an existing member with the same key, preserving order.
    Json& set(std::string key, Json value) {
        if (type_ != Type::Object) { type_ = Type::Object; object_.clear(); }
        for (Member& m : object_) {
            if (m.first == key) { m.second = std::move(value); return *this; }
        }
        object_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    /// Pointer to the member named `key`, or nullptr when absent / not an object.
    [[nodiscard]] const Json* find(std::string_view key) const noexcept {
        if (type_ != Type::Object) return nullptr;
        for (const Member& m : object_) {
            if (m.first == key) return &m.second;
        }
        return nullptr;
    }

    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        return find(key) != nullptr;
    }

    // --- Typed convenience accessors (with fallbacks) ----------------------

    /// String member `key`, or `fallback` when absent / not a string.
    [[nodiscard]] std::string stringOr(std::string_view key, std::string fallback = {}) const {
        const Json* m = find(key);
        return (m && m->isString()) ? m->string_ : std::move(fallback);
    }

    /// Integer member `key`, or `fallback` when absent / not numeric. A double
    /// payload is truncated toward zero.
    [[nodiscard]] std::int64_t intOr(std::string_view key, std::int64_t fallback = 0) const {
        const Json* m = find(key);
        if (!m) return fallback;
        if (m->type_ == Type::Int) return m->int_;
        if (m->type_ == Type::Double) return static_cast<std::int64_t>(m->double_);
        return fallback;
    }

    /// Number member `key` as a double, or `fallback` when absent / not numeric.
    [[nodiscard]] double doubleOr(std::string_view key, double fallback = 0.0) const {
        const Json* m = find(key);
        return (m && m->isNumber()) ? m->asDouble() : fallback;
    }

    /// Bool member `key`, or `fallback` when absent / not a bool.
    [[nodiscard]] bool boolOr(std::string_view key, bool fallback = false) const {
        const Json* m = find(key);
        return (m && m->isBool()) ? m->bool_ : fallback;
    }

    // --- Serialization / parsing ------------------------------------------

    /// Compact JSON text. `indent > 0` pretty-prints with that many spaces.
    [[nodiscard]] std::string dump(int indent = 0) const;

    /// Parse JSON text. Returns the value on success or an InvalidArgument Error
    /// describing the first problem encountered.
    [[nodiscard]] static Result<Json> parse(std::string_view text);

    [[nodiscard]] friend bool operator==(const Json& a, const Json& b);
    [[nodiscard]] friend bool operator!=(const Json& a, const Json& b) { return !(a == b); }

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type         type_ = Type::Null;
    bool         bool_ = false;
    std::int64_t int_ = 0;
    double       double_ = 0.0;
    std::string  string_;
    Array        array_;
    Object       object_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_JSON_HPP
