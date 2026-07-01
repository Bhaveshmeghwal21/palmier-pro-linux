// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/Json.cpp — serialization and a recursive-descent parser for the small
// service-layer JSON value declared in Json.hpp.

#include "services/Json.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace palmier::services {

namespace {

// --- Serialization helpers -------------------------------------------------

void appendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buf{};
                    std::snprintf(buf.data(), buf.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf.data();
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string formatDouble(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";  // JSON has no NaN/Inf; emit null defensively.
    }
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.17g", value);
    return std::string(buf.data());
}

void newlineIndent(std::string& out, int indent, int depth) {
    if (indent <= 0) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

// --- Parser ---------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<Json> parse() {
        skipWhitespace();
        Result<Json> value = parseValue();
        if (value.isError()) return value;
        skipWhitespace();
        if (pos_ != text_.size()) {
            return err<Json>(invalidArgument("Json::parse: trailing characters after value"));
        }
        return value;
    }

private:
    Result<Json> parseValue() {
        if (pos_ >= text_.size()) {
            return err<Json>(invalidArgument("Json::parse: unexpected end of input"));
        }
        const char c = text_[pos_];
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return parseString();
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
                return err<Json>(invalidArgument(
                    std::string("Json::parse: unexpected character '") + c + "'"));
        }
    }

    Result<Json> parseObject() {
        ++pos_;  // consume '{'
        Json obj = Json::object();
        skipWhitespace();
        if (peek() == '}') { ++pos_; return obj; }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return err<Json>(invalidArgument("Json::parse: expected string key in object"));
            }
            Result<Json> key = parseString();
            if (key.isError()) return key;
            skipWhitespace();
            if (peek() != ':') {
                return err<Json>(invalidArgument("Json::parse: expected ':' after object key"));
            }
            ++pos_;  // consume ':'
            skipWhitespace();
            Result<Json> value = parseValue();
            if (value.isError()) return value;
            obj.set(key.value().asString(), std::move(value.value()));
            skipWhitespace();
            const char sep = peek();
            if (sep == ',') { ++pos_; continue; }
            if (sep == '}') { ++pos_; break; }
            return err<Json>(invalidArgument("Json::parse: expected ',' or '}' in object"));
        }
        return obj;
    }

    Result<Json> parseArray() {
        ++pos_;  // consume '['
        Json arr = Json::array();
        skipWhitespace();
        if (peek() == ']') { ++pos_; return arr; }
        while (true) {
            skipWhitespace();
            Result<Json> value = parseValue();
            if (value.isError()) return value;
            arr.push_back(std::move(value.value()));
            skipWhitespace();
            const char sep = peek();
            if (sep == ',') { ++pos_; continue; }
            if (sep == ']') { ++pos_; break; }
            return err<Json>(invalidArgument("Json::parse: expected ',' or ']' in array"));
        }
        return arr;
    }

    Result<Json> parseString() {
        ++pos_;  // consume opening quote
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return Json(std::move(out));
            if (c == '\\') {
                if (pos_ >= text_.size()) break;
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        Result<void> u = decodeUnicodeEscape(out);
                        if (u.isError()) return err<Json>(std::move(u).error());
                        break;
                    }
                    default:
                        return err<Json>(invalidArgument(
                            "Json::parse: invalid string escape"));
                }
            } else {
                out.push_back(c);
            }
        }
        return err<Json>(invalidArgument("Json::parse: unterminated string"));
    }

    // Decode a \uXXXX escape (and a following low surrogate) into UTF-8.
    Result<void> decodeUnicodeEscape(std::string& out) {
        auto readHex4 = [&](unsigned& value) -> bool {
            if (pos_ + 4 > text_.size()) return false;
            value = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = text_[pos_++];
                value <<= 4;
                if (h >= '0' && h <= '9') value |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') value |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') value |= static_cast<unsigned>(h - 'A' + 10);
                else return false;
            }
            return true;
        };

        unsigned code = 0;
        if (!readHex4(code)) {
            return err(invalidArgument("Json::parse: invalid \\u escape"));
        }
        // Combine a UTF-16 surrogate pair when present.
        if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned low = 0;
            if (!readHex4(low)) {
                return err(invalidArgument("Json::parse: invalid low surrogate"));
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        // Encode as UTF-8.
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return ok();
    }

    Result<Json> parseBool() {
        if (text_.substr(pos_, 4) == "true") { pos_ += 4; return Json(true); }
        if (text_.substr(pos_, 5) == "false") { pos_ += 5; return Json(false); }
        return err<Json>(invalidArgument("Json::parse: invalid literal"));
    }

    Result<Json> parseNull() {
        if (text_.substr(pos_, 4) == "null") { pos_ += 4; return Json(nullptr); }
        return err<Json>(invalidArgument("Json::parse: invalid literal"));
    }

    Result<Json> parseNumber() {
        const std::size_t start = pos_;
        bool isDouble = false;
        if (peek() == '-') ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isDouble = true;
                ++pos_;
            } else {
                break;
            }
        }
        const std::string token(text_.substr(start, pos_ - start));
        if (token.empty() || token == "-") {
            return err<Json>(invalidArgument("Json::parse: malformed number"));
        }
        if (!isDouble) {
            // Prefer an exact 64-bit integer; fall back to double on overflow.
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end == token.c_str() + token.size()) {
                return Json(static_cast<std::int64_t>(v));
            }
        }
        char* end = nullptr;
        const double d = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) {
            return err<Json>(invalidArgument("Json::parse: malformed number"));
        }
        return Json(d);
    }

    [[nodiscard]] char peek() const noexcept {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    std::string_view text_;
    std::size_t      pos_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void Json::dumpTo(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Int:    out += std::to_string(int_); break;
        case Type::Double: out += formatDouble(double_); break;
        case Type::String: appendEscaped(out, string_); break;
        case Type::Array: {
            if (array_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (std::size_t i = 0; i < array_.size(); ++i) {
                if (i != 0) out.push_back(',');
                newlineIndent(out, indent, depth + 1);
                array_[i].dumpTo(out, indent, depth + 1);
            }
            newlineIndent(out, indent, depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (object_.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (std::size_t i = 0; i < object_.size(); ++i) {
                if (i != 0) out.push_back(',');
                newlineIndent(out, indent, depth + 1);
                appendEscaped(out, object_[i].first);
                out.push_back(':');
                if (indent > 0) out.push_back(' ');
                object_[i].second.dumpTo(out, indent, depth + 1);
            }
            newlineIndent(out, indent, depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Result<Json> Json::parse(std::string_view text) {
    Parser parser(text);
    return parser.parse();
}

// ---------------------------------------------------------------------------
// Equality
// ---------------------------------------------------------------------------

bool operator==(const Json& a, const Json& b) {
    if (a.type_ != b.type_) {
        // Treat equal-valued Int/Double as unequal only across representations;
        // callers comparing schemas/results use matching representations.
        return false;
    }
    switch (a.type_) {
        case Json::Type::Null:   return true;
        case Json::Type::Bool:   return a.bool_ == b.bool_;
        case Json::Type::Int:    return a.int_ == b.int_;
        case Json::Type::Double: return a.double_ == b.double_;
        case Json::Type::String: return a.string_ == b.string_;
        case Json::Type::Array:  return a.array_ == b.array_;
        case Json::Type::Object: return a.object_ == b.object_;
    }
    return false;
}

}  // namespace palmier::services
