/// @file test_uint64_pointer_writer.cpp
/// @brief Tests for uint64_t, JSON Pointer (RFC 6901), and JsonWriter.

#include <json/json.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

using namespace yajson;

// === uint64_t support ===

TEST(UInt64, ConstructAndType) {
    JsonValue v(uint64_t(42));
    EXPECT_TRUE(v.is_uinteger());
    EXPECT_TRUE(v.is_number());
    EXPECT_FALSE(v.is_integer());
    EXPECT_EQ(v.type(), Type::UInteger);
    EXPECT_EQ(v.as_uinteger(), 42u);
}

TEST(UInt64, MaxValue) {
    constexpr uint64_t mx = std::numeric_limits<uint64_t>::max();
    JsonValue v(mx);
    EXPECT_EQ(v.as_uinteger(), mx);
}

TEST(UInt64, AsFloat) {
    JsonValue v(uint64_t(1000));
    EXPECT_DOUBLE_EQ(v.as_float(), 1000.0);
}

TEST(UInt64, AsIntegerOk) {
    JsonValue v(uint64_t(42));
    EXPECT_EQ(v.as_integer(), 42);
}

TEST(UInt64, AsIntegerOverflow) {
    uint64_t big = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
    JsonValue v(big);
    EXPECT_THROW(v.as_integer(), TypeError);
}

TEST(UInt64, AsUIntFromInt) {
    JsonValue v(int64_t(99));
    EXPECT_EQ(v.as_uinteger(), 99u);
}

TEST(UInt64, AsUIntFromNegative) {
    JsonValue v(int64_t(-1));
    EXPECT_THROW(v.as_uinteger(), TypeError);
}

TEST(UInt64, GetTemplate) {
    JsonValue v(uint64_t(123));
    EXPECT_EQ(v.get<uint64_t>(), 123u);
}

TEST(UInt64, Serialize) {
    uint64_t big = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 100;
    JsonValue v(big);
    EXPECT_EQ(v.dump(), std::to_string(big));
}

TEST(UInt64, ParseLarge) {
    auto v = parse("9223372036854775808");
    EXPECT_TRUE(v.is_uinteger());
    EXPECT_EQ(v.as_uinteger(), 9223372036854775808ULL);
}

TEST(UInt64, ParseMax) {
    auto v = parse("18446744073709551615");
    EXPECT_TRUE(v.is_uinteger());
    EXPECT_EQ(v.as_uinteger(), std::numeric_limits<uint64_t>::max());
}

TEST(UInt64, ParseNormal) {
    auto v = parse("42");
    EXPECT_TRUE(v.is_integer());
}

TEST(UInt64, RoundTrip) {
    uint64_t val = 18446744073709551000ULL;
    auto v2 = parse(JsonValue(val).dump());
    EXPECT_TRUE(v2.is_uinteger());
    EXPECT_EQ(v2.as_uinteger(), val);
}

TEST(UInt64, Equality) {
    EXPECT_EQ(JsonValue(uint64_t(100)), JsonValue(uint64_t(100)));
    EXPECT_NE(JsonValue(uint64_t(1)), JsonValue(uint64_t(2)));
}

TEST(UInt64, CrossType) {
    EXPECT_EQ(JsonValue(uint64_t(42)), JsonValue(int64_t(42)));
}

TEST(UInt64, CopyMove) {
    JsonValue a(uint64_t(999));
    JsonValue b = a;
    EXPECT_EQ(b.as_uinteger(), 999u);
    JsonValue c = std::move(a);
    EXPECT_EQ(c.as_uinteger(), 999u);
    EXPECT_TRUE(a.is_null());
}

TEST(UInt64, Conversion) {
    uint64_t val = 12345;
    JsonValue j;
    to_json(j, val);
    EXPECT_TRUE(j.is_uinteger());
    uint64_t out = 0;
    from_json(j, out);
    EXPECT_EQ(out, 12345u);
}

TEST(UInt64, TypeNameStr) {
    EXPECT_STREQ(type_name(Type::UInteger), "uinteger");
}

// === JSON Pointer ===

TEST(JsonPtr, EmptyRoot) {
    auto v = parse(R"({"a":1})");
    JsonPointer p("");
    EXPECT_TRUE(p.empty());
    EXPECT_TRUE(p.resolve(v).is_object());
}

TEST(JsonPtr, SimpleKey) {
    auto v = parse(R"({"foo":"bar","baz":42})");
    EXPECT_EQ(JsonPointer("/foo").resolve(v).as_string(), "bar");
    EXPECT_EQ(JsonPointer("/baz").resolve(v).as_integer(), 42);
}

TEST(JsonPtr, Nested) {
    auto v = parse(R"({"a":{"b":{"c":true}}})");
    EXPECT_TRUE(JsonPointer("/a/b/c").resolve(v).as_bool());
}

TEST(JsonPtr, ArrayIdx) {
    auto v = parse(R"({"items":[10,20,30]})");
    EXPECT_EQ(JsonPointer("/items/0").resolve(v).as_integer(), 10);
    EXPECT_EQ(JsonPointer("/items/2").resolve(v).as_integer(), 30);
}

TEST(JsonPtr, EscapeTilde) {
    auto v = parse(R"({"a/b":1,"c~d":2})");
    EXPECT_EQ(JsonPointer("/a~1b").resolve(v).as_integer(), 1);
    EXPECT_EQ(JsonPointer("/c~0d").resolve(v).as_integer(), 2);
}

TEST(JsonPtr, ToString) {
    JsonPointer p("/a~1b/c~0d/0");
    EXPECT_EQ(p.to_string(), "/a~1b/c~0d/0");
}

TEST(JsonPtr, AppendParent) {
    auto child = JsonPointer("/a/b").append("c");
    EXPECT_EQ(child.to_string(), "/a/b/c");
    EXPECT_EQ(child.parent().to_string(), "/a/b");
}

TEST(JsonPtr, TryResolveOk) {
    auto v = parse(R"({"x":42})");
    auto* p = JsonPointer("/x").try_resolve(v);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->as_integer(), 42);
}

TEST(JsonPtr, TryResolveMiss) {
    auto v = parse(R"({"x":42})");
    EXPECT_EQ(JsonPointer("/y").try_resolve(v), nullptr);
}

TEST(JsonPtr, MutableResolve) {
    auto v = parse(R"({"d":{"v":0}})");
    JsonPointer("/d/v").resolve(v) = JsonValue(99);
    EXPECT_EQ(v["d"]["v"].as_integer(), 99);
}

TEST(JsonPtr, Set) {
    auto v = JsonValue::object();
    JsonPointer("/a/b").set(v, JsonValue(42));
    EXPECT_EQ(v["a"]["b"].as_integer(), 42);
}

TEST(JsonPtr, SetRoot) {
    JsonValue v(1);
    JsonPointer("").set(v, JsonValue("x"));
    EXPECT_EQ(v.as_string(), "x");
}

TEST(JsonPtr, Erase) {
    auto v = parse(R"({"a":1,"b":2,"c":3})");
    EXPECT_TRUE(JsonPointer("/b").erase(v));
    EXPECT_FALSE(v.contains("b"));
}

TEST(JsonPtr, EraseArr) {
    auto v = parse(R"({"arr":[10,20,30]})");
    EXPECT_TRUE(JsonPointer("/arr/1").erase(v));
    EXPECT_EQ(v["arr"].size(), 2u);
    EXPECT_EQ(v["arr"][1].as_integer(), 30);
}

TEST(JsonPtr, InvalidNoSlash) {
    EXPECT_THROW(JsonPointer("x"), ParseError);
}

TEST(JsonPtr, InvalidIdx) {
    auto v = parse("[1,2,3]");
    EXPECT_THROW(JsonPointer("/5").resolve(v), OutOfRangeError);
}

TEST(JsonPtr, Convenience) {
    auto v = parse(R"({"d":{"v":99}})");
    EXPECT_EQ(resolve(v, "/d/v").as_integer(), 99);
}

TEST(JsonPtr, EmptyKey) {
    auto v = parse(R"({"":42})");
    EXPECT_EQ(JsonPointer("/").resolve(v).as_integer(), 42);
}

TEST(JsonPtr, EqualityOp) {
    EXPECT_EQ(JsonPointer("/a/b"), JsonPointer("/a/b"));
    EXPECT_NE(JsonPointer("/a"), JsonPointer("/b"));
}

// === JsonWriter ===

TEST(Writer, Null) {
    std::string b;
    JsonWriter w(b);
    w.null_value();
    EXPECT_EQ(b, "null");
    EXPECT_TRUE(w.is_complete());
}

TEST(Writer, Bool) {
    std::string b;
    JsonWriter w(b);
    w.bool_value(true);
    EXPECT_EQ(b, "true");
}

TEST(Writer, Int) {
    std::string b;
    JsonWriter w(b);
    w.int_value(-42);
    EXPECT_EQ(b, "-42");
}

TEST(Writer, UInt) {
    std::string b;
    JsonWriter w(b);
    w.uint_value(18446744073709551615ULL);
    EXPECT_EQ(b, "18446744073709551615");
}

TEST(Writer, Float) {
    std::string b;
    JsonWriter w(b);
    w.float_value(3.14);
    EXPECT_NE(b.find("3.14"), std::string::npos);
}

TEST(Writer, NaN) {
    std::string b;
    JsonWriter w(b);
    w.float_value(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(b, "null");
}

TEST(Writer, Str) {
    std::string b;
    JsonWriter w(b);
    w.string_value("hi");
    EXPECT_EQ(b, "\"hi\"");
}

TEST(Writer, StrEsc) {
    std::string b;
    JsonWriter w(b);
    w.string_value("a\nb\t\"\\");
    EXPECT_EQ(b, "\"a\\nb\\t\\\"\\\\\"");
}

TEST(Writer, EmptyObj) {
    std::string b;
    JsonWriter w(b);
    w.begin_object().end_object();
    EXPECT_EQ(b, "{}");
}

TEST(Writer, Obj) {
    std::string b;
    JsonWriter w(b);
    w.begin_object().key("n").string_value("A").key("a").int_value(30).end_object();
    auto v = parse(b);
    EXPECT_EQ(v["n"].as_string(), "A");
    EXPECT_EQ(v["a"].as_integer(), 30);
}

TEST(Writer, EmptyArr) {
    std::string b;
    JsonWriter w(b);
    w.begin_array().end_array();
    EXPECT_EQ(b, "[]");
}

TEST(Writer, Arr) {
    std::string b;
    JsonWriter w(b);
    w.begin_array().int_value(1).int_value(2).int_value(3).end_array();
    EXPECT_EQ(parse(b).size(), 3u);
}

TEST(Writer, Nested) {
    std::string b;
    JsonWriter w(b);
    w.begin_object();
    w.key("u").begin_array();
    w.begin_object().key("id").int_value(1).key("n").string_value("B").end_object();
    w.begin_object().key("id").int_value(2).key("n").string_value("E").end_object();
    w.end_array();
    w.end_object();
    EXPECT_EQ(parse(b)["u"][1]["n"].as_string(), "E");
}

TEST(Writer, Pretty) {
    std::string b;
    JsonWriter w(b, 2);
    w.begin_object().key("a").int_value(1).end_object();
    EXPECT_NE(b.find('\n'), std::string::npos);
    EXPECT_EQ(parse(b)["a"].as_integer(), 1);
}

TEST(Writer, OStream) {
    std::ostringstream oss;
    JsonWriter w(oss);
    w.begin_array().string_value("hi").null_value().end_array();
    w.flush();
    EXPECT_EQ(parse(oss.str())[0].as_string(), "hi");
}

TEST(Writer, RawJson) {
    std::string b;
    JsonWriter w(b);
    w.begin_object().key("d").raw_json(R"({"p":"s"})").end_object();
    EXPECT_EQ(parse(b)["d"]["p"].as_string(), "s");
}

TEST(Writer, Depth) {
    std::string b;
    JsonWriter w(b);
    EXPECT_EQ(w.depth(), 0u);
    w.begin_object();
    EXPECT_EQ(w.depth(), 1u);
    w.key("a").begin_array();
    EXPECT_EQ(w.depth(), 2u);
    w.end_array();
    EXPECT_EQ(w.depth(), 1u);
    w.end_object();
    EXPECT_EQ(w.depth(), 0u);
}

TEST(Writer, BadEndObj) {
    std::string b;
    JsonWriter w(b);
    EXPECT_THROW(w.end_object(), TypeError);
}

TEST(Writer, BadEndArr) {
    std::string b;
    JsonWriter w(b);
    EXPECT_THROW(w.end_array(), TypeError);
}

TEST(Writer, KeyOutside) {
    std::string b;
    JsonWriter w(b);
    w.begin_array();
    EXPECT_THROW(w.key("x"), TypeError);
}

TEST(Writer, Large) {
    std::string b;
    JsonWriter w(b);
    w.begin_array();
    for (int i = 0; i < 10000; ++i) w.int_value(i);
    w.end_array();
    auto v = parse(b);
    EXPECT_EQ(v.size(), 10000u);
    EXPECT_EQ(v[9999].as_integer(), 9999);
}

TEST(Writer, ControlChar) {
    std::string b;
    JsonWriter w(b);
    std::string inp;
    inp += '\x01';
    inp += '\x1F';
    w.string_value(inp);
    EXPECT_NE(b.find("\\u0001"), std::string::npos);
    EXPECT_NE(b.find("\\u001f"), std::string::npos);
}
