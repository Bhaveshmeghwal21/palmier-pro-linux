// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/services/tool_schema_test.cpp — example-based unit tests for the single
// tool argument specification (task 3.1; Requirements 9.3, 9.9).
//
// design.md decision D3 ("Schema/handler agreement") requires that the JSON
// Schema `tools/list` publishes and the validator that runs before a tool
// executes be generated from one ArgSpec list, so they cannot drift. These tests
// pin both renderings of every constraint in the vocabulary:
//
//   * each JsonKind renders its draft-07 "type" and accepts/rejects the matching
//     JSON payload (an integer argument rejects a fractional number);
//   * required arguments appear in the schema's "required" array and their
//     absence is rejected, while an optional argument may be omitted;
//   * every bound — minimum/maximum for integers and numbers, minLength/maxLength
//     for strings and minItems/maxItems for arrays — is both published and
//     enforced, inclusively at the boundary;
//   * a closed value set is published as "enum" and non-members are rejected;
//   * a UUID argument publishes "format":"uuid" and rejects a non-canonical
//     string;
//   * "additionalProperties" is false and an undeclared member is rejected
//     (Requirement 9.9 — an unknown key must not slip through silently);
//   * toJsonSchema() is stable: repeated renderings of the same declaration are
//     byte-identical, and properties/required follow declaration order.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/Error.hpp"
#include "core/Uuid.hpp"
#include "services/Json.hpp"
#include "services/ToolSchema.hpp"

namespace palmier::services {
namespace {

// --- Helpers ---------------------------------------------------------------

/// The `properties` sub-object of a rendered schema.
const Json& properties(const Json& schema) {
    const Json* props = schema.find("properties");
    EXPECT_NE(props, nullptr);
    return *props;
}

/// The rendered property object for argument `name`.
const Json& property(const Json& schema, const char* name) {
    const Json* p = properties(schema).find(name);
    EXPECT_NE(p, nullptr) << "no rendered property named " << name;
    return *p;
}

/// The `required` array rendered as a vector of names.
std::vector<std::string> requiredNames(const Json& schema) {
    std::vector<std::string> names;
    const Json* required = schema.find("required");
    EXPECT_NE(required, nullptr);
    if (required != nullptr && required->isArray()) {
        for (const Json& entry : required->asArray()) names.push_back(entry.asString());
    }
    return names;
}

Json objectWith(std::initializer_list<Json::Member> members) {
    Json out = Json::object();
    for (const auto& [key, value] : members) out.set(key, value);
    return out;
}

const std::string kUuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";

// --- Kinds -----------------------------------------------------------------

TEST(ToolSchemaKinds, EachKindRendersItsDraft07TypeName) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "obj", .kind = JsonKind::Object})
        .arg(ArgSpec{.name = "arr", .kind = JsonKind::Array})
        .arg(ArgSpec{.name = "str", .kind = JsonKind::String})
        .arg(ArgSpec{.name = "i", .kind = JsonKind::Integer})
        .arg(ArgSpec{.name = "n", .kind = JsonKind::Number})
        .arg(ArgSpec{.name = "b", .kind = JsonKind::Bool});

    const Json rendered = schema.toJsonSchema();
    EXPECT_EQ(rendered.stringOr("type"), "object");
    EXPECT_EQ(property(rendered, "obj").stringOr("type"), "object");
    EXPECT_EQ(property(rendered, "arr").stringOr("type"), "array");
    EXPECT_EQ(property(rendered, "str").stringOr("type"), "string");
    EXPECT_EQ(property(rendered, "i").stringOr("type"), "integer");
    EXPECT_EQ(property(rendered, "n").stringOr("type"), "number");
    EXPECT_EQ(property(rendered, "b").stringOr("type"), "boolean");
}

TEST(ToolSchemaKinds, EachKindAcceptsItsOwnPayloadAndRejectsOthers) {
    struct Case {
        JsonKind kind;
        Json     accepted;
        Json     rejected;
    };
    const Case cases[] = {
        {JsonKind::Object, Json::object(), Json("x")},
        {JsonKind::Array, Json::array(), Json::object()},
        {JsonKind::String, Json("x"), Json(std::int64_t{1})},
        {JsonKind::Integer, Json(std::int64_t{7}), Json("7")},
        {JsonKind::Number, Json(1.5), Json(true)},
        {JsonKind::Bool, Json(true), Json(std::int64_t{1})},
    };

    for (const Case& c : cases) {
        ToolSchema schema;
        schema.arg(ArgSpec{.name = "value", .kind = c.kind});
        EXPECT_TRUE(schema.validate(objectWith({{"value", c.accepted}})).isOk())
            << "kind " << jsonKindName(c.kind) << " rejected its own payload";
        const Result<void> rejection = schema.validate(objectWith({{"value", c.rejected}}));
        ASSERT_TRUE(rejection.isError());
        EXPECT_EQ(rejection.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(rejection.error().message().find("value"), std::string::npos);
        EXPECT_NE(rejection.error().message().find(jsonKindName(c.kind)), std::string::npos);
    }
}

TEST(ToolSchemaKinds, IntegerRejectsAFractionalNumberButNumberAcceptsAnInteger) {
    ToolSchema integerArg;
    integerArg.arg(ArgSpec{.name = "ns", .kind = JsonKind::Integer});
    EXPECT_TRUE(integerArg.validate(objectWith({{"ns", Json(std::int64_t{5})}})).isOk());
    EXPECT_TRUE(integerArg.validate(objectWith({{"ns", Json(5.5)}})).isError());

    ToolSchema numberArg;
    numberArg.arg(ArgSpec{.name = "fps", .kind = JsonKind::Number});
    EXPECT_TRUE(numberArg.validate(objectWith({{"fps", Json(std::int64_t{30})}})).isOk());
    EXPECT_TRUE(numberArg.validate(objectWith({{"fps", Json(29.97)}})).isOk());
}

TEST(ToolSchemaKinds, NonObjectInputIsRejected) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "value", .kind = JsonKind::String});
    for (const Json& input : {Json(nullptr), Json("x"), Json::array(), Json(std::int64_t{1})}) {
        const Result<void> rejection = schema.validate(input);
        ASSERT_TRUE(rejection.isError());
        EXPECT_EQ(rejection.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(rejection.error().message().find("JSON object"), std::string::npos);
    }
}

TEST(ToolSchemaKinds, AnEmptySchemaAcceptsAnEmptyObject) {
    const ToolSchema schema;
    const Json rendered = schema.toJsonSchema();
    EXPECT_TRUE(properties(rendered).asObject().empty());
    EXPECT_TRUE(requiredNames(rendered).empty());
    EXPECT_TRUE(schema.validate(Json::object()).isOk());
}

// --- Required vs optional --------------------------------------------------

TEST(ToolSchemaRequired, RequiredArgumentsArePublishedAndEnforced) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "trackId", .kind = JsonKind::String, .required = true})
        .arg(ArgSpec{.name = "assetId", .kind = JsonKind::String, .required = true})
        .arg(ArgSpec{.name = "sourcePath", .kind = JsonKind::String});

    const Json rendered = schema.toJsonSchema();
    EXPECT_EQ(requiredNames(rendered), (std::vector<std::string>{"trackId", "assetId"}));

    EXPECT_TRUE(schema.validate(objectWith({{"trackId", Json("t")},
                                            {"assetId", Json("a")}}))
                    .isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"trackId", Json("t")},
                                            {"assetId", Json("a")},
                                            {"sourcePath", Json("/tmp/x.mp4")}}))
                    .isOk());

    const Result<void> missing = schema.validate(objectWith({{"trackId", Json("t")}}));
    ASSERT_TRUE(missing.isError());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(missing.error().message().find("assetId"), std::string::npos);
}

// --- Integer and number bounds --------------------------------------------

TEST(ToolSchemaBounds, IntegerBoundsArePublishedAndEnforcedInclusively) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "width",
                       .kind = JsonKind::Integer,
                       .required = true,
                       .minInt = 16,
                       .maxInt = 7680});

    const Json rendered = property(schema.toJsonSchema(), "width");
    EXPECT_EQ(rendered.intOr("minimum"), 16);
    EXPECT_EQ(rendered.intOr("maximum"), 7680);

    EXPECT_TRUE(schema.validate(objectWith({{"width", Json(std::int64_t{16})}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"width", Json(std::int64_t{7680})}})).isOk());

    const Result<void> low = schema.validate(objectWith({{"width", Json(std::int64_t{15})}}));
    ASSERT_TRUE(low.isError());
    EXPECT_EQ(low.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(low.error().message().find("width"), std::string::npos);
    EXPECT_NE(low.error().message().find("16"), std::string::npos);

    const Result<void> high = schema.validate(objectWith({{"width", Json(std::int64_t{7681})}}));
    ASSERT_TRUE(high.isError());
    EXPECT_EQ(high.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(high.error().message().find("7680"), std::string::npos);
}

TEST(ToolSchemaBounds, NumberBoundsArePublishedAndEnforcedInclusively) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "opacity",
                       .kind = JsonKind::Number,
                       .minNum = 0.0,
                       .maxNum = 1.0});

    const Json rendered = property(schema.toJsonSchema(), "opacity");
    EXPECT_DOUBLE_EQ(rendered.doubleOr("minimum", -1.0), 0.0);
    EXPECT_DOUBLE_EQ(rendered.doubleOr("maximum", -1.0), 1.0);

    EXPECT_TRUE(schema.validate(objectWith({{"opacity", Json(0.0)}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"opacity", Json(1.0)}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"opacity", Json(0.5)}})).isOk());

    const Result<void> low = schema.validate(objectWith({{"opacity", Json(-0.001)}}));
    ASSERT_TRUE(low.isError());
    EXPECT_EQ(low.error().code(), ErrorCode::OutOfRange);

    const Result<void> high = schema.validate(objectWith({{"opacity", Json(1.001)}}));
    ASSERT_TRUE(high.isError());
    EXPECT_EQ(high.error().code(), ErrorCode::OutOfRange);
}

TEST(ToolSchemaBounds, BoundsAreInertForKindsThatCannotCarryThem) {
    // A string argument carrying numeric bounds neither publishes them nor
    // enforces them, so the schema and the validator stay in agreement.
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "label",
                       .kind = JsonKind::String,
                       .minInt = 5,
                       .maxNum = 9.0});

    const Json rendered = property(schema.toJsonSchema(), "label");
    EXPECT_FALSE(rendered.contains("minimum"));
    EXPECT_FALSE(rendered.contains("maximum"));
    EXPECT_TRUE(schema.validate(objectWith({{"label", Json("anything")}})).isOk());
}

// --- Length bounds ---------------------------------------------------------

TEST(ToolSchemaLength, StringLengthBoundsArePublishedAndEnforced) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "name",
                       .kind = JsonKind::String,
                       .required = true,
                       .minLength = std::size_t{1},
                       .maxLength = std::size_t{4}});

    const Json rendered = property(schema.toJsonSchema(), "name");
    EXPECT_EQ(rendered.intOr("minLength"), 1);
    EXPECT_EQ(rendered.intOr("maxLength"), 4);

    EXPECT_TRUE(schema.validate(objectWith({{"name", Json("a")}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"name", Json("abcd")}})).isOk());

    const Result<void> tooShort = schema.validate(objectWith({{"name", Json("")}}));
    ASSERT_TRUE(tooShort.isError());
    EXPECT_EQ(tooShort.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(tooShort.error().message().find("name"), std::string::npos);

    const Result<void> tooLong = schema.validate(objectWith({{"name", Json("abcde")}}));
    ASSERT_TRUE(tooLong.isError());
    EXPECT_EQ(tooLong.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(tooLong.error().message().find("4"), std::string::npos);
}

TEST(ToolSchemaLength, ArrayLengthBoundsArePublishedAsItemCounts) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "order",
                       .kind = JsonKind::Array,
                       .required = true,
                       .minLength = std::size_t{1},
                       .maxLength = std::size_t{2}});

    const Json rendered = property(schema.toJsonSchema(), "order");
    EXPECT_EQ(rendered.intOr("minItems"), 1);
    EXPECT_EQ(rendered.intOr("maxItems"), 2);
    EXPECT_FALSE(rendered.contains("minLength"));

    EXPECT_TRUE(schema.validate(objectWith({{"order", Json::array({Json("a")})}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"order", Json::array({Json("a"), Json("b")})}}))
                    .isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"order", Json::array()}})).isError());
    EXPECT_TRUE(schema.validate(
                       objectWith({{"order", Json::array({Json("a"), Json("b"), Json("c")})}}))
                    .isError());
}

// --- Enum membership -------------------------------------------------------

TEST(ToolSchemaEnum, ClosedValueSetIsPublishedAndEnforced) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "kind",
                       .kind = JsonKind::String,
                       .required = true,
                       .enumValues = {"video", "audio"}});

    const Json rendered = property(schema.toJsonSchema(), "kind");
    const Json* values = rendered.find("enum");
    ASSERT_NE(values, nullptr);
    ASSERT_TRUE(values->isArray());
    ASSERT_EQ(values->asArray().size(), 2u);
    EXPECT_EQ(values->asArray()[0].asString(), "video");
    EXPECT_EQ(values->asArray()[1].asString(), "audio");

    EXPECT_TRUE(schema.validate(objectWith({{"kind", Json("video")}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"kind", Json("audio")}})).isOk());

    const Result<void> rejection = schema.validate(objectWith({{"kind", Json("subtitle")}}));
    ASSERT_TRUE(rejection.isError());
    EXPECT_EQ(rejection.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(rejection.error().message().find("video"), std::string::npos);
    EXPECT_NE(rejection.error().message().find("audio"), std::string::npos);
}

TEST(ToolSchemaEnum, MembershipIsCaseSensitive) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "codec",
                       .kind = JsonKind::String,
                       .enumValues = {"h264", "hevc", "vp9"}});
    EXPECT_TRUE(schema.validate(objectWith({{"codec", Json("hevc")}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"codec", Json("HEVC")}})).isError());
}

// --- UUID format -----------------------------------------------------------

TEST(ToolSchemaUuid, UuidFormatIsPublishedAndEnforced) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "clipId",
                       .kind = JsonKind::String,
                       .required = true,
                       .uuid = true});

    EXPECT_EQ(property(schema.toJsonSchema(), "clipId").stringOr("format"), "uuid");

    EXPECT_TRUE(schema.validate(objectWith({{"clipId", Json(kUuid)}})).isOk());
    EXPECT_TRUE(schema.validate(objectWith({{"clipId", Json(Uuid::generateV4().toString())}}))
                    .isOk());

    for (const char* invalid : {"", "not-a-uuid", "3f2504e0-4f89-41d3-9a0c-0305e82c330",
                                "3f2504e04f8941d39a0c0305e82c3301x"}) {
        const Result<void> rejection = schema.validate(objectWith({{"clipId", Json(invalid)}}));
        ASSERT_TRUE(rejection.isError()) << "accepted invalid UUID '" << invalid << "'";
        EXPECT_EQ(rejection.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(rejection.error().message().find("clipId"), std::string::npos);
    }
}

// --- Unknown keys ----------------------------------------------------------

TEST(ToolSchemaUnknownKeys, AdditionalPropertiesIsFalseAndUndeclaredMembersAreRejected) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "clipId", .kind = JsonKind::String, .required = true});

    const Json rendered = schema.toJsonSchema();
    const Json* additional = rendered.find("additionalProperties");
    ASSERT_NE(additional, nullptr);
    ASSERT_TRUE(additional->isBool());
    EXPECT_FALSE(additional->asBool());

    const Result<void> rejection = schema.validate(
        objectWith({{"clipId", Json("c")}, {"clipID", Json("typo")}}));
    ASSERT_TRUE(rejection.isError());
    EXPECT_EQ(rejection.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(rejection.error().message().find("clipID"), std::string::npos);
}

// --- Declaration list and stability ---------------------------------------

TEST(ToolSchemaDeclaration, ArgsPreserveDeclarationOrderAndRedeclarationReplacesInPlace) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "a", .kind = JsonKind::String})
        .arg(ArgSpec{.name = "b", .kind = JsonKind::Integer})
        .arg(ArgSpec{.name = "a", .kind = JsonKind::Integer, .required = true});

    ASSERT_EQ(schema.size(), 2u);
    EXPECT_EQ(schema.args()[0].name, "a");
    EXPECT_EQ(schema.args()[0].kind, JsonKind::Integer);
    EXPECT_TRUE(schema.args()[0].required);
    EXPECT_EQ(schema.args()[1].name, "b");
    ASSERT_NE(schema.find("b"), nullptr);
    EXPECT_EQ(schema.find("b")->kind, JsonKind::Integer);
    EXPECT_EQ(schema.find("missing"), nullptr);

    const Json rendered = schema.toJsonSchema();
    ASSERT_EQ(properties(rendered).asObject().size(), 2u);
    EXPECT_EQ(properties(rendered).asObject()[0].first, "a");
    EXPECT_EQ(properties(rendered).asObject()[1].first, "b");
}

TEST(ToolSchemaStability, RenderingIsDeterministicAndKeyOrderIsFixed) {
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "clipId",
                       .kind = JsonKind::String,
                       .required = true,
                       .description = "UUID of the clip to trim.",
                       .uuid = true})
        .arg(ArgSpec{.name = "boundaryNs",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "New source boundary in nanoseconds.",
                     .minInt = 0})
        .arg(ArgSpec{.name = "edge",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Which edge to trim.",
                     .enumValues = {"start", "end"}});

    const std::string first = schema.toJsonSchema().dump();
    const std::string second = schema.toJsonSchema().dump();
    EXPECT_EQ(first, second);
    EXPECT_EQ(schema.toJsonSchema(), schema.toJsonSchema());

    // Top-level key order matches the tool surface's existing hand-written
    // schemas, so task 3.2 can convert each tool without changing the published
    // payload's shape.
    const Json rendered = schema.toJsonSchema();
    const Json::Object& members = rendered.asObject();
    ASSERT_EQ(members.size(), 4u);
    EXPECT_EQ(members[0].first, "type");
    EXPECT_EQ(members[1].first, "properties");
    EXPECT_EQ(members[2].first, "required");
    EXPECT_EQ(members[3].first, "additionalProperties");

    // A property leads with "type" then "description", as `prop()` does today.
    const Json::Object& clipId = property(rendered, "clipId").asObject();
    ASSERT_GE(clipId.size(), 2u);
    EXPECT_EQ(clipId[0].first, "type");
    EXPECT_EQ(clipId[1].first, "description");

    // An omitted description is simply absent rather than rendered empty.
    ToolSchema terse;
    terse.arg(ArgSpec{.name = "value", .kind = JsonKind::Bool});
    const Json terseRendered = terse.toJsonSchema();
    EXPECT_FALSE(property(terseRendered, "value").contains("description"));

    // A rendered schema parses back to the identical value.
    const Result<Json> reparsed = Json::parse(first);
    ASSERT_TRUE(reparsed.isOk());
    EXPECT_EQ(reparsed.value(), rendered);
}

TEST(ToolSchemaStability, ARealisticToolDeclarationValidatesItsExpectedArguments) {
    // Shaped like `timeline.add_track` + `timeline.export` fragments so the
    // combined vocabulary is exercised on one schema.
    ToolSchema schema;
    schema.arg(ArgSpec{.name = "outputPath",
                       .kind = JsonKind::String,
                       .required = true,
                       .description = "Destination file path.",
                       .minLength = std::size_t{1},
                       .maxLength = std::size_t{4096}})
        .arg(ArgSpec{.name = "codec",
                     .kind = JsonKind::String,
                     .required = true,
                     .description = "Video codec.",
                     .enumValues = {"h264", "hevc", "vp9"}})
        .arg(ArgSpec{.name = "fps",
                     .kind = JsonKind::Number,
                     .required = true,
                     .description = "Output frame rate.",
                     .minNum = 1.0,
                     .maxNum = 120.0})
        .arg(ArgSpec{.name = "bitrateKbps",
                     .kind = JsonKind::Integer,
                     .required = true,
                     .description = "Video bit rate.",
                     .minInt = 100,
                     .maxInt = 200000})
        .arg(ArgSpec{.name = "trackId",
                     .kind = JsonKind::String,
                     .description = "Optional track UUID.",
                     .uuid = true})
        .arg(ArgSpec{.name = "overwrite",
                     .kind = JsonKind::Bool,
                     .description = "Overwrite acknowledgement."});

    const Json valid = objectWith({{"outputPath", Json("/tmp/out.mp4")},
                                   {"codec", Json("h264")},
                                   {"fps", Json(29.97)},
                                   {"bitrateKbps", Json(std::int64_t{8000})},
                                   {"trackId", Json(kUuid)},
                                   {"overwrite", Json(true)}});
    EXPECT_TRUE(schema.validate(valid).isOk());

    // Each single-field mutation is rejected for its own reason.
    Json badCodec = valid;
    badCodec.set("codec", Json("av1"));
    EXPECT_TRUE(schema.validate(badCodec).isError());

    Json badFps = valid;
    badFps.set("fps", Json(240.0));
    EXPECT_TRUE(schema.validate(badFps).isError());

    Json badBitrate = valid;
    badBitrate.set("bitrateKbps", Json(std::int64_t{99}));
    EXPECT_TRUE(schema.validate(badBitrate).isError());

    Json badTrack = valid;
    badTrack.set("trackId", Json("not-a-uuid"));
    EXPECT_TRUE(schema.validate(badTrack).isError());

    Json emptyPath = valid;
    emptyPath.set("outputPath", Json(""));
    EXPECT_TRUE(schema.validate(emptyPath).isError());

    Json extra = valid;
    extra.set("container", Json("mp4"));
    EXPECT_TRUE(schema.validate(extra).isError());
}

}  // namespace
}  // namespace palmier::services
