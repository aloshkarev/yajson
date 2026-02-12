#pragma once

/// @file parse_options.hpp
/// @author Aleksandr Loshkarev
/// @brief Non-standard JSON parser extension options.
///
/// Available extensions:
///   - C/C++ style comments (// and /* */)
///   - Trailing commas in arrays and objects
///   - Single-quoted strings
///   - Unquoted object keys (identifiers)
///   - NaN and Infinity literals
///   - Hexadecimal integers (0xFF)
///   - Control characters in strings

#include <cstddef>

namespace yajson {

/// @brief Parser configuration for standard and non-standard JSON.
struct ParseOptions {
    // ─── Non-standard extensions (all disabled by default) ──────────────

    /// Allow C/C++ comments: // line, /* block */
    bool allow_comments         = false;

    /// Allow trailing commas: [1,2,3,] and {"a":1,"b":2,}
    bool allow_trailing_commas  = false;

    /// Allow single-quoted strings: 'hello'
    bool allow_single_quotes    = false;

    /// Allow unquoted object keys: {key: "value"}
    bool allow_unquoted_keys    = false;

    /// Allow NaN, Infinity, -Infinity as numeric literals
    bool allow_nan_inf          = false;

    /// Allow hexadecimal numbers: 0xFF
    bool allow_hex_numbers      = false;

    /// Allow control characters (< 0x20) in strings without escaping
    bool allow_control_chars    = false;

    /// Allow duplicate keys in objects (last value wins)
    bool allow_duplicate_keys   = true;

    // ─── Limits ──────────────────────────────────────────────────────────

    /// Maximum nesting depth (0 = use the value from config.hpp)
    size_t max_depth = 0;

    // ─── Factory methods ─────────────────────────────────────────────────

    /// Strict JSON (RFC 8259) — all extensions disabled.
    static constexpr ParseOptions strict() noexcept {
        return {};
    }

    /// Lenient mode — popular extensions enabled.
    static constexpr ParseOptions lenient() noexcept {
        ParseOptions opts;
        opts.allow_comments        = true;
        opts.allow_trailing_commas = true;
        opts.allow_single_quotes   = true;
        opts.allow_unquoted_keys   = true;
        opts.allow_nan_inf         = true;
        return opts;
    }

    /// JSON5 mode — maximum compatibility.
    static constexpr ParseOptions json5() noexcept {
        ParseOptions opts;
        opts.allow_comments        = true;
        opts.allow_trailing_commas = true;
        opts.allow_single_quotes   = true;
        opts.allow_unquoted_keys   = true;
        opts.allow_nan_inf         = true;
        opts.allow_hex_numbers     = true;
        opts.allow_control_chars   = true;
        return opts;
    }
};

} // namespace yajson
