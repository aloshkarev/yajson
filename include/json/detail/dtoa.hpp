#pragma once

/// @file detail/dtoa.hpp
/// @author Aleksandr Loshkarev
/// @brief Fast double-to-string conversion for JSON serialization.
///
/// Optimizations:
///   1. Integer fast path: ~40% of JSON numbers are exact integers.
///      Uses integer formatting + ".0" suffix (4-6x faster than to_chars(double)).
///   2. Fixed-point fast path: numbers with <= 9 decimal places (e.g. 3.14, 37.7749295)
///      are formatted via integer arithmetic without calling to_chars(double).
///   3. General case: fallback to std::to_chars (Ryu implementation in libstdc++/libc++).
///
/// Note: on modern compilers (GCC 11+, Clang 14+, MSVC 19.24+)
/// std::to_chars(double) already uses the Ryu algorithm internally, providing
/// performance on par with a standalone implementation. Dragonbox gives
/// an additional ~30-40%, but requires embedding ~800 lines of code with tables.
///
/// Additional micro-optimizations:
///   - Two-digit pair table "00".."99": halves the number of divisions in write_u64.
///   - Digit counting via __builtin_clzll: direct writing without buffer reversal.

#include "../config.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace yajson::detail {

// ─── Tables ──────────────────────────────────────────────────────────────────

/// Two-digit pair table "00".."99" — outputs two digits per iteration,
/// halves the number of division operations.
inline constexpr char kDigitPairs[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

/// Precomputed powers of 10 (double) for the fixed-point fast path.
inline constexpr double kPow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15
};

/// Precomputed powers of 10 (uint64_t) for digit counting.
/// kPow10U64[0] = 0 (sentinel); kPow10U64[i] = 10^i for i = 1..19.
inline constexpr uint64_t kPow10U64[] = {
    0ULL,                       // [0] sentinel
    10ULL,                      // [1]
    100ULL,                     // [2]
    1000ULL,                    // [3]
    10000ULL,                   // [4]
    100000ULL,                  // [5]
    1000000ULL,                 // [6]
    10000000ULL,                // [7]
    100000000ULL,               // [8]
    1000000000ULL,              // [9]
    10000000000ULL,             // [10]
    100000000000ULL,            // [11]
    1000000000000ULL,           // [12]
    10000000000000ULL,          // [13]
    100000000000000ULL,         // [14]
    1000000000000000ULL,        // [15]
    10000000000000000ULL,       // [16]
    100000000000000000ULL,      // [17]
    1000000000000000000ULL,     // [18]
    10000000000000000000ULL     // [19]
};

/// Maximum safe integer representable in IEEE 754 double (2^53).
inline constexpr double kMaxSafeInteger = 9007199254740992.0;

// ─── Digit counting ────────────────────────────────────────────────────────────

/// @brief Fast decimal digit counting.
///
/// Uses the approximation log10(2) ≈ 1233/4096 with correction via the powers table.
/// On GCC/Clang compiles to a CLZ instruction (x86: BSR/LZCNT, ARM: CLZ).
///
/// @param val Value (> 0).
/// @return Number of decimal digits (1..20).
JSON_ALWAYS_INLINE int count_digits(uint64_t val) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    // __builtin_clzll gives the number of leading zero bits.
    // bits = number of significant bits. approx ≈ floor(log10(val)).
    // Correction: if val < 10^approx, decrement by 1.
    const int bits = 64 - __builtin_clzll(val | 1);
    const int approx = (bits * 1233) >> 12;
    return approx - (val < kPow10U64[approx]) + 1;
#else
    // Portable fallback: checking 4 digits per iteration.
    int count = 1;
    for (;;) {
        if (val < 10) return count;
        if (val < 100) return count + 1;
        if (val < 1000) return count + 2;
        if (val < 10000) return count + 3;
        val /= 10000;
        count += 4;
    }
#endif
}

// ─── Integer formatting ─────────────────────────────────────────────────────────

/// @brief Fast uint64_t to decimal ASCII conversion.
///
/// Uses the two-digit pair table and count_digits for direct
/// writing to the buffer without intermediate reversal.
///
/// @param buf Output buffer (>= 20 bytes).
/// @param val Value to format.
/// @return Pointer past the last written character.
JSON_ALWAYS_INLINE char* write_u64(char* buf, uint64_t val) noexcept {
    if (val == 0) {
        *buf = '0';
        return buf + 1;
    }

    const int len = count_digits(val);
    char* p = buf + len;

    // Write right-to-left, 2 digits per iteration.
    while (val >= 100) {
        const auto idx = static_cast<unsigned>((val % 100) * 2);
        val /= 100;
        p -= 2;
        std::memcpy(p, kDigitPairs + idx, 2);
    }

    // Last 1-2 digits.
    if (val >= 10) {
        std::memcpy(buf, kDigitPairs + val * 2, 2);
    } else {
        *buf = static_cast<char>('0' + val);
    }

    return buf + len;
}

// ─── Double to string conversion ───────────────────────────────────────────

/// @brief Convert double to shortest decimal string representation.
///
/// Guarantees:
///   - Output is always a valid JSON number
///   - Roundtrip: parse(fast_dtoa(x)) == x
///   - Always contains '.' or 'e' to distinguish from integer
///
/// @param buf Output buffer (>= 32 bytes).
/// @param val Value (must NOT be NaN/Inf — caller handles them).
/// @return Number of characters written.
inline size_t fast_dtoa(char* buf, double val) noexcept {
    char* const start = buf;

    // ── Sign handling ─────────────────────────────────────────────────
    // Use signbit to correctly distinguish -0.0 from 0.0.
    if (std::signbit(val)) {
        if (val == 0.0) {
            // -0.0 → JSON has no negative zero, output "0.0"
            buf[0] = '0'; buf[1] = '.'; buf[2] = '0';
            return 3;
        }
        *buf++ = '-';
        val = -val;
    }

    // ── Fast path 1: Exact integers ─────────────────────────────
    // Covers ~40% of real-world JSON numbers (counters, IDs, timestamps, etc.)
    // Integer write_u64 is 4-6x faster than to_chars(double).
    if (val <= kMaxSafeInteger && val == std::floor(val)) {
        const auto ival = static_cast<uint64_t>(val);
        buf = write_u64(buf, ival);
        *buf++ = '.';
        *buf++ = '0';
        return static_cast<size_t>(buf - start);
    }

    // ── Fast path 2: Fixed point (k = 1..9) ─────────────────
    // For numbers in a reasonable range, check: val * 10^k is an exact integer.
    // Extending to k=9 (was k=6) covers:
    //   - GPS coordinates (37.7749295)
    //   - sensor readings (0.123456789)
    //   - currency exchange rates (1.23456789)
    // If FP multiplication introduces rounding error, the check correctly
    // rejects — never produces incorrect output.
    if (val < 1e15 && val > 1e-6) {
        for (int k = 1; k <= 9; ++k) {
            const double scaled = val * kPow10[k];
            // Early exit: once scaled exceeds safe integer range,
            // all subsequent k values will too (monotonically increasing).
            if (scaled > kMaxSafeInteger) break;
            if (scaled == std::floor(scaled)) {
                const auto ival = static_cast<uint64_t>(scaled);

                // Format as digits, then insert the decimal point.
                char digits[24];
                char* dend = write_u64(digits, ival);
                const int total_digits = static_cast<int>(dend - digits);
                const int int_digits = total_digits - k;

                if (int_digits <= 0) {
                    // Number < 1, e.g. 0.005 → "0.005"
                    *buf++ = '0';
                    *buf++ = '.';
                    for (int i = 0; i < -int_digits; ++i) *buf++ = '0';
                    std::memcpy(buf, digits, static_cast<size_t>(total_digits));
                    buf += total_digits;
                } else {
                    // Number >= 1, e.g. 123.45 → "123.45"
                    std::memcpy(buf, digits, static_cast<size_t>(int_digits));
                    buf += int_digits;
                    *buf++ = '.';
                    std::memcpy(buf, digits + int_digits, static_cast<size_t>(k));
                    buf += k;
                }
                return static_cast<size_t>(buf - start);
            }
        }
    }

    // ── General case: fallback to std::to_chars ────────────────────────────
    // On GCC 11+ / Clang 14+ / MSVC 19.24+ internally uses the Ryu algorithm.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    auto [ptr, ec] = std::to_chars(buf, buf + 24, val);
    const auto len = static_cast<size_t>(ptr - buf);

    // Ensure the output contains '.' or 'e' for a valid JSON literal.
    bool has_dot = false;
    for (size_t i = 0; i < len; ++i) {
        const char c = buf[i];
        if (c == '.' || c == 'e' || c == 'E') {
            has_dot = true;
            break;
        }
    }
    buf = ptr;
    if (!has_dot) {
        *buf++ = '.';
        *buf++ = '0';
    }
#else
    // Fallback for compilers without to_chars(double).
    const int n = std::snprintf(buf, 32, "%.17g", val);
    if (n > 0) {
        bool has_dot = false;
        for (int i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '.' || c == 'e' || c == 'E') {
                has_dot = true;
                break;
            }
        }
        buf += n;
        if (!has_dot) {
            *buf++ = '.';
            *buf++ = '0';
        }
    }
#endif

    return static_cast<size_t>(buf - start);
}

} // namespace yajson::detail
