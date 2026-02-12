#pragma once

/// @file parser.hpp
/// @author Aleksandr Loshkarev
/// @brief High-performance recursive JSON parser.
///
/// Features:
///   - SIMD-accelerated whitespace skipping and string scanning
///   - Inline integer accumulation (no from_chars on the hot path)
///   - Branch prediction hints for hot paths
///   - Non-standard JSON extensions (comments, trailing commas, etc.)
///   - Exception-free parsing via try_parse() with error_code
///   - Recursion depth limiting to protect against stack overflow
///   - Full UTF-8 support, including surrogate pairs

#include "config.hpp"
#include "detail/simd.hpp"
#include "detail/utf8.hpp"
#include "error.hpp"
#include "parse_options.hpp"
#include "value.hpp"

#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace yajson {
namespace detail {

/// @brief High-performance recursive JSON parser.
class Parser {
public:
    /// @brief Parse a JSON string (with exceptions).
    ///
    /// Internally creates a stack-buffered std::pmr::monotonic_buffer_resource
    /// for parser temporaries (string building, duplicate-key set). When an
    /// arena is active, the arena is used instead — enabling zero-copy strings.
    [[nodiscard]] static JsonValue parse(std::string_view input,
                                         const ParseOptions& opts = {}) {
        // Stack-buffered monotonic resource for parser temporary allocations.
        // When an arena is active, use the arena directly (zero-copy path).
        // Cache the TLS arena pointer once here — the Parser constructor will
        // cache it further, avoiding repeated TLS access inside the hot loop.
        auto* arena = detail::current_arena;
        alignas(16) char temp_buf[4096];
        std::pmr::monotonic_buffer_resource local_mbr(
            temp_buf, sizeof(temp_buf), std::pmr::new_delete_resource());
        auto* mr = arena
                 ? static_cast<std::pmr::memory_resource*>(arena)
                 : static_cast<std::pmr::memory_resource*>(&local_mbr);

        Parser p(input.data(), input.data() + input.size(), opts, mr, arena);
        JsonValue result = p.parse_value();
        p.skip_whitespace();
        if (p.opts_.allow_comments) p.skip_comments();
        if (JSON_UNLIKELY(p.ptr_ < p.end_)) {
            p.error("unexpected trailing content", errc::trailing_content);
        }
        return result;
    }

    /// @brief Parse a JSON string (no exceptions, error_code).
    [[nodiscard]] static result<JsonValue> try_parse(
            std::string_view input, const ParseOptions& opts = {}) noexcept {
        try {
            return {parse(input, opts), {}};
        } catch (const ParseError& e) {
            return {JsonValue{}, e.code()};
        } catch (...) {
            return {JsonValue{}, make_error_code(errc::unexpected_character)};
        }
    }

private:
    const char* ptr_;
    const char* end_;
    const char* begin_;
    ParseOptions opts_;
    size_t depth_ = 0;
    size_t max_depth_;
    std::pmr::memory_resource* temp_mr_;  ///< Temporary allocator for parser internals
    MonotonicArena* arena_;               ///< Cached TLS arena pointer (avoids repeated TLS access)
    std::pmr::memory_resource* resource_; ///< Cached pmr resource (arena or new_delete)

    Parser(const char* begin, const char* end,
           const ParseOptions& opts,
           std::pmr::memory_resource* temp_mr,
           MonotonicArena* arena) noexcept
        : ptr_(begin), end_(end), begin_(begin), opts_(opts)
        , max_depth_(opts.max_depth > 0 ? opts.max_depth : YAJSON_MAX_DEPTH)
        , temp_mr_(temp_mr)
        , arena_(arena)
        , resource_(arena_ ? static_cast<std::pmr::memory_resource*>(arena_)
                           : std::pmr::new_delete_resource()) {}

    // ─── Error reporting ──────────────────────────────────────────────────

    [[nodiscard]] SourceLocation current_location() const noexcept {
        SourceLocation loc;
        loc.offset = static_cast<size_t>(ptr_ - begin_);
        for (const char* p = begin_; p < ptr_; ++p) {
            if (*p == '\n') { ++loc.line; loc.column = 1; }
            else { ++loc.column; }
        }
        return loc;
    }

    [[noreturn]] JSON_NOINLINE void error(const std::string& msg,
                                           errc code = errc::unexpected_character) {
        throw ParseError(msg, current_location(), code);
    }

    [[noreturn]] JSON_NOINLINE void error_unexpected_end() {
        throw ParseError("unexpected end of input", current_location(),
                         errc::unexpected_end_of_input);
    }

    [[noreturn]] JSON_NOINLINE void error_unexpected_char() {
        if (ptr_ >= end_) error_unexpected_end();
        throw ParseError(
            std::string("unexpected character '") + *ptr_ + "'",
            current_location(), errc::unexpected_character);
    }

    // ─── Depth tracking ──────────────────────────────────────────────────────

    void push_depth() {
        if (JSON_UNLIKELY(++depth_ > max_depth_)) {
            error("maximum nesting depth exceeded", errc::max_depth_exceeded);
        }
    }

    void pop_depth() noexcept { --depth_; }

    // ─── Whitespace and comments ───────────────────────────────────────

    void skip_whitespace() noexcept {
        // Fast path 0: current char is not whitespace (most common case)
        if (JSON_LIKELY(ptr_ < end_ && static_cast<unsigned char>(*ptr_) > ' ')) {
            return;
        }
        // Fast path 1: single whitespace char (very common in JSON after ':' and ',')
        // Avoids the NOINLINE slow-path call for the most frequent whitespace pattern.
        if (ptr_ + 1 < end_ && static_cast<unsigned char>(ptr_[1]) > ' ') {
            ++ptr_;
            return;
        }
        // Fast path 2: two whitespace chars (e.g. ", " or ":\n")
        if (ptr_ + 2 < end_ && static_cast<unsigned char>(ptr_[2]) > ' ') {
            ptr_ += 2;
            return;
        }
        skip_whitespace_slow();
    }

    JSON_NOINLINE void skip_whitespace_slow() noexcept {
        // Try SIMD first (we already know *ptr_ is whitespace from the caller)
        if (ptr_ + 16 <= end_) {
            ptr_ = simd::skip_whitespace(ptr_, end_);
            return;
        }
        // Scalar fallback for tail bytes
        while (ptr_ < end_) {
            char c = *ptr_;
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ++ptr_;
            else return;
        }
    }

    void skip_comments() {
        while (ptr_ + 1 < end_ && *ptr_ == '/') {
            if (ptr_[1] == '/') {
                // Line comment
                ptr_ += 2;
                while (ptr_ < end_ && *ptr_ != '\n') ++ptr_;
                if (ptr_ < end_) ++ptr_; // skip \n
                skip_whitespace();
            } else if (ptr_[1] == '*') {
                // Block comment
                ptr_ += 2;
                while (ptr_ + 1 < end_) {
                    if (ptr_[0] == '*' && ptr_[1] == '/') {
                        ptr_ += 2;
                        break;
                    }
                    ++ptr_;
                }
                skip_whitespace();
            } else {
                break;
            }
        }
    }

    /// @brief Skip whitespace and (optionally) comments.
    /// Inlined for the common case (no comments): just skip_whitespace().
    /// The opts_.allow_comments check is well-predicted but the extra function
    /// call overhead adds up with millions of JSON values.
    JSON_ALWAYS_INLINE void skip_ws_and_comments() {
        skip_whitespace();
        if (JSON_UNLIKELY(opts_.allow_comments)) {
            skip_comments_and_ws();
        }
    }

    /// @brief Slow path: skip comments then whitespace (only called when comments enabled).
    JSON_NOINLINE void skip_comments_and_ws() {
        skip_comments();
        skip_whitespace();
    }

    // ─── Character reading ────────────────────────────────────────────

    char peek() const noexcept {
        if (JSON_UNLIKELY(ptr_ >= end_)) return '\0';
        return *ptr_;
    }

    char advance() {
        if (JSON_UNLIKELY(ptr_ >= end_)) error_unexpected_end();
        return *ptr_++;
    }

    void expect(char c) {
        if (JSON_LIKELY(ptr_ < end_ && *ptr_ == c)) {
            ++ptr_;
            return;
        }
        if (ptr_ >= end_) error_unexpected_end();
        error(std::string("expected '") + c + "', got '" + *ptr_ + "'");
    }

    /// Template version ensures memcmp sees a compile-time length,
    /// allowing the compiler to lower it to a single word comparison.
    template <size_t N>
    void expect_literal(const char (&literal)[N]) {
        constexpr size_t len = N - 1;  // exclude null terminator
        if (JSON_UNLIKELY(static_cast<size_t>(end_ - ptr_) < len) ||
            JSON_UNLIKELY(std::memcmp(ptr_, literal, len) != 0)) {
            error(std::string("expected '") + literal + "'", errc::invalid_literal);
        }
        ptr_ += len;
    }

    // ─── Value parsing ───────────────────────────────────────────────────────

    JsonValue parse_value() {
        skip_ws_and_comments();
        if (JSON_UNLIKELY(ptr_ >= end_)) error_unexpected_end();

        switch (*ptr_) {
            case '"': return parse_string_value();
            case '\'':
                if (opts_.allow_single_quotes) return parse_string_value_sq();
                error_unexpected_char();
            case '{': return parse_object();
            case '[': return parse_array();
            case 't': return parse_true();
            case 'f': return parse_false();
            case 'n': return parse_null();
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return parse_number();
            case 'N':
                if (opts_.allow_nan_inf) return parse_nan();
                error_unexpected_char();
            case 'I':
                if (opts_.allow_nan_inf) return parse_infinity(false);
                error_unexpected_char();
            default:
                error_unexpected_char();
        }
    }

    JsonValue parse_null() {
        expect_literal("null");
        return JsonValue(nullptr);
    }

    JsonValue parse_true() {
        expect_literal("true");
        return JsonValue(true);
    }

    JsonValue parse_false() {
        expect_literal("false");
        return JsonValue(false);
    }

    JsonValue parse_nan() {
        expect_literal("NaN");
        return JsonValue(std::numeric_limits<double>::quiet_NaN());
    }

    JsonValue parse_infinity(bool negative) {
        expect_literal("Infinity");
        double val = std::numeric_limits<double>::infinity();
        return JsonValue(negative ? -val : val);
    }

    // ─── String parsing (full UTF-8 support) ──────────────────────────────

    /// @brief Parse a double-quoted string and return as std::string.
    /// Fast path: if no escape sequences, return directly from input span
    /// without constructing a pmr::string at all (~95% of JSON strings).
    std::string parse_string() {
        expect('"');
        // Fast path: probe for closing quote with SIMD
        const char* delim = simd::find_string_delimiter(ptr_, end_);
        if (JSON_LIKELY(delim < end_ && *delim == '"')) {
            // No escapes — zero-copy from input buffer
            std::string result(ptr_, static_cast<size_t>(delim - ptr_));
            ptr_ = delim + 1;
            return result;
        }
        // Slow path: has escape sequences, use pmr::string builder
        std::pmr::string buf(temp_mr_);
        if (delim > ptr_) {
            buf.append(ptr_, static_cast<size_t>(delim - ptr_));
            ptr_ = delim;
        }
        parse_string_content_into(buf, '"');
        return std::string(buf.data(), buf.size());
    }

    std::string parse_string_sq() {
        expect('\'');
        std::pmr::string buf(temp_mr_);
        parse_string_content_into(buf, '\'');
        return std::string(buf.data(), buf.size());
    }

    /// @brief Build string content into a pmr::string buffer.
    /// The template-less version avoids code duplication between the
    /// std::string return path (keys) and the zero-copy JsonValue path (values).
    void parse_string_content_into(std::pmr::string& result, char quote) {
        // Note: the memchr pre-scan was removed — the SIMD find_string_delimiter
        // in the main loop already identifies the closing quote, making the
        // pre-scan redundant (double scanning the same bytes). For PMR with
        // monotonic_buffer_resource, geometric growth is cheap (no deallocation).
        for (;;) {
            if (quote == '"') {
                // SIMD-accelerated search for '"' or '\\'
                const char* delim = simd::find_string_delimiter(ptr_, end_);
                if (delim > ptr_) {
                    result.append(ptr_, static_cast<size_t>(delim - ptr_));
                    ptr_ = delim;
                }
            } else {
                // Scalar search for single-quote delimiter — batch copy
                const char* run_start = ptr_;
                while (ptr_ < end_ && *ptr_ != quote && *ptr_ != '\\') ++ptr_;
                if (ptr_ > run_start)
                    result.append(run_start, static_cast<size_t>(ptr_ - run_start));
            }

            if (JSON_UNLIKELY(ptr_ >= end_)) {
                error("unterminated string", errc::unterminated_string);
            }

            char c = *ptr_;
            if (JSON_LIKELY(c == quote)) {
                ++ptr_;
                return;
            }

            // c must be '\\' here: find_string_delimiter stops at '"' or '\\',
            // and the quote case is handled above. No other character is possible.
            ++ptr_;
            parse_escape(result);
        }
    }

    void parse_escape(std::pmr::string& out) {
        if (JSON_UNLIKELY(ptr_ >= end_))
            error("unterminated escape sequence", errc::invalid_escape);
        char c = *ptr_++;
        switch (c) {
            case '"':  out.push_back('"');  return;
            case '\\': out.push_back('\\'); return;
            case '/':  out.push_back('/');  return;
            case 'b':  out.push_back('\b'); return;
            case 'f':  out.push_back('\f'); return;
            case 'n':  out.push_back('\n'); return;
            case 'r':  out.push_back('\r'); return;
            case 't':  out.push_back('\t'); return;
            case '\'':
                if (opts_.allow_single_quotes) { out.push_back('\''); return; }
                error(std::string("invalid escape '\\") + c + "'", errc::invalid_escape);
            case 'u':  parse_unicode_escape(out); return;
            default:
                error(std::string("invalid escape '\\") + c + "'", errc::invalid_escape);
        }
    }

    // 256-byte hex lookup table: 0xFF = invalid, otherwise nibble value.
    // One table lookup + one comparison per nibble instead of 3-way if-else
    // (up to 6 comparisons). Eliminates branch mispredictions on mixed-case hex.
    static constexpr uint8_t kHexTable[256] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, 0x08,0x09,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // '0'-'9'
        0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 'A'-'F'
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 'a'-'f'
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    };

    uint32_t parse_hex4() {
        if (JSON_UNLIKELY(end_ - ptr_ < 4))
            error("incomplete unicode escape", errc::invalid_unicode_escape);
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t nib = kHexTable[static_cast<unsigned char>(ptr_[i])];
            if (JSON_UNLIKELY(nib > 15))
                error("invalid hex digit in unicode escape", errc::invalid_unicode_escape);
            val = (val << 4) | nib;
        }
        ptr_ += 4;
        return val;
    }

    void parse_unicode_escape(std::pmr::string& out) {
        uint32_t cp = parse_hex4();

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (JSON_UNLIKELY(end_ - ptr_ < 2 || ptr_[0] != '\\' || ptr_[1] != 'u')) {
                error("missing low surrogate", errc::invalid_unicode_escape);
            }
            ptr_ += 2;
            uint32_t low = parse_hex4();
            if (JSON_UNLIKELY(low < 0xDC00 || low > 0xDFFF)) {
                error("invalid low surrogate value", errc::invalid_unicode_escape);
            }
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        } else if (JSON_UNLIKELY(cp >= 0xDC00 && cp <= 0xDFFF)) {
            error("unexpected low surrogate", errc::invalid_unicode_escape);
        }

        // Use char-buffer overload of utf8::encode to avoid std::string dependency
        char buf[4];
        unsigned n = utf8::encode(cp, buf);
        out.append(buf, n);
    }

    /// @brief Parse a double-quoted string value with zero-copy arena optimization.
    ///
    /// When an arena is active: builds the string directly in the arena via
    /// pmr::string, then creates a JsonValue pointing to the same arena memory
    /// (zero-copy — no intermediate malloc or memcpy for strings > SSO).
    ///
    /// When no arena: builds via pmr::string in the stack-buffered
    /// monotonic_buffer_resource, then copies to a heap-allocated std::string
    /// (still faster than std::string due to avoided intermediate reallocs).
    JsonValue parse_string_value() {
        expect('"');
        // Fast path: probe for closing quote with SIMD
        const char* delim = simd::find_string_delimiter(ptr_, end_);
        if (JSON_LIKELY(delim < end_ && *delim == '"')) {
            // No escapes — construct JsonValue directly from input span
            std::string_view sv(ptr_, static_cast<size_t>(delim - ptr_));
            ptr_ = delim + 1;
            return JsonValue(sv);
        }
        // Slow path: has escape sequences
        std::pmr::string buf(temp_mr_);
        if (delim > ptr_) {
            buf.append(ptr_, static_cast<size_t>(delim - ptr_));
            ptr_ = delim;
        }
        parse_string_content_into(buf, '"');
        return pmr_string_to_value(buf);
    }

    JsonValue parse_string_value_sq() {
        expect('\'');
        std::pmr::string buf(temp_mr_);
        parse_string_content_into(buf, '\'');
        return pmr_string_to_value(buf);
    }

    /// @brief Convert a parser-internal pmr::string to a JsonValue.
    /// For the arena path with long strings: zero-copy (point to arena data).
    /// For all other cases: construct via string_view (triggers normal init_string).
    JsonValue pmr_string_to_value(const std::pmr::string& s) {
        const size_t len = s.size();
        // Arena zero-copy: when temp_mr_ IS the arena and string > SSO,
        // the pmr::string's data is already in the arena. Create a JsonValue
        // pointing directly to it — no allocation, no copy.
        if (arena_ &&
            temp_mr_ == static_cast<std::pmr::memory_resource*>(arena_) &&
            len > JsonValue::kSsoMax) {
            JsonValue v;
            v.kind_ = Type::String;
            v.sso_len_ = JsonValue::kHeapTag;
            v.set_arena_str(s.data(), static_cast<uint32_t>(len));
            return v;
        }
        // Non-arena or SSO: construct normally
        return JsonValue(std::string_view(s.data(), len));
    }

    // ─── Number parsing (inline accumulation for fast integer path) ───────────

    JsonValue parse_number() {
        const char* start = ptr_;
        bool negative = false;

        if (*ptr_ == '-') {
            negative = true;
            ++ptr_;
            if (JSON_UNLIKELY(ptr_ >= end_))
                error("invalid number", errc::invalid_number);

            // Check for -Infinity
            if (opts_.allow_nan_inf && *ptr_ == 'I') {
                return parse_infinity(true);
            }
        }

        // Hex numbers: 0x or 0X
        if (opts_.allow_hex_numbers && *ptr_ == '0' &&
            ptr_ + 1 < end_ && (ptr_[1] == 'x' || ptr_[1] == 'X')) {
            return parse_hex_number(negative);
        }

        if (JSON_UNLIKELY(static_cast<unsigned>(*ptr_ - '0') > 9u)) {
            error("invalid number", errc::invalid_number);
        }

        // Fast integer path — track digit count incrementally (avoids post-hoc
        // digit counting via 64-bit division which costs ~35-90 cycles/digit).
        uint64_t int_val = 0;
        bool int_overflow = false;
        int int_digits = 0;  // number of integer digits scanned

        if (*ptr_ == '0') {
            ++ptr_;
            // int_digits stays 0 for leading zero
        } else {
            int_val = static_cast<uint64_t>(*ptr_ - '0');
            ++ptr_;
            int_digits = 1;
            // Precomputed constants: avoid 64-bit division on every digit
            constexpr uint64_t kOverflowThreshold = UINT64_MAX / 10;       // 1844674407370955161
            constexpr uint64_t kOverflowLastDigit = UINT64_MAX % 10;       // 5
            while (ptr_ < end_ && static_cast<unsigned>(*ptr_ - '0') <= 9u) {
                uint64_t digit = static_cast<uint64_t>(*ptr_ - '0');
                if (JSON_UNLIKELY(int_val > kOverflowThreshold ||
                                  (int_val == kOverflowThreshold && digit > kOverflowLastDigit))) {
                    int_overflow = true;
                    ++ptr_;
                    while (ptr_ < end_ && static_cast<unsigned>(*ptr_ - '0') <= 9u) ++ptr_;
                    break;
                }
                int_val = int_val * 10 + digit;
                ++ptr_;
                ++int_digits;
            }
        }

        bool is_float = false;

        // ── Float fast path: accumulate mantissa + exponent in one scan ──
        // For numbers with <= 19 significant digits (covers 99%+ of JSON
        // floats), we can reconstruct the double directly from the mantissa
        // and exponent without re-parsing via from_chars/strtod.
        uint64_t mantissa = int_val;
        int32_t frac_digits = 0;  // number of digits after '.'
        int32_t explicit_exp = 0; // exponent from 'e'/'E' part
        bool mantissa_overflow = int_overflow;
        constexpr int kMaxMantissaDigits = 19;  // max digits in uint64_t
        int total_digits = int_digits;  // reuse incrementally tracked count

        if (ptr_ < end_ && *ptr_ == '.') {
            is_float = true;
            ++ptr_;
            if (JSON_UNLIKELY(ptr_ >= end_ || static_cast<unsigned>(*ptr_ - '0') > 9u)) {
                error("expected digit after decimal point", errc::invalid_number);
            }
            // Accumulate fractional digits into mantissa
            while (ptr_ < end_ && static_cast<unsigned>(*ptr_ - '0') <= 9u) {
                uint64_t digit = static_cast<uint64_t>(*ptr_ - '0');
                if (total_digits < kMaxMantissaDigits) {
                    mantissa = mantissa * 10 + digit;
                    ++frac_digits;
                    ++total_digits;
                } else {
                    mantissa_overflow = true;
                }
                ++ptr_;
            }
        }

        if (ptr_ < end_ && (*ptr_ == 'e' || *ptr_ == 'E')) {
            is_float = true;
            ++ptr_;
            bool neg_exp = false;
            if (ptr_ < end_ && (*ptr_ == '+' || *ptr_ == '-')) {
                neg_exp = (*ptr_ == '-');
                ++ptr_;
            }
            if (JSON_UNLIKELY(ptr_ >= end_ || static_cast<unsigned>(*ptr_ - '0') > 9u)) {
                error("expected digit in exponent", errc::invalid_number);
            }
            while (ptr_ < end_ && static_cast<unsigned>(*ptr_ - '0') <= 9u) {
                explicit_exp = explicit_exp * 10 + (*ptr_ - '0');
                if (explicit_exp > 400) explicit_exp = 400; // clamp for safety
                ++ptr_;
            }
            if (neg_exp) explicit_exp = -explicit_exp;
        }

        if (JSON_LIKELY(!is_float && !int_overflow)) {
            if (negative) {
                constexpr uint64_t kMaxNeg =
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
                if (JSON_LIKELY(int_val <= kMaxNeg)) {
                    return JsonValue(-static_cast<int64_t>(int_val));
                }
            } else {
                if (JSON_LIKELY(int_val <= static_cast<uint64_t>(
                                    std::numeric_limits<int64_t>::max()))) {
                    return JsonValue(static_cast<int64_t>(int_val));
                }
                return JsonValue(int_val);
            }
        }

        // ── Inline float reconstruction (avoids re-parsing) ──────────────
        // Works when mantissa fits in uint64_t and exponent is moderate.
        if (JSON_LIKELY(!mantissa_overflow && !int_overflow)) {
            int32_t exp10 = explicit_exp - frac_digits;
            // Common case: exp10 in [-22, +22] — exact powers of 10 in double
            if (exp10 >= -22 && exp10 <= 22) {
                // Table of exact powers of 10 representable in double
                static constexpr double kPow10[] = {
                    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                    1e20, 1e21, 1e22
                };
                double d;
                if (exp10 >= 0) {
                    d = static_cast<double>(mantissa) * kPow10[exp10];
                } else {
                    d = static_cast<double>(mantissa) / kPow10[-exp10];
                }
                if (negative) d = -d;
                return JsonValue(d);
            }
        }

        // Fallback to full-precision parser (from_chars / strtod)
        return parse_float_slow(start);
    }

    JSON_NOINLINE JsonValue parse_hex_number(bool negative) {
        ptr_ += 2; // skip 0x
        if (JSON_UNLIKELY(ptr_ >= end_))
            error("incomplete hex number", errc::invalid_number);

        uint64_t val = 0;
        bool has_digit = false;
        while (ptr_ < end_) {
            char h = *ptr_;
            uint64_t digit;
            if (h >= '0' && h <= '9')      digit = static_cast<uint64_t>(h - '0');
            else if (h >= 'a' && h <= 'f') digit = static_cast<uint64_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') digit = static_cast<uint64_t>(h - 'A' + 10);
            else break;
            has_digit = true;
            val = (val << 4) | digit;
            ++ptr_;
        }
        if (JSON_UNLIKELY(!has_digit))
            error("expected hex digit", errc::invalid_number);

        auto signed_val = static_cast<int64_t>(val);
        return JsonValue(negative ? -signed_val : signed_val);
    }

    JSON_NOINLINE JsonValue parse_float_slow(const char* start) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        double dbl_val = 0.0;
        auto [p, ec] = std::from_chars(start, ptr_, dbl_val);
        if (JSON_LIKELY(ec == std::errc{})) {
            return JsonValue(dbl_val);
        }
        error("invalid number", errc::invalid_number);
#else
        char buf[64];
        size_t len = static_cast<size_t>(ptr_ - start);
        if (JSON_LIKELY(len < sizeof(buf))) {
            std::memcpy(buf, start, len);
            buf[len] = '\0';
            char* end_ptr = nullptr;
            double dbl_val = std::strtod(buf, &end_ptr);
            if (JSON_LIKELY(end_ptr == buf + len)) {
                return JsonValue(dbl_val);
            }
        } else {
            std::string num_str(start, ptr_);
            char* end_ptr = nullptr;
            double dbl_val = std::strtod(num_str.c_str(), &end_ptr);
            if (end_ptr == num_str.c_str() + num_str.size()) {
                return JsonValue(dbl_val);
            }
        }
        error("invalid number", errc::invalid_number);
#endif
    }

    // ─── Array parsing ────────────────────────────────────────────────────

    JsonValue parse_array() {
        ++ptr_;
        push_depth();
        skip_ws_and_comments();

        if (JSON_UNLIKELY(ptr_ >= end_))
            error("unterminated array", errc::unterminated_array);

        if (*ptr_ == ']') {
            ++ptr_;
            pop_depth();
            return JsonValue(Array(resource_), arena_);
        }

        Array arr(resource_);

        // ── Count-ahead heuristic: estimate array size from commas ───────
        // Only at shallow nesting (depth <= 2) AND remaining input > 256 bytes.
        // For small inputs (typical network messages ~100-150 bytes), the
        // count-ahead scan costs more than it saves — vector growth from 4→8
        // elements is cheaper than double-scanning the entire message.
        const size_t remaining = static_cast<size_t>(end_ - ptr_);
        if (depth_ <= 2 && remaining > 256) {
            int est = 1;
            int depth_scan = 0;
            const size_t scan_max = remaining < 512 ? remaining : 512;
            for (size_t i = 0; i < scan_max; ++i) {
                char ch = ptr_[i];
                if (ch == '{' || ch == '[') ++depth_scan;
                else if (ch == '}' || ch == ']') {
                    if (depth_scan == 0) break;
                    --depth_scan;
                }
                else if (ch == ',' && depth_scan == 0) ++est;
                else if (ch == '"') {
                    for (++i; i < scan_max && ptr_[i] != '"'; ++i) {
                        if (ptr_[i] == '\\') ++i;
                    }
                }
            }
            arr.reserve(static_cast<size_t>(est < 8 ? 8 : est));
        } else {
            arr.reserve(8);
        }

        for (;;) {
            arr.push_back(parse_value());
            skip_ws_and_comments();

            if (JSON_UNLIKELY(ptr_ >= end_))
                error("unterminated array", errc::unterminated_array);

            if (*ptr_ == ',') {
                ++ptr_;
                skip_ws_and_comments();
                // Trailing comma
                if (opts_.allow_trailing_commas && ptr_ < end_ && *ptr_ == ']') {
                    ++ptr_;
                    pop_depth();
                    return JsonValue(std::move(arr), arena_);
                }
                continue;
            }
            if (JSON_LIKELY(*ptr_ == ']')) {
                ++ptr_;
                pop_depth();
                return JsonValue(std::move(arr), arena_);
            }
            error("expected ',' or ']' in array");
        }
    }

    // ─── Object parsing ──────────────────────────────────────────────────

    /// @brief Check whether a character is a valid start of an unquoted key.
    static bool is_ident_start(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c == '$';
    }

    static bool is_ident_char(char c) noexcept {
        return is_ident_start(c) || (c >= '0' && c <= '9');
    }

    std::string parse_unquoted_key() {
        const char* start = ptr_;
        if (JSON_UNLIKELY(ptr_ >= end_ || !is_ident_start(*ptr_))) {
            error("expected identifier for unquoted key");
        }
        ++ptr_;
        while (ptr_ < end_ && is_ident_char(*ptr_)) ++ptr_;
        return std::string(start, static_cast<size_t>(ptr_ - start));
    }

    JsonValue parse_object() {
        ++ptr_;
        push_depth();
        skip_ws_and_comments();

        if (JSON_UNLIKELY(ptr_ >= end_))
            error("unterminated object", errc::unterminated_object);

        if (*ptr_ == '}') {
            ++ptr_;
            pop_depth();
            return JsonValue(Object(resource_), arena_);
        }

        Object obj(resource_);

        // ── Count-ahead heuristic: estimate object size from commas ──────
        // Only at shallow nesting (depth <= 2) AND remaining input > 256 bytes.
        // For small inputs (typical network messages ~100-150 bytes), skip
        // the pre-scan — it costs more than letting vector grow naturally.
        const size_t obj_remaining = static_cast<size_t>(end_ - ptr_);
        if (depth_ <= 2 && obj_remaining > 256) {
            int est = 1;
            int depth_scan = 0;
            const size_t scan_max = obj_remaining < 512 ? obj_remaining : 512;
            for (size_t i = 0; i < scan_max; ++i) {
                char ch = ptr_[i];
                if (ch == '{' || ch == '[') ++depth_scan;
                else if (ch == '}' || ch == ']') {
                    if (depth_scan == 0) break;
                    --depth_scan;
                }
                else if (ch == ',' && depth_scan == 0) ++est;
                else if (ch == '"') {
                    for (++i; i < scan_max && ptr_[i] != '"'; ++i) {
                        if (ptr_[i] == '\\') ++i;
                    }
                }
            }
            obj.reserve(static_cast<size_t>(est < 8 ? 8 : est));
        } else {
            obj.reserve(8);
        }

        // For detecting duplicate keys (only when disallowed),
        // use a PMR hash set backed by temp_mr_ for O(1) lookup.
        using seen_set_t = std::pmr::unordered_set<std::string_view,
            detail::StringHash, detail::StringEqual>;
        std::optional<seen_set_t> seen_keys;
        if (JSON_UNLIKELY(!opts_.allow_duplicate_keys)) {
            seen_keys.emplace(0, detail::StringHash{}, detail::StringEqual{}, temp_mr_);
        }

        for (;;) {
            skip_ws_and_comments();

            // Parse key
            std::string key;
            if (ptr_ < end_ && *ptr_ == '"') {
                key = parse_string();
            } else if (opts_.allow_single_quotes && ptr_ < end_ && *ptr_ == '\'') {
                key = parse_string_sq();
            } else if (opts_.allow_unquoted_keys && ptr_ < end_ && is_ident_start(*ptr_)) {
                key = parse_unquoted_key();
            } else {
                error("expected string key in object", errc::unterminated_object);
            }

            skip_ws_and_comments();
            expect(':');

            JsonValue value = parse_value();

            if (JSON_LIKELY(opts_.allow_duplicate_keys)) {
                // ── Fast path: direct emplace_back, NO duplicate checking ──
                // Skip insert() entirely — it does linear scan for <16 entries
                // and hash lookup for >=16. Instead, append directly and defer
                // index building to after the closing '}'. This matches
                // Boost.JSON's approach of batch construction.
                obj.entries.emplace_back(std::move(key), std::move(value));
            } else {
                // Duplicate detection via PMR hash set with string_view.
                obj.entries.emplace_back(std::move(key), std::move(value));
                auto [it, inserted] = seen_keys->emplace(
                    std::string_view(obj.entries.back().first));
                if (JSON_UNLIKELY(!inserted)) {
                    std::string dup_key(obj.entries.back().first);
                    obj.entries.pop_back();
                    error("duplicate key: \"" + dup_key + "\"", errc::duplicate_key);
                }
            }

            skip_ws_and_comments();
            if (JSON_UNLIKELY(ptr_ >= end_))
                error("unterminated object", errc::unterminated_object);

            if (*ptr_ == ',') {
                ++ptr_;
                skip_ws_and_comments();
                if (opts_.allow_trailing_commas && ptr_ < end_ && *ptr_ == '}') {
                    ++ptr_;
                    pop_depth();
                    finalize_object(obj);
                    return JsonValue(std::move(obj), arena_);
                }
                continue;
            }
            if (JSON_LIKELY(*ptr_ == '}')) {
                ++ptr_;
                pop_depth();
                finalize_object(obj);
                return JsonValue(std::move(obj), arena_);
            }
            error("expected ',' or '}' in object");
        }
    }

    /// @brief Finalize a parsed object: build hash index and dedup if needed.
    ///
    /// Called once after the closing '}' instead of per-entry insert().
    /// For large objects (>= kIndexThreshold): builds the hash index in a
    /// single pass. The index naturally maps to the last occurrence of each
    /// key, providing "last-value-wins" semantics.
    /// If duplicates exist (index_.size() < entries.size()), compacts the
    /// entries vector to remove earlier duplicates.
    ///
    /// For small objects (< kIndexThreshold): does a lightweight O(n²)
    /// reverse-dedup (n < 16, at most ~120 comparisons).
    void finalize_object(Object& obj) {
        auto& entries = obj.entries;
        const size_t n = entries.size();

        if (n >= Object::kIndexThreshold) {
            // Build hash index (single pass). (*index_)[key] = i iterates
            // forward, so the last index for each key wins.
            obj.rebuild_index();

            // Check for duplicates: index has fewer entries than the vector.
            if (obj.index_->size() < n) {
                // Compact: keep only entries whose index matches their position.
                size_t write = 0;
                for (size_t i = 0; i < entries.size(); ++i) {
                    auto it = obj.index_->find(std::string_view(entries[i].first));
                    if (it->second == i) {
                        if (write != i) entries[write] = std::move(entries[i]);
                        ++write;
                    }
                }
                entries.resize(write);
                // Re-index after compaction (indices changed).
                obj.rebuild_index();
            }
        } else if (n >= 2) {
            // Small object: O(n²) dedup, last-value-wins (n < 16).
            // For the common case (no duplicates): inner loop finds no matches.
            for (size_t i = 0; i < entries.size(); ) {
                bool has_later_dup = false;
                for (size_t j = i + 1; j < entries.size(); ++j) {
                    if (entries[i].first == entries[j].first) {
                        has_later_dup = true;
                        break;
                    }
                }
                if (JSON_UNLIKELY(has_later_dup)) {
                    entries.erase(entries.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
    }
};

} // namespace detail

// ─── Public parsing API ─────────────────────────────────────────────────────

/// @brief Parse JSON from a string (with exceptions).
[[nodiscard]] inline JsonValue parse(std::string_view input) {
    return detail::Parser::parse(input);
}

/// @brief Parse JSON with options (with exceptions).
[[nodiscard]] inline JsonValue parse(std::string_view input,
                                     const ParseOptions& opts) {
    return detail::Parser::parse(input, opts);
}

/// @brief Parse JSON (no exceptions, returns result with error_code).
[[nodiscard]] inline result<JsonValue> try_parse(std::string_view input) {
    return detail::Parser::try_parse(input);
}

/// @brief Parse JSON with options (no exceptions).
[[nodiscard]] inline result<JsonValue> try_parse(std::string_view input,
                                                  const ParseOptions& opts) {
    return detail::Parser::try_parse(input, opts);
}

} // namespace yajson
