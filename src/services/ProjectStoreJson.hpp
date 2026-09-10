// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStoreJson.hpp - a minimal JSON value, recursive-descent parser and
// pretty-printing writer, sufficient for the `.palmier` document and nothing more.
//
// design.md lists no JSON library among the dependencies, so the store carries its
// own. This implements exactly the subset the format needs: objects, arrays, strings
// (standard escapes and \uXXXX), numbers, booleans and null. Integers are preserved
// losslessly by keeping the raw lexeme rather than going through a double, which
// matters because Duration ticks are int64 nanosecond counts.
//
// Kept apart from ProjectStoreInternal.hpp because nothing here knows anything about
// projects, clips or tracks - it is reusable JSON, and a reader looking for the format
// helpers should not have to scroll past it.

#ifndef PALMIER_SERVICES_PROJECTSTOREJSON_HPP
#define PALMIER_SERVICES_PROJECTSTOREJSON_HPP

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

[[noreturn]] void fail(std::string message);

struct JsonParseError {
    std::string message;
};

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string number;  ///< Raw numeric lexeme (integers preserved exactly).
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> members;  ///< Object entries.

    [[nodiscard]] bool isObject() const noexcept { return type == Type::Object; }
    [[nodiscard]] bool isArray() const noexcept { return type == Type::Array; }
    [[nodiscard]] bool isString() const noexcept { return type == Type::String; }
    [[nodiscard]] bool isNumber() const noexcept { return type == Type::Number; }
    [[nodiscard]] bool isBool() const noexcept { return type == Type::Bool; }
    [[nodiscard]] bool isNull() const noexcept { return type == Type::Null; }

    // Look up an object member by key; nullptr if absent (or not an object).
    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : members) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skipWs();
        JsonValue v = parseValue();
        skipWs();
        if (pos_ != text_.size()) fail("trailing content after JSON document");
        return v;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : text_[pos_]; }

    void skipWs() {
        while (!atEnd()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void expect(char c) {
        if (atEnd() || text_[pos_] != c) {
            fail(std::string{"expected '"} + c + "'");
        }
        ++pos_;
    }

    JsonValue parseValue() {
        skipWs();
        if (atEnd()) fail("unexpected end of input");
        switch (peek()) {
            case '{':
                return parseObject();
            case '[':
                return parseArray();
            case '"': {
                JsonValue v;
                v.type = JsonValue::Type::String;
                v.string = parseString();
                return v;
            }
            case 't':
            case 'f':
                return parseBool();
            case 'n':
                return parseNull();
            default:
                return parseNumber();
        }
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue v;
        v.type = JsonValue::Type::Object;
        skipWs();
        if (peek() == '}') {
            ++pos_;
            return v;
        }
        for (;;) {
            skipWs();
            if (peek() != '"') fail("expected string key in object");
            std::string key = parseString();
            skipWs();
            expect(':');
            JsonValue val = parseValue();
            v.members.emplace_back(std::move(key), std::move(val));
            skipWs();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                break;
            }
            fail("expected ',' or '}' in object");
        }
        return v;
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue v;
        v.type = JsonValue::Type::Array;
        skipWs();
        if (peek() == ']') {
            ++pos_;
            return v;
        }
        for (;;) {
            v.array.push_back(parseValue());
            skipWs();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                break;
            }
            fail("expected ',' or ']' in array");
        }
        return v;
    }

    JsonValue parseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) {
            v.boolean = true;
            pos_ += 4;
        } else if (text_.compare(pos_, 5, "false") == 0) {
            v.boolean = false;
            pos_ += 5;
        } else
            fail("invalid literal");
        return v;
    }

    JsonValue parseNull() {
        if (text_.compare(pos_, 4, "null") != 0) fail("invalid literal");
        pos_ += 4;
        JsonValue v;
        v.type = JsonValue::Type::Null;
        return v;
    }

    JsonValue parseNumber() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (!atEnd()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' ||
                c == 'E') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) fail("invalid number");
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.number = std::string{text_.substr(start, pos_ - start)};
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (!atEnd()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (atEnd()) fail("unterminated escape");
                const char e = text_[pos_++];
                switch (e) {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '/':
                        out += '/';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u':
                        out += parseUnicodeEscape();
                        break;
                    default:
                        fail("invalid string escape");
                }
            } else {
                out += c;
            }
        }
        fail("unterminated string");
    }

    // Decode a \uXXXX escape (and a following low surrogate, if present) to UTF-8.
    std::string parseUnicodeEscape() {
        auto hex4 = [this]() -> unsigned {
            if (pos_ + 4 > text_.size()) fail("truncated \\u escape");
            unsigned value = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = text_[pos_++];
                value <<= 4;
                if (c >= '0' && c <= '9')
                    value |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    value |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    value |= static_cast<unsigned>(c - 'A' + 10);
                else
                    fail("invalid hex digit in \\u escape");
            }
            return value;
        };

        unsigned cp = hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate; expect a following \uXXXX low surrogate.
            if (pos_ + 2 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                pos_ += 2;
                const unsigned low = hex4();
                if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
                fail("unpaired high surrogate");
            }
        }
        return encodeUtf8(cp);
    }

    static std::string encodeUtf8(unsigned cp) {
        std::string out;
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }
};

class JsonWriter {
public:
    [[nodiscard]] std::string take() { return std::move(out_); }

    void beginObject() {
        punctuateValue();
        out_ += "{\n";
        ++indent_;
        firstStack_.push_back(true);
    }
    void endObject() { closeContainer('}'); }
    void beginArray() {
        punctuateValue();
        out_ += "[\n";
        ++indent_;
        firstStack_.push_back(true);
    }
    void endArray() { closeContainer(']'); }

    void key(std::string_view k) {
        separateEntry();
        indentLine();
        writeQuoted(k);
        out_ += ": ";
        pendingKey_ = true;
    }

    void valueString(std::string_view s) {
        punctuateValue();
        writeQuoted(s);
    }
    void valueBool(bool b) {
        punctuateValue();
        out_ += b ? "true" : "false";
    }
    void valueRaw(std::string_view raw) {
        punctuateValue();
        out_ += raw;
    }
    void valueInt(std::int64_t v) {
        punctuateValue();
        out_ += std::to_string(v);
    }
    void valueUint(std::uint64_t v) {
        punctuateValue();
        out_ += std::to_string(v);
    }
    void valueNull() {
        punctuateValue();
        out_ += "null";
    }

    void valueDouble(double v) {
        punctuateValue();
        char buf[64];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (ec == std::errc{}) {
            out_.append(buf, ptr);
        } else {
            out_ += std::to_string(v);  // extremely unlikely fallback
        }
    }

private:
    std::string out_;
    int indent_ = 0;
    std::vector<bool> firstStack_;
    bool pendingKey_ = false;

    void indentLine() { out_.append(static_cast<std::size_t>(indent_) * 2, ' '); }

    // Called before writing a value: if the value follows a key, nothing to do
    // (the key already emitted separator/indent). Otherwise it's an array element,
    // which needs entry separation + indentation.
    void punctuateValue() {
        if (pendingKey_) {
            pendingKey_ = false;
            return;
        }
        separateEntry();
        indentLine();
    }

    // Emit ",\n" between sibling entries; track first-entry per container.
    void separateEntry() {
        if (!firstStack_.empty()) {
            if (firstStack_.back()) {
                firstStack_.back() = false;
            } else {
                out_ += ",\n";
            }
        }
    }

    void closeContainer(char close) {
        --indent_;
        firstStack_.pop_back();
        out_ += '\n';
        indentLine();
        out_ += close;
    }

    void writeQuoted(std::string_view s) {
        out_ += '"';
        for (const char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"':
                    out_ += "\\\"";
                    break;
                case '\\':
                    out_ += "\\\\";
                    break;
                case '\b':
                    out_ += "\\b";
                    break;
                case '\f':
                    out_ += "\\f";
                    break;
                case '\n':
                    out_ += "\\n";
                    break;
                case '\r':
                    out_ += "\\r";
                    break;
                case '\t':
                    out_ += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += ch;  // pass UTF-8 bytes through unescaped
                    }
            }
        }
        out_ += '"';
    }
};

}  // namespace palmier::services::detail

#endif  // PALMIER_SERVICES_PROJECTSTOREJSON_HPP
