#pragma once

/// @file error.hpp
/// @author Aleksandr Loshkarev
/// @brief Error types for yajson: exceptions + std::error_code system.
///
/// Dual error reporting:
///   - Via exceptions: ParseError, TypeError, OutOfRangeError (default)
///   - Via error_code: yajson::errc enum + json_category() (exception-free)
///
/// Use try_parse(input) for exception-free parsing.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>

namespace yajson {

// =====================================================================
// Source position for parse errors
// =====================================================================

/// @brief Position in the source JSON text.
struct SourceLocation {
    size_t line   = 1;  ///< Line number (1-based)
    size_t column = 1;  ///< Column number (1-based)
    size_t offset = 0;  ///< Byte offset from the beginning
};

// =====================================================================
// Error code enumeration
// =====================================================================

/// @brief JSON error codes for std::error_code integration.
enum class errc : int {
    ok = 0,

    // Parse errors (1-49)
    unexpected_end_of_input = 1,
    unexpected_character    = 2,
    invalid_escape          = 3,
    invalid_unicode_escape  = 4,
    invalid_number          = 5,
    unterminated_string     = 6,
    unterminated_array      = 7,
    unterminated_object     = 8,
    trailing_content        = 9,
    max_depth_exceeded      = 10,
    invalid_literal         = 11,
    duplicate_key           = 12,
    invalid_utf8            = 13,
    invalid_comment         = 14,

    // Value access errors (50-79)
    type_mismatch           = 50,
    out_of_range            = 51,
    key_not_found           = 52,
    integer_overflow        = 53,

    // Serialization errors (80-99)
    nan_or_infinity         = 80,
};

// =====================================================================
// Error category
// =====================================================================

namespace detail {

class json_error_category_impl : public std::error_category {
public:
    const char* name() const noexcept override {
        return "json";
    }

    std::string message(int ev) const override {
        switch (static_cast<errc>(ev)) {
            case errc::ok:                      return "success";
            case errc::unexpected_end_of_input:  return "unexpected end of input";
            case errc::unexpected_character:      return "unexpected character";
            case errc::invalid_escape:           return "invalid escape sequence";
            case errc::invalid_unicode_escape:    return "invalid unicode escape";
            case errc::invalid_number:           return "invalid number";
            case errc::unterminated_string:       return "unterminated string";
            case errc::unterminated_array:        return "unterminated array";
            case errc::unterminated_object:       return "unterminated object";
            case errc::trailing_content:         return "trailing content after JSON";
            case errc::max_depth_exceeded:        return "maximum nesting depth exceeded";
            case errc::invalid_literal:          return "invalid literal";
            case errc::duplicate_key:            return "duplicate key";
            case errc::invalid_utf8:             return "invalid UTF-8 encoding";
            case errc::invalid_comment:          return "invalid comment";
            case errc::type_mismatch:            return "type mismatch";
            case errc::out_of_range:             return "index out of range";
            case errc::key_not_found:            return "key not found";
            case errc::integer_overflow:         return "integer overflow";
            case errc::nan_or_infinity:          return "NaN or Infinity not representable";
            default:                             return "unknown json error";
        }
    }

    std::error_condition default_error_condition(int ev) const noexcept override {
        auto e = static_cast<errc>(ev);
        if (e == errc::ok) {
            return {};
        }
        if (static_cast<int>(e) < 50) {
            return std::error_condition(static_cast<int>(e), *this);
        }
        return std::error_condition(ev, *this);
    }
};

} // namespace detail

/// @brief Get the json error category singleton.
inline const std::error_category& json_category() noexcept {
    static const detail::json_error_category_impl instance;
    return instance;
}

/// @brief Create an error_code from yajson::errc.
inline std::error_code make_error_code(errc e) noexcept {
    return {static_cast<int>(e), json_category()};
}

/// @brief Create an error_condition from yajson::errc.
inline std::error_condition make_error_condition(errc e) noexcept {
    return {static_cast<int>(e), json_category()};
}

// =====================================================================
// Exception types
// =====================================================================

/// @brief JSON parse error with source position information.
class ParseError : public std::system_error {
public:
    ParseError(const std::string& message, SourceLocation loc,
               errc code = errc::unexpected_character)
        : std::system_error(make_error_code(code), format_message(message, loc))
        , location_(loc) {}

    /// @brief Error position in the source text.
    [[nodiscard]] const SourceLocation& location() const noexcept {
        return location_;
    }

private:
    static std::string format_message(const std::string& msg,
                                      const SourceLocation& loc) {
        return "JSON parse error at line " + std::to_string(loc.line) +
               ", column " + std::to_string(loc.column) + ": " + msg;
    }

    SourceLocation location_;
};

/// @brief Type mismatch error when accessing a value.
class TypeError : public std::system_error {
public:
    explicit TypeError(const std::string& msg)
        : std::system_error(make_error_code(errc::type_mismatch), msg) {}
};

/// @brief Out-of-range error (array index or missing key).
class OutOfRangeError : public std::system_error {
public:
    explicit OutOfRangeError(const std::string& msg)
        : std::system_error(make_error_code(errc::out_of_range), msg) {}
};

// =====================================================================
// Result type for exception-free operations
// =====================================================================

/// @brief Simple result type: value + error_code.
/// Usage: auto [val, ec] = yajson::try_parse(input);
template <typename T>
struct result {
    T value;
    std::error_code ec;

    explicit operator bool() const noexcept { return !ec; }
    bool has_value() const noexcept { return !ec; }
};

} // namespace yajson

// Register yajson::errc as an error_code enum
namespace std {
template <>
struct is_error_code_enum<yajson::errc> : true_type {};
} // namespace std
