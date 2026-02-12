/// @file test_conformance.cpp
/// @brief Conformance and edge-case tests for parser/serializer (safety net for perf changes).
///
/// Covers: numbers (boundaries, overflow, leading zero), strings (escapes, UTF-8),
/// round-trip, nested structures. Run after every parser change to ensure correctness.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Numbers — edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceNumbers, Zero) {
    auto v = parse("0");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), 0);
}

TEST(ConformanceNumbers, MinusZeroFloat) {
    auto v = parse("-0.0");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), -0.0);
}

TEST(ConformanceNumbers, Int64Min) {
    auto v = parse("-9223372036854775808");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), std::numeric_limits<int64_t>::min());
}

TEST(ConformanceNumbers, Int64Max) {
    auto v = parse("9223372036854775807");
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.as_integer(), std::numeric_limits<int64_t>::max());
}

TEST(ConformanceNumbers, LeadingZeroRejected) {
    // "01" is invalid: leading zero then digit leaves trailing content
    EXPECT_THROW(parse("01"), ParseError);
}

TEST(ConformanceNumbers, IntegerOverflowBecomesFloatOrUint) {
    // 9223372036854775808 > INT64_MAX → parsed as float or uint64 depending on path
    auto v = parse("9223372036854775808");
    EXPECT_TRUE(v.is_number());
    if (v.is_float())
        EXPECT_DOUBLE_EQ(v.as_float(), 9223372036854775808.0);
    else
        EXPECT_TRUE(v.is_uinteger() && v.as_uinteger() == 9223372036854775808ULL);
}

TEST(ConformanceNumbers, SmallFloat) {
    auto v = parse("1e-308");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 1e-308);
}

TEST(ConformanceNumbers, LargeFloat) {
    auto v = parse("1e308");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 1e308);
}

TEST(ConformanceNumbers, FloatExponentPlus) {
    auto v = parse("1e+2");
    EXPECT_TRUE(v.is_float());
    EXPECT_DOUBLE_EQ(v.as_float(), 100.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Strings — edge cases and escapes
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceStrings, Empty) {
    auto v = parse(R"("")");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "");
}

TEST(ConformanceStrings, NullByte) {
    auto v = parse(R"("\u0000")");
    std::string expected(1, '\0');
    EXPECT_EQ(v.as_string(), expected);
}

TEST(ConformanceStrings, AllStandardEscapes) {
    auto v = parse(R"("\"\\\/\b\f\n\r\t")");
    EXPECT_EQ(v.as_string(), "\"\\/\b\f\n\r\t");
}

TEST(ConformanceStrings, QuoteAndBackslash) {
    auto v = parse(R"("say \"hello\" path\\to\\file")");
    EXPECT_EQ(v.as_string(), "say \"hello\" path\\to\\file");
}

TEST(ConformanceStrings, LongString) {
    std::string longStr(2000, 'x');
    std::string input = "\"" + longStr + "\"";
    auto v = parse(input);
    EXPECT_EQ(v.as_string(), longStr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Control characters (0x00-0x1F) and DEL (0x7F)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceStrings, AllControlCharsViaEscape) {
    // Verify all 32 control characters (0x00-0x1F) can be parsed via \uXXXX
    for (int i = 0; i <= 0x1F; ++i) {
        char hex[5];
        std::snprintf(hex, sizeof(hex), "%04X", i);
        std::string input = std::string("\"\\u") + hex + "\"";
        auto v = parse(input);
        std::string expected(1, static_cast<char>(i));
        EXPECT_EQ(v.as_string(), expected) << "Failed for control char 0x" << hex;
    }
}

TEST(ConformanceStrings, ControlCharsRoundtrip) {
    // Parse → serialize → parse for all control characters
    for (int i = 0; i <= 0x1F; ++i) {
        char hex[5];
        std::snprintf(hex, sizeof(hex), "%04X", i);
        std::string input = std::string("\"\\u") + hex + "\"";
        auto v1 = parse(input);
        std::string serialized = v1.dump();
        auto v2 = parse(serialized);
        EXPECT_EQ(v1.as_string(), v2.as_string())
            << "Round-trip failed for control char 0x" << hex;
    }
}

TEST(ConformanceStrings, DelCharPassthrough) {
    // DEL (0x7F) is not a control character per JSON spec — it should pass through
    std::string input = std::string("\"a") + '\x7F' + "b\"";
    auto v = parse(input);
    std::string expected = std::string("a") + '\x7F' + "b";
    EXPECT_EQ(v.as_string(), expected);
}

TEST(ConformanceStrings, MixedControlAndUtf8) {
    // Mix control chars, ASCII, Cyrillic, and emoji
    auto v = parse(R"("Hello\n\tПривет\r\n😀\\end")");
    EXPECT_EQ(v.as_string(), "Hello\n\tПривет\r\n😀\\end");
}

TEST(ConformanceStrings, ControlCharsInObjectKeys) {
    auto v = parse(R"({"key\twith\ttabs": 1, "key\nwith\nnewlines": 2})");
    EXPECT_EQ(v["key\twith\ttabs"].as_integer(), 1);
    EXPECT_EQ(v["key\nwith\nnewlines"].as_integer(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UTF-8 and Unicode escapes
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceUtf8, DirectUtf8) {
    auto v = parse("\"Привет мир\"");
    EXPECT_EQ(v.as_string(), "Привет мир");
}

TEST(ConformanceUtf8, UnicodeEscapeBMP) {
    auto v = parse(R"("\u0041\u0042")");
    EXPECT_EQ(v.as_string(), "AB");
}

TEST(ConformanceUtf8, SurrogatePair) {
    auto v = parse(R"("\uD83D\uDE00")");
    EXPECT_EQ(v.as_string(), "😀");
}

TEST(ConformanceUtf8, MixedDirectAndEscape) {
    auto v = parse(R"("Hello \u4E16\u754C!")");
    EXPECT_EQ(v.as_string(), "Hello 世界!");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Round-trip: parse → dump → parse
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceRoundtrip, Primitives) {
    auto v1 = parse(R"(null)");
    EXPECT_EQ(parse(v1.dump()), v1);

    v1 = parse("true");
    EXPECT_EQ(parse(v1.dump()), v1);

    v1 = parse("42");
    EXPECT_EQ(parse(v1.dump()), v1);

    v1 = parse("3.14");
    EXPECT_EQ(parse(v1.dump()), v1);

    v1 = parse(R"("hello")");
    EXPECT_EQ(parse(v1.dump()), v1);
}

TEST(ConformanceRoundtrip, Array) {
    std::string input = R"([1,"two",true,null,3.14])";
    auto v1 = parse(input);
    auto v2 = parse(v1.dump());
    EXPECT_EQ(v1, v2);
}

TEST(ConformanceRoundtrip, Object) {
    std::string input = R"({"a":1,"b":"x","c":[]})";
    auto v1 = parse(input);
    auto v2 = parse(v1.dump());
    EXPECT_EQ(v1, v2);
}

TEST(ConformanceRoundtrip, NestedAndUtf8) {
    std::string input = R"({"name":"Тест","nested":{"arr":[1,2],"emoji":"😀"}})";
    auto v1 = parse(input);
    std::string out = v1.dump();
    auto v2 = parse(out);
    EXPECT_EQ(v1, v2);
}

TEST(ConformanceRoundtrip, PrettyThenCompact) {
    std::string input = R"({"a":1,"b":[2,3]})";
    auto v1 = parse(input);
    std::string pretty = v1.dump(2);
    auto v2 = parse(pretty);
    EXPECT_EQ(v1, v2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Nested structures
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceNested, DeepArray) {
    std::string deep;
    constexpr int depth = 50;
    for (int i = 0; i < depth; ++i) deep += "[";
    deep += "1";
    for (int i = 0; i < depth; ++i) deep += "]";
    auto v = parse(deep);
    const JsonValue* cur = &v;
    for (int i = 0; i < depth; ++i) {
        ASSERT_TRUE(cur->is_array());
        ASSERT_EQ(cur->size(), 1u);
        cur = &(*cur)[0];
    }
    EXPECT_EQ(cur->as_integer(), 1);
}

TEST(ConformanceNested, DeepObject) {
    // Build {"x":{"x":{"x":...{"x":1}}}} with depth levels; depth steps of ["x"] reach 1
    constexpr int depth = 15;
    std::string deep;
    for (int i = 0; i < depth; ++i) deep += "{\"x\":";
    deep += "1";
    for (int i = 0; i < depth; ++i) deep += "}";
    auto v = parse(deep);
    const JsonValue* cur = &v;
    for (int i = 0; i < depth; ++i) {
        ASSERT_TRUE(cur->is_object());
        ASSERT_TRUE(cur->contains("x"));
        cur = &(*cur)["x"];
    }
    EXPECT_TRUE(cur->is_integer());
    EXPECT_EQ(cur->as_integer(), 1);
}

TEST(ConformanceNested, MixedDeep) {
    std::string input = R"({"a":[{"b":[{"c":1}]}]})";
    auto v = parse(input);
    EXPECT_EQ(v["a"][0]["b"][0]["c"].as_integer(), 1);
}

TEST(ConformanceNested, ArrayOfObjects) {
    auto v = parse(R"([{"id":1},{"id":2},{"id":3}])");
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0]["id"].as_integer(), 1);
    EXPECT_EQ(v[2]["id"].as_integer(), 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Invalid / error cases (conformance: must stay rejected)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConformanceErrors, EmptyInput) {
    EXPECT_THROW(parse(""), ParseError);
}

TEST(ConformanceErrors, TrailingCommaArray) {
    EXPECT_THROW(parse("[1,]"), ParseError);
}

TEST(ConformanceErrors, TrailingCommaObject) {
    EXPECT_THROW(parse(R"({"a":1,})"), ParseError);
}

TEST(ConformanceErrors, UnterminatedString) {
    EXPECT_THROW(parse(R"("open)"), ParseError);
}

TEST(ConformanceErrors, InvalidLiteral) {
    EXPECT_THROW(parse("nul"), ParseError);
    EXPECT_THROW(parse("tru"), ParseError);
    EXPECT_THROW(parse("fals"), ParseError);
}

TEST(ConformanceErrors, InvalidNumber) {
    EXPECT_THROW(parse("--1"), ParseError);
    EXPECT_THROW(parse("1."), ParseError);
    EXPECT_THROW(parse("1e"), ParseError);
}

TEST(ConformanceErrors, TrailingContent) {
    EXPECT_THROW(parse("null x"), ParseError);
}
