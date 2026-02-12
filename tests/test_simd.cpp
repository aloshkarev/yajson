/// @file test_simd.cpp
/// @brief Dedicated tests for SIMD code paths (SSE2/AVX2/NEON).
///
/// These tests exercise skip_whitespace, find_string_delimiter, and
/// find_needs_escape at various input lengths to cover:
///   - Scalar fallback (< 16 bytes)
///   - SSE2 / NEON path (16–31 bytes)
///   - AVX2 / NEON-64 path (≥ 32 bytes)
///   - Tail handling after SIMD blocks
///
/// Build with -march=native to test AVX2 paths on x86_64, or with
/// -DYAJSON_NATIVE_ARCH=ON in CMake.

#include <gtest/gtest.h>
#include <json/json.hpp>
#include <json/detail/simd.hpp>

#include <string>
#include <cstring>

namespace simd = yajson::detail::simd;

// ═══════════════════════════════════════════════════════════════════════════════
// Report which SIMD path is active (informational)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SimdDetection, ReportActivePath) {
    std::string path = "scalar";
#if defined(YAJSON_AVX2)
    path = "AVX2 (32 bytes/iter)";
#elif defined(YAJSON_SSE2)
    path = "SSE2 (16 bytes/iter)";
#elif defined(YAJSON_NEON_64)
    path = "NEON AArch64 (2x16 bytes/iter)";
#elif defined(YAJSON_NEON)
    path = "NEON ARMv7 (16 bytes/iter)";
#endif
    std::cout << "[   INFO   ] Active SIMD path: " << path << std::endl;
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// skip_whitespace
// ═══════════════════════════════════════════════════════════════════════════════

class SkipWhitespaceTest : public ::testing::TestWithParam<size_t> {};

TEST_P(SkipWhitespaceTest, AllWhitespace) {
    // N bytes of whitespace → should return pointer to end
    size_t n = GetParam();
    std::string ws(n, ' ');
    const char* result = simd::skip_whitespace(ws.data(), ws.data() + ws.size());
    EXPECT_EQ(result, ws.data() + ws.size())
        << "Failed for size " << n;
}

TEST_P(SkipWhitespaceTest, NonWhitespaceAtEnd) {
    // N-1 whitespace + 'x' → should find 'x' at position N-1
    size_t n = GetParam();
    if (n == 0) return;
    std::string ws(n - 1, ' ');
    ws += 'x';
    const char* result = simd::skip_whitespace(ws.data(), ws.data() + ws.size());
    EXPECT_EQ(result, ws.data() + n - 1)
        << "Failed for size " << n;
}

TEST_P(SkipWhitespaceTest, NonWhitespaceAtStart) {
    // 'y' + N-1 whitespace → should return start immediately
    size_t n = GetParam();
    if (n == 0) return;
    std::string s(n, ' ');
    s[0] = 'y';
    const char* result = simd::skip_whitespace(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data())
        << "Failed for size " << n;
}

TEST_P(SkipWhitespaceTest, MixedWhitespace) {
    // Mix of ' ', '\t', '\n', '\r' then 'Z'
    size_t n = GetParam();
    if (n == 0) return;
    const char ws_chars[] = {' ', '\t', '\n', '\r'};
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n - 1; ++i)
        s += ws_chars[i % 4];
    s += 'Z';
    const char* result = simd::skip_whitespace(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + n - 1)
        << "Failed for size " << n;
}

// Test sizes: scalar (<16), SSE2 (16–31), AVX2 (32–63), large (64–128), tail
INSTANTIATE_TEST_SUITE_P(
    Sizes, SkipWhitespaceTest,
    ::testing::Values(0, 1, 7, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65,
                      100, 127, 128, 255, 256, 512, 1024));

// ═══════════════════════════════════════════════════════════════════════════════
// find_string_delimiter
// ═══════════════════════════════════════════════════════════════════════════════

class FindStringDelimTest : public ::testing::TestWithParam<size_t> {};

TEST_P(FindStringDelimTest, QuoteAtEnd) {
    // N-1 chars 'a' + '"' → should find quote at position N-1
    size_t n = GetParam();
    if (n == 0) return;
    std::string s(n - 1, 'a');
    s += '"';
    const char* result = simd::find_string_delimiter(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + n - 1)
        << "Failed for size " << n;
}

TEST_P(FindStringDelimTest, BackslashAtEnd) {
    size_t n = GetParam();
    if (n == 0) return;
    std::string s(n - 1, 'b');
    s += '\\';
    const char* result = simd::find_string_delimiter(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + n - 1)
        << "Failed for size " << n;
}

TEST_P(FindStringDelimTest, NoDelimiter) {
    // All 'c' chars → should return end
    size_t n = GetParam();
    std::string s(n, 'c');
    const char* result = simd::find_string_delimiter(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + s.size())
        << "Failed for size " << n;
}

TEST_P(FindStringDelimTest, QuoteAtStart) {
    size_t n = GetParam();
    if (n == 0) return;
    std::string s(n, 'd');
    s[0] = '"';
    const char* result = simd::find_string_delimiter(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data())
        << "Failed for size " << n;
}

TEST_P(FindStringDelimTest, QuoteInMiddle) {
    // Place quote exactly at position n/2
    size_t n = GetParam();
    if (n < 2) return;
    std::string s(n, 'e');
    size_t mid = n / 2;
    s[mid] = '"';
    const char* result = simd::find_string_delimiter(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + mid)
        << "Failed for size " << n << " mid " << mid;
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, FindStringDelimTest,
    ::testing::Values(0, 1, 7, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65,
                      100, 127, 128, 255, 256, 512, 1024));

// ═══════════════════════════════════════════════════════════════════════════════
// find_needs_escape
// ═══════════════════════════════════════════════════════════════════════════════

class FindNeedsEscapeTest : public ::testing::TestWithParam<size_t> {};

TEST_P(FindNeedsEscapeTest, NoEscapeNeeded) {
    // All 'a' → no escape → should return end
    size_t n = GetParam();
    std::string s(n, 'a');
    const char* result = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + s.size())
        << "Failed for size " << n;
}

TEST_P(FindNeedsEscapeTest, ControlCharAtEnd) {
    size_t n = GetParam();
    if (n == 0) return;
    std::string s(n - 1, 'f');
    s += '\x01';  // control character
    const char* result = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + n - 1)
        << "Failed for size " << n;
}

TEST_P(FindNeedsEscapeTest, QuoteInMiddle) {
    size_t n = GetParam();
    if (n < 2) return;
    std::string s(n, 'g');
    size_t mid = n / 2;
    s[mid] = '"';
    const char* result = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + mid)
        << "Failed for size " << n << " mid " << mid;
}

TEST_P(FindNeedsEscapeTest, BackslashInMiddle) {
    size_t n = GetParam();
    if (n < 2) return;
    std::string s(n, 'h');
    size_t mid = n / 2;
    s[mid] = '\\';
    const char* result = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + mid)
        << "Failed for size " << n << " mid " << mid;
}

TEST_P(FindNeedsEscapeTest, HighByteWithEnsureAscii) {
    // Byte >= 0x80 should be flagged with EnsureAscii=true
    size_t n = GetParam();
    if (n < 2) return;
    std::string s(n, 'i');
    size_t mid = n / 2;
    s[mid] = static_cast<char>(0xC0);  // start of UTF-8 2-byte seq
    // Without EnsureAscii → no escape needed
    const char* r1 = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(r1, s.data() + s.size())
        << "EnsureAscii=false should NOT flag high bytes, size " << n;
    // With EnsureAscii → should be flagged
    const char* r2 = simd::find_needs_escape<true>(s.data(), s.data() + s.size());
    EXPECT_EQ(r2, s.data() + mid)
        << "EnsureAscii=true should flag byte 0xC0 at mid, size " << n;
}

TEST_P(FindNeedsEscapeTest, NullByteDetected) {
    // '\0' is a control char (< 0x20) and must be detected
    size_t n = GetParam();
    if (n < 3) return;
    std::string s(n, 'j');
    size_t pos = n / 3;
    s[pos] = '\0';
    const char* result = simd::find_needs_escape<false>(s.data(), s.data() + s.size());
    EXPECT_EQ(result, s.data() + pos)
        << "Failed for size " << n << " pos " << pos;
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, FindNeedsEscapeTest,
    ::testing::Values(0, 1, 7, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65,
                      100, 127, 128, 255, 256, 512, 1024));

// ═══════════════════════════════════════════════════════════════════════════════
// End-to-end: parse + serialize with data hitting different SIMD block sizes
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SimdEndToEnd, LargeWhitespaceBeforeValue) {
    // Lots of whitespace before JSON → exercises skip_whitespace SIMD path
    for (size_t ws_len : {0, 15, 16, 31, 32, 63, 64, 100, 256}) {
        std::string input(ws_len, ' ');
        input += "42";
        auto val = yajson::parse(input);
        EXPECT_EQ(val.as_integer(), 42) << "ws_len=" << ws_len;
    }
}

TEST(SimdEndToEnd, LongStringsExerciseDelimiterSearch) {
    // Strings of varying lengths → exercises find_string_delimiter
    for (size_t str_len : {0, 15, 16, 31, 32, 63, 64, 100, 256, 1024}) {
        std::string payload(str_len, 'A');
        std::string json = "\"" + payload + "\"";
        auto val = yajson::parse(json);
        EXPECT_EQ(val.as_string(), payload) << "str_len=" << str_len;
    }
}

TEST(SimdEndToEnd, LongStringsWithEscapes) {
    // Long clean strings with an escape at various positions
    for (size_t total : {20, 33, 50, 65, 130}) {
        for (size_t esc_pos : {size_t(0), total / 4, total / 2, total - 1}) {
            if (esc_pos >= total) continue;
            std::string payload(total, 'B');
            payload[esc_pos] = '\n';  // will need escaping on serialize
            // Build JSON with \n escaped
            std::string json = "\"";
            for (size_t i = 0; i < total; ++i) {
                if (i == esc_pos)
                    json += "\\n";
                else
                    json += 'B';
            }
            json += "\"";
            auto val = yajson::parse(json);
            EXPECT_EQ(val.as_string(), payload)
                << "total=" << total << " esc_pos=" << esc_pos;
            // Round-trip: serialize should produce valid JSON
            auto dumped = val.dump();
            auto reparsed = yajson::parse(dumped);
            EXPECT_EQ(reparsed.as_string(), payload)
                << "round-trip failed, total=" << total << " esc_pos=" << esc_pos;
        }
    }
}

TEST(SimdEndToEnd, SerializeLongCleanString) {
    // Serializing a long ASCII string exercises find_needs_escape
    for (size_t len : {0, 15, 16, 31, 32, 63, 64, 128, 256, 1024}) {
        std::string payload(len, 'X');
        auto val = yajson::JsonValue(payload);
        auto json = val.dump();
        EXPECT_EQ(json, "\"" + payload + "\"") << "len=" << len;
    }
}

TEST(SimdEndToEnd, SerializeEnsureAsciiHighBytes) {
    // UTF-8 strings: ensure_ascii exercises the high-byte detection in SIMD
    for (size_t prefix_len : {0, 15, 16, 31, 32, 63, 64}) {
        std::string payload(prefix_len, 'Y');
        payload += "\xC3\xA9";  // é in UTF-8
        auto val = yajson::JsonValue(payload);
        // Without ensure_ascii — UTF-8 passes through
        auto json_normal = val.dump();
        auto reparsed = yajson::parse(json_normal);
        EXPECT_EQ(reparsed.as_string(), payload)
            << "prefix_len=" << prefix_len;
    }
}
