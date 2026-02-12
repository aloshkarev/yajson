/// @file test_new_features.cpp
/// @brief Tests for new yajson features:
///   - error_code/system_error
///   - Non-standard JSON parsing (comments, trailing commas, etc.)
///   - Object O(1) key lookup
///   - Streaming parser/serializer
///   - Conversion system (to_json/from_json)
///   - string_view API

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Error code system (error_category, error_code, system_error)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ErrorCode, CategoryName) {
    EXPECT_STREQ(json_category().name(), "json");
}

TEST(ErrorCode, MakeErrorCode) {
    auto ec = make_error_code(errc::type_mismatch);
    EXPECT_TRUE(static_cast<bool>(ec));
    EXPECT_EQ(ec.category().name(), std::string("json"));
    EXPECT_NE(ec.message().find("type mismatch"), std::string::npos);
}

TEST(ErrorCode, SuccessIsNoError) {
    auto ec = make_error_code(errc::ok);
    EXPECT_FALSE(static_cast<bool>(ec));
}

TEST(ErrorCode, TryParseSuccess) {
    auto [val, ec] = try_parse(R"({"a": 1})");
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_TRUE(val.is_object());
    EXPECT_EQ(val["a"].as_integer(), 1);
}

TEST(ErrorCode, TryParseError) {
    auto [val, ec] = try_parse("{invalid}");
    EXPECT_TRUE(static_cast<bool>(ec));
}

TEST(ErrorCode, TryParseTrailingContent) {
    auto [val, ec] = try_parse("42 extra");
    EXPECT_TRUE(static_cast<bool>(ec));
}

TEST(ErrorCode, ResultType) {
    auto r = try_parse(R"(42)");
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value.as_integer(), 42);
}

TEST(ErrorCode, ExceptionsAreSystemError) {
    try {
        auto v = parse("{invalid}");
        (void)v;
        FAIL() << "expected exception";
    } catch (const std::system_error& e) {
        EXPECT_NE(std::string(e.what()).find("parse error"), std::string::npos);
        EXPECT_TRUE(static_cast<bool>(e.code()));
    }
}

TEST(ErrorCode, TypeErrorIsSystemError) {
    try {
        static_cast<void>(JsonValue(42).as_string());
        FAIL() << "expected exception";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), make_error_code(errc::type_mismatch));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Non-standard JSON (ParseOptions)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ParseOptions, StrictModeDefault) {
    auto opts = ParseOptions::strict();
    EXPECT_FALSE(opts.allow_comments);
    EXPECT_FALSE(opts.allow_trailing_commas);
    EXPECT_FALSE(opts.allow_single_quotes);
}

TEST(ParseOptions, LineComments) {
    auto opts = ParseOptions::lenient();
    auto v = parse(R"({
        // This is a comment
        "a": 1,
        // Another comment
        "b": 2
    })", opts);
    EXPECT_EQ(v["a"].as_integer(), 1);
    EXPECT_EQ(v["b"].as_integer(), 2);
}

TEST(ParseOptions, BlockComments) {
    auto opts = ParseOptions::lenient();
    auto v = parse(R"({
        /* block comment */
        "a": /* inline */ 1
    })", opts);
    EXPECT_EQ(v["a"].as_integer(), 1);
}

TEST(ParseOptions, TrailingCommaArray) {
    auto opts = ParseOptions::lenient();
    auto v = parse("[1, 2, 3,]", opts);
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[2].as_integer(), 3);
}

TEST(ParseOptions, TrailingCommaObject) {
    auto opts = ParseOptions::lenient();
    auto v = parse(R"({"a": 1, "b": 2,})", opts);
    EXPECT_EQ(v["a"].as_integer(), 1);
    EXPECT_EQ(v["b"].as_integer(), 2);
}

TEST(ParseOptions, SingleQuotedStrings) {
    auto opts = ParseOptions::lenient();
    auto v = parse("{'hello': 'world'}", opts);
    EXPECT_EQ(v["hello"].as_string(), "world");
}

TEST(ParseOptions, UnquotedKeys) {
    auto opts = ParseOptions::lenient();
    auto v = parse("{key: 42, another_key: true}", opts);
    EXPECT_EQ(v["key"].as_integer(), 42);
    EXPECT_TRUE(v["another_key"].as_bool());
}

TEST(ParseOptions, NaNLiteral) {
    auto opts = ParseOptions::lenient();
    auto v = parse("NaN", opts);
    EXPECT_TRUE(v.is_float());
    EXPECT_TRUE(std::isnan(v.as_float()));
}

TEST(ParseOptions, InfinityLiteral) {
    auto opts = ParseOptions::lenient();
    auto v = parse("Infinity", opts);
    EXPECT_TRUE(v.is_float());
    EXPECT_TRUE(std::isinf(v.as_float()));
    EXPECT_GT(v.as_float(), 0);
}

TEST(ParseOptions, NegativeInfinity) {
    auto opts = ParseOptions::lenient();
    auto v = parse("-Infinity", opts);
    EXPECT_TRUE(v.is_float());
    EXPECT_TRUE(std::isinf(v.as_float()));
    EXPECT_LT(v.as_float(), 0);
}

TEST(ParseOptions, HexNumbers) {
    auto opts = ParseOptions::json5();
    auto v = parse("0xFF", opts);
    EXPECT_EQ(v.as_integer(), 255);
}

TEST(ParseOptions, HexNumbersNegative) {
    auto opts = ParseOptions::json5();
    auto v = parse("-0x10", opts);
    EXPECT_EQ(v.as_integer(), -16);
}

TEST(ParseOptions, StrictRejectsComments) {
    EXPECT_THROW(parse("// comment\n42"), ParseError);
}

TEST(ParseOptions, StrictRejectsTrailingComma) {
    EXPECT_THROW(parse("[1,2,]"), ParseError);
}

TEST(ParseOptions, DuplicateKeyLastWins) {
    auto opts = ParseOptions{};
    opts.allow_duplicate_keys = true;
    auto v = parse(R"({"a": 1, "a": 2})", opts);
    EXPECT_EQ(v["a"].as_integer(), 2);
}

TEST(ParseOptions, DuplicateKeyReject) {
    auto opts = ParseOptions{};
    opts.allow_duplicate_keys = false;
    EXPECT_THROW(parse(R"({"a": 1, "a": 2})", opts), ParseError);
}

TEST(ParseOptions, DepthLimit) {
    ParseOptions opts;
    opts.max_depth = 3;
    EXPECT_NO_THROW(parse("[[1]]", opts));
    EXPECT_THROW(parse("[[[[1]]]]", opts), ParseError);
}

TEST(ParseOptions, LenientComplexDocument) {
    auto opts = ParseOptions::lenient();
    auto v = parse(R"({
        // Config file
        name: 'MyApp',
        version: 42,
        features: [
            'feature1',
            'feature2',  // trailing comma
        ],
        limits: {
            max_connections: Infinity,
            timeout: NaN,
        },
    })", opts);
    EXPECT_EQ(v["name"].as_string(), "MyApp");
    EXPECT_EQ(v["version"].as_integer(), 42);
    EXPECT_EQ(v["features"].size(), 2u);
    EXPECT_TRUE(std::isinf(v["limits"]["max_connections"].as_float()));
    EXPECT_TRUE(std::isnan(v["limits"]["timeout"].as_float()));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Object O(1) key lookup
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ObjectLookup, SmallObjectLinearScan) {
    auto v = JsonValue::object();
    for (int i = 0; i < 5; ++i) {
        v.insert("key" + std::to_string(i), JsonValue(i));
    }
    // Linear scan for small objects
    EXPECT_EQ(v["key3"].as_integer(), 3);
    EXPECT_TRUE(v.contains("key0"));
    EXPECT_FALSE(v.contains("nonexistent"));
}

TEST(ObjectLookup, LargeObjectHashIndex) {
    // Object with > threshold entries triggers hash index
    auto v = JsonValue::object();
    for (int i = 0; i < 100; ++i) {
        v.insert("key" + std::to_string(i), JsonValue(i));
    }
    // O(1) lookup
    EXPECT_EQ(v["key50"].as_integer(), 50);
    EXPECT_EQ(v["key99"].as_integer(), 99);
    EXPECT_TRUE(v.contains("key0"));
    EXPECT_FALSE(v.contains("missing"));
}

TEST(ObjectLookup, FindReturnsNullOnMissing) {
    auto v = JsonValue::object();
    v.insert("x", JsonValue(1));
    EXPECT_NE(v.find("x"), nullptr);
    EXPECT_EQ(v.find("y"), nullptr);
}

TEST(ObjectLookup, PreservesInsertionOrder) {
    auto v = JsonValue::object();
    v.insert("c", JsonValue(3));
    v.insert("a", JsonValue(1));
    v.insert("b", JsonValue(2));

    const auto& obj = v.as_object();
    auto it = obj.begin();
    EXPECT_EQ(it->first, "c"); ++it;
    EXPECT_EQ(it->first, "a"); ++it;
    EXPECT_EQ(it->first, "b");
}

TEST(ObjectLookup, EraseInvalidatesIndex) {
    auto v = JsonValue::object();
    for (int i = 0; i < 20; ++i) {
        v.insert("k" + std::to_string(i), JsonValue(i));
    }
    EXPECT_TRUE(v.contains("k5"));
    v.erase("k5");
    EXPECT_FALSE(v.contains("k5"));
    EXPECT_TRUE(v.contains("k10")); // Other keys still accessible
}

TEST(ObjectLookup, InsertUpdatesExisting) {
    auto v = JsonValue::object();
    v.insert("x", JsonValue(1));
    EXPECT_EQ(v["x"].as_integer(), 1);
    v.insert("x", JsonValue(99));
    EXPECT_EQ(v["x"].as_integer(), 99);
    EXPECT_EQ(v.size(), 1u); // No duplicate
}

// ═══════════════════════════════════════════════════════════════════════════════
// Streaming parser / serializer
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Streaming, ParseFromIStream) {
    std::istringstream iss(R"({"key": "value", "num": 42})");
    auto v = parse(iss);
    EXPECT_EQ(v["key"].as_string(), "value");
    EXPECT_EQ(v["num"].as_integer(), 42);
}

TEST(Streaming, ParseFromIStreamError) {
    std::istringstream iss("{bad json}");
    EXPECT_THROW(parse(iss), ParseError);
}

TEST(Streaming, TryParseFromIStream) {
    std::istringstream iss("[1,2,3]");
    auto [val, ec] = try_parse(iss);
    EXPECT_FALSE(static_cast<bool>(ec));
    EXPECT_EQ(val.size(), 3u);
}

TEST(Streaming, OperatorExtract) {
    std::istringstream iss("42");
    JsonValue v;
    iss >> v;
    EXPECT_EQ(v.as_integer(), 42);
}

TEST(Streaming, SerializeToOStream) {
    auto v = parse(R"({"a":1})");
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), R"({"a":1})");
}

TEST(Streaming, SerializeToOStreamWithOptions) {
    auto v = parse("[1,2,3]");
    std::ostringstream oss;
    SerializeOptions opts;
    opts.indent = 2;
    yajson::serialize(oss, v, opts);
    auto result = oss.str();
    EXPECT_NE(result.find('\n'), std::string::npos); // pretty-printed
}

TEST(Streaming, NaNInfinitySerialize) {
    SerializeOptions opts;
    opts.allow_nan_inf = true;
    EXPECT_EQ(serialize(JsonValue(std::numeric_limits<double>::quiet_NaN()), opts), "NaN");
    EXPECT_EQ(serialize(JsonValue(std::numeric_limits<double>::infinity()), opts), "Infinity");
    EXPECT_EQ(serialize(JsonValue(-std::numeric_limits<double>::infinity()), opts), "-Infinity");
}

TEST(Streaming, SortKeysOption) {
    auto v = JsonValue::object();
    v.insert("c", JsonValue(3));
    v.insert("a", JsonValue(1));
    v.insert("b", JsonValue(2));

    SerializeOptions opts;
    opts.sort_keys = true;
    auto s = serialize(v, opts);
    EXPECT_EQ(s, R"({"a":1,"b":2,"c":3})");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Conversion system (to_json / from_json)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Conversion, BasicTypesRoundtrip) {
    EXPECT_EQ(to_value(42).as_integer(), 42);
    EXPECT_EQ(to_value(3.14).as_float(), 3.14);
    EXPECT_EQ(to_value(true).as_bool(), true);
    EXPECT_EQ(to_value(std::string("hello")).as_string(), "hello");
    EXPECT_TRUE(to_value(nullptr).is_null());
}

TEST(Conversion, FromValueBasicTypes) {
    EXPECT_EQ(from_value<int>(JsonValue(42)), 42);
    EXPECT_DOUBLE_EQ(from_value<double>(JsonValue(3.14)), 3.14);
    EXPECT_EQ(from_value<bool>(JsonValue(true)), true);
    EXPECT_EQ(from_value<std::string>(JsonValue("hi")), "hi");
}

TEST(Conversion, VectorRoundtrip) {
    std::vector<int> v = {1, 2, 3, 4, 5};
    auto j = to_value(v);
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 5u);

    auto v2 = from_value<std::vector<int>>(j);
    EXPECT_EQ(v, v2);
}

TEST(Conversion, MapRoundtrip) {
    std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
    auto j = to_value(m);
    EXPECT_TRUE(j.is_object());

    auto m2 = from_value<std::map<std::string, int>>(j);
    EXPECT_EQ(m, m2);
}

TEST(Conversion, UnorderedMapRoundtrip) {
    std::unordered_map<std::string, double> m = {{"x", 1.5}, {"y", 2.5}};
    auto j = to_value(m);
    EXPECT_TRUE(j.is_object());

    auto m2 = from_value<std::unordered_map<std::string, double>>(j);
    EXPECT_EQ(m, m2);
}

TEST(Conversion, OptionalWithValue) {
    std::optional<int> opt = 42;
    auto j = to_value(opt);
    EXPECT_EQ(j.as_integer(), 42);

    auto opt2 = from_value<std::optional<int>>(j);
    EXPECT_TRUE(opt2.has_value());
    EXPECT_EQ(*opt2, 42);
}

TEST(Conversion, OptionalNull) {
    std::optional<int> opt = std::nullopt;
    auto j = to_value(opt);
    EXPECT_TRUE(j.is_null());

    auto opt2 = from_value<std::optional<int>>(j);
    EXPECT_FALSE(opt2.has_value());
}

TEST(Conversion, NestedContainers) {
    std::vector<std::vector<int>> vv = {{1, 2}, {3, 4}};
    auto j = to_value(vv);
    EXPECT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0].size(), 2u);

    auto vv2 = from_value<std::vector<std::vector<int>>>(j);
    EXPECT_EQ(vv, vv2);
}

TEST(Conversion, FromValueOrFallback) {
    EXPECT_EQ(from_value_or<int>(JsonValue("not a number"), 99), 99);
    EXPECT_EQ(from_value_or<int>(JsonValue(42), 99), 42);
}

// Struct conversion with macros
struct Point {
    double x = 0;
    double y = 0;
};
JSON_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)

TEST(Conversion, StructNonIntrusive) {
    Point p{1.5, 2.5};
    auto j = to_value(p);
    EXPECT_EQ(j["x"].as_float(), 1.5);
    EXPECT_EQ(j["y"].as_float(), 2.5);

    auto p2 = from_value<Point>(j);
    EXPECT_EQ(p2.x, 1.5);
    EXPECT_EQ(p2.y, 2.5);
}

struct Color {
    int r = 0, g = 0, b = 0;
    std::string name;
    JSON_DEFINE_TYPE_INTRUSIVE(Color, r, g, b, name)
};

TEST(Conversion, StructIntrusive) {
    Color c{255, 128, 0, "orange"};
    auto j = to_value(c);
    EXPECT_EQ(j["r"].as_integer(), 255);
    EXPECT_EQ(j["name"].as_string(), "orange");

    auto c2 = from_value<Color>(j);
    EXPECT_EQ(c2.r, 255);
    EXPECT_EQ(c2.g, 128);
    EXPECT_EQ(c2.b, 0);
    EXPECT_EQ(c2.name, "orange");
}

struct Config {
    std::string host;
    int port = 0;
    std::vector<std::string> features;
    std::optional<double> timeout;
};
JSON_DEFINE_TYPE_NON_INTRUSIVE(Config, host, port, features, timeout)

TEST(Conversion, StructWithContainers) {
    Config cfg{"localhost", 8080, {"auth", "logging"}, 30.0};
    auto j = to_value(cfg);

    auto cfg2 = from_value<Config>(j);
    EXPECT_EQ(cfg2.host, "localhost");
    EXPECT_EQ(cfg2.port, 8080);
    EXPECT_EQ(cfg2.features.size(), 2u);
    EXPECT_EQ(cfg2.features[0], "auth");
    EXPECT_TRUE(cfg2.timeout.has_value());
    EXPECT_DOUBLE_EQ(*cfg2.timeout, 30.0);
}

TEST(Conversion, FullRoundtrip) {
    // struct -> JsonValue -> string -> parse -> JsonValue -> struct
    Point p{42.0, -17.5};
    auto j1 = to_value(p);
    auto s = j1.dump();
    auto j2 = parse(s);
    auto p2 = from_value<Point>(j2);
    EXPECT_DOUBLE_EQ(p2.x, 42.0);
    EXPECT_DOUBLE_EQ(p2.y, -17.5);
}

TEST(Conversion, VectorOfStructs) {
    std::vector<Point> pts = {{1, 2}, {3, 4}, {5, 6}};
    auto j = to_value(pts);
    EXPECT_EQ(j.size(), 3u);

    auto pts2 = from_value<std::vector<Point>>(j);
    EXPECT_EQ(pts2.size(), 3u);
    EXPECT_EQ(pts2[1].x, 3);
    EXPECT_EQ(pts2[1].y, 4);
}

// ═══════════════════════════════════════════════════════════════════════════════
// string_view API
// ═══════════════════════════════════════════════════════════════════════════════

TEST(StringViewAPI, ConstructFromStringView) {
    std::string_view sv = "hello";
    JsonValue v(sv);
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(StringViewAPI, ContainsWithStringView) {
    auto v = parse(R"({"key": 1})");
    std::string_view key = "key";
    EXPECT_TRUE(v.contains(key));
    EXPECT_FALSE(v.contains(std::string_view("other")));
}

TEST(StringViewAPI, FindWithStringView) {
    auto v = parse(R"({"x": 42})");
    std::string_view key = "x";
    auto* p = v.find(key);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->as_integer(), 42);
}

TEST(StringViewAPI, InsertWithStringView) {
    auto v = JsonValue::object();
    std::string_view key = "test";
    v.insert(key, JsonValue(1));
    EXPECT_EQ(v["test"].as_integer(), 1);
}

TEST(StringViewAPI, GetTemplateStringView) {
    JsonValue v("hello");
    auto sv = v.get<std::string_view>();
    EXPECT_EQ(sv, "hello");
}

// ═══════════════════════════════════════════════════════════════════════════════
// PMR allocator availability
// ═══════════════════════════════════════════════════════════════════════════════

#if YAJSON_HAS_PMR
TEST(Allocator, PMRAvailable) {
    // Verify PMR types are accessible
    auto* mr = yajson::get_default_resource();
    EXPECT_NE(mr, nullptr);
}

TEST(Allocator, ScopedResource) {
    char buf[4096];
    std::pmr::monotonic_buffer_resource mbr(buf, sizeof(buf));
    {
        yajson::ScopedResource scoped(&mbr);
        // Default resource changed within scope
        EXPECT_EQ(std::pmr::get_default_resource(), &mbr);
    }
    // Restored outside scope
}
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Depth limiting
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DepthLimit, DefaultAllowsDeepNesting) {
    std::string deep = "[";
    for (int i = 0; i < 100; ++i) deep += "[";
    deep += "1";
    for (int i = 0; i < 101; ++i) deep += "]";
    EXPECT_NO_THROW(parse(deep));
}

TEST(DepthLimit, CustomLimitEnforced) {
    ParseOptions opts;
    opts.max_depth = 5;

    std::string ok = "[[[[1]]]]"; // depth 4
    EXPECT_NO_THROW(parse(ok, opts));

    std::string too_deep = "[[[[[[1]]]]]]"; // depth 6
    EXPECT_THROW(parse(too_deep, opts), ParseError);
}
