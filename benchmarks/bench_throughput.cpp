/// @file bench_throughput.cpp
/// @brief Comprehensive throughput benchmarks for yajson.
///
/// Measures:
///   - Peak parse throughput (MB/s) for different document types
///   - Peak serialize throughput (MB/s)
///   - Latency distribution (min/avg/max per operation)
///   - Scaling characteristics (document size vs throughput)
///   - Individual component performance (numbers, strings, objects, arrays)
///   - O(1) vs O(n) object lookup comparison
///   - Streaming vs string serialization
///   - error_code path overhead
///   - Conversion system overhead

#include <json/json.hpp>

#include <benchmark/benchmark.h>

#include <cmath>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Data generators
// ═══════════════════════════════════════════════════════════════════════════════

/// Pure integer array: [0,1,2,...,N-1]
static std::string gen_int_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += std::to_string(i);
    }
    return s + "]";
}

/// Pure float array: [0.1,1.1,...,N-1.1]
static std::string gen_float_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", i * 1.123456);
        s += buf;
    }
    return s + "]";
}

/// Pure string array with varying lengths
static std::string gen_string_array(int n, int avg_len) {
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

/// Flat object with N string keys
static std::string gen_flat_object(int n) {
    std::string s = "{";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += "\"key_" + std::to_string(i) + "\":" + std::to_string(i);
    }
    return s + "}";
}

/// Nested objects: {a:{b:{c:...{val:1}...}}}
static std::string gen_nested_object(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i)
        s += "{\"l" + std::to_string(i) + "\":";
    s += "42";
    for (int i = 0; i < depth; ++i)
        s += "}";
    return s;
}

/// Realistic API response (~2KB per item)
static std::string gen_api_response(int items) {
    std::string s = R"({"status":"ok","count":)" + std::to_string(items) + R"(,"items":[)";
    for (int i = 0; i < items; ++i) {
        if (i) s += ',';
        s += R"({"id":)" + std::to_string(1000 + i);
        s += R"(,"name":"User )" + std::to_string(i) + R"(")";
        s += R"(,"email":"user)" + std::to_string(i) + R"(@example.com")";
        s += R"(,"age":)" + std::to_string(20 + i % 50);
        s += R"(,"score":)" + std::to_string(50.0 + i * 0.7);
        s += R"(,"active":)" + std::string(i % 3 ? "true" : "false");
        s += R"(,"tags":["alpha","beta","gamma"])";
        s += R"(,"metadata":{"created":"2025-01-01","updated":"2025-06-15","version":)" +
             std::to_string(i % 10) + "}}";
    }
    s += "]}";
    return s;
}

/// Strings with unicode escapes
static std::string gen_unicode_heavy(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += R"("\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417")";
    }
    return s + "]";
}

/// Boolean-heavy document
static std::string gen_bool_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += (i % 2) ? "true" : "false";
    }
    return s + "]";
}

/// Null-heavy document
static std::string gen_null_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += "null";
    }
    return s + "]";
}

/// Mixed types
static std::string gen_mixed_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        switch (i % 6) {
            case 0: s += "null"; break;
            case 1: s += (i % 3) ? "true" : "false"; break;
            case 2: s += std::to_string(i * 17); break;
            case 3: { char b[32]; std::snprintf(b, 32, "%.4f", i*0.31); s += b; } break;
            case 4: s += "\"str_" + std::to_string(i) + "\""; break;
            case 5: s += "[" + std::to_string(i) + "]"; break;
        }
    }
    return s + "]";
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSE THROUGHPUT — measures MB/s for different document types
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Parse_IntArray(benchmark::State& state) {
    auto input = gen_int_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_IntArray)->Arg(100)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_TP_Parse_FloatArray(benchmark::State& state) {
    auto input = gen_float_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_FloatArray)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_TP_Parse_StringArray(benchmark::State& state) {
    auto n = static_cast<int>(state.range(0));
    auto len = static_cast<int>(state.range(1));
    auto input = gen_string_array(n, len);
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_StringArray)
    ->Args({1000, 8})    // Short strings (SSO)
    ->Args({1000, 64})   // Medium strings
    ->Args({1000, 256})  // Long strings
    ->Args({100, 1024}); // Very long strings

static void BM_TP_Parse_BoolArray(benchmark::State& state) {
    auto input = gen_bool_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_BoolArray)->Arg(1000)->Arg(10000);

static void BM_TP_Parse_NullArray(benchmark::State& state) {
    auto input = gen_null_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_NullArray)->Arg(1000)->Arg(10000);

static void BM_TP_Parse_MixedArray(benchmark::State& state) {
    auto input = gen_mixed_array(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_MixedArray)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_TP_Parse_FlatObject(benchmark::State& state) {
    auto input = gen_flat_object(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_FlatObject)->Arg(10)->Arg(50)->Arg(200)->Arg(1000);

static void BM_TP_Parse_NestedObject(benchmark::State& state) {
    auto input = gen_nested_object(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_NestedObject)->Arg(10)->Arg(50)->Arg(200);

static void BM_TP_Parse_UnicodeEscape(benchmark::State& state) {
    auto input = gen_unicode_heavy(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_UnicodeEscape)->Arg(100)->Arg(1000);

static void BM_TP_Parse_ApiResponse(benchmark::State& state) {
    auto input = gen_api_response(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Parse_ApiResponse)->Arg(10)->Arg(100)->Arg(500);

// ═══════════════════════════════════════════════════════════════════════════════
// SERIALIZE THROUGHPUT — measures MB/s output
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Serialize_IntArray(benchmark::State& state) {
    auto v = parse(gen_int_array(static_cast<int>(state.range(0))));
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
        state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(s.size()));
    }
}
BENCHMARK(BM_TP_Serialize_IntArray)->Arg(100)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_TP_Serialize_FloatArray(benchmark::State& state) {
    auto v = parse(gen_float_array(static_cast<int>(state.range(0))));
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    // Approximate output size
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(
        gen_float_array(static_cast<int>(state.range(0))).size()));
}
BENCHMARK(BM_TP_Serialize_FloatArray)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_TP_Serialize_FlatObject(benchmark::State& state) {
    auto v = parse(gen_flat_object(static_cast<int>(state.range(0))));
    std::string out;
    for (auto _ : state) {
        out = v.dump();
        benchmark::DoNotOptimize(out);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(out.size()));
}
BENCHMARK(BM_TP_Serialize_FlatObject)->Arg(10)->Arg(50)->Arg(200)->Arg(1000);

static void BM_TP_Serialize_ApiResponse(benchmark::State& state) {
    auto v = parse(gen_api_response(static_cast<int>(state.range(0))));
    std::string out;
    for (auto _ : state) {
        out = v.dump();
        benchmark::DoNotOptimize(out);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(out.size()));
    state.counters["out_bytes"] = static_cast<double>(out.size());
}
BENCHMARK(BM_TP_Serialize_ApiResponse)->Arg(10)->Arg(100)->Arg(500);

static void BM_TP_Serialize_Pretty(benchmark::State& state) {
    auto v = parse(gen_api_response(static_cast<int>(state.range(0))));
    std::string out;
    for (auto _ : state) {
        out = v.dump(2);
        benchmark::DoNotOptimize(out);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(out.size()));
}
BENCHMARK(BM_TP_Serialize_Pretty)->Arg(10)->Arg(100);

// ═══════════════════════════════════════════════════════════════════════════════
// STREAMING vs STRING serialization comparison
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Serialize_ToString(benchmark::State& state) {
    auto v = parse(gen_api_response(100));
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    auto out = v.dump();
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(out.size()));
}
BENCHMARK(BM_TP_Serialize_ToString);

static void BM_TP_Serialize_ToStream(benchmark::State& state) {
    auto v = parse(gen_api_response(100));
    for (auto _ : state) {
        std::ostringstream oss;
        oss << v;
        benchmark::DoNotOptimize(oss.str());
    }
    auto out = v.dump();
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(out.size()));
}
BENCHMARK(BM_TP_Serialize_ToStream);

// ═══════════════════════════════════════════════════════════════════════════════
// ROUNDTRIP (parse + serialize) — end-to-end throughput
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Roundtrip_ApiResponse(benchmark::State& state) {
    auto input = gen_api_response(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto v = parse(input);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
    state.counters["doc_bytes"] = static_cast<double>(input.size());
}
BENCHMARK(BM_TP_Roundtrip_ApiResponse)->Arg(10)->Arg(100)->Arg(500);

// ═══════════════════════════════════════════════════════════════════════════════
// OBJECT LOOKUP — O(1) vs linear scan comparison
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_ObjectLookup_NonConst(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key_" + std::to_string(i), JsonValue(i));
    }
    // Non-const: uses/builds hash index for large objects
    int idx = 0;
    for (auto _ : state) {
        auto& v = obj["key_" + std::to_string(idx % count)];
        benchmark::DoNotOptimize(v);
        ++idx;
    }
    state.counters["keys"] = static_cast<double>(count);
}
BENCHMARK(BM_TP_ObjectLookup_NonConst)
    ->Arg(5)->Arg(10)->Arg(16)->Arg(20)->Arg(50)->Arg(100)->Arg(500)->Arg(1000);

static void BM_TP_ObjectLookup_Find(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key_" + std::to_string(i), JsonValue(i));
    }
    // Trigger index build via non-const access
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
BENCHMARK(BM_TP_ObjectLookup_Find)
    ->Arg(5)->Arg(10)->Arg(16)->Arg(20)->Arg(50)->Arg(100)->Arg(500)->Arg(1000);

// ═══════════════════════════════════════════════════════════════════════════════
// ERROR_CODE PATH — try_parse vs parse overhead
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Parse_Throwing(benchmark::State& state) {
    auto input = gen_api_response(50);
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_Throwing);

static void BM_TP_Parse_ErrorCode(benchmark::State& state) {
    auto input = gen_api_response(50);
    for (auto _ : state) {
        auto r = try_parse(input);
        benchmark::DoNotOptimize(r.value);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_ErrorCode);

// ═══════════════════════════════════════════════════════════════════════════════
// CONVERSION THROUGHPUT — struct <-> JSON
// ═══════════════════════════════════════════════════════════════════════════════

struct BenchRecord {
    std::string id;
    std::string name;
    std::string email;
    int age = 0;
    double score = 0;
    bool active = false;
    std::vector<std::string> tags;
};
JSON_DEFINE_TYPE_NON_INTRUSIVE(BenchRecord, id, name, email, age, score, active, tags)

static void BM_TP_Conv_StructToJson(benchmark::State& state) {
    BenchRecord r{"id-001", "Alice Smith", "alice@example.com",
                  30, 95.5, true, {"admin", "editor", "viewer"}};
    for (auto _ : state) {
        auto j = to_value(r);
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_TP_Conv_StructToJson);

static void BM_TP_Conv_JsonToStruct(benchmark::State& state) {
    BenchRecord r{"id-001", "Alice Smith", "alice@example.com",
                  30, 95.5, true, {"admin", "editor", "viewer"}};
    auto j = to_value(r);
    for (auto _ : state) {
        auto r2 = from_value<BenchRecord>(j);
        benchmark::DoNotOptimize(r2);
    }
}
BENCHMARK(BM_TP_Conv_JsonToStruct);

static void BM_TP_Conv_StructToString(benchmark::State& state) {
    BenchRecord r{"id-001", "Alice Smith", "alice@example.com",
                  30, 95.5, true, {"admin", "editor", "viewer"}};
    for (auto _ : state) {
        auto j = to_value(r);
        auto s = j.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_TP_Conv_StructToString);

static void BM_TP_Conv_StringToStruct(benchmark::State& state) {
    BenchRecord r{"id-001", "Alice Smith", "alice@example.com",
                  30, 95.5, true, {"admin", "editor", "viewer"}};
    auto s = to_value(r).dump();
    for (auto _ : state) {
        auto j = parse(s);
        auto r2 = from_value<BenchRecord>(j);
        benchmark::DoNotOptimize(r2);
    }
}
BENCHMARK(BM_TP_Conv_StringToStruct);

static void BM_TP_Conv_VecStructRoundtrip(benchmark::State& state) {
    auto n = static_cast<int>(state.range(0));
    std::vector<BenchRecord> vec;
    vec.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        vec.push_back({"id-" + std::to_string(i),
                        "User " + std::to_string(i),
                        "user" + std::to_string(i) + "@test.com",
                        20 + i % 50, 50.0 + i * 0.5, i % 2 == 0,
                        {"tag" + std::to_string(i % 5)}});
    }
    for (auto _ : state) {
        auto j = to_value(vec);
        auto s = j.dump();
        auto j2 = parse(s);
        auto vec2 = from_value<std::vector<BenchRecord>>(j2);
        benchmark::DoNotOptimize(vec2);
    }
    state.counters["records"] = static_cast<double>(n);
}
BENCHMARK(BM_TP_Conv_VecStructRoundtrip)->Arg(10)->Arg(100)->Arg(1000);

// ═══════════════════════════════════════════════════════════════════════════════
// NON-STANDARD PARSING — overhead of extensions
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_TP_Parse_Strict(benchmark::State& state) {
    auto input = gen_api_response(100);
    for (auto _ : state) {
        auto v = parse(input, ParseOptions::strict());
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_Strict);

static void BM_TP_Parse_Lenient(benchmark::State& state) {
    auto input = gen_api_response(100);
    for (auto _ : state) {
        auto v = parse(input, ParseOptions::lenient());
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_TP_Parse_Lenient);
