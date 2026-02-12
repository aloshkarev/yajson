#pragma once

/// @file simd.hpp
/// @author Aleksandr Loshkarev
/// @brief SIMD-accelerated utilities for JSON parsing and serialization.
///
/// Supported platforms:
///   - x86_64: SSE2 (baseline), AVX2 (32 bytes/iteration), SSE4.2 (optional)
///   - ARM/AArch64: NEON 16 bytes/iteration, AArch64 2×16 = 32 bytes/iteration
/// Falls back to scalar implementation when SIMD is unavailable.

#include <cstddef>
#include <cstdint>

// ─── Detection of available SIMD extensions ──────────────────────────────────
#if defined(YAJSON_SIMD_ENABLED)
    #if defined(__AVX2__)
        #define YAJSON_AVX2 1
        #include <immintrin.h>
    #elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        #define YAJSON_SSE2 1
        #include <emmintrin.h>
    #endif
    #if defined(__SSE4_2__)
        #define YAJSON_SSE42 1
        #include <nmmintrin.h>
    #endif
    #if defined(__ARM_NEON) || defined(__ARM_NEON__)
        #define YAJSON_NEON 1
        #include <arm_neon.h>
        // AArch64 has 32 NEON registers — we can process 2×16 bytes in parallel
        #if defined(__aarch64__) || defined(_M_ARM64)
            #define YAJSON_NEON_64 1
        #endif
    #endif
#endif

namespace yajson::detail::simd {

// ─── Portable bit-scan helpers ───────────────────────────────────────────────
namespace {

inline int ctz32(uint32_t v) noexcept {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, v);
    return static_cast<int>(idx);
#else
    return __builtin_ctz(v);
#endif
}

// ─── NEON movemask emulation ─────────────────────────────────────────────────
// Converts a 16-byte NEON comparison result (each byte = 0x00 or 0xFF) into
// a 16-bit bitmask (1 bit per byte), equivalent to x86 _mm_movemask_epi8.
// Uses vpadd_u8 horizontal adds — efficient on both ARMv7 and AArch64.

#if defined(YAJSON_NEON)

/// @brief Convert 128-bit NEON comparison result to 16-bit bitmask.
/// Each byte of @p v must be 0x00 or 0xFF. Returns a uint16_t where bit N
/// corresponds to byte N of @p v.
inline uint16_t neon_movemask(uint8x16_t v) noexcept {
    // Mask each byte to its positional bit (bit 0..7 in low half, 0..7 in high)
    static const uint8_t kBitMaskData[16] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
    };
    const uint8x16_t bit_mask = vld1q_u8(kBitMaskData);
    uint8x16_t masked = vandq_u8(v, bit_mask);
    // Horizontal add: 16 bytes → 8 bytes → 4 → 2 (each half collapses to 1 byte)
    uint8x8_t paired = vpadd_u8(vget_low_u8(masked), vget_high_u8(masked));
    paired = vpadd_u8(paired, paired);
    paired = vpadd_u8(paired, paired);
    return vget_lane_u16(vreinterpret_u16_u8(paired), 0);
}

#endif // YAJSON_NEON

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
//  skip_whitespace — find the first non-whitespace character
// ═════════════════════════════════════════════════════════════════════════════

inline const char* skip_whitespace(const char* ptr, const char* end) noexcept {
#if defined(YAJSON_AVX2)
    // AVX2: process 32 bytes per iteration (Haswell+, ~2x SSE2 throughput)
    const __m256i ws_space = _mm256_set1_epi8(' ');
    const __m256i ws_tab   = _mm256_set1_epi8('\t');
    const __m256i ws_nl    = _mm256_set1_epi8('\n');
    const __m256i ws_cr    = _mm256_set1_epi8('\r');

    while (ptr + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
        __m256i cmp = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, ws_space),
                            _mm256_cmpeq_epi8(chunk, ws_tab)),
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, ws_nl),
                            _mm256_cmpeq_epi8(chunk, ws_cr)));
        uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(cmp));
        if (mask == 0xFFFFFFFFu) {
            ptr += 32;
            continue;
        }
        uint32_t non_ws = ~mask;
        return ptr + ctz32(non_ws);
    }
    // Handle remaining 16..31 bytes with SSE2 (AVX2 implies SSE2)
    {
        const __m128i ws_space_128 = _mm_set1_epi8(' ');
        const __m128i ws_tab_128   = _mm_set1_epi8('\t');
        const __m128i ws_nl_128    = _mm_set1_epi8('\n');
        const __m128i ws_cr_128    = _mm_set1_epi8('\r');
        while (ptr + 16 <= end) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
            __m128i cmp = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(chunk, ws_space_128),
                             _mm_cmpeq_epi8(chunk, ws_tab_128)),
                _mm_or_si128(_mm_cmpeq_epi8(chunk, ws_nl_128),
                             _mm_cmpeq_epi8(chunk, ws_cr_128)));
            int mask16 = _mm_movemask_epi8(cmp);
            if (mask16 == 0xFFFF) { ptr += 16; continue; }
            uint32_t non_ws16 = static_cast<uint32_t>(~mask16) & 0xFFFF;
            return ptr + ctz32(non_ws16);
        }
    }

#elif defined(YAJSON_SSE2)
    // SSE2: process 16 bytes per iteration
    const __m128i ws_space = _mm_set1_epi8(' ');
    const __m128i ws_tab   = _mm_set1_epi8('\t');
    const __m128i ws_nl    = _mm_set1_epi8('\n');
    const __m128i ws_cr    = _mm_set1_epi8('\r');

    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
        __m128i cmp = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(chunk, ws_space),
                         _mm_cmpeq_epi8(chunk, ws_tab)),
            _mm_or_si128(_mm_cmpeq_epi8(chunk, ws_nl),
                         _mm_cmpeq_epi8(chunk, ws_cr)));
        int mask = _mm_movemask_epi8(cmp);
        if (mask == 0xFFFF) { ptr += 16; continue; }
        uint32_t non_ws = static_cast<uint32_t>(~mask) & 0xFFFF;
        return ptr + ctz32(non_ws);
    }

#elif defined(YAJSON_NEON)
    const uint8x16_t ws_space = vdupq_n_u8(' ');
    const uint8x16_t ws_tab   = vdupq_n_u8('\t');
    const uint8x16_t ws_nl    = vdupq_n_u8('\n');
    const uint8x16_t ws_cr    = vdupq_n_u8('\r');

#if defined(YAJSON_NEON_64)
    // AArch64: process 2×16 = 32 bytes per iteration (32 NEON registers available)
    while (ptr + 32 <= end) {
        uint8x16_t chunk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint8x16_t chunk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr + 16));
        uint8x16_t cmp0 = vorrq_u8(
            vorrq_u8(vceqq_u8(chunk0, ws_space), vceqq_u8(chunk0, ws_tab)),
            vorrq_u8(vceqq_u8(chunk0, ws_nl),    vceqq_u8(chunk0, ws_cr)));
        uint8x16_t cmp1 = vorrq_u8(
            vorrq_u8(vceqq_u8(chunk1, ws_space), vceqq_u8(chunk1, ws_tab)),
            vorrq_u8(vceqq_u8(chunk1, ws_nl),    vceqq_u8(chunk1, ws_cr)));
        uint16_t mask0 = neon_movemask(cmp0);
        if (mask0 != 0xFFFF) {
            uint16_t non_ws = static_cast<uint16_t>(~mask0);
            return ptr + ctz32(non_ws);
        }
        uint16_t mask1 = neon_movemask(cmp1);
        if (mask1 != 0xFFFF) {
            uint16_t non_ws = static_cast<uint16_t>(~mask1);
            return ptr + 16 + ctz32(non_ws);
        }
        ptr += 32;
    }
#endif // YAJSON_NEON_64

    // 16-byte tail (or primary loop on ARMv7)
    while (ptr + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint8x16_t cmp = vorrq_u8(
            vorrq_u8(vceqq_u8(chunk, ws_space), vceqq_u8(chunk, ws_tab)),
            vorrq_u8(vceqq_u8(chunk, ws_nl),    vceqq_u8(chunk, ws_cr)));
        uint16_t mask = neon_movemask(cmp);
        if (mask == 0xFFFF) { ptr += 16; continue; }
        uint16_t non_ws = static_cast<uint16_t>(~mask);
        return ptr + ctz32(non_ws);
    }
#endif

    // Scalar fallback for remaining bytes
    while (ptr < end) {
        char c = *ptr;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return ptr;
        ++ptr;
    }
    return ptr;
}

// ═════════════════════════════════════════════════════════════════════════════
//  find_string_delimiter — find closing quote or backslash
// ═════════════════════════════════════════════════════════════════════════════

inline const char* find_string_delimiter(const char* ptr,
                                         const char* end) noexcept {
#if defined(YAJSON_AVX2)
    const __m256i q_quote  = _mm256_set1_epi8('"');
    const __m256i q_bslash = _mm256_set1_epi8('\\');

    while (ptr + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
        __m256i cmp = _mm256_or_si256(
            _mm256_cmpeq_epi8(chunk, q_quote),
            _mm256_cmpeq_epi8(chunk, q_bslash));
        uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(cmp));
        if (mask != 0) return ptr + ctz32(mask);
        ptr += 32;
    }
    // Tail: SSE2 for remaining 16..31 bytes
    {
        const __m128i q_quote_128  = _mm_set1_epi8('"');
        const __m128i q_bslash_128 = _mm_set1_epi8('\\');
        while (ptr + 16 <= end) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
            __m128i cmp = _mm_or_si128(
                _mm_cmpeq_epi8(chunk, q_quote_128),
                _mm_cmpeq_epi8(chunk, q_bslash_128));
            int mask16 = _mm_movemask_epi8(cmp);
            if (mask16 != 0) return ptr + ctz32(static_cast<uint32_t>(mask16));
            ptr += 16;
        }
    }

#elif defined(YAJSON_SSE2)
    const __m128i q_quote  = _mm_set1_epi8('"');
    const __m128i q_bslash = _mm_set1_epi8('\\');

    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
        __m128i cmp = _mm_or_si128(
            _mm_cmpeq_epi8(chunk, q_quote),
            _mm_cmpeq_epi8(chunk, q_bslash));
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0) return ptr + ctz32(static_cast<uint32_t>(mask));
        ptr += 16;
    }

#elif defined(YAJSON_NEON)
    const uint8x16_t q_quote  = vdupq_n_u8('"');
    const uint8x16_t q_bslash = vdupq_n_u8('\\');

#if defined(YAJSON_NEON_64)
    // AArch64: process 2×16 = 32 bytes per iteration
    while (ptr + 32 <= end) {
        uint8x16_t chunk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint8x16_t chunk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr + 16));
        uint8x16_t cmp0 = vorrq_u8(vceqq_u8(chunk0, q_quote),
                                    vceqq_u8(chunk0, q_bslash));
        uint8x16_t cmp1 = vorrq_u8(vceqq_u8(chunk1, q_quote),
                                    vceqq_u8(chunk1, q_bslash));
        uint16_t mask0 = neon_movemask(cmp0);
        if (mask0 != 0) return ptr + ctz32(mask0);
        uint16_t mask1 = neon_movemask(cmp1);
        if (mask1 != 0) return ptr + 16 + ctz32(mask1);
        ptr += 32;
    }
#endif // YAJSON_NEON_64

    // 16-byte tail (or primary loop on ARMv7)
    while (ptr + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint8x16_t cmp = vorrq_u8(vceqq_u8(chunk, q_quote),
                                   vceqq_u8(chunk, q_bslash));
        uint16_t mask = neon_movemask(cmp);
        if (mask != 0) return ptr + ctz32(mask);
        ptr += 16;
    }
#endif

    // Scalar fallback
    while (ptr < end) {
        if (*ptr == '"' || *ptr == '\\') return ptr;
        ++ptr;
    }
    return ptr;
}

// ═════════════════════════════════════════════════════════════════════════════
//  find_needs_escape — templated on EnsureAscii for branch elimination
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Find the first byte requiring JSON escaping.
/// Templated on EnsureAscii to eliminate the runtime branch from the SIMD loop.
/// Escaping required for: control chars (0x00-0x1F), '"' (0x22), '\\' (0x5C).
/// When EnsureAscii=true, also flag bytes >= 0x80.
template <bool EnsureAscii>
inline const char* find_needs_escape(const char* ptr, const char* end) noexcept {
#if defined(YAJSON_AVX2)
    const __m256i q_quote  = _mm256_set1_epi8('"');
    const __m256i q_bslash = _mm256_set1_epi8('\\');
    const __m256i bias     = _mm256_set1_epi8(static_cast<char>(0x80u));
    const __m256i thresh   = _mm256_set1_epi8(static_cast<char>(0x80u + 0x20u));

    while (ptr + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
        __m256i biased = _mm256_xor_si256(chunk, bias);
        __m256i ctrl = _mm256_cmpgt_epi8(thresh, biased);  // unsigned c < 0x20
        __m256i special = _mm256_or_si256(
            _mm256_cmpeq_epi8(chunk, q_quote),
            _mm256_cmpeq_epi8(chunk, q_bslash));
        __m256i needs = _mm256_or_si256(ctrl, special);
        if constexpr (EnsureAscii) {
            __m256i hi = _mm256_cmpgt_epi8(_mm256_setzero_si256(), chunk); // signed < 0 ⟹ byte ≥ 0x80
            needs = _mm256_or_si256(needs, hi);
        }
        uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(needs));
        if (mask != 0) return ptr + ctz32(mask);
        ptr += 32;
    }
    // Tail: SSE2
    {
        const __m128i q_quote_128  = _mm_set1_epi8('"');
        const __m128i q_bslash_128 = _mm_set1_epi8('\\');
        const __m128i bias_128     = _mm_set1_epi8(static_cast<char>(0x80u));
        const __m128i thresh_128   = _mm_set1_epi8(static_cast<char>(0x80u + 0x20u));
        while (ptr + 16 <= end) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
            __m128i biased = _mm_xor_si128(chunk, bias_128);
            __m128i ctrl = _mm_cmplt_epi8(biased, thresh_128);
            __m128i special = _mm_or_si128(
                _mm_cmpeq_epi8(chunk, q_quote_128),
                _mm_cmpeq_epi8(chunk, q_bslash_128));
            __m128i needs = _mm_or_si128(ctrl, special);
            if constexpr (EnsureAscii) {
                __m128i hi = _mm_cmplt_epi8(chunk, _mm_setzero_si128());
                needs = _mm_or_si128(needs, hi);
            }
            int mask16 = _mm_movemask_epi8(needs);
            if (mask16 != 0) return ptr + ctz32(static_cast<uint32_t>(mask16));
            ptr += 16;
        }
    }

#elif defined(YAJSON_SSE2)
    const __m128i q_quote  = _mm_set1_epi8('"');
    const __m128i q_bslash = _mm_set1_epi8('\\');
    const __m128i bias     = _mm_set1_epi8(static_cast<char>(0x80u));
    const __m128i thresh   = _mm_set1_epi8(static_cast<char>(0x80u + 0x20u));

    while (ptr + 16 <= end) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
        __m128i biased = _mm_xor_si128(chunk, bias);
        __m128i ctrl = _mm_cmplt_epi8(biased, thresh);
        __m128i special = _mm_or_si128(
            _mm_cmpeq_epi8(chunk, q_quote),
            _mm_cmpeq_epi8(chunk, q_bslash));
        __m128i needs = _mm_or_si128(ctrl, special);
        if constexpr (EnsureAscii) {
            __m128i hi = _mm_cmplt_epi8(chunk, _mm_setzero_si128());
            needs = _mm_or_si128(needs, hi);
        }
        int mask = _mm_movemask_epi8(needs);
        if (mask != 0) return ptr + ctz32(static_cast<uint32_t>(mask));
        ptr += 16;
    }

#elif defined(YAJSON_NEON)
    const uint8x16_t q_quote  = vdupq_n_u8('"');
    const uint8x16_t q_bslash = vdupq_n_u8('\\');
    const uint8x16_t ctrl_max = vdupq_n_u8(0x1F);
    [[maybe_unused]] const uint8x16_t hi_min = vdupq_n_u8(0x80);

    // Helper lambda: compute needs-escape mask for a single 16-byte chunk
    auto compute_needs = [&](uint8x16_t chunk) -> uint8x16_t {
        uint8x16_t ctrl = vcleq_u8(chunk, ctrl_max);
        uint8x16_t special = vorrq_u8(vceqq_u8(chunk, q_quote),
                                       vceqq_u8(chunk, q_bslash));
        uint8x16_t needs = vorrq_u8(ctrl, special);
        if constexpr (EnsureAscii) {
            uint8x16_t hi = vcgeq_u8(chunk, hi_min);
            needs = vorrq_u8(needs, hi);
        }
        return needs;
    };

#if defined(YAJSON_NEON_64)
    // AArch64: process 2×16 = 32 bytes per iteration
    while (ptr + 32 <= end) {
        uint8x16_t chunk0 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint8x16_t chunk1 = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr + 16));
        uint16_t mask0 = neon_movemask(compute_needs(chunk0));
        if (mask0 != 0) return ptr + ctz32(mask0);
        uint16_t mask1 = neon_movemask(compute_needs(chunk1));
        if (mask1 != 0) return ptr + 16 + ctz32(mask1);
        ptr += 32;
    }
#endif // YAJSON_NEON_64

    // 16-byte tail (or primary loop on ARMv7)
    while (ptr + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(ptr));
        uint16_t mask = neon_movemask(compute_needs(chunk));
        if (mask != 0) return ptr + ctz32(mask);
        ptr += 16;
    }
#endif

    // Scalar fallback
    while (ptr < end) {
        auto c = static_cast<unsigned char>(*ptr);
        if (c < 0x20 || c == '"' || c == '\\') return ptr;
        if constexpr (EnsureAscii) {
            if (c >= 0x80) return ptr;
        }
        ++ptr;
    }
    return ptr;
}

/// @brief Non-templated wrapper for backward compatibility / runtime dispatch.
inline const char* find_needs_escape(const char* ptr, const char* end,
                                     bool ensure_ascii) noexcept {
    if (ensure_ascii)
        return find_needs_escape<true>(ptr, end);
    else
        return find_needs_escape<false>(ptr, end);
}

} // namespace yajson::detail::simd
