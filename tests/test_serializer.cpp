/// @file test_serializer.cpp
/// @brief Unit tests for yajson::serialize() / JsonValue::dump().

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Compact serialization
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Serializer, Null) {
    EXPECT_EQ(JsonValue(nullptr).dump(), "null");
}

TEST(Serializer, BoolTrue) {
    EXPECT_EQ(JsonValue(true).dump(), "true");
}

TEST(Serializer, BoolFalse) {
    EXPECT_EQ(JsonValue(false).dump(), "false");
}

TEST(Serializer, IntegerPositive) {
    EXPECT_EQ(JsonValue(42).dump(), "42");
}

TEST(Serializer, IntegerNegative) {
    EXPECT_EQ(JsonValue(-100).dump(), "-100");
}

TEST(Serializer, IntegerZero) {
    EXPECT_EQ(JsonValue(0).dump(), "0");
}

TEST(Serializer, IntegerLarge) {
    int64_t large = 9223372036854775807LL;
    EXPECT_EQ(JsonValue(large).dump(), "9223372036854775807");
}

TEST(Serializer, FloatSimple) {
    auto s = JsonValue(3.14).dump();
    // Should contain "3.14"
    EXPECT_NE(s.find("3.14"), std::string::npos);
}

TEST(Serializer, FloatInteger) {
    auto s = JsonValue(1.0).dump();
    // Should contain a dot to distinguish from integer
    EXPECT_NE(s.find('.'), std::string::npos);
}

TEST(Serializer, FloatNaN) {
    EXPECT_EQ(JsonValue(std::nan("")).dump(), "null");
}

TEST(Serializer, FloatInfinity) {
    EXPECT_EQ(JsonValue(std::numeric_limits<double>::infinity()).dump(), "null");
}

TEST(Serializer, StringSimple) {
    EXPECT_EQ(JsonValue("hello").dump(), "\"hello\"");
}

TEST(Serializer, StringEmpty) {
    EXPECT_EQ(JsonValue("").dump(), "\"\"");
}

TEST(Serializer, StringWithEscapes) {
    JsonValue v("line1\nline2\ttab");
    EXPECT_EQ(v.dump(), "\"line1\\nline2\\ttab\"");
}

TEST(Serializer, StringWithQuotes) {
    JsonValue v("say \"hello\"");
    EXPECT_EQ(v.dump(), "\"say \\\"hello\\\"\"");
}

TEST(Serializer, StringWithBackslash) {
    JsonValue v("path\\to\\file");
    EXPECT_EQ(v.dump(), "\"path\\\\to\\\\file\"");
}

TEST(Serializer, StringWithControlChars) {
    std::string s;
    s += '\x01'; // Control character
    JsonValue v(s);
    auto result = v.dump();
    EXPECT_NE(result.find("\\u0001"), std::string::npos);
}

TEST(Serializer, EmptyArray) {
    EXPECT_EQ(JsonValue::array().dump(), "[]");
}

TEST(Serializer, ArrayOfIntegers) {
    Array arr = {JsonValue(1), JsonValue(2), JsonValue(3)};
    EXPECT_EQ(JsonValue(arr).dump(), "[1,2,3]");
}

TEST(Serializer, EmptyObject) {
    EXPECT_EQ(JsonValue::object().dump(), "{}");
}

TEST(Serializer, SimpleObject) {
    Object obj = {{"a", JsonValue(1)}, {"b", JsonValue("two")}};
    EXPECT_EQ(JsonValue(obj).dump(), R"({"a":1,"b":"two"})");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Formatted output (pretty-print)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Serializer, PrettyNull) {
    EXPECT_EQ(JsonValue(nullptr).dump(2), "null");
}

TEST(Serializer, PrettyArray) {
    Array arr = {JsonValue(1), JsonValue(2)};
    std::string expected = "[\n  1,\n  2\n]";
    EXPECT_EQ(JsonValue(arr).dump(2), expected);
}

TEST(Serializer, PrettyObject) {
    Object obj = {{"key", JsonValue("value")}};
    std::string expected = "{\n  \"key\": \"value\"\n}";
    EXPECT_EQ(JsonValue(obj).dump(2), expected);
}

TEST(Serializer, PrettyNested) {
    auto inner = JsonValue::array();
    inner.push_back(JsonValue(1));
    inner.push_back(JsonValue(2));

    auto obj = JsonValue::object();
    obj["data"] = std::move(inner);

    std::string expected =
        "{\n"
        "  \"data\": [\n"
        "    1,\n"
        "    2\n"
        "  ]\n"
        "}";
    EXPECT_EQ(obj.dump(2), expected);
}

TEST(Serializer, PrettyIndent4) {
    Array arr = {JsonValue(1)};
    std::string expected = "[\n    1\n]";
    EXPECT_EQ(JsonValue(arr).dump(4), expected);
}

TEST(Serializer, PrettyEmptyContainers) {
    auto obj = JsonValue::object();
    obj["arr"] = JsonValue::array();
    obj["obj"] = JsonValue::object();

    std::string expected =
        "{\n"
        "  \"arr\": [],\n"
        "  \"obj\": {}\n"
        "}";
    EXPECT_EQ(obj.dump(2), expected);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Free function serialize()
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Serializer, FreeFunctionCompact) {
    JsonValue v(42);
    EXPECT_EQ(serialize(v), "42");
}

TEST(Serializer, FreeFunctionPretty) {
    Array arr = {JsonValue(1)};
    EXPECT_EQ(serialize(JsonValue(arr), 2), "[\n  1\n]");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Complex structures
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Serializer, ComplexDocument) {
    auto doc = JsonValue::object();
    doc["name"] = JsonValue("test");
    doc["version"] = JsonValue(1);
    doc["active"] = JsonValue(true);
    doc["data"] = JsonValue(nullptr);

    auto tags = JsonValue::array();
    tags.push_back(JsonValue("alpha"));
    tags.push_back(JsonValue("beta"));
    doc["tags"] = std::move(tags);

    auto compact = doc.dump();
    auto reparsed = parse(compact);
    EXPECT_EQ(doc, reparsed);
}

TEST(Serializer, LargeArrayPerformance) {
    // Verify correctness of large array serialization
    auto arr = JsonValue::array();
    for (int i = 0; i < 10000; ++i) {
        arr.push_back(JsonValue(i));
    }

    auto s = arr.dump();
    EXPECT_FALSE(s.empty());

    // Roundtrip
    auto reparsed = parse(s);
    EXPECT_EQ(reparsed.size(), 10000u);
    EXPECT_EQ(reparsed[0].as_integer(), 0);
    EXPECT_EQ(reparsed[9999].as_integer(), 9999);
}
