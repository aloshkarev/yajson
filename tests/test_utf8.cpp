/// @file test_utf8.cpp
/// @brief Unit tests for UTF-8 support — parsing, serialization, roundtrip.

#include <json/json.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace yajson;

// ═══════════════════════════════════════════════════════════════════════════════
// Parsing Unicode escape sequences (\uXXXX)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Parser, AsciiEscape) {
    // \u0041 = 'A'
    auto v = parse(R"("\u0041")");
    EXPECT_EQ(v.as_string(), "A");
}

TEST(Utf8Parser, NullEscape) {
    // \u0000 = null character
    auto v = parse(R"("\u0000")");
    std::string expected(1, '\0');
    EXPECT_EQ(v.as_string(), expected);
}

TEST(Utf8Parser, CyrillicEscape) {
    // \u041F = 'П' (Cyrillic capital Pe)
    auto v = parse(R"("\u041f")");
    // UTF-8: 0xD0 0x9F
    EXPECT_EQ(v.as_string(), "\xD0\x9F");
}

TEST(Utf8Parser, MultipleUnicodeEscapes) {
    // "Привет" (Hello) in Unicode escapes
    auto v = parse(R"("\u041F\u0440\u0438\u0432\u0435\u0442")");
    EXPECT_EQ(v.as_string(), "Привет");
}

TEST(Utf8Parser, ChineseEscape) {
    // \u4F60 = '你', \u597D = '好'
    auto v = parse(R"("\u4f60\u597d")");
    EXPECT_EQ(v.as_string(), "你好");
}

TEST(Utf8Parser, JapaneseEscape) {
    // \u3053\u3093\u306B\u3061\u306F = こんにちは
    auto v = parse(R"("\u3053\u3093\u306b\u3061\u306f")");
    EXPECT_EQ(v.as_string(), "こんにちは");
}

TEST(Utf8Parser, EuroSign) {
    // \u20AC = '€'
    auto v = parse(R"("\u20ac")");
    EXPECT_EQ(v.as_string(), "€");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Surrogate pairs (codepoints > U+FFFF)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Parser, SurrogatePairEmoji) {
    // U+1F600 (😀) = \uD83D\uDE00
    auto v = parse(R"("\uD83D\uDE00")");
    EXPECT_EQ(v.as_string(), "😀");
}

TEST(Utf8Parser, SurrogatePairMusicalNote) {
    // U+1D11E (𝄞 Musical Symbol G Clef) = \uD834\uDD1E
    auto v = parse(R"("\uD834\uDD1E")");
    // UTF-8: F0 9D 84 9E
    EXPECT_EQ(v.as_string(), "\xF0\x9D\x84\x9E");
}

TEST(Utf8Parser, SurrogatePairPile) {
    // U+1F4A9 (💩) = \uD83D\uDCA9
    auto v = parse(R"("\uD83D\uDCA9")");
    EXPECT_EQ(v.as_string(), "💩");
}

TEST(Utf8Parser, MultipleSurrogatePairs) {
    // 😀😎 = \uD83D\uDE00\uD83D\uDE0E
    auto v = parse(R"("\uD83D\uDE00\uD83D\uDE0E")");
    EXPECT_EQ(v.as_string(), "😀😎");
}

TEST(Utf8Parser, MixedEscapesAndText) {
    auto v = parse(R"("Hello \u4E16\u754C! \uD83D\uDE00")");
    EXPECT_EQ(v.as_string(), "Hello 世界! 😀");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Surrogate pair errors
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Parser, ErrorLoneSurrogateHigh) {
    // High surrogate without low surrogate
    EXPECT_THROW(parse(R"("\uD83D")"), ParseError);
}

TEST(Utf8Parser, ErrorLoneSurrogateLow) {
    // Low surrogate without high surrogate
    EXPECT_THROW(parse(R"("\uDC00")"), ParseError);
}

TEST(Utf8Parser, ErrorInvalidLowSurrogate) {
    // High surrogate + invalid low surrogate
    EXPECT_THROW(parse(R"("\uD83D\u0041")"), ParseError);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Direct UTF-8 in strings (without escape)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Parser, DirectUtf8Cyrillic) {
    auto v = parse("\"Привет мир\"");
    EXPECT_EQ(v.as_string(), "Привет мир");
}

TEST(Utf8Parser, DirectUtf8Chinese) {
    auto v = parse("\"你好世界\"");
    EXPECT_EQ(v.as_string(), "你好世界");
}

TEST(Utf8Parser, DirectUtf8Emoji) {
    auto v = parse("\"Hello 😀🌍\"");
    EXPECT_EQ(v.as_string(), "Hello 😀🌍");
}

TEST(Utf8Parser, DirectUtf8Mixed) {
    auto v = parse("\"café résumé naïve\"");
    EXPECT_EQ(v.as_string(), "café résumé naïve");
}

TEST(Utf8Parser, Utf8ObjectKeys) {
    auto v = parse(R"({"имя": "Алиса", "город": "Москва"})");
    EXPECT_EQ(v["имя"].as_string(), "Алиса");
    EXPECT_EQ(v["город"].as_string(), "Москва");
}

// ═══════════════════════════════════════════════════════════════════════════════
// UTF-8 serialization
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Serializer, PassthroughUtf8) {
    // By default UTF-8 passes through as-is
    JsonValue v("Привет 😀");
    auto s = v.dump();
    EXPECT_EQ(s, "\"Привет 😀\"");
}

TEST(Utf8Serializer, EnsureAsciiCyrillic) {
    JsonValue v("Пр");
    SerializeOptions opts;
    opts.ensure_ascii = true;
    auto s = yajson::serialize(v, opts);
    // 'П' = U+041F -> \u041f, 'р' = U+0440 -> \u0440
    EXPECT_EQ(s, "\"\\u041f\\u0440\"");
}

TEST(Utf8Serializer, EnsureAsciiEmoji) {
    JsonValue v("😀");
    SerializeOptions opts;
    opts.ensure_ascii = true;
    auto s = yajson::serialize(v, opts);
    // U+1F600 = \uD83D\uDE00 (surrogate pair)
    EXPECT_EQ(s, "\"\\ud83d\\ude00\"");
}

TEST(Utf8Serializer, EnsureAsciiMixed) {
    JsonValue v("Hi 世界!");
    SerializeOptions opts;
    opts.ensure_ascii = true;
    auto s = yajson::serialize(v, opts);
    EXPECT_EQ(s, "\"Hi \\u4e16\\u754c!\"");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Roundtrip: parse → dump → parse (UTF-8)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Roundtrip, CyrillicDirect) {
    std::string input = "\"Привет мир\"";
    auto v1 = parse(input);
    auto output = v1.dump();
    auto v2 = parse(output);
    EXPECT_EQ(v1, v2);
}

TEST(Utf8Roundtrip, EmojiDirect) {
    std::string input = "\"Hello 😀🌍💩\"";
    auto v1 = parse(input);
    auto output = v1.dump();
    auto v2 = parse(output);
    EXPECT_EQ(v1, v2);
}

TEST(Utf8Roundtrip, UnicodeEscapeToUtf8) {
    // Parse escape -> get UTF-8 -> serialize -> parse again
    auto v1 = parse(R"("\u041F\u0440\u0438\u0432\u0435\u0442")");
    EXPECT_EQ(v1.as_string(), "Привет");
    auto output = v1.dump();
    auto v2 = parse(output);
    EXPECT_EQ(v2.as_string(), "Привет");
}

TEST(Utf8Roundtrip, SurrogatePairRoundtrip) {
    auto v1 = parse(R"("\uD83D\uDE00")");
    EXPECT_EQ(v1.as_string(), "😀");
    auto output = v1.dump();
    auto v2 = parse(output);
    EXPECT_EQ(v1, v2);
}

TEST(Utf8Roundtrip, EnsureAsciiRoundtrip) {
    JsonValue v("Привет 😀");
    SerializeOptions opts;
    opts.ensure_ascii = true;
    auto s = yajson::serialize(v, opts);
    auto v2 = parse(s);
    EXPECT_EQ(v, v2);
}

TEST(Utf8Roundtrip, ComplexUtf8Document) {
    auto v1 = parse(R"({
        "name": "Тест",
        "emoji": "😀🎉",
        "chinese": "你好",
        "japanese": "こんにちは",
        "mixed": "café résumé",
        "escaped": "\u0041\u0042\u0043",
        "surrogate": "\uD83D\uDE00"
    })");

    EXPECT_EQ(v1["name"].as_string(), "Тест");
    EXPECT_EQ(v1["emoji"].as_string(), "😀🎉");
    EXPECT_EQ(v1["chinese"].as_string(), "你好");
    EXPECT_EQ(v1["japanese"].as_string(), "こんにちは");
    EXPECT_EQ(v1["mixed"].as_string(), "café résumé");
    EXPECT_EQ(v1["escaped"].as_string(), "ABC");
    EXPECT_EQ(v1["surrogate"].as_string(), "😀");

    // Full roundtrip
    auto serialized = v1.dump(2);
    auto v2 = parse(serialized);
    EXPECT_EQ(v1, v2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UTF-8 utilities (detail::utf8)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Utf8Utils, EncodeAscii) {
    std::string out;
    yajson::detail::utf8::encode('A', out);
    EXPECT_EQ(out, "A");
}

TEST(Utf8Utils, Encode2Byte) {
    std::string out;
    yajson::detail::utf8::encode(0x041F, out); // П
    EXPECT_EQ(out, "\xD0\x9F");
}

TEST(Utf8Utils, Encode3Byte) {
    std::string out;
    yajson::detail::utf8::encode(0x4F60, out); // 你
    EXPECT_EQ(out, "\xE4\xBD\xA0");
}

TEST(Utf8Utils, Encode4Byte) {
    std::string out;
    yajson::detail::utf8::encode(0x1F600, out); // 😀
    EXPECT_EQ(out, "\xF0\x9F\x98\x80");
}

TEST(Utf8Utils, DecodeAscii) {
    const char* p = "A";
    const char* end = p + 1;
    uint32_t cp = yajson::detail::utf8::decode(p, end);
    EXPECT_EQ(cp, 0x41u);
    EXPECT_EQ(p, end);
}

TEST(Utf8Utils, Decode4Byte) {
    std::string s = "😀";
    const char* p = s.data();
    const char* end = s.data() + s.size();
    uint32_t cp = yajson::detail::utf8::decode(p, end);
    EXPECT_EQ(cp, 0x1F600u);
    EXPECT_EQ(p, end);
}

TEST(Utf8Utils, Validate) {
    EXPECT_TRUE(yajson::detail::utf8::validate("Hello", "Hello" + 5));
    std::string utf8 = "Привет 😀";
    EXPECT_TRUE(yajson::detail::utf8::validate(utf8.data(), utf8.data() + utf8.size()));

    // Invalid UTF-8: lone continuation byte
    const char invalid[] = {static_cast<char>(0x80), 0};
    EXPECT_FALSE(yajson::detail::utf8::validate(invalid, invalid + 1));
}

TEST(Utf8Utils, EncodeEscapedBMP) {
    std::string out;
    yajson::detail::utf8::encode_escaped(0x041F, out);
    EXPECT_EQ(out, "\\u041f");
}

TEST(Utf8Utils, EncodeEscapedSurrogatePair) {
    std::string out;
    yajson::detail::utf8::encode_escaped(0x1F600, out);
    EXPECT_EQ(out, "\\ud83d\\ude00");
}
