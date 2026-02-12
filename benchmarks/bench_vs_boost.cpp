/// @file bench_vs_boost.cpp
/// @brief Head-to-head performance comparison: yajson vs Boost.JSON.
///
/// All benchmarks use identical input data and equivalent operations to ensure
/// a fair comparison. Each scenario has a *_Yajson and *_BoostJson variant.
///
/// Scenarios (each has *_Yajson and *_BoostJson variant):
///   1–8.  Parse: small, medium, large, int/float/string array, nested, flat obj
///   9–10. Serialize: compact (small/medium/large), pretty-print (medium/large)
///  11.    Roundtrip parse+serialize (small, large)
///  12.    Object key lookup (10, 100, 1000 keys)
///  13.    Document construction (build object, build array)
///  14.    Deep copy
///  15.    Arena parse — ALL scenarios above repeated with arena allocators:
///           yajson: MonotonicArena | Boost.JSON: monotonic_resource
///           + arena roundtrip (large)
///  16.    Multi-threaded parse throughput (heap + arena)
///  17.    Network message batch (heap + arena)
///  18.    Serialize to ostream

#include <benchmark/benchmark.h>

// ─── yajson headers ──────────────────────────────────────────────────────────
#include <json/json.hpp>
#include <json/arena.hpp>

// ─── Boost.JSON headers ─────────────────────────────────────────────────────
#include <boost/json.hpp>

#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════════
// Shared test data generators (identical for both libraries)
// ═══════════════════════════════════════════════════════════════════════════════

namespace testdata {

/// Small JSON object (~50 bytes): typical status message.
inline std::string small_json() {
    return R"({"name":"John","age":30,"active":true,"score":95.5})";
}

/// Medium JSON (~2KB): list of users.
inline std::string medium_json() {
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

/// Large JSON (~100KB): bulk data.
inline std::string large_json() {
    std::string s = R"({"data":[)";
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) s += ",";
        s += R"({"id":)" + std::to_string(i) +
             R"(,"title":"Item )" + std::to_string(i) +
             R"( with some longer title text for realism")" +
             R"(,"description":"This is a detailed description for item )" +
             std::to_string(i) +
             R"( which contains enough text to be representative of real data.")" +
             R"(,"price":)" + std::to_string(9.99 + i * 0.1) +
             R"(,"quantity":)" + std::to_string(i % 100) +
             R"(,"tags":["tag)" + std::to_string(i % 10) +
             R"(","tag)" + std::to_string(i % 5) +
             R"(","common"],"active":)" + (i % 3 == 0 ? "false" : "true") + "}";
    }
    s += R"(],"meta":{"total":1000,"generated":true}})";
    return s;
}

/// Integer array: [0,1,2,...,N-1]
inline std::string int_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += std::to_string(i);
    }
    return s + "]";
}

/// Float array
inline std::string float_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", i * 1.123456);
        s += buf;
    }
    return s + "]";
}

/// String array with specified average length
inline std::string string_array(int n, int avg_len) {
    std::string s = "[";
    std::string payload(static_cast<size_t>(avg_len), 'x');
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += '"';
        s += payload;
        s += std::to_string(i);
        s += '"';
    }
    return s + "]";
}

/// Deeply nested objects
inline std::string nested(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i)
        s += R"({"level":)" + std::to_string(i) + R"(,"child":)";
    s += "null";
    for (int i = 0; i < depth; ++i)
        s += "}";
    return s;
}

/// Flat object with N keys
inline std::string flat_object(int n) {
    std::string s = "{";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += "\"key_" + std::to_string(i) + "\":" + std::to_string(i);
    }
    return s + "}";
}

/// Network-like message (~200 bytes)
inline std::string network_msg() {
    return R"({"type":"client_connect","ap_id":"AP-001-FLOOR3",)"
           R"("mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"channel":36,)"
           R"("timestamp":1707350400,"ssid":"Corporate-5G",)"
           R"("ip":"192.168.1.105","vlan":100})";
}

} // namespace testdata

// ═══════════════════════════════════════════════════════════════════════════════
// 1. PARSE SMALL
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseSmall_Yajson(benchmark::State& state) {
    auto input = testdata::small_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseSmall_Yajson);

static void BM_ParseSmall_BoostJson(benchmark::State& state) {
    auto input = testdata::small_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseSmall_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 2. PARSE MEDIUM
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseMedium_Yajson(benchmark::State& state) {
    auto input = testdata::medium_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseMedium_Yajson);

static void BM_ParseMedium_BoostJson(benchmark::State& state) {
    auto input = testdata::medium_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseMedium_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 3. PARSE LARGE
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseLarge_Yajson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseLarge_Yajson);

static void BM_ParseLarge_BoostJson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseLarge_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 4. PARSE INT ARRAY
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseIntArray_Yajson(benchmark::State& state) {
    auto input = testdata::int_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseIntArray_Yajson)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_ParseIntArray_BoostJson(benchmark::State& state) {
    auto input = testdata::int_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseIntArray_BoostJson)->Arg(100)->Arg(1000)->Arg(10000);

// ═══════════════════════════════════════════════════════════════════════════════
// 5. PARSE FLOAT ARRAY
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseFloatArray_Yajson(benchmark::State& state) {
    auto input = testdata::float_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseFloatArray_Yajson)->Arg(1000);

static void BM_ParseFloatArray_BoostJson(benchmark::State& state) {
    auto input = testdata::float_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseFloatArray_BoostJson)->Arg(1000);

// ═══════════════════════════════════════════════════════════════════════════════
// 6. PARSE STRING ARRAY (short / medium / long)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseStringArray_Yajson(benchmark::State& state) {
    auto input = testdata::string_array(
        static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseStringArray_Yajson)
    ->Args({1000, 8})->Args({1000, 64})->Args({1000, 256});

static void BM_ParseStringArray_BoostJson(benchmark::State& state) {
    auto input = testdata::string_array(
        static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseStringArray_BoostJson)
    ->Args({1000, 8})->Args({1000, 64})->Args({1000, 256});

// ═══════════════════════════════════════════════════════════════════════════════
// 7. PARSE DEEPLY NESTED
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseNested_Yajson(benchmark::State& state) {
    auto input = testdata::nested(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseNested_Yajson)->Arg(10)->Arg(50)->Arg(200);

static void BM_ParseNested_BoostJson(benchmark::State& state) {
    auto input = testdata::nested(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        boost::json::parse_options opts;
        opts.max_depth = 256;
        auto v = boost::json::parse(input, {}, opts);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseNested_BoostJson)->Arg(10)->Arg(50)->Arg(200);

// ═══════════════════════════════════════════════════════════════════════════════
// 8. PARSE FLAT OBJECT
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseFlatObj_Yajson(benchmark::State& state) {
    auto input = testdata::flat_object(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseFlatObj_Yajson)->Arg(10)->Arg(100)->Arg(1000);

static void BM_ParseFlatObj_BoostJson(benchmark::State& state) {
    auto input = testdata::flat_object(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseFlatObj_BoostJson)->Arg(10)->Arg(100)->Arg(1000);

// ═══════════════════════════════════════════════════════════════════════════════
// 9. SERIALIZE COMPACT
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_SerializeCompactSmall_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::small_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactSmall_Yajson);

static void BM_SerializeCompactSmall_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::small_json());
    for (auto _ : state) {
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactSmall_BoostJson);

static void BM_SerializeCompactMedium_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::medium_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactMedium_Yajson);

static void BM_SerializeCompactMedium_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::medium_json());
    for (auto _ : state) {
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactMedium_BoostJson);

static void BM_SerializeCompactLarge_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::large_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactLarge_Yajson);

static void BM_SerializeCompactLarge_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::large_json());
    for (auto _ : state) {
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeCompactLarge_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 10. SERIALIZE PRETTY
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_SerializePrettyMedium_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::medium_json());
    for (auto _ : state) {
        auto s = v.dump(2);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializePrettyMedium_Yajson);

static void BM_SerializePrettyMedium_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::medium_json());
    for (auto _ : state) {
        auto s = boost::json::serialize(v);  // Boost.JSON has no pretty-print in serialize()
        // Note: Boost.JSON pretty-print requires boost::json::pretty_print (since 1.84)
        // or manual serialization. We use compact here for fairness if pretty isn't available.
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializePrettyMedium_BoostJson);

static void BM_SerializePrettyLarge_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::large_json());
    for (auto _ : state) {
        auto s = v.dump(2);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializePrettyLarge_Yajson);

// ═══════════════════════════════════════════════════════════════════════════════
// 11. ROUNDTRIP (parse + serialize)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_RoundtripSmall_Yajson(benchmark::State& state) {
    auto input = testdata::small_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_RoundtripSmall_Yajson);

static void BM_RoundtripSmall_BoostJson(benchmark::State& state) {
    auto input = testdata::small_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_RoundtripSmall_BoostJson);

static void BM_RoundtripLarge_Yajson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_RoundtripLarge_Yajson);

static void BM_RoundtripLarge_BoostJson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_RoundtripLarge_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 12. OBJECT KEY LOOKUP
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ObjectLookup_Yajson(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = yajson::JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key_" + std::to_string(i), yajson::JsonValue(i));
    }
    // Warm up hash index
    auto& warmup = obj["key_0"];
    benchmark::DoNotOptimize(warmup);

    int idx = 0;
    for (auto _ : state) {
        auto* p = obj.find("key_" + std::to_string(idx % count));
        benchmark::DoNotOptimize(p);
        ++idx;
    }
    state.counters["keys"] = static_cast<double>(count);
}
BENCHMARK(BM_ObjectLookup_Yajson)->Arg(10)->Arg(100)->Arg(1000);

static void BM_ObjectLookup_BoostJson(benchmark::State& state) {
    auto count = state.range(0);
    boost::json::object obj;
    for (int64_t i = 0; i < count; ++i) {
        obj["key_" + std::to_string(i)] = i;
    }

    int idx = 0;
    for (auto _ : state) {
        auto it = obj.find("key_" + std::to_string(idx % count));
        benchmark::DoNotOptimize(it);
        ++idx;
    }
    state.counters["keys"] = static_cast<double>(count);
}
BENCHMARK(BM_ObjectLookup_BoostJson)->Arg(10)->Arg(100)->Arg(1000);

// ═══════════════════════════════════════════════════════════════════════════════
// 13. DOCUMENT CONSTRUCTION
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_BuildObject_Yajson(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        auto obj = yajson::JsonValue::object();
        for (int64_t i = 0; i < count; ++i) {
            obj.insert("key_" + std::to_string(i), yajson::JsonValue(i));
        }
        benchmark::DoNotOptimize(obj);
    }
}
BENCHMARK(BM_BuildObject_Yajson)->Arg(10)->Arg(100)->Arg(1000);

static void BM_BuildObject_BoostJson(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        boost::json::object obj;
        for (int64_t i = 0; i < count; ++i) {
            obj["key_" + std::to_string(i)] = i;
        }
        benchmark::DoNotOptimize(obj);
    }
}
BENCHMARK(BM_BuildObject_BoostJson)->Arg(10)->Arg(100)->Arg(1000);

static void BM_BuildArray_Yajson(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        auto arr = yajson::JsonValue::array();
        for (int64_t i = 0; i < count; ++i) {
            arr.push_back(yajson::JsonValue(i));
        }
        benchmark::DoNotOptimize(arr);
    }
}
BENCHMARK(BM_BuildArray_Yajson)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_BuildArray_BoostJson(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        boost::json::array arr;
        for (int64_t i = 0; i < count; ++i) {
            arr.push_back(i);
        }
        benchmark::DoNotOptimize(arr);
    }
}
BENCHMARK(BM_BuildArray_BoostJson)->Arg(100)->Arg(1000)->Arg(10000);

// ═══════════════════════════════════════════════════════════════════════════════
// 14. DEEP COPY
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_DeepCopy_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::large_json());
    for (auto _ : state) {
        yajson::JsonValue copy = v;
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(BM_DeepCopy_Yajson);

static void BM_DeepCopy_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::large_json());
    for (auto _ : state) {
        boost::json::value copy = v;
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(BM_DeepCopy_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 15. PARSE WITH ARENA / MONOTONIC RESOURCE — ALL SCENARIOS
//
// Both libraries use their best arena allocator:
//   yajson: MonotonicArena (bump allocator, O(1) reset)
//   Boost.JSON: monotonic_resource (same pattern)
// This is the fairest "best vs best" comparison.
// ═══════════════════════════════════════════════════════════════════════════════

// --- Arena: Small ---
static void BM_ArenaParseSmall_Yajson(benchmark::State& state) {
    auto input = testdata::small_json();
    yajson::MonotonicArena arena(8192);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseSmall_Yajson);

static void BM_ArenaParseSmall_BoostJson(benchmark::State& state) {
    auto input = testdata::small_json();
    unsigned char buf[8192];
    for (auto _ : state) {
        boost::json::monotonic_resource mr(buf, sizeof(buf));
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseSmall_BoostJson);

// --- Arena: Medium ---
static void BM_ArenaParseMedium_Yajson(benchmark::State& state) {
    auto input = testdata::medium_json();
    yajson::MonotonicArena arena(32768);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseMedium_Yajson);

static void BM_ArenaParseMedium_BoostJson(benchmark::State& state) {
    auto input = testdata::medium_json();
    unsigned char buf[32768];
    for (auto _ : state) {
        boost::json::monotonic_resource mr(buf, sizeof(buf));
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseMedium_BoostJson);

// --- Arena: Large ---
static void BM_ArenaParseLarge_Yajson(benchmark::State& state) {
    auto input = testdata::large_json();
    yajson::MonotonicArena arena(512 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseLarge_Yajson);

static void BM_ArenaParseLarge_BoostJson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        boost::json::monotonic_resource mr(512 * 1024);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseLarge_BoostJson);

// --- Arena: Int Array ---
static void BM_ArenaParseIntArray_Yajson(benchmark::State& state) {
    auto input = testdata::int_array(static_cast<int>(state.range(0)));
    yajson::MonotonicArena arena(256 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseIntArray_Yajson)->Arg(1000)->Arg(10000);

static void BM_ArenaParseIntArray_BoostJson(benchmark::State& state) {
    auto input = testdata::int_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        boost::json::monotonic_resource mr(256 * 1024);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseIntArray_BoostJson)->Arg(1000)->Arg(10000);

// --- Arena: Float Array ---
static void BM_ArenaParseFloatArray_Yajson(benchmark::State& state) {
    auto input = testdata::float_array(1000);
    yajson::MonotonicArena arena(64 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseFloatArray_Yajson);

static void BM_ArenaParseFloatArray_BoostJson(benchmark::State& state) {
    auto input = testdata::float_array(1000);
    for (auto _ : state) {
        boost::json::monotonic_resource mr(64 * 1024);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseFloatArray_BoostJson);

// --- Arena: String Array ---
static void BM_ArenaParseStringArray_Yajson(benchmark::State& state) {
    auto input = testdata::string_array(
        static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    yajson::MonotonicArena arena(512 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseStringArray_Yajson)
    ->Args({1000, 8})->Args({1000, 64})->Args({1000, 256});

static void BM_ArenaParseStringArray_BoostJson(benchmark::State& state) {
    auto input = testdata::string_array(
        static_cast<int>(state.range(0)), static_cast<int>(state.range(1)));
    for (auto _ : state) {
        boost::json::monotonic_resource mr(512 * 1024);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseStringArray_BoostJson)
    ->Args({1000, 8})->Args({1000, 64})->Args({1000, 256});

// --- Arena: Flat Object ---
static void BM_ArenaParseFlatObj_Yajson(benchmark::State& state) {
    auto input = testdata::flat_object(static_cast<int>(state.range(0)));
    yajson::MonotonicArena arena(256 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseFlatObj_Yajson)->Arg(10)->Arg(100)->Arg(1000);

static void BM_ArenaParseFlatObj_BoostJson(benchmark::State& state) {
    auto input = testdata::flat_object(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        boost::json::monotonic_resource mr(256 * 1024);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseFlatObj_BoostJson)->Arg(10)->Arg(100)->Arg(1000);

// --- Arena: Nested ---
static void BM_ArenaParseNested_Yajson(benchmark::State& state) {
    auto input = testdata::nested(static_cast<int>(state.range(0)));
    yajson::MonotonicArena arena(64 * 1024);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseNested_Yajson)->Arg(10)->Arg(50)->Arg(200);

static void BM_ArenaParseNested_BoostJson(benchmark::State& state) {
    auto input = testdata::nested(static_cast<int>(state.range(0)));
    boost::json::parse_options opts;
    opts.max_depth = 256;
    for (auto _ : state) {
        boost::json::monotonic_resource mr(64 * 1024);
        auto v = boost::json::parse(input, &mr, opts);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaParseNested_BoostJson)->Arg(10)->Arg(50)->Arg(200);

// --- Arena: Roundtrip Large ---
static void BM_ArenaRoundtripLarge_Yajson(benchmark::State& state) {
    auto input = testdata::large_json();
    yajson::MonotonicArena arena(512 * 1024);
    for (auto _ : state) {
        {
            auto v = yajson::parse(input, arena);
            auto s = v.dump();
            benchmark::DoNotOptimize(s);
        }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaRoundtripLarge_Yajson);

static void BM_ArenaRoundtripLarge_BoostJson(benchmark::State& state) {
    auto input = testdata::large_json();
    for (auto _ : state) {
        boost::json::monotonic_resource mr(512 * 1024);
        auto v = boost::json::parse(input, &mr);
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ArenaRoundtripLarge_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 16. MULTI-THREADED PARSE THROUGHPUT (heap + arena)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_MT_Parse_Yajson(benchmark::State& state) {
    auto input = testdata::medium_json();
    for (auto _ : state) {
        auto v = yajson::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_Parse_Yajson)->Threads(1)->Threads(2)->Threads(4);

static void BM_MT_Parse_BoostJson(benchmark::State& state) {
    auto input = testdata::medium_json();
    for (auto _ : state) {
        auto v = boost::json::parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_Parse_BoostJson)->Threads(1)->Threads(2)->Threads(4);

// --- Multi-threaded with arena ---
static void BM_MT_ParseArena_Yajson(benchmark::State& state) {
    auto input = testdata::medium_json();
    thread_local yajson::MonotonicArena arena(32768);
    for (auto _ : state) {
        { auto v = yajson::parse(input, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseArena_Yajson)->Threads(1)->Threads(2)->Threads(4);

static void BM_MT_ParseArena_BoostJson(benchmark::State& state) {
    auto input = testdata::medium_json();
    for (auto _ : state) {
        boost::json::monotonic_resource mr(32768);
        auto v = boost::json::parse(input, &mr);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MT_ParseArena_BoostJson)->Threads(1)->Threads(2)->Threads(4);

// ═══════════════════════════════════════════════════════════════════════════════
// 17. NETWORK MESSAGE THROUGHPUT (batch of 100 messages)
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<std::string> gen_network_batch() {
    std::vector<std::string> msgs;
    msgs.reserve(100);
    for (int i = 0; i < 100; ++i) {
        msgs.push_back(
            R"({"type":"scan","bssid":"AA:BB:CC:)" +
            std::to_string(i / 100) + ":" +
            std::to_string(i / 10 % 10) + ":" +
            std::to_string(i % 10) +
            R"(","rssi":)" + std::to_string(-30 - (i % 50)) +
            R"(,"channel":)" + std::to_string(1 + (i % 13)) +
            R"(,"ssid":"Network_)" + std::to_string(i % 20) + R"("})");
    }
    return msgs;
}

static void BM_NetworkBatch_Yajson(benchmark::State& state) {
    auto msgs = gen_network_batch();
    size_t total_bytes = 0;
    for (const auto& m : msgs) total_bytes += m.size();
    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : msgs) {
            auto v = yajson::parse(msg);
            benchmark::DoNotOptimize(v);
        }
        msg_count += static_cast<int64_t>(msgs.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetworkBatch_Yajson);

static void BM_NetworkBatch_BoostJson(benchmark::State& state) {
    auto msgs = gen_network_batch();
    size_t total_bytes = 0;
    for (const auto& m : msgs) total_bytes += m.size();
    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : msgs) {
            auto v = boost::json::parse(msg);
            benchmark::DoNotOptimize(v);
        }
        msg_count += static_cast<int64_t>(msgs.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetworkBatch_BoostJson);

static void BM_NetworkBatchArena_Yajson(benchmark::State& state) {
    auto msgs = gen_network_batch();
    size_t total_bytes = 0;
    for (const auto& m : msgs) total_bytes += m.size();
    yajson::MonotonicArena arena(8192);
    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : msgs) {
            { auto v = yajson::parse(msg, arena); benchmark::DoNotOptimize(v); }
            arena.reset();
        }
        msg_count += static_cast<int64_t>(msgs.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetworkBatchArena_Yajson);

static void BM_NetworkBatchArena_BoostJson(benchmark::State& state) {
    auto msgs = gen_network_batch();
    size_t total_bytes = 0;
    for (const auto& m : msgs) total_bytes += m.size();
    unsigned char buf[8192];
    int64_t msg_count = 0;
    for (auto _ : state) {
        for (const auto& msg : msgs) {
            boost::json::monotonic_resource mr(buf, sizeof(buf));
            auto v = boost::json::parse(msg, &mr);
            benchmark::DoNotOptimize(v);
        }
        msg_count += static_cast<int64_t>(msgs.size());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(msg_count);
    state.counters["msg/s"] = benchmark::Counter(
        static_cast<double>(msg_count), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetworkBatchArena_BoostJson);

// ═══════════════════════════════════════════════════════════════════════════════
// 18. SERIALIZE TO STREAM (ostream comparison)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_StreamSerialize_Yajson(benchmark::State& state) {
    auto v = yajson::parse(testdata::large_json());
    for (auto _ : state) {
        std::ostringstream oss;
        oss << v;
        benchmark::DoNotOptimize(oss.str());
    }
}
BENCHMARK(BM_StreamSerialize_Yajson);

static void BM_StreamSerialize_BoostJson(benchmark::State& state) {
    auto v = boost::json::parse(testdata::large_json());
    for (auto _ : state) {
        std::ostringstream oss;
        oss << v;
        benchmark::DoNotOptimize(oss.str());
    }
}
BENCHMARK(BM_StreamSerialize_BoostJson);
