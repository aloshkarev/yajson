/// @file bench_json.cpp
/// @brief Performance benchmarks for yajson library.
///
/// Measured operations:
///   - Parsing (small, medium, large documents)
///   - Serialization (compact, pretty-print)
///   - Data access (read, write, lookup)
///   - Thread-safe operations
///   - JSON document construction

#include <json/json.hpp>

#include <benchmark/benchmark.h>

#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Test data generators
// ═══════════════════════════════════════════════════════════════════════════════

/// Generate small JSON object (~100 bytes).
static std::string generate_small_json() {
    return R"({"name":"John","age":30,"active":true,"score":95.5})";
}

/// Generate medium JSON document (~2KB).
static std::string generate_medium_json() {
    std::string s = R"({
        "users": [)";
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

/// Generate large JSON document (~100KB).
static std::string generate_large_json() {
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

/// Generate integer array.
static std::string generate_int_array(int count) {
    std::string s = "[";
    for (int i = 0; i < count; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(i);
    }
    s += "]";
    return s;
}

/// Generate deeply nested structure.
static std::string generate_deeply_nested(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i) {
        s += R"({"level":)" + std::to_string(i) + R"(,"child":)";
    }
    s += "null";
    for (int i = 0; i < depth; ++i) {
        s += "}";
    }
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Parsing benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ParseSmall(benchmark::State& state) {
    auto input = generate_small_json();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseSmall);

static void BM_ParseMedium(benchmark::State& state) {
    auto input = generate_medium_json();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseMedium);

static void BM_ParseLarge(benchmark::State& state) {
    auto input = generate_large_json();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseLarge);

static void BM_ParseIntArray(benchmark::State& state) {
    auto count = state.range(0);
    auto input = generate_int_array(static_cast<int>(count));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseIntArray)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_ParseDeeplyNested(benchmark::State& state) {
    auto depth = state.range(0);
    auto input = generate_deeply_nested(static_cast<int>(depth));
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseDeeplyNested)->Arg(10)->Arg(50)->Arg(200);

// ═══════════════════════════════════════════════════════════════════════════════
// Serialization benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_SerializeSmallCompact(benchmark::State& state) {
    auto v = parse(generate_small_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeSmallCompact);

static void BM_SerializeMediumCompact(benchmark::State& state) {
    auto v = parse(generate_medium_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeMediumCompact);

static void BM_SerializeLargeCompact(benchmark::State& state) {
    auto v = parse(generate_large_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeLargeCompact);

static void BM_SerializeMediumPretty(benchmark::State& state) {
    auto v = parse(generate_medium_json());
    for (auto _ : state) {
        auto s = v.dump(2);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeMediumPretty);

static void BM_SerializeLargePretty(benchmark::State& state) {
    auto v = parse(generate_large_json());
    for (auto _ : state) {
        auto s = v.dump(2);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeLargePretty);

// ═══════════════════════════════════════════════════════════════════════════════
// Data access benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ObjectLookup(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key" + std::to_string(i), JsonValue(i));
    }

    int idx = 0;
    for (auto _ : state) {
        auto key = "key" + std::to_string(idx % count);
        auto& v = obj[key];
        benchmark::DoNotOptimize(v);
        ++idx;
    }
}
BENCHMARK(BM_ObjectLookup)->Arg(5)->Arg(20)->Arg(100)->Arg(1000);

static void BM_ArrayAccess(benchmark::State& state) {
    auto count = state.range(0);
    auto arr = JsonValue::array();
    for (int64_t i = 0; i < count; ++i) {
        arr.push_back(JsonValue(i));
    }

    size_t idx = 0;
    for (auto _ : state) {
        auto& v = arr[idx % static_cast<size_t>(count)];
        benchmark::DoNotOptimize(v);
        ++idx;
    }
}
BENCHMARK(BM_ArrayAccess)->Arg(10)->Arg(100)->Arg(10000);

static void BM_ObjectContains(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key" + std::to_string(i), JsonValue(i));
    }

    int idx = 0;
    for (auto _ : state) {
        auto key = "key" + std::to_string(idx % count);
        bool found = obj.contains(key);
        benchmark::DoNotOptimize(found);
        ++idx;
    }
}
BENCHMARK(BM_ObjectContains)->Arg(5)->Arg(20)->Arg(100);

// ═══════════════════════════════════════════════════════════════════════════════
// Document construction benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_BuildObject(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        auto obj = JsonValue::object();
        for (int64_t i = 0; i < count; ++i) {
            obj.insert("key" + std::to_string(i), JsonValue(i));
        }
        benchmark::DoNotOptimize(obj);
    }
}
BENCHMARK(BM_BuildObject)->Arg(10)->Arg(100)->Arg(1000);

static void BM_BuildArray(benchmark::State& state) {
    auto count = state.range(0);
    for (auto _ : state) {
        auto arr = JsonValue::array();
        for (int64_t i = 0; i < count; ++i) {
            arr.push_back(JsonValue(i));
        }
        benchmark::DoNotOptimize(arr);
    }
}
BENCHMARK(BM_BuildArray)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// ═══════════════════════════════════════════════════════════════════════════════
// Roundtrip benchmarks (parse + serialize)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_RoundtripSmall(benchmark::State& state) {
    auto input = generate_small_json();
    for (auto _ : state) {
        auto v = parse(input);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_RoundtripSmall);

static void BM_RoundtripLarge(benchmark::State& state) {
    auto input = generate_large_json();
    for (auto _ : state) {
        auto v = parse(input);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_RoundtripLarge);

// ═══════════════════════════════════════════════════════════════════════════════
// Thread safety benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_ThreadSafeRead(benchmark::State& state) {
    auto obj = JsonValue::object();
    for (int i = 0; i < 100; ++i) {
        obj.insert("key" + std::to_string(i), JsonValue(i));
    }
    ThreadSafeJson tsj(std::move(obj));

    for (auto _ : state) {
        tsj.read([](const JsonValue& v) {
            benchmark::DoNotOptimize(v.size());
        });
    }
}
BENCHMARK(BM_ThreadSafeRead)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_ThreadSafeWrite(benchmark::State& state) {
    ThreadSafeJson tsj(JsonValue(int64_t(0)));

    for (auto _ : state) {
        tsj.write([](JsonValue& v) {
            v = JsonValue(v.as_integer() + 1);
        });
    }
}
BENCHMARK(BM_ThreadSafeWrite)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_ThreadSafeReadWriteMix(benchmark::State& state) {
    auto obj = JsonValue::object();
    obj["counter"] = JsonValue(int64_t(0));
    ThreadSafeJson tsj(std::move(obj));

    for (auto _ : state) {
        // 80% reads, 20% writes
        if (state.iterations() % 5 == 0) {
            tsj.write([](JsonValue& v) {
                auto cnt = v["counter"].as_integer();
                v["counter"] = JsonValue(cnt + 1);
            });
        } else {
            tsj.read([](const JsonValue& v) {
                benchmark::DoNotOptimize(v.size());
            });
        }
    }
}
BENCHMARK(BM_ThreadSafeReadWriteMix)->Threads(1)->Threads(4)->Threads(8);

// ═══════════════════════════════════════════════════════════════════════════════
// Copy benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_CopySmall(benchmark::State& state) {
    auto v = parse(generate_small_json());
    for (auto _ : state) {
        JsonValue copy = v;
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(BM_CopySmall);

static void BM_CopyLarge(benchmark::State& state) {
    auto v = parse(generate_large_json());
    for (auto _ : state) {
        JsonValue copy = v;
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(BM_CopyLarge);

static void BM_MoveValue(benchmark::State& state) {
    for (auto _ : state) {
        auto v = parse(generate_medium_json());
        JsonValue moved = std::move(v);
        benchmark::DoNotOptimize(moved);
    }
}
BENCHMARK(BM_MoveValue);

// ═══════════════════════════════════════════════════════════════════════════════
// UTF-8 benchmarks
// ═══════════════════════════════════════════════════════════════════════════════

/// Generate JSON with Unicode escape sequences.
static std::string generate_unicode_escape_json() {
    std::string s = R"({"strings":[)";
    for (int i = 0; i < 100; ++i) {
        if (i > 0) s += ",";
        s += R"("\u041F\u0440\u0438\u0432\u0435\u0442 \u004D\u0069\u0072 )";
        s += std::to_string(i);
        s += R"(")";
    }
    s += "]}";
    return s;
}

/// Generate JSON with direct UTF-8 text.
static std::string generate_utf8_direct_json() {
    std::string s = R"({"strings":[)";
    for (int i = 0; i < 100; ++i) {
        if (i > 0) s += ",";
        s += "\"Привет Мир ";
        s += std::to_string(i);
        s += "\"";
    }
    s += "]}";
    return s;
}

static void BM_ParseUnicodeEscape(benchmark::State& state) {
    auto input = generate_unicode_escape_json();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseUnicodeEscape);

static void BM_ParseUtf8Direct(benchmark::State& state) {
    auto input = generate_utf8_direct_json();
    for (auto _ : state) {
        auto v = parse(input);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseUtf8Direct);

static void BM_SerializeEnsureAscii(benchmark::State& state) {
    auto v = parse(generate_utf8_direct_json());
    SerializeOptions opts;
    opts.ensure_ascii = true;
    for (auto _ : state) {
        auto s = yajson::serialize(v, opts);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeEnsureAscii);

static void BM_SerializeUtf8Passthrough(benchmark::State& state) {
    auto v = parse(generate_utf8_direct_json());
    for (auto _ : state) {
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeUtf8Passthrough);

// =============================================================================
// O(1) object lookup benchmarks
// =============================================================================

static void BM_ObjectLookupConstTime(benchmark::State& state) {
    auto count = state.range(0);
    auto obj = JsonValue::object();
    for (int64_t i = 0; i < count; ++i) {
        obj.insert("key" + std::to_string(i), JsonValue(i));
    }
    auto& warmup = obj["key0"];
    benchmark::DoNotOptimize(warmup);

    const auto& cobj = obj;
    int idx = 0;
    for (auto _ : state) {
        auto key = "key" + std::to_string(idx % count);
        const auto* found = cobj.find(key);
        benchmark::DoNotOptimize(found);
        ++idx;
    }
}
BENCHMARK(BM_ObjectLookupConstTime)->Arg(5)->Arg(20)->Arg(100)->Arg(1000);

// =============================================================================
// Streaming serializer benchmarks
// =============================================================================

static void BM_StreamSerializeLarge(benchmark::State& state) {
    auto v = parse(generate_large_json());
    for (auto _ : state) {
        std::ostringstream oss;
        oss << v;
        benchmark::DoNotOptimize(oss.str());
    }
}
BENCHMARK(BM_StreamSerializeLarge);

// =============================================================================
// Non-standard JSON parsing benchmarks
// =============================================================================

static std::string generate_json_with_comments() {
    std::string s = "{\n";
    for (int i = 0; i < 50; ++i) {
        s += "  // comment for key " + std::to_string(i) + "\n";
        s += "  \"key" + std::to_string(i) + "\": " + std::to_string(i);
        if (i < 49) s += ",";
        s += "\n";
    }
    s += "}";
    return s;
}

static void BM_ParseWithComments(benchmark::State& state) {
    auto input = generate_json_with_comments();
    ParseOptions opts;
    opts.allow_comments = true;
    for (auto _ : state) {
        auto v = parse(input, opts);
        benchmark::DoNotOptimize(v);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(input.size()));
}
BENCHMARK(BM_ParseWithComments);

// =============================================================================
// try_parse (error_code) benchmarks
// =============================================================================

static void BM_TryParseSuccess(benchmark::State& state) {
    auto input = generate_medium_json();
    for (auto _ : state) {
        auto r = try_parse(input);
        benchmark::DoNotOptimize(r.value);
    }
}
BENCHMARK(BM_TryParseSuccess);

static void BM_TryParseError(benchmark::State& state) {
    std::string input = "{invalid json content here}";
    for (auto _ : state) {
        auto r = try_parse(input);
        benchmark::DoNotOptimize(r.ec);
    }
}
BENCHMARK(BM_TryParseError);

// =============================================================================
// Type conversion (to_json/from_json) benchmarks
// =============================================================================

struct BenchSmallStruct {
    std::string name;
    int value = 0;
    bool active = false;
};
JSON_DEFINE_TYPE_NON_INTRUSIVE(BenchSmallStruct, name, value, active)

struct BenchMediumStruct {
    std::string id;
    std::string name;
    std::string email;
    int age = 0;
    double score = 0;
    bool active = false;
    std::vector<std::string> tags;
};
JSON_DEFINE_TYPE_NON_INTRUSIVE(BenchMediumStruct, id, name, email, age, score, active, tags)

static void BM_ConvStructToJson(benchmark::State& state) {
    BenchSmallStruct s{"test", 42, true};
    for (auto _ : state) {
        auto j = to_value(s);
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_ConvStructToJson);

static void BM_ConvJsonToStruct(benchmark::State& state) {
    BenchSmallStruct s{"test", 42, true};
    auto j = to_value(s);
    for (auto _ : state) {
        auto s2 = from_value<BenchSmallStruct>(j);
        benchmark::DoNotOptimize(s2);
    }
}
BENCHMARK(BM_ConvJsonToStruct);

static void BM_ConvMediumStructToString(benchmark::State& state) {
    BenchMediumStruct ms{"id123", "Alice", "alice@test.com", 30, 95.5, true,
                    {"admin", "user"}};
    for (auto _ : state) {
        auto j = to_value(ms);
        auto s = j.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_ConvMediumStructToString);

static void BM_ConvStringToMediumStruct(benchmark::State& state) {
    BenchMediumStruct ms{"id123", "Alice", "alice@test.com", 30, 95.5, true,
                    {"admin", "user"}};
    auto s = to_value(ms).dump();
    for (auto _ : state) {
        auto j = parse(s);
        auto ms2 = from_value<BenchMediumStruct>(j);
        benchmark::DoNotOptimize(ms2);
    }
}
BENCHMARK(BM_ConvStringToMediumStruct);

static void BM_ConvVectorOfStructs(benchmark::State& state) {
    std::vector<BenchSmallStruct> vec;
    for (int i = 0; i < 100; ++i) {
        vec.push_back({"item" + std::to_string(i), i, i % 2 == 0});
    }
    for (auto _ : state) {
        auto j = to_value(vec);
        auto s = j.dump();
        auto j2 = parse(s);
        auto vec2 = from_value<std::vector<BenchSmallStruct>>(j2);
        benchmark::DoNotOptimize(vec2);
    }
}
BENCHMARK(BM_ConvVectorOfStructs);

static void BM_SerializeSortKeys(benchmark::State& state) {
    auto v = parse(generate_medium_json());
    SerializeOptions opts;
    opts.sort_keys = true;
    for (auto _ : state) {
        auto s = yajson::serialize(v, opts);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SerializeSortKeys);
