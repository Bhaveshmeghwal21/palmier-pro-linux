// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStore.cpp — implementation of the documented `.palmier` project
// store declared in ProjectStore.hpp (Requirements 3.5, 3.6).
//
// The store is a single UTF-8 JSON document. To avoid adding a third-party JSON
// dependency (design.md Dependencies lists no JSON library), this file carries a
// small, self-contained JSON writer and a recursive-descent parser, both scoped to
// this translation unit. They implement exactly the subset the `.palmier` format
// needs: objects, arrays, strings (with the standard escapes and \uXXXX), numbers
// (integers are preserved losslessly by keeping the raw lexeme), booleans, and
// null.
//
// Numbers that represent Duration ticks are int64 nanosecond counts; parsing them
// through the raw lexeme (not a double) keeps full 64-bit precision. Fractional
// values (gain/opacity) round-trip via std::to_chars/std::from_chars shortest form.

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

namespace palmier::services {

namespace {

// The document "format" magic and the top-level field names.
constexpr std::string_view kFormatMagic = "palmier-project";

// ===========================================================================
// Minimal JSON value + recursive-descent parser (this TU only).
// ===========================================================================

// A parse failure carrying a human-readable message. Never escapes this file:
// deserializeProject() catches it and converts it to an ErrorCode::InvalidArgument
// Result, so exceptions are not part of the public API.
struct JsonParseError {
    std::string message;
};

[[noreturn]] void fail(std::string message) { throw JsonParseError{std::move(message)}; }

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string number; ///< Raw numeric lexeme (integers preserved exactly).
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> members; ///< Object entries.

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
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': {
                JsonValue v;
                v.type = JsonValue::Type::String;
                v.string = parseString();
                return v;
            }
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue v;
        v.type = JsonValue::Type::Object;
        skipWs();
        if (peek() == '}') { ++pos_; return v; }
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
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; break; }
            fail("expected ',' or '}' in object");
        }
        return v;
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue v;
        v.type = JsonValue::Type::Array;
        skipWs();
        if (peek() == ']') { ++pos_; return v; }
        for (;;) {
            v.array.push_back(parseValue());
            skipWs();
            const char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; break; }
            fail("expected ',' or ']' in array");
        }
        return v;
    }

    JsonValue parseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) { v.boolean = true; pos_ += 4; }
        else if (text_.compare(pos_, 5, "false") == 0) { v.boolean = false; pos_ += 5; }
        else fail("invalid literal");
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
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
                c == 'e' || c == 'E') {
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
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': out += parseUnicodeEscape(); break;
                    default: fail("invalid string escape");
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
                if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
                else fail("invalid hex digit in \\u escape");
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

// ===========================================================================
// JSON writer (pretty-printed, this TU only).
// ===========================================================================

class JsonWriter {
public:
    [[nodiscard]] std::string take() { return std::move(out_); }

    void beginObject() { punctuateValue(); out_ += "{\n"; ++indent_; firstStack_.push_back(true); }
    void endObject() { closeContainer('}'); }
    void beginArray() { punctuateValue(); out_ += "[\n"; ++indent_; firstStack_.push_back(true); }
    void endArray() { closeContainer(']'); }

    void key(std::string_view k) {
        separateEntry();
        indentLine();
        writeQuoted(k);
        out_ += ": ";
        pendingKey_ = true;
    }

    void valueString(std::string_view s) { punctuateValue(); writeQuoted(s); }
    void valueBool(bool b) { punctuateValue(); out_ += b ? "true" : "false"; }
    void valueRaw(std::string_view raw) { punctuateValue(); out_ += raw; }
    void valueInt(std::int64_t v) { punctuateValue(); out_ += std::to_string(v); }
    void valueUint(std::uint64_t v) { punctuateValue(); out_ += std::to_string(v); }
    void valueNull() { punctuateValue(); out_ += "null"; }

    void valueDouble(double v) {
        punctuateValue();
        char buf[64];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (ec == std::errc{}) {
            out_.append(buf, ptr);
        } else {
            out_ += std::to_string(v); // extremely unlikely fallback
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
        if (pendingKey_) { pendingKey_ = false; return; }
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
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\b': out_ += "\\b"; break;
                case '\f': out_ += "\\f"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += ch; // pass UTF-8 bytes through unescaped
                    }
            }
        }
        out_ += '"';
    }
};

// ===========================================================================
// Enum <-> stable string-key mappings.
// ===========================================================================

std::string_view colorSpaceKey(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Unknown:     return "unknown";
        case ColorSpace::Srgb:        return "srgb";
        case ColorSpace::Rec709:      return "rec709";
        case ColorSpace::Rec2020:     return "rec2020";
        case ColorSpace::Rec2100Pq:   return "rec2100pq";
        case ColorSpace::Rec2100Hlg:  return "rec2100hlg";
        case ColorSpace::DisplayP3:   return "displayp3";
        case ColorSpace::LinearSrgb:  return "linearsrgb";
    }
    return "unknown";
}

std::optional<ColorSpace> colorSpaceFromKey(std::string_view k) {
    if (k == "unknown") return ColorSpace::Unknown;
    if (k == "srgb") return ColorSpace::Srgb;
    if (k == "rec709") return ColorSpace::Rec709;
    if (k == "rec2020") return ColorSpace::Rec2020;
    if (k == "rec2100pq") return ColorSpace::Rec2100Pq;
    if (k == "rec2100hlg") return ColorSpace::Rec2100Hlg;
    if (k == "displayp3") return ColorSpace::DisplayP3;
    if (k == "linearsrgb") return ColorSpace::LinearSrgb;
    return std::nullopt;
}

std::string_view trackKindKey(TrackKind k) {
    return k == TrackKind::Audio ? "audio" : "video";
}

std::optional<TrackKind> trackKindFromKey(std::string_view k) {
    if (k == "video") return TrackKind::Video;
    if (k == "audio") return TrackKind::Audio;
    return std::nullopt;
}

// `invert_colors` is the schema-1.1 addition (Requirement 14.4). It is spelled in
// snake_case because that is the value the tool surface accepts for the same
// effect kind (see ToolRegistry's effect-type key); the four older keys keep their
// original camelCase spelling so 1.0 documents keep reading unchanged.
std::string_view effectTypeKey(EffectType t) {
    switch (t) {
        case EffectType::Brightness:    return "brightness";
        case EffectType::Contrast:      return "contrast";
        case EffectType::Blur:          return "blur";
        case EffectType::CropTransform: return "cropTransform";
        case EffectType::ColorGrade:    return "colorGrade";
        case EffectType::InvertColors:  return "invert_colors";
        case EffectType::Custom:        return "custom";
    }
    return "custom";
}

std::optional<EffectType> effectTypeFromKey(std::string_view k) {
    if (k == "brightness") return EffectType::Brightness;
    if (k == "contrast") return EffectType::Contrast;
    if (k == "blur") return EffectType::Blur;
    if (k == "cropTransform") return EffectType::CropTransform;
    if (k == "colorGrade") return EffectType::ColorGrade;
    if (k == "invert_colors") return EffectType::InvertColors;
    if (k == "custom") return EffectType::Custom;
    return std::nullopt;
}

std::string_view transitionKindKey(TransitionKind k) {
    switch (k) {
        case TransitionKind::Crossfade:  return "crossfade";
        case TransitionKind::DipToColor: return "dipToColor";
        case TransitionKind::Wipe:       return "wipe";
        case TransitionKind::Slide:      return "slide";
        case TransitionKind::Fade:       return "fade";
    }
    return "crossfade";
}

std::optional<TransitionKind> transitionKindFromKey(std::string_view k) {
    if (k == "crossfade") return TransitionKind::Crossfade;
    if (k == "dipToColor") return TransitionKind::DipToColor;
    if (k == "wipe") return TransitionKind::Wipe;
    if (k == "slide") return TransitionKind::Slide;
    if (k == "fade") return TransitionKind::Fade;
    return std::nullopt;
}

// ===========================================================================
// Serialization: Project -> JSON tree writer.
// ===========================================================================

void writeUuid(JsonWriter& w, std::string_view field, const Uuid& id) {
    w.key(field);
    w.valueString(id.toString());
}

void writeDuration(JsonWriter& w, std::string_view field, Duration d) {
    w.key(field);
    w.valueInt(d.ticks());
}

void writeMediaAssetRef(JsonWriter& w, const MediaAssetRef& ref) {
    w.beginObject();
    writeUuid(w, "assetId", ref.assetId);
    w.key("sourcePath");
    w.valueString(ref.sourcePath);
    w.endObject();
}

void writeEffect(JsonWriter& w, const Effect& fx) {
    w.beginObject();
    writeUuid(w, "id", fx.id);
    w.key("type");
    w.valueString(effectTypeKey(fx.type));
    w.key("parameters");
    w.beginObject();
    for (const auto& [name, value] : fx.parameters) {
        w.key(name);
        w.valueDouble(value);
    }
    w.endObject();
    w.endObject();
}

void writeTransition(JsonWriter& w, const Transition& t) {
    w.beginObject();
    writeUuid(w, "id", t.id);
    w.key("kind");
    w.valueString(transitionKindKey(t.kind));
    writeDuration(w, "duration", t.duration);
    w.endObject();
}

void writeClip(JsonWriter& w, const Clip& clip) {
    w.beginObject();
    writeUuid(w, "id", clip.id);
    w.key("assetRef");
    writeMediaAssetRef(w, clip.assetRef);
    writeDuration(w, "timelineStart", clip.timelineStart);
    writeDuration(w, "sourceIn", clip.sourceIn);
    writeDuration(w, "sourceOut", clip.sourceOut);
    w.key("effects");
    w.beginArray();
    for (const Effect& fx : clip.effects) writeEffect(w, fx);
    w.endArray();
    w.key("transitionIn");
    if (clip.transitionIn.has_value()) {
        writeTransition(w, *clip.transitionIn);
    } else {
        w.valueNull();
    }
    w.key("gain");
    w.valueDouble(clip.gain);
    w.key("opacity");
    w.valueDouble(clip.opacity);
    w.endObject();
}

void writeTrack(JsonWriter& w, const Track& track) {
    w.beginObject();
    writeUuid(w, "id", track.id);
    w.key("kind");
    w.valueString(trackKindKey(track.kind));
    // Schema 1.1: always written, optional on read (default "").
    w.key("name");
    w.valueString(track.name);
    w.key("muted");
    w.valueBool(track.muted);
    w.key("locked");
    w.valueBool(track.locked);
    w.key("clips");
    w.beginArray();
    for (const Clip& clip : track.clips) writeClip(w, clip);
    w.endArray();
    w.endObject();
}

void writeClipGroup(JsonWriter& w, const ClipGroup& group) {
    w.beginObject();
    writeUuid(w, "id", group.id);
    w.key("clipIds");
    w.beginArray();
    for (const ClipId& clipId : group.clipIds) w.valueString(clipId.toString());
    w.endArray();
    w.endObject();
}

void writeProject(JsonWriter& w, const Project& p) {
    w.beginObject();
    writeUuid(w, "id", p.id);
    w.key("name");
    w.valueString(p.name);
    w.key("timelineFps");
    w.beginObject();
    w.key("num");
    w.valueInt(p.timelineFps.numerator());
    w.key("den");
    w.valueInt(p.timelineFps.denominator());
    w.endObject();
    w.key("canvas");
    w.beginObject();
    w.key("width");
    w.valueUint(p.canvas.width);
    w.key("height");
    w.valueUint(p.canvas.height);
    w.endObject();
    w.key("colorSpace");
    w.valueString(colorSpaceKey(p.colorSpace));
    w.key("tracks");
    w.beginArray();
    for (const Track& track : p.tracks) writeTrack(w, track);
    w.endArray();
    w.key("assets");
    w.beginArray();
    for (const MediaAssetRef& asset : p.assets) writeMediaAssetRef(w, asset);
    w.endArray();
    // Schema 1.1: always written, optional on read (default []). Persisted
    // faithfully and interpreted by nothing — see core/ClipGroup.hpp.
    w.key("clipGroups");
    w.beginArray();
    for (const ClipGroup& group : p.clipGroups) writeClipGroup(w, group);
    w.endArray();
    w.endObject();
}

// ===========================================================================
// Deserialization helpers: JSON tree -> Project (throw JsonParseError on error).
// ===========================================================================

const JsonValue& requireMember(const JsonValue& obj, std::string_view key) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr) fail("missing required field '" + std::string{key} + "'");
    return *v;
}

const JsonValue& requireObject(const JsonValue& v, std::string_view what) {
    if (!v.isObject()) fail(std::string{what} + " must be an object");
    return v;
}

const JsonValue& requireArray(const JsonValue& v, std::string_view what) {
    if (!v.isArray()) fail(std::string{what} + " must be an array");
    return v;
}

std::string requireString(const JsonValue& obj, std::string_view key) {
    const JsonValue& v = requireMember(obj, key);
    if (!v.isString()) fail("field '" + std::string{key} + "' must be a string");
    return v.string;
}

bool requireBool(const JsonValue& obj, std::string_view key) {
    const JsonValue& v = requireMember(obj, key);
    if (!v.isBool()) fail("field '" + std::string{key} + "' must be a boolean");
    return v.boolean;
}

std::int64_t parseInt64(const JsonValue& v, std::string_view key) {
    if (!v.isNumber()) fail("field '" + std::string{key} + "' must be a number");
    std::int64_t out = 0;
    const char* begin = v.number.data();
    const char* end = v.number.data() + v.number.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        fail("field '" + std::string{key} + "' is not a valid integer");
    }
    return out;
}

std::int64_t requireInt64(const JsonValue& obj, std::string_view key) {
    return parseInt64(requireMember(obj, key), key);
}

double parseDouble(const JsonValue& v, std::string_view what) {
    if (!v.isNumber()) fail("field '" + std::string{what} + "' must be a number");
    double out = 0.0;
    const char* begin = v.number.data();
    const char* end = v.number.data() + v.number.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        fail("field '" + std::string{what} + "' is not a valid number");
    }
    return out;
}

double requireDouble(const JsonValue& obj, std::string_view key) {
    return parseDouble(requireMember(obj, key), key);
}

Uuid requireUuid(const JsonValue& obj, std::string_view key) {
    const std::string text = requireString(obj, key);
    std::optional<Uuid> id = Uuid::parse(text);
    if (!id.has_value()) fail("field '" + std::string{key} + "' is not a valid UUID");
    return *id;
}

Duration readDuration(const JsonValue& obj, std::string_view key) {
    return Duration::fromNanoseconds(requireInt64(obj, key));
}

// --- Schema 1.1 optional reads ---------------------------------------------
//
// Every field schema 1.1 added is read through one of these: absent (or JSON
// null) yields the documented default, so a 1.0 document loads unchanged; present
// but ill-typed is still a hard error, so a corrupt 1.1 document is not silently
// downgraded to defaults.

std::string optionalString(const JsonValue& obj, std::string_view key,
                           std::string fallback) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr || v->isNull()) return fallback;
    if (!v->isString()) fail("field '" + std::string{key} + "' must be a string");
    return v->string;
}

Uuid parseUuidString(const JsonValue& v, std::string_view what) {
    if (!v.isString()) fail(std::string{what} + " must be a string");
    const std::optional<Uuid> id = Uuid::parse(v.string);
    if (!id.has_value()) fail(std::string{what} + " is not a valid UUID");
    return *id;
}

ClipGroup readClipGroup(const JsonValue& v) {
    requireObject(v, "clipGroup");
    ClipGroup group;
    group.id = requireUuid(v, "id");
    const JsonValue& clipIds = requireArray(requireMember(v, "clipIds"), "clipGroup.clipIds");
    for (const JsonValue& c : clipIds.array) {
        group.clipIds.push_back(parseUuidString(c, "clipGroup.clipIds entry"));
    }
    return group;
}

std::vector<ClipGroup> readClipGroups(const JsonValue& projectObj) {
    const JsonValue* v = projectObj.find("clipGroups");
    if (v == nullptr || v->isNull()) return {}; // 1.0 default
    requireArray(*v, "clipGroups");
    std::vector<ClipGroup> groups;
    groups.reserve(v->array.size());
    for (const JsonValue& g : v->array) groups.push_back(readClipGroup(g));
    return groups;
}

MediaAssetRef readMediaAssetRef(const JsonValue& v) {
    requireObject(v, "assetRef");
    MediaAssetRef ref;
    ref.assetId = requireUuid(v, "assetId");
    ref.sourcePath = requireString(v, "sourcePath");
    return ref;
}

Effect readEffect(const JsonValue& v) {
    requireObject(v, "effect");
    Effect fx;
    fx.id = requireUuid(v, "id");
    const std::optional<EffectType> type = effectTypeFromKey(requireString(v, "type"));
    if (!type.has_value()) fail("unknown effect type");
    fx.type = *type;
    const JsonValue& params = requireObject(requireMember(v, "parameters"), "parameters");
    for (const auto& [name, pv] : params.members) {
        fx.parameters.emplace(name, parseDouble(pv, name));
    }
    return fx;
}

Transition readTransition(const JsonValue& v) {
    requireObject(v, "transitionIn");
    Transition t;
    t.id = requireUuid(v, "id");
    const std::optional<TransitionKind> kind = transitionKindFromKey(requireString(v, "kind"));
    if (!kind.has_value()) fail("unknown transition kind");
    t.kind = *kind;
    t.duration = readDuration(v, "duration");
    return t;
}

Clip readClip(const JsonValue& v) {
    requireObject(v, "clip");
    Clip clip;
    clip.id = requireUuid(v, "id");
    clip.assetRef = readMediaAssetRef(requireMember(v, "assetRef"));
    clip.timelineStart = readDuration(v, "timelineStart");
    clip.sourceIn = readDuration(v, "sourceIn");
    clip.sourceOut = readDuration(v, "sourceOut");

    const JsonValue& effects = requireArray(requireMember(v, "effects"), "clip.effects");
    for (const JsonValue& e : effects.array) clip.effects.push_back(readEffect(e));

    const JsonValue& transition = requireMember(v, "transitionIn");
    if (!transition.isNull()) clip.transitionIn = readTransition(transition);

    clip.gain = requireDouble(v, "gain");
    clip.opacity = requireDouble(v, "opacity");
    return clip;
}

Track readTrack(const JsonValue& v) {
    requireObject(v, "track");
    Track track;
    track.id = requireUuid(v, "id");
    const std::optional<TrackKind> kind = trackKindFromKey(requireString(v, "kind"));
    if (!kind.has_value()) fail("unknown track kind");
    track.kind = *kind;
    track.name = optionalString(v, "name", ""); // schema 1.1; default ""
    track.muted = requireBool(v, "muted");
    track.locked = requireBool(v, "locked");
    const JsonValue& clips = requireArray(requireMember(v, "clips"), "track.clips");
    for (const JsonValue& c : clips.array) track.clips.push_back(readClip(c));
    return track;
}

Project readProject(const JsonValue& v) {
    requireObject(v, "project");
    Project p;
    p.id = requireUuid(v, "id");
    p.name = requireString(v, "name");

    const JsonValue& fps = requireObject(requireMember(v, "timelineFps"), "timelineFps");
    p.timelineFps = FrameRate{requireInt64(fps, "num"), requireInt64(fps, "den")};

    const JsonValue& canvas = requireObject(requireMember(v, "canvas"), "canvas");
    const std::int64_t width = requireInt64(canvas, "width");
    const std::int64_t height = requireInt64(canvas, "height");
    if (width < 0 || height < 0) fail("canvas dimensions must be non-negative");
    p.canvas = Resolution{static_cast<std::uint32_t>(width),
                          static_cast<std::uint32_t>(height)};

    const std::optional<ColorSpace> cs = colorSpaceFromKey(requireString(v, "colorSpace"));
    if (!cs.has_value()) fail("unknown colorSpace");
    p.colorSpace = *cs;

    const JsonValue& tracks = requireArray(requireMember(v, "tracks"), "tracks");
    for (const JsonValue& t : tracks.array) p.tracks.push_back(readTrack(t));

    const JsonValue& assets = requireArray(requireMember(v, "assets"), "assets");
    for (const JsonValue& a : assets.array) p.assets.push_back(readMediaAssetRef(a));

    p.clipGroups = readClipGroups(v); // schema 1.1; default []

    return p;
}

} // namespace

std::string serializeProject(const Project& project) {
    JsonWriter w;
    w.beginObject();
    w.key("format");
    w.valueString(kFormatMagic);
    w.key("version");
    w.valueString(project.version.toString());
    w.key("project");
    writeProject(w, project);
    w.endObject();
    return w.take();
}

Result<Project> deserializeProject(std::string_view text) {
    try {
        JsonParser parser{text};
        const JsonValue root = parser.parse();
        if (!root.isObject()) {
            return invalidArgument("`.palmier` document root must be a JSON object");
        }

        // Verify the format magic so we don't misinterpret arbitrary JSON.
        const std::string format = requireString(root, "format");
        if (format != kFormatMagic) {
            return invalidArgument("not a `.palmier` document (unexpected format '" +
                                   format + "')");
        }

        // Read and enforce schema-version compatibility before building the project.
        const std::string versionText = requireString(root, "version");
        const std::optional<SchemaVersion> version = SchemaVersion::parse(versionText);
        if (!version.has_value()) {
            return invalidArgument("malformed schema version '" + versionText + "'");
        }
        if (!version->isSupported()) {
            return unsupported("`.palmier` schema version " + version->toString() +
                               " is not supported by this build (current " +
                               SchemaVersion::current().toString() + ")");
        }

        Project project = readProject(requireMember(root, "project"));
        project.version = *version;
        return project;
    } catch (const JsonParseError& e) {
        return invalidArgument("malformed `.palmier` document: " + e.message);
    }
}

Result<void> saveProjectToFile(const Project& project,
                               const std::filesystem::path& path) {
    const std::string text = serializeProject(project);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return makeError(ErrorCode::Io,
                         "could not open '" + path.string() + "' for writing");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        return makeError(ErrorCode::Io,
                         "failed while writing '" + path.string() + "'");
    }
    return ok();
}

Result<Project> loadProjectFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::Io,
                         "could not open '" + path.string() + "' for reading");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return makeError(ErrorCode::Io, "failed while reading '" + path.string() + "'");
    }
    return deserializeProject(buffer.str());
}

} // namespace palmier::services
