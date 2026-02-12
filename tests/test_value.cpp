/// @file test_value.cpp
/// @brief Unit tests for yajson::JsonValue — constructors, types, access, mutation.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Constructors and type checking
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, DefaultConstructorIsNull) {
    JsonValue v;
    EXPECT_TRUE(v.is_null());
    EXPECT_EQ(v.type(), Type::Null);
}

TEST(JsonValue, NullptrConstructor) {
    JsonValue v(nullptr);
    EXPECT_TRUE(v.is_null());
}

TEST(JsonValue, BoolConstructor) {
    JsonValue t(true);
    JsonValue f(false);

    EXPECT_TRUE(t.is_bool());
    EXPECT_TRUE(f.is_bool());
    EXPECT_EQ(t.as_bool(), true);
    EXPECT_EQ(f.as_bool(), false);
    EXPECT_EQ(t.type(), Type::Bool);
}

TEST(JsonValue, IntegerConstructor) {
    JsonValue v_int(42);
    JsonValue v_int64(int64_t(1234567890123LL));
    JsonValue v_neg(-100);
    JsonValue v_zero(0);

    EXPECT_TRUE(v_int.is_integer());
    EXPECT_TRUE(v_int64.is_integer());
    EXPECT_TRUE(v_neg.is_integer());
    EXPECT_TRUE(v_zero.is_integer());

    EXPECT_EQ(v_int.as_integer(), 42);
    EXPECT_EQ(v_int64.as_integer(), 1234567890123LL);
    EXPECT_EQ(v_neg.as_integer(), -100);
    EXPECT_EQ(v_zero.as_integer(), 0);
}

TEST(JsonValue, FloatConstructor) {
    JsonValue v(3.14);
    EXPECT_TRUE(v.is_float());
    EXPECT_TRUE(v.is_number());
    EXPECT_DOUBLE_EQ(v.as_float(), 3.14);
    EXPECT_EQ(v.type(), Type::Float);
}

TEST(JsonValue, StringConstructors) {
    // const char*
    JsonValue v1("hello");
    EXPECT_TRUE(v1.is_string());
    EXPECT_EQ(v1.as_string(), "hello");

    // std::string copy
    std::string s = "world";
    JsonValue v2(s);
    EXPECT_EQ(v2.as_string(), "world");

    // std::string move
    std::string s2 = "moved";
    JsonValue v3(std::move(s2));
    EXPECT_EQ(v3.as_string(), "moved");
}

TEST(JsonValue, ArrayConstructor) {
    Array arr = {JsonValue(1), JsonValue(2), JsonValue(3)};
    JsonValue v(arr);

    EXPECT_TRUE(v.is_array());
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].as_integer(), 1);
    EXPECT_EQ(v[1].as_integer(), 2);
    EXPECT_EQ(v[2].as_integer(), 3);
}

TEST(JsonValue, ArrayMoveConstructor) {
    Array arr = {JsonValue("a"), JsonValue("b")};
    JsonValue v(std::move(arr));

    EXPECT_TRUE(v.is_array());
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].as_string(), "a");
}

TEST(JsonValue, ObjectConstructor) {
    Object obj = {{"name", JsonValue("test")}, {"value", JsonValue(42)}};
    JsonValue v(obj);

    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v["name"].as_string(), "test");
    EXPECT_EQ(v["value"].as_integer(), 42);
}

TEST(JsonValue, StaticFactories) {
    auto arr = JsonValue::array();
    auto obj = JsonValue::object();

    EXPECT_TRUE(arr.is_array());
    EXPECT_TRUE(obj.is_object());
    EXPECT_TRUE(arr.empty());
    EXPECT_TRUE(obj.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Value access — type checking and exceptions
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, TypeErrorOnWrongAccess) {
    JsonValue v(42);
    EXPECT_THROW(v.as_bool(), TypeError);
    EXPECT_THROW(v.as_string(), TypeError);
    EXPECT_THROW(v.as_array(), TypeError);
    EXPECT_THROW(v.as_object(), TypeError);
}

TEST(JsonValue, AsFloatConvertsInteger) {
    JsonValue v(100);
    EXPECT_DOUBLE_EQ(v.as_float(), 100.0);
}

TEST(JsonValue, IsNumberForBothTypes) {
    EXPECT_TRUE(JsonValue(42).is_number());
    EXPECT_TRUE(JsonValue(3.14).is_number());
    EXPECT_FALSE(JsonValue("42").is_number());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Templated access get<T>()
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, GetTemplate) {
    JsonValue b(true);
    JsonValue i(42);
    JsonValue f(2.5);
    JsonValue s("hello");

    EXPECT_EQ(b.get<bool>(), true);
    EXPECT_EQ(i.get<int64_t>(), 42);
    EXPECT_EQ(i.get<int>(), 42);
    EXPECT_DOUBLE_EQ(f.get<double>(), 2.5);
    EXPECT_EQ(s.get<std::string>(), "hello");
}

TEST(JsonValue, GetOrWithDefault) {
    JsonValue v(42);
    EXPECT_EQ(v.get_or<int64_t>(0), 42);
    EXPECT_EQ(v.get_or<std::string>("default"), "default"); // Wrong type — returns the default value
}

// ═══════════════════════════════════════════════════════════════════════════════
// Access by index and key
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, ArrayIndexAccess) {
    Array arr = {JsonValue(10), JsonValue(20), JsonValue(30)};
    JsonValue v(arr);

    EXPECT_EQ(v[0].as_integer(), 10);
    EXPECT_EQ(v[2].as_integer(), 30);
    EXPECT_THROW(v[5], OutOfRangeError);
}

TEST(JsonValue, ObjectKeyAccess) {
    auto obj = JsonValue::object();
    obj["key1"] = JsonValue("value1");
    obj["key2"] = JsonValue(100);

    EXPECT_EQ(obj["key1"].as_string(), "value1");
    EXPECT_EQ(obj["key2"].as_integer(), 100);
}

TEST(JsonValue, ObjectKeyCreatesEntryOnMiss) {
    auto obj = JsonValue::object();
    // Accessing a non-existent key creates a null entry
    JsonValue& ref = obj["new_key"];
    EXPECT_TRUE(ref.is_null());
    EXPECT_EQ(obj.size(), 1u);
}

TEST(JsonValue, ObjectContains) {
    auto obj = JsonValue::object();
    obj["x"] = JsonValue(1);

    EXPECT_TRUE(obj.contains("x"));
    EXPECT_FALSE(obj.contains("y"));
}

TEST(JsonValue, ObjectFind) {
    auto obj = JsonValue::object();
    obj["key"] = JsonValue(42);

    const JsonValue* found = obj.find("key");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->as_integer(), 42);

    EXPECT_EQ(obj.find("missing"), nullptr);
}

TEST(JsonValue, ConstObjectThrowsOnMissingKey) {
    const JsonValue obj = JsonValue(Object{{"a", JsonValue(1)}});
    EXPECT_EQ(obj["a"].as_integer(), 1);
    EXPECT_THROW(obj["missing"], OutOfRangeError);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Mutation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, ArrayPushBack) {
    auto arr = JsonValue::array();
    arr.push_back(JsonValue(1));
    arr.push_back(JsonValue("two"));
    arr.push_back(JsonValue(3.0));

    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].as_integer(), 1);
    EXPECT_EQ(arr[1].as_string(), "two");
    EXPECT_DOUBLE_EQ(arr[2].as_float(), 3.0);
}

TEST(JsonValue, ObjectInsert) {
    auto obj = JsonValue::object();
    obj.insert("key", JsonValue("value"));
    EXPECT_EQ(obj["key"].as_string(), "value");

    // Update existing key
    obj.insert("key", JsonValue("updated"));
    EXPECT_EQ(obj["key"].as_string(), "updated");
    EXPECT_EQ(obj.size(), 1u); // Size did not change
}

TEST(JsonValue, ObjectErase) {
    auto obj = JsonValue::object();
    obj["a"] = JsonValue(1);
    obj["b"] = JsonValue(2);
    obj["c"] = JsonValue(3);

    EXPECT_TRUE(obj.erase("b"));
    EXPECT_EQ(obj.size(), 2u);
    EXPECT_FALSE(obj.contains("b"));
    EXPECT_FALSE(obj.erase("nonexistent"));
}

TEST(JsonValue, Clear) {
    auto arr = JsonValue::array();
    arr.push_back(JsonValue(1));
    arr.push_back(JsonValue(2));
    arr.clear();
    EXPECT_TRUE(arr.empty());

    auto obj = JsonValue::object();
    obj["x"] = JsonValue(1);
    obj.clear();
    EXPECT_TRUE(obj.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Copy and move semantics
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, CopySemantics) {
    auto original = JsonValue::object();
    original["key"] = JsonValue("value");

    JsonValue copy = original;
    EXPECT_EQ(copy["key"].as_string(), "value");

    // Modifying the copy does not affect the original
    copy["key"] = JsonValue("modified");
    EXPECT_EQ(original["key"].as_string(), "value");
    EXPECT_EQ(copy["key"].as_string(), "modified");
}

TEST(JsonValue, MoveSemantics) {
    auto arr = JsonValue::array();
    arr.push_back(JsonValue("test"));

    JsonValue moved = std::move(arr);
    EXPECT_TRUE(moved.is_array());
    EXPECT_EQ(moved[0].as_string(), "test");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Comparison
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, EqualityNull) {
    EXPECT_EQ(JsonValue(), JsonValue(nullptr));
}

TEST(JsonValue, EqualityBool) {
    EXPECT_EQ(JsonValue(true), JsonValue(true));
    EXPECT_NE(JsonValue(true), JsonValue(false));
}

TEST(JsonValue, EqualityNumbers) {
    EXPECT_EQ(JsonValue(42), JsonValue(42));
    EXPECT_NE(JsonValue(42), JsonValue(43));
    // integer == float when numeric values are equal
    EXPECT_EQ(JsonValue(42), JsonValue(42.0));
}

TEST(JsonValue, EqualityStrings) {
    EXPECT_EQ(JsonValue("hello"), JsonValue("hello"));
    EXPECT_NE(JsonValue("hello"), JsonValue("world"));
}

TEST(JsonValue, EqualityDifferentTypes) {
    EXPECT_NE(JsonValue(42), JsonValue("42"));
    EXPECT_NE(JsonValue(true), JsonValue(1));
}

TEST(JsonValue, EqualityArrays) {
    Array a1 = {JsonValue(1), JsonValue(2)};
    Array a2 = {JsonValue(1), JsonValue(2)};
    Array a3 = {JsonValue(1), JsonValue(3)};

    EXPECT_EQ(JsonValue(a1), JsonValue(a2));
    EXPECT_NE(JsonValue(a1), JsonValue(a3));
}

TEST(JsonValue, EqualityObjects) {
    auto o1 = JsonValue::object();
    o1["a"] = JsonValue(1); o1["b"] = JsonValue(2);
    auto o2 = JsonValue::object();
    o2["a"] = JsonValue(1); o2["b"] = JsonValue(2);
    auto o3 = JsonValue::object();
    o3["a"] = JsonValue(1); o3["b"] = JsonValue(3);

    EXPECT_EQ(o1, o2);
    EXPECT_NE(o1, o3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Nested structures
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, NestedStructures) {
    auto root = JsonValue::object();
    root["name"] = JsonValue("test");

    auto inner = JsonValue::array();
    inner.push_back(JsonValue(1));
    inner.push_back(JsonValue(2));

    auto nested_obj = JsonValue::object();
    nested_obj["data"] = std::move(inner);

    root["nested"] = std::move(nested_obj);

    EXPECT_EQ(root["name"].as_string(), "test");
    EXPECT_EQ(root["nested"]["data"][0].as_integer(), 1);
    EXPECT_EQ(root["nested"]["data"][1].as_integer(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Size and emptiness
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JsonValue, SizeAndEmpty) {
    EXPECT_EQ(JsonValue().size(), 0u);
    EXPECT_TRUE(JsonValue().empty());

    EXPECT_EQ(JsonValue(42).size(), 0u);
    EXPECT_FALSE(JsonValue(42).empty());

    auto arr = JsonValue::array();
    EXPECT_TRUE(arr.empty());
    arr.push_back(JsonValue(1));
    EXPECT_EQ(arr.size(), 1u);
    EXPECT_FALSE(arr.empty());

    auto obj = JsonValue::object();
    EXPECT_TRUE(obj.empty());
    obj["x"] = JsonValue(1);
    EXPECT_EQ(obj.size(), 1u);
    EXPECT_FALSE(obj.empty());
}
