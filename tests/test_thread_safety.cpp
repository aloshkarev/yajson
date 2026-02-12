/// @file test_thread_safety.cpp
/// @brief Unit tests for yajson::ThreadSafeJson — thread safety.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Basic operations
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ThreadSafeJson, DefaultConstruction) {
    ThreadSafeJson tsj;
    EXPECT_EQ(tsj.type(), Type::Null);
}

TEST(ThreadSafeJson, ConstructFromValue) {
    auto obj = JsonValue::object();
    obj["key"] = JsonValue("value");

    ThreadSafeJson tsj(obj);
    EXPECT_EQ(tsj.type(), Type::Object);
    EXPECT_EQ(tsj.size(), 1u);
}

TEST(ThreadSafeJson, MoveConstruction) {
    auto obj = JsonValue::object();
    obj["key"] = JsonValue("value");

    ThreadSafeJson tsj(std::move(obj));
    EXPECT_EQ(tsj.type(), Type::Object);
}

TEST(ThreadSafeJson, ReadAccess) {
    ThreadSafeJson tsj(JsonValue(42));
    int64_t result = 0;
    tsj.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, 42);
}

TEST(ThreadSafeJson, WriteAccess) {
    ThreadSafeJson tsj(JsonValue(0));
    tsj.write([](JsonValue& v) {
        v = JsonValue(42);
    });

    int64_t result = 0;
    tsj.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, 42);
}

TEST(ThreadSafeJson, Snapshot) {
    auto obj = JsonValue::object();
    obj["key"] = JsonValue("value");
    ThreadSafeJson tsj(obj);

    JsonValue snap = tsj.snapshot();
    EXPECT_EQ(snap["key"].as_string(), "value");
}

TEST(ThreadSafeJson, Dump) {
    ThreadSafeJson tsj(JsonValue(42));
    EXPECT_EQ(tsj.dump(), "42");
}

TEST(ThreadSafeJson, Assign) {
    ThreadSafeJson tsj(JsonValue(1));
    tsj.assign(JsonValue(2));

    int64_t result = 0;
    tsj.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, 2);
}

TEST(ThreadSafeJson, InsertAndErase) {
    ThreadSafeJson tsj(JsonValue::object());

    tsj.insert("key", JsonValue(42));
    EXPECT_EQ(tsj.size(), 1u);

    bool erased = tsj.erase("key");
    EXPECT_TRUE(erased);
    EXPECT_EQ(tsj.size(), 0u);
}

TEST(ThreadSafeJson, PushBack) {
    ThreadSafeJson tsj(JsonValue::array());
    tsj.push_back(JsonValue(1));
    tsj.push_back(JsonValue(2));
    EXPECT_EQ(tsj.size(), 2u);
}

TEST(ThreadSafeJson, Update) {
    ThreadSafeJson tsj(JsonValue(10));
    tsj.update([](JsonValue v) -> JsonValue {
        return JsonValue(v.as_integer() * 2);
    });

    int64_t result = 0;
    tsj.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, 20);
}

// ═══════════════════════════════════════════════════════════════════════════════
// RAII-guards
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ThreadSafeJson, ReadGuard) {
    ThreadSafeJson tsj(JsonValue(42));

    auto guard = tsj.read_guard();
    EXPECT_EQ(guard->as_integer(), 42);
    EXPECT_TRUE((*guard).is_integer());
}

TEST(ThreadSafeJson, WriteGuard) {
    ThreadSafeJson tsj(JsonValue::object());

    {
        auto guard = tsj.write_guard();
        (*guard)["key"] = JsonValue("value");
    }

    auto snap = tsj.snapshot();
    EXPECT_EQ(snap["key"].as_string(), "value");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Multithreaded tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ThreadSafeJson, ConcurrentReads) {
    auto obj = JsonValue::object();
    for (int i = 0; i < 100; ++i) {
        obj.insert("key" + std::to_string(i), JsonValue(i));
    }
    ThreadSafeJson tsj(std::move(obj));

    constexpr int kThreads = 8;
    constexpr int kIterations = 10000;

    std::atomic<int> error_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tsj, &error_count, t]() {
            for (int i = 0; i < kIterations; ++i) {
                tsj.read([&error_count, t, i](const JsonValue& v) {
                    int key_idx = (t * kIterations + i) % 100;
                    auto key = "key" + std::to_string(key_idx);
                    const auto* found = v.find(key);
                    if (!found || found->as_integer() != key_idx) {
                        error_count.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(error_count.load(), 0);
}

TEST(ThreadSafeJson, ConcurrentWrites) {
    ThreadSafeJson tsj(JsonValue(int64_t(0)));

    constexpr int kThreads = 8;
    constexpr int kIterations = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tsj]() {
            for (int i = 0; i < kIterations; ++i) {
                tsj.write([](JsonValue& v) {
                    v = JsonValue(v.as_integer() + 1);
                });
            }
        });
    }

    for (auto& th : threads) th.join();

    int64_t result = 0;
    tsj.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, kThreads * kIterations);
}

TEST(ThreadSafeJson, ConcurrentReadWrite) {
    // Test concurrent reading and writing.
    // Writers increment counter, readers verify validity.
    ThreadSafeJson tsj(JsonValue(int64_t(0)));

    constexpr int kReaders = 4;
    constexpr int kWriters = 4;
    constexpr int kIterations = 5000;

    std::atomic<bool> stop{false};
    std::atomic<int> read_errors{0};

    // Readers
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&tsj, &stop, &read_errors]() {
            while (!stop.load(std::memory_order_acquire)) {
                tsj.read([&read_errors](const JsonValue& v) {
                    if (!v.is_integer()) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    int64_t val = v.as_integer();
                    if (val < 0) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        });
    }

    // Writers
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int t = 0; t < kWriters; ++t) {
        writers.emplace_back([&tsj]() {
            for (int i = 0; i < kIterations; ++i) {
                tsj.write([](JsonValue& v) {
                    v = JsonValue(v.as_integer() + 1);
                });
            }
        });
    }

    // Wait for writers to finish
    for (auto& th : writers) th.join();
    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    EXPECT_EQ(read_errors.load(), 0);

    int64_t final_val = 0;
    tsj.read([&final_val](const JsonValue& v) {
        final_val = v.as_integer();
    });
    EXPECT_EQ(final_val, kWriters * kIterations);
}

TEST(ThreadSafeJson, ConcurrentObjectInsert) {
    ThreadSafeJson tsj(JsonValue::object());

    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tsj, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                tsj.insert(key, JsonValue(t * kPerThread + i));
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(tsj.size(), static_cast<size_t>(kThreads * kPerThread));
}

TEST(ThreadSafeJson, ConcurrentArrayPushBack) {
    ThreadSafeJson tsj(JsonValue::array());

    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tsj, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                tsj.push_back(JsonValue(t * kPerThread + i));
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(tsj.size(), static_cast<size_t>(kThreads * kPerThread));
}

// ═══════════════════════════════════════════════════════════════════════════════
// ThreadSafeJson move semantics
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ThreadSafeJson, MoveAssignment) {
    ThreadSafeJson a(JsonValue(1));
    ThreadSafeJson b(JsonValue(2));

    b = std::move(a);

    int64_t result = 0;
    b.read([&result](const JsonValue& v) {
        result = v.as_integer();
    });
    EXPECT_EQ(result, 1);
}
