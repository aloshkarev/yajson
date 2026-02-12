/// @file bench_arena.cpp
/// @brief Benchmarks for MonotonicArena vs standard allocation.
///
/// Measures:
///   - Parse small/medium/large documents: arena vs heap
///   - Arena reuse (parse + reset loop) vs fresh allocation
///   - Multi-threaded parsing with per-thread arenas
///   - Throughput in messages/sec for network-like workloads

#include <json/json.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace yajson;

// =============================================================================
// Test data generators
// =============================================================================

/// Small JSON object (~50 bytes): typical AP status notification.
static std::string gen_small() {
    return R"({"name":"John","age":30,"active":true,"score":95.5})";
}

/// Medium JSON (~2KB): typical list of connected clients.
static std::string gen_medium() {
    std::string s = R"({"users":[)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) s += ",";
        s += R"({"id":)" + std::to_string(i) +
             R"(,"name":"user_)" + std::to_string(i) +
             R"(","email":"user)" + std::to_string(i) +
             R"(@test.com","active":)" + (i % 2 == 0 ? "true" : "false") +
             R"(,"score":)" + std::to_string(50.0 + i * 2.5) + "}";
    }
    s += R"(],"total":20,"page":1,"version":"2.0"})";
    return s;
}

/// Large JSON (~100KB): typical scan results or bulk data export.
static std::string gen_large() {
    std::string s = R"({"data":[)";
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) s += ",";
        s += R"({"id":)" + std::to_string(i) +
             R"(,"title":"Item )" + std::to_string(i) +
             R"( with some longer title text for realism")" +
             R"(,"description":"This is a detailed description for item )" +
             std::to_string(i) +
             R"( which contains enough text to be representative.")" +
             R"(,"price":)" + std::to_string(9.99 + i * 0.1) +
             R"(,"quantity":)" + std::to_string(i % 100) +
             R"(,"tags":["tag)" + std::to_string(i % 10) +
             R"(","tag)" + std::to_string(i % 5) +
             R"(","common"],"active":)" + (i % 3 == 0 ? "false" : "true") + "}";
    }
    s += R"(],"meta":{"total":1000,"generated":true}})";
    return s;
}

/// Network-like message (~200 bytes): AP event notification.
static std::string gen_network_msg() {
    return R"({"type":"client_connect","ap_id":"AP-001-FLOOR3",)"
           R"("mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"channel":36,)"
           R"("timestamp":1707350400,"ssid":"Corporate-5G",)"
           R"("ip":"192.168.1.105","vlan":100})";
}

// =============================================================================
// Single-threaded: Arena vs Heap parsing
// =============================================================================

static void BM_ParseSmall_Heap(benchmark::State& state) {
    auto input = gen_small();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseSmall_Heap);

static void BM_ParseSmall_Arena(benchmark::State& state) {
    auto input = gen_small();
    MonotonicArena arena(8192);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseSmall_Arena);

static void BM_ParseMedium_Heap(benchmark::State& state) {
    auto input = gen_medium();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseMedium_Heap);

static void BM_ParseMedium_Arena(benchmark::State& state) {
    auto input = gen_medium();
    MonotonicArena arena(32768);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseMedium_Arena);

static void BM_ParseLarge_Heap(benchmark::State& state) {
    auto input = gen_large();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseLarge_Heap);

static void BM_ParseLarge_Arena(benchmark::State& state) {
    auto input = gen_large();
    MonotonicArena arena(512 * 1024);  // 512KB initial

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseLarge_Arena);

// =============================================================================
// Arena reuse efficiency (simulates message processing loop)
// =============================================================================

static void BM_ArenaReuse_NetworkMsg(benchmark::State& state) {
    auto input = gen_network_msg();
    MonotonicArena arena(4096);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArenaReuse_NetworkMsg);

static void BM_HeapAlloc_NetworkMsg(benchmark::State& state) {
    auto input = gen_network_msg();

    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapAlloc_NetworkMsg);

// =============================================================================
// Multi-threaded parsing benchmarks
// =============================================================================

static void BM_MT_ParseMedium_Heap(benchmark::State& state) {
    auto input = gen_medium();

    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseMedium_Heap)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_MT_ParseMedium_Arena(benchmark::State& state) {
    auto input = gen_medium();
    // Each thread gets its own arena (thread_local ensures no contention)
    thread_local MonotonicArena arena(32768);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseMedium_Arena)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_MT_ParseNetworkMsg_Heap(benchmark::State& state) {
    auto input = gen_network_msg();

    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseNetworkMsg_Heap)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_MT_ParseNetworkMsg_Arena(benchmark::State& state) {
    auto input = gen_network_msg();
    thread_local MonotonicArena arena(4096);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseNetworkMsg_Arena)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// =============================================================================
// Throughput estimation (messages/sec)
// =============================================================================

static void BM_Throughput_Heap(benchmark::State& state) {
    std::vector<std::string> messages;
    messages.reserve(100);
    for (int i = 0; i < 100; ++i) {
        messages.push_back(
            R"({"type":"scan","bssid":"AA:BB:CC:)" +
            std::to_string(i / 100) + ":" +
            std::to_string(i / 10 % 10) + ":" +
            std::to_string(i % 10) +
            R"(","rssi":)" + std::to_string(-30 - (i % 50)) +
            R"(,"channel":)" + std::to_string(1 + (i % 13)) +
            R"(,"ssid":"Network_)" + std::to_string(i % 20) + R"("})");
    }

    size_t total_bytes = 0;
    for (const auto& m : messages) total_bytes += m.size();

    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : messages) {
            auto v = parse(msg);
            benchmark::DoNotOptimize(v);
        }
        msg_count += static_cast<int64_t>(messages.size());
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Throughput_Heap);

static void BM_Throughput_Arena(benchmark::State& state) {
    std::vector<std::string> messages;
    messages.reserve(100);
    for (int i = 0; i < 100; ++i) {
        messages.push_back(
            R"({"type":"scan","bssid":"AA:BB:CC:)" +
            std::to_string(i / 100) + ":" +
            std::to_string(i / 10 % 10) + ":" +
            std::to_string(i % 10) +
            R"(","rssi":)" + std::to_string(-30 - (i % 50)) +
            R"(,"channel":)" + std::to_string(1 + (i % 13)) +
            R"(,"ssid":"Network_)" + std::to_string(i % 20) + R"("})");
    }

    size_t total_bytes = 0;
    for (const auto& m : messages) total_bytes += m.size();

    MonotonicArena arena(8192);

    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : messages) {
            {
                auto v = parse(msg, arena);
                benchmark::DoNotOptimize(v);
            }
            arena.reset();
        }
        msg_count += static_cast<int64_t>(messages.size());
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Throughput_Arena);

static void BM_Throughput_Arena_MT(benchmark::State& state) {
    std::vector<std::string> messages;
    messages.reserve(100);
    for (int i = 0; i < 100; ++i) {
        messages.push_back(
            R"({"type":"scan","bssid":"AA:BB:CC:)" +
            std::to_string(i / 100) + ":" +
            std::to_string(i / 10 % 10) + ":" +
            std::to_string(i % 10) +
            R"(","rssi":)" + std::to_string(-30 - (i % 50)) +
            R"(,"channel":)" + std::to_string(1 + (i % 13)) +
            R"(,"ssid":"Network_)" + std::to_string(i % 20) + R"("})");
    }

    size_t total_bytes = 0;
    for (const auto& m : messages) total_bytes += m.size();

    thread_local MonotonicArena arena(8192);

    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : messages) {
            {
                auto v = parse(msg, arena);
                benchmark::DoNotOptimize(v);
            }
            arena.reset();
        }
        msg_count += static_cast<int64_t>(messages.size());
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Throughput_Arena_MT)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// =============================================================================
// Arena memory statistics
// =============================================================================

static void BM_ArenaMemoryOverhead(benchmark::State& state) {
    auto input = gen_medium();
    MonotonicArena arena(65536);

    for (auto _ : state) {
        {
            auto v = parse(input, arena);
            benchmark::DoNotOptimize(v);

            state.counters["arena_used"] = static_cast<double>(arena.bytes_used());
            state.counters["arena_alloc"] = static_cast<double>(arena.bytes_allocated());
            state.counters["overflow_blocks"] = static_cast<double>(arena.block_count());
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaMemoryOverhead);
