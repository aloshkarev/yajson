#pragma once

/// @file hash.hpp
/// @author Aleksandr Loshkarev
/// @brief Fast string hashing for JSON object key lookup.
///
/// Uses wyhash-inspired mixing for runtime hashing — 2-4x faster than FNV-1a
/// on short strings (typical JSON keys: 4-20 bytes). Processes 8 bytes per
/// iteration with a single multiply, falling back to sub-word loads for
/// shorter keys. No external dependencies.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace yajson::detail {

/// @brief wyhash-inspired string hasher — optimized for short keys.
///
/// Design rationale:
///   - JSON keys are typically 4-20 bytes (e.g. "id", "name", "timestamp").
///   - FNV-1a processes 1 byte/iteration with a data dependency chain.
///   - This hasher processes 8 bytes/iteration (or the full key in 1-2 loads
///     for short keys), achieving 2-4x speedup on typical JSON workloads.
///   - Maintains `is_transparent` for heterogeneous lookup in C++17.
struct StringHash {
    using is_transparent = void;  // Heterogeneous lookup

    // ─── Core mixing function ─────────────────────────────────────────────

    static size_t hash(const char* data, size_t len) noexcept {
        // wyhash-inspired: fast multiply-mix for short keys.
        // Constants from wyhash v4 (public domain, Wang Yi).
        constexpr uint64_t kSeed  = 0xa0761d6478bd642fULL;
        constexpr uint64_t kSeed2 = 0xe7037ed1a0b428dbULL;

        uint64_t h = kSeed ^ (len * kSeed2);

        if (len <= 8) {
            // Short key (most JSON keys): 1-2 loads cover the entire string.
            uint64_t a = 0, b = 0;
            if (len >= 4) {
                // 4..8 bytes: read first 4 and last 4 (may overlap, that's fine)
                std::memcpy(&a, data, 4);
                std::memcpy(&b, data + len - 4, 4);
            } else if (len > 0) {
                // 1..3 bytes: pack into a single value
                a = static_cast<uint64_t>(static_cast<unsigned char>(data[0])) << 16
                  | static_cast<uint64_t>(static_cast<unsigned char>(data[len >> 1])) << 8
                  | static_cast<uint64_t>(static_cast<unsigned char>(data[len - 1]));
            }
            h ^= a;
            h *= kSeed2;
            h ^= b;
            h *= kSeed;
        } else if (len <= 16) {
            // 9..16 bytes: two 8-byte loads
            uint64_t a, b;
            std::memcpy(&a, data, 8);
            std::memcpy(&b, data + len - 8, 8);
            h ^= a;
            h *= kSeed2;
            h ^= b;
            h *= kSeed;
        } else {
            // >16 bytes: process 16 bytes per iteration
            const char* p = data;
            const char* const stop = data + len - 16;
            while (p <= stop) {
                uint64_t a, b;
                std::memcpy(&a, p, 8);
                std::memcpy(&b, p + 8, 8);
                h ^= a;
                h *= kSeed2;
                h ^= b;
                h *= kSeed;
                p += 16;
            }
            // Final 1-16 bytes (overlapping with last iteration)
            uint64_t a, b;
            std::memcpy(&a, data + len - 16, 8);
            std::memcpy(&b, data + len - 8, 8);
            h ^= a;
            h *= kSeed2;
            h ^= b;
            h *= kSeed;
        }

        // Final avalanche
        h ^= h >> 32;
        h *= kSeed;
        h ^= h >> 29;
        return static_cast<size_t>(h);
    }

    // ─── Operator overloads for transparent hashing ───────────────────────

    size_t operator()(std::string_view sv) const noexcept {
        return hash(sv.data(), sv.size());
    }

    size_t operator()(const std::string& s) const noexcept {
        return hash(s.data(), s.size());
    }

    size_t operator()(const char* s) const noexcept {
        return hash(s, std::strlen(s));  // SIMD-optimized strlen in glibc/musl
    }
};

/// @brief Transparent comparator for heterogeneous lookup.
struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

} // namespace yajson::detail
