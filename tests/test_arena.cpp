/// @file test_arena.cpp
/// @brief Unit tests for MonotonicArena and arena-aware JSON parsing.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace yajson;

// =============================================================================
// MonotonicArena basic tests
// =============================================================================

TEST(MonotonicArena, DefaultConstruction) {
    MonotonicArena arena;
    EXPECT_GT(arena.bytes_allocated(), 0u);
    EXPECT_EQ(arena.bytes_used(), 0u);
}

TEST(MonotonicArena, StackBufferConstruction) {
    alignas(16) char buf[1024];
    MonotonicArena arena(buf, sizeof(buf));
    EXPECT_EQ(arena.bytes_allocated(), sizeof(buf));
    EXPECT_EQ(arena.bytes_used(), 0u);
    EXPECT_EQ(arena.bytes_remaining(), sizeof(buf));
}

TEST(MonotonicArena, BasicAllocation) {
    MonotonicArena arena(4096);
    void* p1 = arena.allocate(100);
    ASSERT_NE(p1, nullptr);
    EXPECT_GE(arena.bytes_used(), 100u);

    void* p2 = arena.allocate(200);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_GE(arena.bytes_used(), 300u);
}

TEST(MonotonicArena, AlignedAllocation) {
    MonotonicArena arena(4096);

    // Allocate 1 byte to potentially misalign
    arena.allocate(1, 1);

    // Request 8-byte aligned allocation
    void* p = arena.allocate(16, 8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);

    // Request 16-byte aligned allocation
    arena.allocate(1, 1);
    void* p2 = arena.allocate(32, 16);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0u);
}

TEST(MonotonicArena, OverflowToHeapBlock) {
    alignas(16) char buf[64];
    MonotonicArena arena(buf, sizeof(buf));

    // First allocation fits in the stack buffer
    void* p1 = arena.allocate(32, 1);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(arena.block_count(), 0u);

    // This allocation exceeds the stack buffer, triggers overflow
    void* p2 = arena.allocate(128, 1);
    ASSERT_NE(p2, nullptr);
    EXPECT_GE(arena.block_count(), 1u);
    EXPECT_GT(arena.bytes_allocated(), sizeof(buf));
}

TEST(MonotonicArena, Reset) {
    alignas(16) char buf[1024];
    MonotonicArena arena(buf, sizeof(buf));

    arena.allocate(512, 1);
    EXPECT_GE(arena.bytes_used(), 512u);

    arena.reset();
    EXPECT_EQ(arena.bytes_used(), 0u);
    EXPECT_EQ(arena.bytes_remaining(), sizeof(buf));

    // Can allocate again after reset
    void* p = arena.allocate(256, 1);
    ASSERT_NE(p, nullptr);
}

TEST(MonotonicArena, ResetFreesOverflowBlocks) {
    alignas(16) char buf[64];
    MonotonicArena arena(buf, sizeof(buf));

    // Force overflow
    arena.allocate(256, 1);
    EXPECT_GE(arena.block_count(), 1u);

    arena.reset();
    EXPECT_EQ(arena.block_count(), 0u);
    EXPECT_EQ(arena.bytes_remaining(), sizeof(buf));
}

TEST(MonotonicArena, Construct) {
    MonotonicArena arena(4096);
    auto* s = arena.construct<std::string>("hello world");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "hello world");
    s->~basic_string();  // Manual destruct (arena doesn't call destructors)
}

TEST(MonotonicArena, HeapOnlyReset) {
    MonotonicArena arena(512);
    arena.allocate(256, 1);
    EXPECT_GE(arena.bytes_used(), 256u);

    arena.reset();
    EXPECT_EQ(arena.bytes_used(), 0u);

    void* p = arena.allocate(128, 1);
    ASSERT_NE(p, nullptr);
}

// =============================================================================
// ArenaScope tests
// =============================================================================

TEST(ArenaScope, ActivatesAndDeactivates) {
    MonotonicArena arena(4096);
    EXPECT_EQ(detail::current_arena, nullptr);

    {
        ArenaScope scope(arena);
        EXPECT_EQ(detail::current_arena, &arena);
    }
    EXPECT_EQ(detail::current_arena, nullptr);
}

TEST(ArenaScope, Nesting) {
    MonotonicArena arena1(4096);
    MonotonicArena arena2(4096);

    EXPECT_EQ(detail::current_arena, nullptr);
    {
        ArenaScope s1(arena1);
        EXPECT_EQ(detail::current_arena, &arena1);
        {
            ArenaScope s2(arena2);
            EXPECT_EQ(detail::current_arena, &arena2);
        }
        EXPECT_EQ(detail::current_arena, &arena1);
    }
    EXPECT_EQ(detail::current_arena, nullptr);
}

// =============================================================================
// Arena-aware JsonValue tests
// =============================================================================

TEST(ArenaJsonValue, SsoStringUnaffected) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    // Short string uses SSO regardless of arena
    JsonValue v("hello");
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string_view(), "hello");

    size_t used = arena.bytes_used();
    // SSO string should not allocate from arena
    JsonValue v2("short");
    EXPECT_EQ(arena.bytes_used(), used);
}

TEST(ArenaJsonValue, LongStringUsesArena) {
    MonotonicArena arena(4096);
    size_t before = arena.bytes_used();

    {
        ArenaScope scope(arena);

        // String longer than SSO threshold (15 chars)
        std::string long_str(100, 'x');
        JsonValue v(long_str);
        EXPECT_TRUE(v.is_string());
        EXPECT_EQ(v.as_string_view(), long_str);
        EXPECT_GT(arena.bytes_used(), before);

        // Verify the string data can be read correctly
        EXPECT_EQ(v.as_string(), long_str);
    }
}

TEST(ArenaJsonValue, ArenaStringMoveConstruct) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    std::string long_str(50, 'A');
    JsonValue v(std::move(long_str));
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string_view().size(), 50u);
    EXPECT_EQ(v.as_string_view(), std::string(50, 'A'));
}

TEST(ArenaJsonValue, ArrayUsesArena) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    Array arr = {JsonValue(1), JsonValue(2), JsonValue(3)};
    JsonValue v(std::move(arr));
    EXPECT_TRUE(v.is_array());
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].as_integer(), 1);
    EXPECT_EQ(v[2].as_integer(), 3);
}

TEST(ArenaJsonValue, ObjectUsesArena) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    Object obj;
    obj.insert("key", JsonValue(42));
    JsonValue v(std::move(obj));
    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(v["key"].as_integer(), 42);
}

TEST(ArenaJsonValue, MovePreservesArenaFlag) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    std::string long_str(100, 'B');
    JsonValue v1(long_str);
    EXPECT_EQ(v1.as_string_view(), long_str);

    // Move construct
    JsonValue v2(std::move(v1));
    EXPECT_TRUE(v1.is_null());  // moved-from
    EXPECT_EQ(v2.as_string_view(), long_str);

    // Move assign
    JsonValue v3;
    v3 = std::move(v2);
    EXPECT_TRUE(v2.is_null());  // moved-from
    EXPECT_EQ(v3.as_string_view(), long_str);
}

TEST(ArenaJsonValue, CopyFromArenaProducesIndependentValue) {
    MonotonicArena arena(4096);
    std::string long_str(100, 'C');

    JsonValue copy_outside;
    {
        ArenaScope scope(arena);
        JsonValue v(long_str);

        // Copy while arena scope is active -> copy goes to arena
        JsonValue copy_inside(v);
        EXPECT_EQ(copy_inside.as_string_view(), long_str);
    }

    // Re-enter to make copy that should still be valid after arena reset
    // Actually: copy outside arena scope should go to heap
    {
        ArenaScope scope(arena);
        JsonValue v(long_str);

        // Leave scope first, then copy
        detail::current_arena = nullptr;
        copy_outside = v;  // copy-assign outside arena scope -> heap allocated
        detail::current_arena = &arena;
    }

    arena.reset();

    // Heap-allocated copy should survive arena reset
    EXPECT_EQ(copy_outside.as_string_view(), long_str);
    EXPECT_EQ(copy_outside.as_string(), long_str);
}

TEST(ArenaJsonValue, SwapPreservesFlags) {
    MonotonicArena arena(4096);
    ArenaScope scope(arena);

    std::string str_a(100, 'A');
    std::string str_b(100, 'B');
    JsonValue a(str_a);
    JsonValue b(str_b);

    a.swap(b);
    EXPECT_EQ(a.as_string_view(), str_b);
    EXPECT_EQ(b.as_string_view(), str_a);
}

// =============================================================================
// Arena parsing tests
// =============================================================================

TEST(ArenaParse, SmallDocument) {
    MonotonicArena arena(4096);
    auto v = parse(R"({"name":"John","age":30,"active":true})", arena);

    EXPECT_TRUE(v.is_object());
    EXPECT_EQ(v["name"].as_string_view(), "John");
    EXPECT_EQ(v["age"].as_integer(), 30);
    EXPECT_EQ(v["active"].as_bool(), true);
}

TEST(ArenaParse, MediumDocument) {
    std::string input = R"({"users":[)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) input += ",";
        input += R"({"id":)" + std::to_string(i) +
                 R"(,"name":"user_)" + std::to_string(i) +
                 R"(","email":"user)" + std::to_string(i) +
                 R"(@example.com"})";
    }
    input += R"(],"total":20})";

    MonotonicArena arena(8192);
    auto v = parse(input, arena);

    EXPECT_EQ(v["total"].as_integer(), 20);
    EXPECT_EQ(v["users"].size(), 20u);
    EXPECT_EQ(v["users"][0]["id"].as_integer(), 0);
    EXPECT_EQ(v["users"][19]["name"].as_string_view(), "user_19");
}

TEST(ArenaParse, LargeDocumentWithLongStrings) {
    // Generate document with strings > SSO threshold
    std::string input = R"({"items":[)";
    for (int i = 0; i < 100; ++i) {
        if (i > 0) input += ",";
        std::string desc(50, 'a' + (i % 26));
        input += R"({"id":)" + std::to_string(i) +
                 R"(,"description":")" + desc + R"("})";
    }
    input += "]}";

    MonotonicArena arena(32768);
    auto v = parse(input, arena);

    EXPECT_EQ(v["items"].size(), 100u);
    for (int i = 0; i < 100; ++i) {
        auto desc = v["items"][static_cast<size_t>(i)]["description"].as_string_view();
        EXPECT_EQ(desc.size(), 50u);
        EXPECT_EQ(desc[0], 'a' + (i % 26));
    }
}

TEST(ArenaParse, TryParseWithArena) {
    MonotonicArena arena(4096);
    auto [v, ec] = try_parse(R"({"ok":true})", arena);

    EXPECT_FALSE(ec);
    EXPECT_TRUE(v["ok"].as_bool());
}

TEST(ArenaParse, TryParseErrorWithArena) {
    MonotonicArena arena(4096);
    auto [v, ec] = try_parse("{invalid}", arena);
    EXPECT_TRUE(ec);
}

TEST(ArenaParse, ArenaReuse) {
    char buf[4096];
    MonotonicArena arena(buf, sizeof(buf));

    // Parse first document
    {
        auto v = parse(R"({"a":1})", arena);
        EXPECT_EQ(v["a"].as_integer(), 1);
    }
    // The arena-allocated values are now out of scope (destructors called)

    arena.reset();

    // Parse second document, reusing the same arena memory
    {
        auto v = parse(R"({"b":2})", arena);
        EXPECT_EQ(v["b"].as_integer(), 2);
    }
}

TEST(ArenaParse, MultipleResetsAndParses) {
    char buf[8192];
    MonotonicArena arena(buf, sizeof(buf));

    for (int round = 0; round < 100; ++round) {
        std::string input = R"({"round":)" + std::to_string(round) +
                            R"(,"data":"value_)" + std::to_string(round) + R"("})";

        auto v = parse(input, arena);
        EXPECT_EQ(v["round"].as_integer(), round);

        arena.reset();
    }
}

TEST(ArenaParse, NestedArraysAndObjects) {
    MonotonicArena arena(8192);
    auto v = parse(R"({
        "matrix": [[1,2,3],[4,5,6],[7,8,9]],
        "meta": {"rows":3,"cols":3,"tags":["numeric","dense"]}
    })", arena);

    EXPECT_EQ(v["matrix"].size(), 3u);
    EXPECT_EQ(v["matrix"][1][2].as_integer(), 6);
    EXPECT_EQ(v["meta"]["tags"][0].as_string_view(), "numeric");
}

TEST(ArenaParse, StackBufferSufficientNoOverflow) {
    // Use a large stack buffer for a small document
    char buf[4096];
    MonotonicArena arena(buf, sizeof(buf));

    auto v = parse(R"({"x":1})", arena);
    EXPECT_EQ(v["x"].as_integer(), 1);
    EXPECT_EQ(arena.block_count(), 0u);  // No heap overflow blocks
}

TEST(ArenaParse, EqualityWithNormalParsedValues) {
    std::string input = R"({"name":"Alice","scores":[95,87,92],"active":true})";

    // Parse normally
    auto v_normal = parse(input);

    // Parse with arena
    MonotonicArena arena(4096);
    auto v_arena = parse(input, arena);

    // Values should be semantically equal
    EXPECT_EQ(v_normal, v_arena);
}

TEST(ArenaParse, SerializeArenaValue) {
    MonotonicArena arena(4096);
    auto v = parse(R"({"key":"value","num":42})", arena);

    std::string serialized = v.dump();
    EXPECT_FALSE(serialized.empty());

    // Re-parse the serialized output to verify roundtrip
    auto v2 = parse(serialized);
    EXPECT_EQ(v, v2);
}

// =============================================================================
// Multi-threaded arena tests
// =============================================================================

TEST(ArenaMultiThread, PerThreadArenas) {
    constexpr int kThreads = 4;
    constexpr int kIterations = 1000;

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&success_count, t]() {
            char buf[4096];
            MonotonicArena arena(buf, sizeof(buf));

            for (int i = 0; i < kIterations; ++i) {
                std::string input = R"({"thread":)" + std::to_string(t) +
                                    R"(,"iter":)" + std::to_string(i) +
                                    R"(,"data":"thread_)" + std::to_string(t) +
                                    "_iter_" + std::to_string(i) + R"("})";

                auto v = parse(input, arena);

                if (v["thread"].as_integer() == t &&
                    v["iter"].as_integer() == i) {
                    ++success_count;
                }

                arena.reset();
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(success_count.load(), kThreads * kIterations);
}

TEST(ArenaMultiThread, ArenaScopeIsThreadLocal) {
    MonotonicArena arena1(4096);
    MonotonicArena arena2(4096);

    std::atomic<bool> t1_ok{false};
    std::atomic<bool> t2_ok{false};

    std::thread t1([&]() {
        ArenaScope s(arena1);
        t1_ok = (detail::current_arena == &arena1);
    });

    std::thread t2([&]() {
        ArenaScope s(arena2);
        t2_ok = (detail::current_arena == &arena2);
    });

    t1.join();
    t2.join();

    EXPECT_TRUE(t1_ok.load());
    EXPECT_TRUE(t2_ok.load());
    EXPECT_EQ(detail::current_arena, nullptr);  // Main thread unaffected
}

// =============================================================================
// ArenaDocument: zero-config arena parsing
// =============================================================================

TEST(ArenaDocument, ParseAndRoot) {
    ArenaDocument doc;
    doc.parse(R"({"a":1,"b":[2,3]})");
    EXPECT_TRUE(doc.root().is_object());
    EXPECT_EQ(doc.root()["a"].as_integer(), 1);
    EXPECT_EQ(doc.root()["b"].size(), 2u);
    EXPECT_EQ(doc.root()["b"][0].as_integer(), 2);
}

TEST(ArenaDocument, ResetAndReuse) {
    ArenaDocument doc;
    doc.parse(R"([1,2,3])");
    EXPECT_TRUE(doc.root().is_array());
    EXPECT_EQ(doc.root().size(), 3u);
    doc.reset();
    EXPECT_TRUE(doc.root().is_null());
    doc.parse(R"("hello")");
    EXPECT_TRUE(doc.root().is_string());
    EXPECT_EQ(doc.root().as_string(), "hello");
}

TEST(ArenaDocument, TryParse) {
    ArenaDocument doc;
    auto res = doc.try_parse(R"({"x":42})");
    ASSERT_TRUE(res);
    EXPECT_EQ(doc.root()["x"].as_integer(), 42);
    res = doc.try_parse("invalid");
    EXPECT_FALSE(res);
}
