/// @file test_leak_asan.cpp
/// @brief Reduced-iteration stress test for ASan/UBSan leak detection.
/// Designed to complete in < 60 seconds under sanitizers.

#include "json/json.hpp"
#include "json/stream_parser.hpp"
#include "json/json_pointer.hpp"
#include "json/arena.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

using namespace yajson;

static std::string make_medium_json(int id) {
    std::string s = "{\"id\":" + std::to_string(id) +
        ",\"name\":\"test_item_" + std::to_string(id) + "\""
        ",\"values\":[1,2,3,4,5,6,7,8,9,10]"
        ",\"nested\":{\"a\":true,\"b\":null,\"c\":3.14159265358979}"
        ",\"tags\":[\"alpha\",\"beta\",\"gamma\",\"delta\"]"
        ",\"description\":\"This is a longer string to test heap allocation and SSO boundary behavior for strings of various lengths\""
        "}";
    return s;
}

static std::string make_large_json(int count) {
    std::string s = "[";
    for (int i = 0; i < count; ++i) {
        if (i > 0) s += ',';
        s += make_medium_json(i);
    }
    s += ']';
    return s;
}

// Test 1: Parse/serialize roundtrip
static void test_parse_serialize_cycle() {
    printf("  [1] Parse/serialize cycle (1K iterations)...\n");
    std::string input = make_medium_json(42);
    for (int i = 0; i < 1000; ++i) {
        auto v = parse(input);
        std::string output = v.dump();
        auto v2 = parse(output);
        assert(v2["id"].as_integer() == 42);
    }
    printf("      PASSED\n");
}

// Test 2: Arena parse/reset cycle
static void test_arena_cycle() {
    printf("  [2] Arena parse/reset cycle (1K iterations)...\n");
    std::string input = make_medium_json(99);
    MonotonicArena arena(4096);
    for (int i = 0; i < 1000; ++i) {
        {
            ArenaScope scope(arena);
            auto v = parse(input);
            assert(v["id"].as_integer() == 99);
        }
        arena.reset();
    }
    printf("      PASSED\n");
}

// Test 3: Large document parse + deep copy + modify + serialize
static void test_large_document() {
    printf("  [3] Large document (100 items, 500 cycles)...\n");
    std::string input = make_large_json(100);
    for (int i = 0; i < 500; ++i) {
        auto v = parse(input);
        assert(v.as_array().size() == 100);
        JsonValue copy = v;
        assert(copy.as_array().size() == 100);
        copy.as_array()[0] = JsonValue(42);
        std::string output = copy.dump();
        assert(!output.empty());
    }
    printf("      PASSED\n");
}

// Test 4: Object mutations
static void test_object_mutations() {
    printf("  [4] Object mutations (2K cycles, 100 keys each)...\n");
    for (int cycle = 0; cycle < 2000; ++cycle) {
        JsonValue obj = JsonValue::object();
        for (int k = 0; k < 100; ++k) {
            obj.insert("key_" + std::to_string(k), JsonValue(k));
        }
        assert(obj.size() == 100);
        for (int k = 0; k < 100; ++k) {
            auto* p = obj.find("key_" + std::to_string(k));
            assert(p && p->as_integer() == k);
        }
        for (int k = 0; k < 50; ++k) {
            obj.erase("key_" + std::to_string(k));
        }
        assert(obj.size() == 50);
        JsonValue copy = obj;
        assert(copy.size() == 50);
    }
    printf("      PASSED\n");
}

// Test 5: JSON Pointer operations
static void test_json_pointer() {
    printf("  [5] JSON Pointer operations (2K cycles)...\n");
    std::string input = R"({
        "users": [
            {"name": "Alice", "age": 30, "tags": ["admin", "user"]},
            {"name": "Bob", "age": 25, "tags": ["user"]}
        ],
        "meta": {"version": 1, "count": 2}
    })";
    for (int i = 0; i < 2000; ++i) {
        auto v = parse(input);
        JsonPointer p1("/users/0/name");
        assert(p1.resolve(v).as_string_view() == "Alice");
        JsonPointer p2("/users/1/age");
        auto* res = p2.try_resolve(v);
        assert(res && res->as_integer() == 25);
        JsonPointer p3("/users/99/name");
        assert(p3.try_resolve(v) == nullptr);
        JsonPointer p4("/meta/new_field");
        p4.set(v, JsonValue("hello"));
        assert(v.find("meta")->find("new_field")->as_string_view() == "hello");
        JsonPointer p5("/meta/new_field");
        assert(p5.erase(v));
        auto p6 = p1.append("extra");
        auto p7 = p1.parent();
        (void)p6.to_string();
        (void)p7.to_string();
    }
    printf("      PASSED\n");
}

// Test 6: Multi-threaded parse stress
static void test_multithreaded() {
    printf("  [6] Multi-threaded parse (4 threads, 500 each)...\n");
    std::string input = make_medium_json(77);
    auto worker = [&input]() {
        MonotonicArena arena(8192);
        for (int i = 0; i < 500; ++i) {
            if (i % 2 == 0) {
                auto v = parse(input);
                assert(v["id"].as_integer() == 77);
                std::string s = v.dump();
                assert(!s.empty());
            } else {
                {
                    ArenaScope scope(arena);
                    auto v = parse(input);
                    assert(v["id"].as_integer() == 77);
                }
                arena.reset();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();
    printf("      PASSED\n");
}

// Test 7: String edge cases
static void test_string_edge_cases() {
    printf("  [7] String edge cases (2K cycles)...\n");
    std::string short_str = "\"abcdefghijklmn\"";
    std::string boundary_str = "\"abcdefghijklmno\"";
    std::string long_str = "\"abcdefghijklmnop\"";
    std::string escaped = "\"hello\\nworld\\t\\\"quoted\\\"\"";
    std::string unicode = "\"\\u0041\\u0042\\u0043\"";
    std::string surrogate = "\"\\uD83D\\uDE00\"";

    for (int i = 0; i < 2000; ++i) {
        auto v1 = parse(short_str);
        assert(v1.as_string_view() == "abcdefghijklmn");
        auto v2 = parse(boundary_str);
        assert(v2.as_string_view() == "abcdefghijklmno");
        auto v3 = parse(long_str);
        assert(v3.as_string_view() == "abcdefghijklmnop");
        auto v4 = parse(escaped);
        assert(v4.as_string_view() == "hello\nworld\t\"quoted\"");
        auto v5 = parse(unicode);
        assert(v5.as_string_view() == "ABC");
        auto v6 = parse(surrogate);
        assert(v6.as_string_view().size() == 4);
        std::string s3 = v3.dump();
        auto v3b = parse(s3);
        assert(v3b.as_string_view() == v3.as_string_view());
    }
    printf("      PASSED\n");
}

// Test 8: Stream parser
static void test_stream_parser() {
    printf("  [8] Stream parser (500 cycles)...\n");
    std::string input = make_large_json(50);
    for (int i = 0; i < 500; ++i) {
        std::istringstream iss(input);
        auto v = parse(iss);
        assert(v.as_array().size() == 50);
    }
    printf("      PASSED\n");
}

// Test 9: Conversions
static void test_conversions() {
    printf("  [9] Type conversions (5K cycles)...\n");
    for (int i = 0; i < 5000; ++i) {
        JsonValue iv(static_cast<int64_t>(i));
        assert(iv.get<int>() == i);
        assert(iv.get_or<int>(0) == i);
        assert(iv.get_or<double>(0.0) == static_cast<double>(i));

        JsonValue sv("hello");
        assert(sv.get_or<std::string>("") == "hello");
        assert(sv.get_or<std::string_view>("") == "hello");

        JsonValue bv(true);
        assert(bv.get_or<bool>(false) == true);

        assert(iv.get_or<std::string>("default") == "default");
        assert(sv.get_or<int>(42) == 42);
    }
    printf("      PASSED\n");
}

// Test 10: Move semantics stress
static void test_move_semantics() {
    printf("  [10] Move semantics stress (2K cycles)...\n");
    for (int cycle = 0; cycle < 2000; ++cycle) {
        JsonValue arr = JsonValue::array();
        for (int i = 0; i < 50; ++i) {
            arr.push_back(JsonValue("string_value_" + std::to_string(i)));
        }
        assert(arr.size() == 50);
        JsonValue moved = std::move(arr);
        assert(moved.size() == 50);
        JsonValue other = JsonValue::object();
        moved.swap(other);
        assert(other.size() == 50);
        assert(moved.is_object());
        moved = std::move(other);
        assert(moved.size() == 50);
        JsonValue copied = moved;
        assert(copied.size() == 50);
    }
    printf("      PASSED\n");
}

int main() {
    printf("=== yajson Memory Leak Stress Test (ASan-friendly) ===\n\n");

    test_parse_serialize_cycle();
    test_arena_cycle();
    test_large_document();
    test_object_mutations();
    test_json_pointer();
    test_multithreaded();
    test_string_edge_cases();
    test_stream_parser();
    test_conversions();
    test_move_semantics();

    printf("\n=== ALL STRESS TESTS PASSED ===\n");
    return 0;
}
