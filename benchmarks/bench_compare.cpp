/// @file bench_compare.cpp
/// @brief Comprehensive head-to-head benchmark: yajson vs Boost.JSON vs RapidJSON
///        vs nlohmann/json vs simdjson.
///
/// All libraries parse/serialize identical data under identical conditions.
/// Scenarios:
///   1. Parse: small (~50B), medium (~2KB), large (~100KB)
///   2. Parse: int array 10K, float array 1K, string array 1Kx256
///   3. Parse: nested depth 200, flat object 1000 keys
///   4. Serialize compact: small, medium, large
///   5. Roundtrip (parse + serialize): large
///   6. Object key lookup: 1000 keys
///   7. Deep copy: large document
///   8. Network message batch: 100 messages
///   9. Arena/pool parse: large (yajson, Boost only)

#include <benchmark/benchmark.h>

// ─── yajson ──────────────────────────────────────────────────────────────────
#include <json/json.hpp>
#include <json/arena.hpp>

// ─── Boost.JSON ──────────────────────────────────────────────────────────────
#include <boost/json.hpp>

// ─── RapidJSON ───────────────────────────────────────────────────────────────
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

// ─── nlohmann/json ───────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>

// ─── simdjson ────────────────────────────────────────────────────────────────
#include <simdjson.h>

#include <sstream>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════════
// Shared test data (identical for all libraries)
// ═══════════════════════════════════════════════════════════════════════════════

namespace td {

inline std::string small_json() {
    return R"({"name":"John","age":30,"active":true,"score":95.5})";
}

inline std::string medium_json() {
    std::string s = R"({"users":[)";
    for (int i = 0; i < 20; ++i) {
        if (i) s += ",";
        s += R"({"id":)" + std::to_string(i) +
             R"(,"name":"user_)" + std::to_string(i) +
             R"(","email":"user)" + std::to_string(i) +
             R"(@test.com","active":)" + (i % 2 == 0 ? "true" : "false") +
             R"(,"score":)" + std::to_string(50.0 + i * 2.5) + "}";
    }
    s += R"(],"total":20,"page":1,"version":"2.0"})";
    return s;
}

inline std::string large_json() {
    std::string s = R"({"data":[)";
    for (int i = 0; i < 1000; ++i) {
        if (i) s += ",";
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

inline std::string int_array(int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += std::to_string(i);
    }
    return s + "]";
}

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

inline std::string nested(int depth) {
    std::string s;
    for (int i = 0; i < depth; ++i)
        s += R"({"level":)" + std::to_string(i) + R"(,"child":)";
    s += "null";
    for (int i = 0; i < depth; ++i) s += "}";
    return s;
}

inline std::string flat_object(int n) {
    std::string s = "{";
    for (int i = 0; i < n; ++i) {
        if (i) s += ',';
        s += "\"key_" + std::to_string(i) + "\":" + std::to_string(i);
    }
    return s + "}";
}

inline std::string network_msg() {
    return R"({"type":"client_connect","ap_id":"AP-001-FLOOR3",)"
           R"("mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"channel":36,)"
           R"("timestamp":1707350400,"ssid":"Corporate-5G",)"
           R"("ip":"192.168.1.105","vlan":100})";
}

inline std::vector<std::string> network_batch() {
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

} // namespace td

// Helper: compute total bytes of a vector of strings
static size_t total_bytes(const std::vector<std::string>& v) {
    size_t n = 0;
    for (auto& s : v) n += s.size();
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 1. PARSE SMALL
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_Small_Yajson(benchmark::State& st) {
    auto in = td::small_json();
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Small_Yajson);

static void BM_Parse_Small_Boost(benchmark::State& st) {
    auto in = td::small_json();
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Small_Boost);

static void BM_Parse_Small_Rapid(benchmark::State& st) {
    auto in = td::small_json();
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Small_Rapid);

static void BM_Parse_Small_Nlohmann(benchmark::State& st) {
    auto in = td::small_json();
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Small_Nlohmann);

static void BM_Parse_Small_Simdjson(benchmark::State& st) {
    auto in = td::small_json();
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Small_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 2. PARSE MEDIUM
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_Medium_Yajson(benchmark::State& st) {
    auto in = td::medium_json();
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Medium_Yajson);

static void BM_Parse_Medium_Boost(benchmark::State& st) {
    auto in = td::medium_json();
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Medium_Boost);

static void BM_Parse_Medium_Rapid(benchmark::State& st) {
    auto in = td::medium_json();
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Medium_Rapid);

static void BM_Parse_Medium_Nlohmann(benchmark::State& st) {
    auto in = td::medium_json();
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Medium_Nlohmann);

static void BM_Parse_Medium_Simdjson(benchmark::State& st) {
    auto in = td::medium_json();
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Medium_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 3. PARSE LARGE
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_Large_Yajson(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Large_Yajson);

static void BM_Parse_Large_Boost(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Large_Boost);

static void BM_Parse_Large_Rapid(benchmark::State& st) {
    auto in = td::large_json();
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Large_Rapid);

static void BM_Parse_Large_Nlohmann(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Large_Nlohmann);

static void BM_Parse_Large_Simdjson(benchmark::State& st) {
    auto in = td::large_json();
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Large_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 4. PARSE INT ARRAY (10K)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_IntArray10K_Yajson(benchmark::State& st) {
    auto in = td::int_array(10000);
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_IntArray10K_Yajson);

static void BM_Parse_IntArray10K_Boost(benchmark::State& st) {
    auto in = td::int_array(10000);
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_IntArray10K_Boost);

static void BM_Parse_IntArray10K_Rapid(benchmark::State& st) {
    auto in = td::int_array(10000);
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_IntArray10K_Rapid);

static void BM_Parse_IntArray10K_Nlohmann(benchmark::State& st) {
    auto in = td::int_array(10000);
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_IntArray10K_Nlohmann);

static void BM_Parse_IntArray10K_Simdjson(benchmark::State& st) {
    auto in = td::int_array(10000);
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_IntArray10K_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 5. PARSE FLOAT ARRAY (1K)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_FloatArray1K_Yajson(benchmark::State& st) {
    auto in = td::float_array(1000);
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FloatArray1K_Yajson);

static void BM_Parse_FloatArray1K_Boost(benchmark::State& st) {
    auto in = td::float_array(1000);
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FloatArray1K_Boost);

static void BM_Parse_FloatArray1K_Rapid(benchmark::State& st) {
    auto in = td::float_array(1000);
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FloatArray1K_Rapid);

static void BM_Parse_FloatArray1K_Nlohmann(benchmark::State& st) {
    auto in = td::float_array(1000);
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FloatArray1K_Nlohmann);

static void BM_Parse_FloatArray1K_Simdjson(benchmark::State& st) {
    auto in = td::float_array(1000);
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FloatArray1K_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 6. PARSE STRING ARRAY (1K x 256 chars)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_StrArray256_Yajson(benchmark::State& st) {
    auto in = td::string_array(1000, 256);
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_StrArray256_Yajson);

static void BM_Parse_StrArray256_Boost(benchmark::State& st) {
    auto in = td::string_array(1000, 256);
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_StrArray256_Boost);

static void BM_Parse_StrArray256_Rapid(benchmark::State& st) {
    auto in = td::string_array(1000, 256);
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_StrArray256_Rapid);

static void BM_Parse_StrArray256_Nlohmann(benchmark::State& st) {
    auto in = td::string_array(1000, 256);
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_StrArray256_Nlohmann);

static void BM_Parse_StrArray256_Simdjson(benchmark::State& st) {
    auto in = td::string_array(1000, 256);
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_StrArray256_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 7. PARSE NESTED (depth 200)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_Nested200_Yajson(benchmark::State& st) {
    auto in = td::nested(200);
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Nested200_Yajson);

static void BM_Parse_Nested200_Boost(benchmark::State& st) {
    auto in = td::nested(200);
    boost::json::parse_options opts;
    opts.max_depth = 256;
    for (auto _ : st) { auto v = boost::json::parse(in, {}, opts); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Nested200_Boost);

static void BM_Parse_Nested200_Rapid(benchmark::State& st) {
    auto in = td::nested(200);
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Nested200_Rapid);

static void BM_Parse_Nested200_Nlohmann(benchmark::State& st) {
    auto in = td::nested(200);
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Nested200_Nlohmann);

static void BM_Parse_Nested200_Simdjson(benchmark::State& st) {
    auto in = td::nested(200);
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_Nested200_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 8. PARSE FLAT OBJECT (1000 keys)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Parse_FlatObj1K_Yajson(benchmark::State& st) {
    auto in = td::flat_object(1000);
    for (auto _ : st) { auto v = yajson::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FlatObj1K_Yajson);

static void BM_Parse_FlatObj1K_Boost(benchmark::State& st) {
    auto in = td::flat_object(1000);
    for (auto _ : st) { auto v = boost::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FlatObj1K_Boost);

static void BM_Parse_FlatObj1K_Rapid(benchmark::State& st) {
    auto in = td::flat_object(1000);
    rapidjson::Document doc;
    for (auto _ : st) { doc.Parse(in.c_str(), in.size()); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FlatObj1K_Rapid);

static void BM_Parse_FlatObj1K_Nlohmann(benchmark::State& st) {
    auto in = td::flat_object(1000);
    for (auto _ : st) { auto v = nlohmann::json::parse(in); benchmark::DoNotOptimize(v); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FlatObj1K_Nlohmann);

static void BM_Parse_FlatObj1K_Simdjson(benchmark::State& st) {
    auto in = td::flat_object(1000);
    simdjson::dom::parser parser;
    simdjson::padded_string padded(in);
    for (auto _ : st) { auto doc = parser.parse(padded); benchmark::DoNotOptimize(doc); }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Parse_FlatObj1K_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 9. SERIALIZE COMPACT — SMALL
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Ser_Small_Yajson(benchmark::State& st) {
    auto v = yajson::parse(td::small_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Small_Yajson);

static void BM_Ser_Small_Boost(benchmark::State& st) {
    auto v = boost::json::parse(td::small_json());
    for (auto _ : st) { auto s = boost::json::serialize(v); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Small_Boost);

static void BM_Ser_Small_Rapid(benchmark::State& st) {
    auto in = td::small_json();
    rapidjson::Document doc;
    doc.Parse(in.c_str(), in.size());
    for (auto _ : st) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        doc.Accept(w);
        benchmark::DoNotOptimize(buf.GetString());
    }
}
BENCHMARK(BM_Ser_Small_Rapid);

static void BM_Ser_Small_Nlohmann(benchmark::State& st) {
    auto v = nlohmann::json::parse(td::small_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Small_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 10. SERIALIZE COMPACT — MEDIUM
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Ser_Medium_Yajson(benchmark::State& st) {
    auto v = yajson::parse(td::medium_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Medium_Yajson);

static void BM_Ser_Medium_Boost(benchmark::State& st) {
    auto v = boost::json::parse(td::medium_json());
    for (auto _ : st) { auto s = boost::json::serialize(v); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Medium_Boost);

static void BM_Ser_Medium_Rapid(benchmark::State& st) {
    auto in = td::medium_json();
    rapidjson::Document doc;
    doc.Parse(in.c_str(), in.size());
    for (auto _ : st) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        doc.Accept(w);
        benchmark::DoNotOptimize(buf.GetString());
    }
}
BENCHMARK(BM_Ser_Medium_Rapid);

static void BM_Ser_Medium_Nlohmann(benchmark::State& st) {
    auto v = nlohmann::json::parse(td::medium_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Medium_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 11. SERIALIZE COMPACT — LARGE
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Ser_Large_Yajson(benchmark::State& st) {
    auto v = yajson::parse(td::large_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Large_Yajson);

static void BM_Ser_Large_Boost(benchmark::State& st) {
    auto v = boost::json::parse(td::large_json());
    for (auto _ : st) { auto s = boost::json::serialize(v); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Large_Boost);

static void BM_Ser_Large_Rapid(benchmark::State& st) {
    auto in = td::large_json();
    rapidjson::Document doc;
    doc.Parse(in.c_str(), in.size());
    for (auto _ : st) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        doc.Accept(w);
        benchmark::DoNotOptimize(buf.GetString());
    }
}
BENCHMARK(BM_Ser_Large_Rapid);

static void BM_Ser_Large_Nlohmann(benchmark::State& st) {
    auto v = nlohmann::json::parse(td::large_json());
    for (auto _ : st) { auto s = v.dump(); benchmark::DoNotOptimize(s); }
}
BENCHMARK(BM_Ser_Large_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 12. ROUNDTRIP LARGE (parse + serialize)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Roundtrip_Large_Yajson(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) {
        auto v = yajson::parse(in);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Roundtrip_Large_Yajson);

static void BM_Roundtrip_Large_Boost(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) {
        auto v = boost::json::parse(in);
        auto s = boost::json::serialize(v);
        benchmark::DoNotOptimize(s);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Roundtrip_Large_Boost);

static void BM_Roundtrip_Large_Rapid(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) {
        rapidjson::Document doc;
        doc.Parse(in.c_str(), in.size());
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        doc.Accept(w);
        benchmark::DoNotOptimize(buf.GetString());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Roundtrip_Large_Rapid);

static void BM_Roundtrip_Large_Nlohmann(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) {
        auto v = nlohmann::json::parse(in);
        auto s = v.dump();
        benchmark::DoNotOptimize(s);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_Roundtrip_Large_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 13. OBJECT KEY LOOKUP (1000 keys)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_Lookup_1K_Yajson(benchmark::State& st) {
    auto obj = yajson::JsonValue::object();
    for (int i = 0; i < 1000; ++i)
        obj.insert("key_" + std::to_string(i), yajson::JsonValue(i));
    auto& warmup = obj["key_0"];
    benchmark::DoNotOptimize(warmup);
    int idx = 0;
    for (auto _ : st) {
        auto* p = obj.find("key_" + std::to_string(idx % 1000));
        benchmark::DoNotOptimize(p);
        ++idx;
    }
}
BENCHMARK(BM_Lookup_1K_Yajson);

static void BM_Lookup_1K_Boost(benchmark::State& st) {
    boost::json::object obj;
    for (int i = 0; i < 1000; ++i) obj["key_" + std::to_string(i)] = i;
    int idx = 0;
    for (auto _ : st) {
        auto it = obj.find("key_" + std::to_string(idx % 1000));
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BM_Lookup_1K_Boost);

static void BM_Lookup_1K_Rapid(benchmark::State& st) {
    // Build JSON, parse into document
    auto in = td::flat_object(1000);
    rapidjson::Document doc;
    doc.Parse(in.c_str(), in.size());
    int idx = 0;
    for (auto _ : st) {
        auto key = "key_" + std::to_string(idx % 1000);
        auto it = doc.FindMember(key.c_str());
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BM_Lookup_1K_Rapid);

static void BM_Lookup_1K_Nlohmann(benchmark::State& st) {
    nlohmann::json obj;
    for (int i = 0; i < 1000; ++i) obj["key_" + std::to_string(i)] = i;
    int idx = 0;
    for (auto _ : st) {
        auto it = obj.find("key_" + std::to_string(idx % 1000));
        benchmark::DoNotOptimize(it);
        ++idx;
    }
}
BENCHMARK(BM_Lookup_1K_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 14. DEEP COPY (large document)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_DeepCopy_Yajson(benchmark::State& st) {
    auto v = yajson::parse(td::large_json());
    for (auto _ : st) { yajson::JsonValue c = v; benchmark::DoNotOptimize(c); }
}
BENCHMARK(BM_DeepCopy_Yajson);

static void BM_DeepCopy_Boost(benchmark::State& st) {
    auto v = boost::json::parse(td::large_json());
    for (auto _ : st) { boost::json::value c = v; benchmark::DoNotOptimize(c); }
}
BENCHMARK(BM_DeepCopy_Boost);

static void BM_DeepCopy_Rapid(benchmark::State& st) {
    auto in = td::large_json();
    rapidjson::Document doc;
    doc.Parse(in.c_str(), in.size());
    for (auto _ : st) {
        rapidjson::Document copy;
        copy.CopyFrom(doc, copy.GetAllocator());
        benchmark::DoNotOptimize(copy);
    }
}
BENCHMARK(BM_DeepCopy_Rapid);

static void BM_DeepCopy_Nlohmann(benchmark::State& st) {
    auto v = nlohmann::json::parse(td::large_json());
    for (auto _ : st) { nlohmann::json c = v; benchmark::DoNotOptimize(c); }
}
BENCHMARK(BM_DeepCopy_Nlohmann);

// ═══════════════════════════════════════════════════════════════════════════════
// 15. NETWORK MESSAGE BATCH (100 messages)
// ═══════════════════════════════════════════════════════════════════════════════

static void BM_NetBatch_Yajson(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) { auto v = yajson::parse(m); benchmark::DoNotOptimize(v); }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Yajson);

static void BM_NetBatch_Boost(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) { auto v = boost::json::parse(m); benchmark::DoNotOptimize(v); }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Boost);

static void BM_NetBatch_Rapid(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    rapidjson::Document doc;
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) { doc.Parse(m.c_str(), m.size()); benchmark::DoNotOptimize(doc); }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Rapid);

static void BM_NetBatch_Nlohmann(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) { auto v = nlohmann::json::parse(m); benchmark::DoNotOptimize(v); }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Nlohmann);

static void BM_NetBatch_Simdjson(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    // Pre-pad all messages
    std::vector<simdjson::padded_string> padded;
    padded.reserve(msgs.size());
    for (auto& m : msgs) padded.emplace_back(m);
    simdjson::dom::parser parser;
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& p : padded) { auto doc = parser.parse(p); benchmark::DoNotOptimize(doc); }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Simdjson);

// ═══════════════════════════════════════════════════════════════════════════════
// 16. ARENA PARSE — yajson MonotonicArena vs Boost monotonic_resource
//     (yajson reuses arena with reset(); Boost creates fresh monotonic_resource
//      per iteration since it lacks reset — this is its intended usage pattern)
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Small ───────────────────────────────────────────────────────────────────

static void BM_ArenaParse_Small_Yajson(benchmark::State& st) {
    auto in = td::small_json();
    yajson::MonotonicArena arena(4096);
    for (auto _ : st) {
        { auto v = yajson::parse(in, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Small_Yajson);

static void BM_ArenaParse_Small_Boost(benchmark::State& st) {
    auto in = td::small_json();
    for (auto _ : st) {
        boost::json::monotonic_resource mr(4096);
        auto v = boost::json::parse(in, &mr);
        benchmark::DoNotOptimize(v);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Small_Boost);

// ─── Medium ──────────────────────────────────────────────────────────────────

static void BM_ArenaParse_Medium_Yajson(benchmark::State& st) {
    auto in = td::medium_json();
    yajson::MonotonicArena arena(32 * 1024);
    for (auto _ : st) {
        { auto v = yajson::parse(in, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Medium_Yajson);

static void BM_ArenaParse_Medium_Boost(benchmark::State& st) {
    auto in = td::medium_json();
    for (auto _ : st) {
        boost::json::monotonic_resource mr(32 * 1024);
        auto v = boost::json::parse(in, &mr);
        benchmark::DoNotOptimize(v);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Medium_Boost);

// ─── Large ───────────────────────────────────────────────────────────────────

static void BM_ArenaParse_Large_Yajson(benchmark::State& st) {
    auto in = td::large_json();
    yajson::MonotonicArena arena(512 * 1024);
    for (auto _ : st) {
        { auto v = yajson::parse(in, arena); benchmark::DoNotOptimize(v); }
        arena.reset();
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Large_Yajson);

static void BM_ArenaParse_Large_Boost(benchmark::State& st) {
    auto in = td::large_json();
    for (auto _ : st) {
        boost::json::monotonic_resource mr(512 * 1024);
        auto v = boost::json::parse(in, &mr);
        benchmark::DoNotOptimize(v);
    }
    st.SetBytesProcessed(st.iterations() * int64_t(in.size()));
}
BENCHMARK(BM_ArenaParse_Large_Boost);

// ─── Network Batch with Arena ────────────────────────────────────────────────

static void BM_NetBatch_Arena_Yajson(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    yajson::MonotonicArena arena(32 * 1024);
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) {
            { auto v = yajson::parse(m, arena); benchmark::DoNotOptimize(v); }
            arena.reset();
        }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Arena_Yajson);

static void BM_NetBatch_Arena_Boost(benchmark::State& st) {
    auto msgs = td::network_batch();
    auto tb = total_bytes(msgs);
    int64_t mc = 0;
    for (auto _ : st) {
        for (auto& m : msgs) {
            boost::json::monotonic_resource mr(4096);
            auto v = boost::json::parse(m, &mr);
            benchmark::DoNotOptimize(v);
        }
        mc += int64_t(msgs.size());
    }
    st.SetBytesProcessed(st.iterations() * int64_t(tb));
    st.counters["msg/s"] = benchmark::Counter(double(mc), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_NetBatch_Arena_Boost);
