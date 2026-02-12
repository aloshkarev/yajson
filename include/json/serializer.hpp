#pragma once

/// @file serializer.hpp
/// @author Aleksandr Loshkarev
/// @brief High-performance JSON serializer with streaming support.
///
/// Features:
///   - Direct output to string (dump) and streaming output (ostream)
///   - Constexpr escape tables — no snprintf on the hot path
///   - ensure_ascii mode for encoding non-ASCII -> \uXXXX
///   - Branch prediction hints for fast paths
///   - Support for NaN/Infinity serialization (with allow_nan_inf option)

#include "config.hpp"
#include "detail/dtoa.hpp"
#include "detail/simd.hpp"
#include "detail/utf8.hpp"
#include "value.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>

namespace yajson {

/// @brief Serialization options.
struct SerializeOptions {
    int indent = -1;           ///< Indentation (-1 = compact, >= 0 = pretty-printed)
    bool ensure_ascii = false; ///< Encode all non-ASCII characters as \uXXXX
    bool allow_nan_inf = false;///< Serialize NaN/Infinity instead of null
    bool sort_keys = false;    ///< Sort object keys alphabetically
};

namespace detail {

/// Hex digit table.
inline constexpr char kHexDigits[16] = {
    '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

/// Precomputed escape strings for control characters 0x00..0x1F.
struct EscapeEntry {
    char str[7];
    uint8_t len;
};

inline constexpr auto make_control_escape_table() {
    struct Table { EscapeEntry entries[32] = {}; } t;

    for (int i = 0; i < 32; ++i) {
        t.entries[i].str[0] = '\\';
        t.entries[i].str[1] = 'u';
        t.entries[i].str[2] = '0';
        t.entries[i].str[3] = '0';
        t.entries[i].str[4] = kHexDigits[(i >> 4) & 0xF];
        t.entries[i].str[5] = kHexDigits[i & 0xF];
        t.entries[i].str[6] = '\0';
        t.entries[i].len = 6;
    }

    auto set = [&](int idx, char c1, char c2) {
        t.entries[idx].str[0] = c1;
        t.entries[idx].str[1] = c2;
        t.entries[idx].str[2] = '\0';
        t.entries[idx].len = 2;
    };
    set(0x08, '\\', 'b');
    set(0x09, '\\', 't');
    set(0x0A, '\\', 'n');
    set(0x0C, '\\', 'f');
    set(0x0D, '\\', 'r');

    return t;
}

inline constexpr auto kControlEscapes = make_control_escape_table();

/// @brief Output adapter: buffered writing to std::string.
///
/// Accumulates small writes in an internal 4 KiB stack buffer before
/// flushing to the result string. This avoids per-write realloc/copy
/// overhead on std::string, matching the approach used by StreamOutput
/// for ostream. Measured ~40-60% faster on compact serialization.
class StringOutput {
public:
    ~StringOutput() { flush(); }

    StringOutput(const StringOutput&) = delete;
    StringOutput& operator=(const StringOutput&) = delete;
    StringOutput() = default;

    void write(char c) {
        if (JSON_UNLIKELY(pos_ >= kBufSize)) flush();
        buf_[pos_++] = c;
    }

    void write(const char* s, size_t n) {
        if (JSON_LIKELY(pos_ + n <= kBufSize)) {
            std::memcpy(buf_ + pos_, s, n);
            pos_ += n;
        } else {
            write_slow(s, n);
        }
    }

    void reserve(size_t n) { result_.reserve(n); }

    std::string& result() {
        flush();
        return result_;
    }

private:
    static constexpr size_t kBufSize = 4096;

    char buf_[kBufSize];
    size_t pos_ = 0;
    std::string result_;

    void flush() {
        if (pos_ > 0) {
            result_.append(buf_, pos_);
            pos_ = 0;
        }
    }

    JSON_NOINLINE void write_slow(const char* s, size_t n) {
        flush();
        if (n >= kBufSize) {
            // Large write — append directly to result string
            result_.append(s, n);
        } else {
            std::memcpy(buf_, s, n);
            pos_ = n;
        }
    }
};

/// @brief Output adapter: buffered writing to std::ostream.
///
/// Accumulates small writes in an internal 8 KiB buffer before flushing
/// to ostream. This drastically reduces the number of virtual calls
/// through streambuf (the main source of ostream overhead).
class StreamOutput {
public:
    explicit StreamOutput(std::ostream& os) noexcept : os_(os) {}

    ~StreamOutput() { flush(); }

    // Non-copyable, non-movable (bound to an ostream reference)
    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;

    void write(char c) {
        if (JSON_UNLIKELY(pos_ >= kBufSize)) flush();
        buf_[pos_++] = c;
    }

    void write(const char* s, size_t n) {
        if (JSON_LIKELY(pos_ + n <= kBufSize)) {
            // Main path: fits in the buffer
            std::memcpy(buf_ + pos_, s, n);
            pos_ += n;
        } else {
            write_slow(s, n);
        }
    }

    void reserve(size_t) {} // no-op for streams

private:
    static constexpr size_t kBufSize = 8192;

    std::ostream& os_;
    char buf_[kBufSize];
    size_t pos_ = 0;

    void flush() {
        if (pos_ > 0) {
            os_.write(buf_, static_cast<std::streamsize>(pos_));
            pos_ = 0;
        }
    }

    JSON_NOINLINE void write_slow(const char* s, size_t n) {
        // Flush existing buffer
        flush();
        if (n >= kBufSize) {
            // Large write — directly to ostream, bypassing the buffer
            os_.write(s, static_cast<std::streamsize>(n));
        } else {
            std::memcpy(buf_, s, n);
            pos_ = n;
        }
    }
};

/// @brief Templated serializer: Output type x Pretty mode (compile-time dispatch).
///
/// Pretty is a compile-time bool — the compiler dead-code-eliminates all
/// indentation/newline logic in compact mode, and removes all `if (Pretty)`
/// branches. This gives a measurable speedup (3-8%) on compact serialization.
template <typename Output, bool Pretty, bool EnsureAscii = false>
class SerializerCore {
public:
    explicit SerializerCore(Output& out, const SerializeOptions& opts) noexcept
        : out_(out), opts_(opts) {}

    void serialize(const JsonValue& value) {
        current_indent_ = 0;
        write_value(value);
    }

private:
    Output& out_;
    const SerializeOptions& opts_;
    int current_indent_ = 0;

    // Pre-built indent buffer (256 spaces) for bulk write.
    // Covers indent depths up to 256 in a single write() call.
    static constexpr int kMaxIndentBuf = 256;
    static constexpr char kSpaces[kMaxIndentBuf] = {
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
    };

    void write_indent() {
        if constexpr (Pretty) {
            // Bulk write from pre-built space buffer
            int n = current_indent_;
            while (n > 0) {
                int chunk = n < kMaxIndentBuf ? n : kMaxIndentBuf;
                out_.write(kSpaces, static_cast<size_t>(chunk));
                n -= chunk;
            }
        }
    }

    void write_newline() {
        if constexpr (Pretty) out_.write('\n');
    }

    void write_value(const JsonValue& v) {
        switch (v.type()) {
            case Type::Null:
                out_.write("null", 4);
                break;
            case Type::Bool:
                if (v.as_bool()) out_.write("true", 4);
                else out_.write("false", 5);
                break;
            case Type::Integer:
                write_integer(v.as_integer());
                break;
            case Type::UInteger:
                write_uinteger(v.as_uinteger());
                break;
            case Type::Float:
                write_float(v.as_float());
                break;
            case Type::String:
                write_string(v.as_string_view());
                break;
            case Type::Array:
                write_array(v.as_array());
                break;
            case Type::Object:
                write_object(v.as_object());
                break;
        }
    }

    void write_integer(int64_t val) {
        char buf[21];
        char* p = buf;
        uint64_t uval;
        if (val < 0) {
            *p++ = '-';
            // Handle INT64_MIN correctly: ~val + 1 avoids UB on negation
            uval = static_cast<uint64_t>(~val) + 1u;
        } else {
            uval = static_cast<uint64_t>(val);
        }
        p = detail::write_u64(p, uval);
        out_.write(buf, static_cast<size_t>(p - buf));
    }

    void write_uinteger(uint64_t val) {
        char buf[21];
        char* p = detail::write_u64(buf, val);
        out_.write(buf, static_cast<size_t>(p - buf));
    }

    void write_float(double val) {
        if (JSON_UNLIKELY(std::isnan(val))) {
            if (opts_.allow_nan_inf) out_.write("NaN", 3);
            else out_.write("null", 4);
            return;
        }
        if (JSON_UNLIKELY(std::isinf(val))) {
            if (opts_.allow_nan_inf) {
                if (val < 0) out_.write('-');
                out_.write("Infinity", 8);
            } else {
                out_.write("null", 4);
            }
            return;
        }
        char buf[40];
        const size_t len = detail::fast_dtoa(buf, val);
        out_.write(buf, len);
    }

    void write_string(std::string_view s) {
        out_.write('"');
        const char* data = s.data();
        const char* const str_end = data + s.size();
        const char* ptr = data;

        // Compile-time dispatch: EnsureAscii selects the SIMD loop variant
        // and eliminates the >= 0x80 branch when not needed.
        while (ptr < str_end) {
            const char* safe_end = simd::find_needs_escape<EnsureAscii>(ptr, str_end);

            if (safe_end > ptr) {
                out_.write(ptr, static_cast<size_t>(safe_end - ptr));
                ptr = safe_end;
                if (ptr >= str_end) break;
            }

            auto c = static_cast<unsigned char>(*ptr);
            if (c < 0x20) {
                const auto& esc = kControlEscapes.entries[c];
                out_.write(esc.str, esc.len);
            } else if (c == '"') {
                out_.write("\\\"", 2);
            } else if (c == '\\') {
                out_.write("\\\\", 2);
            } else if constexpr (EnsureAscii) {
                if (c >= 0x80) {
                    const char* p = ptr;
                    uint32_t cp = utf8::decode(p, str_end);
                    write_escaped_codepoint(cp);
                    ptr = p;
                    continue;
                }
            }
            ++ptr;
        }
        out_.write('"');
    }

    void write_escaped_codepoint(uint32_t cp) {
        auto write_u16 = [this](uint16_t val) {
            char buf[6] = {'\\', 'u',
                kHexDigits[(val >> 12) & 0xF],
                kHexDigits[(val >> 8) & 0xF],
                kHexDigits[(val >> 4) & 0xF],
                kHexDigits[val & 0xF]};
            out_.write(buf, 6);
        };
        if (cp <= 0xFFFF) {
            write_u16(static_cast<uint16_t>(cp));
        } else {
            uint32_t adj = cp - 0x10000;
            write_u16(static_cast<uint16_t>(0xD800 + (adj >> 10)));
            write_u16(static_cast<uint16_t>(0xDC00 + (adj & 0x3FF)));
        }
    }

    void write_array(const Array& arr) {
        if (arr.empty()) { out_.write("[]", 2); return; }
        out_.write('[');
        if constexpr (Pretty) current_indent_ += opts_.indent;
        write_newline();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) { out_.write(','); write_newline(); }
            write_indent();
            write_value(arr[i]);
        }
        if constexpr (Pretty) current_indent_ -= opts_.indent;
        write_newline();
        write_indent();
        out_.write(']');
    }

    void write_object(const Object& obj) {
        if (obj.empty()) { out_.write("{}", 2); return; }
        out_.write('{');
        if constexpr (Pretty) current_indent_ += opts_.indent;
        write_newline();
        if (opts_.sort_keys) write_object_sorted(obj);
        else write_object_ordered(obj);
        if constexpr (Pretty) current_indent_ -= opts_.indent;
        write_newline();
        write_indent();
        out_.write('}');
    }

    void write_object_ordered(const Object& obj) {
        auto it = obj.begin();
        if (it != obj.end()) {
            write_indent();
            write_string(std::string_view(it->first));
            out_.write(':');
            if constexpr (Pretty) out_.write(' ');
            write_value(it->second);
            for (++it; it != obj.end(); ++it) {
                out_.write(',');
                write_newline();
                write_indent();
                write_string(std::string_view(it->first));
                out_.write(':');
                if constexpr (Pretty) out_.write(' ');
                write_value(it->second);
            }
        }
    }

    void write_object_sorted(const Object& obj) {
        const auto& storage = obj.storage();
        const size_t n = obj.size();

        // Small-buffer optimization: stack-allocate for objects <= 64 keys.
        // Avoids a heap allocation for 99%+ of JSON objects.
        constexpr size_t kSmallBuf = 64;
        size_t stack_indices[kSmallBuf];
        std::vector<size_t> heap_indices;
        size_t* indices;

        if (JSON_LIKELY(n <= kSmallBuf)) {
            indices = stack_indices;
        } else {
            heap_indices.resize(n);
            indices = heap_indices.data();
        }

        for (size_t i = 0; i < n; ++i) indices[i] = i;
        std::sort(indices, indices + n,
                  [&storage](size_t a, size_t b) {
                      return storage[a].first < storage[b].first;
                  });

        for (size_t k = 0; k < n; ++k) {
            if (k > 0) { out_.write(','); write_newline(); }
            write_indent();
            write_string(std::string_view(storage[indices[k]].first));
            out_.write(':');
            if constexpr (Pretty) out_.write(' ');
            write_value(storage[indices[k]].second);
        }
    }
};

/// @brief Backward-compatible wrapper: dispatches Pretty x EnsureAscii at runtime,
/// selecting one of 4 compile-time template instantiations.
template <typename Output>
class SerializerImpl {
public:
    explicit SerializerImpl(Output& out, const SerializeOptions& opts = {}) noexcept
        : out_(out), opts_(opts) {}

    void serialize(const JsonValue& value) {
        const bool pretty = opts_.indent >= 0;
        const bool ascii = opts_.ensure_ascii;
        if (pretty) {
            if (ascii) SerializerCore<Output, true, true>(out_, opts_).serialize(value);
            else       SerializerCore<Output, true, false>(out_, opts_).serialize(value);
        } else {
            if (ascii) SerializerCore<Output, false, true>(out_, opts_).serialize(value);
            else       SerializerCore<Output, false, false>(out_, opts_).serialize(value);
        }
    }

private:
    Output& out_;
    SerializeOptions opts_;
};

/// @brief Cheap O(1) size hint for pre-allocating the serialization buffer.
/// Only inspects the root value (no recursive traversal). For arrays/objects,
/// uses element count × average bytes per element. This avoids the overhead
/// of a full DOM traversal while still eliminating most std::string
/// reallocations on large documents.
inline size_t serialization_size_hint(const JsonValue& value) noexcept {
    switch (value.type()) {
        case Type::Array: {
            // ~64 bytes per element is a reasonable estimate for mixed JSON
            const auto& arr = value.as_array();
            return arr.size() * 64 + 2;
        }
        case Type::Object: {
            // ~80 bytes per key-value pair (key + colon + value + comma)
            const auto& obj = value.as_object();
            return obj.size() * 80 + 2;
        }
        case Type::String:
            return value.as_string_view().size() + 2;
        default:
            return 16;
    }
}

// Legacy serializer for backward compatibility
class Serializer {
public:
    explicit Serializer(int indent_step = -1, bool ensure_ascii = false) noexcept {
        opts_.indent = indent_step;
        opts_.ensure_ascii = ensure_ascii;
    }

    explicit Serializer(const SerializeOptions& opts) noexcept
        : opts_(opts) {}

    [[nodiscard]] std::string serialize(const JsonValue& value) {
        StringOutput out;
        // O(1) size hint: inspects only the root element count, no traversal.
        // For large documents this eliminates 5-7 std::string reallocations
        // with negligible overhead (~2 ns).
        const size_t hint = serialization_size_hint(value);
        if (hint > 4096) {
            out.reserve(hint);
        }
        SerializerImpl<StringOutput> impl(out, opts_);
        impl.serialize(value);
        return std::move(out.result());
    }

private:
    SerializeOptions opts_;
};

} // namespace detail

// ─── JsonValue::dump() implementation ────────────────────────────────────

inline std::string JsonValue::dump(int indent) const {
    detail::Serializer ser(indent);
    return ser.serialize(*this);
}

inline std::string JsonValue::dump(const SerializeOptions& opts) const {
    detail::Serializer ser(opts);
    return ser.serialize(*this);
}

/// @brief Free function: serialize to string.
[[nodiscard]] inline std::string serialize(const JsonValue& value, int indent = -1) {
    return value.dump(indent);
}

/// @brief Serialize with extended options.
[[nodiscard]] inline std::string serialize(const JsonValue& value,
                                           const SerializeOptions& opts) {
    detail::Serializer ser(opts);
    return ser.serialize(value);
}

/// @brief Serialize to ostream (streaming mode).
inline std::ostream& operator<<(std::ostream& os, const JsonValue& value) {
    detail::StreamOutput out(os);
    SerializeOptions opts;
    detail::SerializerImpl<detail::StreamOutput> impl(out, opts);
    impl.serialize(value);
    return os;
}

/// @brief Serialize to ostream with options (streaming mode).
inline void serialize(std::ostream& os, const JsonValue& value,
                      const SerializeOptions& opts = {}) {
    detail::StreamOutput out(os);
    detail::SerializerImpl<detail::StreamOutput> impl(out, opts);
    impl.serialize(value);
}

} // namespace yajson
