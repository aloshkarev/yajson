/// @file test_parser.cpp
/// @brief Unit tests for yajson::parse() — JSON parser.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Primitive types
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, Null) {
    auto v = parse("null");
    EXPECT_TRUE(v.is_null());
}

TEST(Parser, True) {
    auto v = parse("true");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), true);
}

TEST(Parser, False) {
    auto v = parse("false");
    EXPECT_TRUE(v.is_bool());
    EXPECT_EQ(v.as_bool(), false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Numbers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, IntegerZero) {
    auto v = parse("0");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), 0);
}

TEST(Parser, PositiveInteger) {
    auto v = parse("42");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), 42);
}

TEST(Parser, NegativeInteger) {
    auto v = parse("-100");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), -100);
}

TEST(Parser, LargeInteger) {
    auto v = parse("9223372036854775807"); // INT64_MAX
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), std::numeric_limits<int64_t>::max());
}

TEST(Parser, FloatSimple) {
    auto v = parse("3.14");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 3.14);
}

TEST(Parser, FloatNegative) {
    auto v = parse("-0.5");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), -0.5);
}

TEST(Parser, FloatWithExponent) {
    auto v = parse("1.5e10");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 1.5e10);
}

TEST(Parser, FloatWithNegativeExponent) {
    auto v = parse("2.5e-3");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 2.5e-3);
}

TEST(Parser, FloatWithPositiveExponent) {
    auto v = parse("1E+5");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 1e5);
}

TEST(Parser, FloatZero) {
    auto v = parse("0.0");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Strings
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, EmptyString) {
    auto v = parse(R"("")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(Parser, SimpleString) {
    auto v = parse(R"("hello world")");
    EXPECT_EQ(v.as_string(), "hello world");
}

TEST(Parser, StringWithEscapes) {
    auto v = parse(R"("line1\nline2\ttab")");
    EXPECT_EQ(v.as_string(), "line1\nline2\ttab");
}

TEST(Parser, StringWithQuoteEscape) {
    auto v = parse(R"("say \"hello\"")");
    EXPECT_EQ(v.as_string(), "say \"hello\"");
}

TEST(Parser, StringWithBackslashEscape) {
    auto v = parse(R"("path\\to\\file")");
    EXPECT_EQ(v.as_string(), "path\\to\\file");
}

TEST(Parser, StringWithSlashEscape) {
    auto v = parse(R"("a\/b")");
    EXPECT_EQ(v.as_string(), "a/b");
}

TEST(Parser, StringWithAllEscapes) {
    auto v = parse(R"("\"\\\b\f\n\r\t")");
    EXPECT_EQ(v.as_string(), "\"\\\b\f\n\r\t");
}

TEST(Parser, StringWithUnicodeEscape) {
    auto v = parse(R"("\u0041")"); // 'A'
    EXPECT_EQ(v.as_string(), "A");
}

TEST(Parser, StringWithUnicodeEscapeLowerHex) {
    auto v = parse(R"("\u006a")"); // 'j'
    EXPECT_EQ(v.as_string(), "j");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Arrays
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, EmptyArray) {
    auto v = parse("[]");
    EXPECT_TRUE(v.is_array());
    EXPECT_TRUE(v.empty());
}

TEST(Parser, ArrayOfIntegers) {
    auto v = parse("[1, 2, 3]");
    EXPECT_TRUE(v.is_array());
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].as_integer(), 1);
    EXPECT_EQ(v[1].as_integer(), 2);
    EXPECT_EQ(v[2].as_integer(), 3);
}

TEST(Parser, ArrayOfMixedTypes) {
    auto v = parse(R"([1, "two", true, null, 3.14])");
    EXPECT_EQ(v.size(), 5u);
    EXPECT_TRUE(v[0].is_integer());
    EXPECT_TRUE(v[1].is_string());
    EXPECT_TRUE(v[2].is_bool());
    EXPECT_TRUE(v[3].is_null());
    EXPECT_TRUE(v[4].is_float());
}

TEST(Parser, NestedArrays) {
    auto v = parse("[[1, 2], [3, [4, 5]]]");
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].size(), 2u);
    EXPECT_EQ(v[1][1][0].as_integer(), 4);
}

TEST(Parser, ArrayWithWhitespace) {
    auto v = parse("  [  1  ,  2  ,  3  ]  ");
    EXPECT_EQ(v.size(), 3u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Objects
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, EmptyObject) {
    auto v = parse("{}");
    EXPECT_TRUE(v.is_object());
    EXPECT_TRUE(v.empty());
}

TEST(Parser, SimpleObject) {
    auto v = parse(R"({"name": "John", "age": 30})");
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v["name"].as_string(), "John");
    EXPECT_EQ(v["age"].as_integer(), 30);
}

TEST(Parser, NestedObject) {
    auto v = parse(R"({
        "person": {
            "name": "Alice",
            "address": {
                "city": "Moscow",
                "zip": "101000"
            }
        }
    })");

    EXPECT_EQ(v["person"]["name"].as_string(), "Alice");
    EXPECT_EQ(v["person"]["address"]["city"].as_string(), "Moscow");
}

TEST(Parser, ObjectWithArray) {
    auto v = parse(R"({"items": [1, 2, 3], "count": 3})");
    EXPECT_EQ(v["items"].size(), 3u);
    EXPECT_EQ(v["count"].as_integer(), 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Complex structures
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, ComplexDocument) {
    const char* input = R"({
        "users": [
            {
                "id": 1,
                "name": "Alice",
                "active": true,
                "scores": [95.5, 87.3, 92.0],
                "metadata": null
            },
            {
                "id": 2,
                "name": "Bob",
                "active": false,
                "scores": [88.1, 76.4],
                "metadata": {"role": "admin"}
            }
        ],
        "total": 2,
        "version": "1.0"
    })";

    auto v = parse(input);
    EXPECT_TRUE(v.is_object());

    const auto& users = v["users"];
    EXPECT_EQ(users.size(), 2u);

    EXPECT_EQ(users[0]["name"].as_string(), "Alice");
    EXPECT_EQ(users[0]["active"].as_bool(), true);
    EXPECT_DOUBLE_EQ(users[0]["scores"][0].as_float(), 95.5);
    EXPECT_TRUE(users[0]["metadata"].is_null());

    EXPECT_EQ(users[1]["metadata"]["role"].as_string(), "admin");
    EXPECT_EQ(v["total"].as_integer(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Roundtrip: parse → dump → parse
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, RoundtripCompact) {
    std::string input = R"({"a":1,"b":[2,3],"c":"hello"})";
    auto v1 = parse(input);
    std::string output = v1.dump();
    auto v2 = parse(output);
    EXPECT_EQ(v1, v2);
}

TEST(Parser, RoundtripPretty) {
    std::string input = R"({"key":"value","arr":[1,2,3]})";
    auto v1 = parse(input);
    std::string pretty = v1.dump(2);
    auto v2 = parse(pretty);
    EXPECT_EQ(v1, v2);
}

TEST(Parser, RoundtripNestedEmpty) {
    std::string input = R"({"a":[],"b":{},"c":null})";
    auto v1 = parse(input);
    auto v2 = parse(v1.dump());
    EXPECT_EQ(v1, v2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Parse errors
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, ErrorEmptyInput) {
    EXPECT_THROW(parse(""), ParseError);
}

TEST(Parser, ErrorTrailingComma) {
    EXPECT_THROW(parse("[1, 2, ]"), ParseError);
}

TEST(Parser, ErrorMissingColon) {
    EXPECT_THROW(parse(R"({"key" "value"})"), ParseError);
}

TEST(Parser, ErrorUnterminatedString) {
    EXPECT_THROW(parse(R"("unterminated)"), ParseError);
}

TEST(Parser, ErrorTrailingContent) {
    EXPECT_THROW(parse("42 extra"), ParseError);
}

TEST(Parser, ErrorInvalidLiteral) {
    EXPECT_THROW(parse("nul"), ParseError);
    EXPECT_THROW(parse("tru"), ParseError);
    EXPECT_THROW(parse("fals"), ParseError);
}

TEST(Parser, ErrorInvalidNumber) {
    EXPECT_THROW(parse("--1"), ParseError);
    EXPECT_THROW(parse("1."), ParseError);
    EXPECT_THROW(parse("1e"), ParseError);
}

TEST(Parser, ErrorUnexpectedCharacter) {
    EXPECT_THROW(parse("@"), ParseError);
}

TEST(Parser, ErrorLocationInfo) {
    try {
        auto val = parse("{\n  \"key\": @\n}");
        (void)val;
        FAIL() << "Expected ParseError";
    } catch (const ParseError& e) {
        EXPECT_EQ(e.location().line, 2u);
        EXPECT_GT(e.location().column, 1u);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Parser, WhitespaceOnly) {
    EXPECT_THROW(parse("   \n\t  "), ParseError);
}

TEST(Parser, DeeplyNested) {
    // Create a deeply nested structure
    std::string deep = "";
    constexpr int depth = 100;
    for (int i = 0; i < depth; ++i) deep += "[";
    deep += "1";
    for (int i = 0; i < depth; ++i) deep += "]";

    auto v = parse(deep);
    // Verify the structure
    const JsonValue* current = &v;
    for (int i = 0; i < depth; ++i) {
        ASSERT_TRUE(current->is_array());
        ASSERT_EQ(current->size(), 1u);
        current = &(*current)[0];
    }
    EXPECT_EQ(current->as_integer(), 1);
}

TEST(Parser, ManyKeys) {
    // Object with many keys
    std::string input = "{";
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) input += ",";
        input += "\"key" + std::to_string(i) + "\":" + std::to_string(i);
    }
    input += "}";

    auto v = parse(input);
    EXPECT_EQ(v.size(), 1000u);
    EXPECT_EQ(v["key0"].as_integer(), 0);
    EXPECT_EQ(v["key999"].as_integer(), 999);
}

TEST(Parser, LargeArray) {
    std::string input = "[";
    for (int i = 0; i < 10000; ++i) {
        if (i > 0) input += ",";
        input += std::to_string(i);
    }
    input += "]";

    auto v = parse(input);
    EXPECT_EQ(v.size(), 10000u);
    EXPECT_EQ(v[0].as_integer(), 0);
    EXPECT_EQ(v[9999].as_integer(), 9999);
}
