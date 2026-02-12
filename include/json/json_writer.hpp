#pragma once

/// @file json_writer.hpp
/// @author Aleksandr Loshkarev
/// @brief Incremental (SAX-style) JSON writer for streaming serialization.
///
/// Allows generating JSON output without building a full DOM tree.
/// Ideal for high-throughput scenarios: large datasets, real-time streams.
///
/// Usage:
///   std::string buf;
///   yajson::JsonWriter w(buf);
///   w.begin_object();
///   w.key("name").string_value("Alice");
///   w.key("scores").begin_array();
///   w.int_value(100).int_value(95);
///   w.end_array();
///   w.end_object();
///   // buf == {"name":"Alice","scores":[100,95]}

#include "config.hpp"
#include "error.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace yajson {

/// @brief Incremental JSON writer with validation and optional formatting.
///
/// The writer maintains a state machine for output structure validation:
/// objects alternate key/value, arrays contain values, commas are inserted automatically.
/// Supports output to a string buffer and ostream.
class JsonWriter {
public:
    /// Constructor: write to a string buffer.
    /// @param out Target string buffer.
    /// @param indent Indentation width for formatting (-1 = compact).
    explicit JsonWriter(std::string& out, int indent = -1) noexcept
        : sbuf_(&out), os_(nullptr), indent_(indent) {}

    /// Constructor: write to an ostream.
    /// @param os Target output stream.
    /// @param indent Indentation width for formatting (-1 = compact).
    explicit JsonWriter(std::ostream& os, int indent = -1) noexcept
        : sbuf_(nullptr), os_(&os), indent_(indent) {}

    ~JsonWriter() = default;

    // Non-copyable, movable
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;
    JsonWriter(JsonWriter&&) = default;
    JsonWriter& operator=(JsonWriter&&) = default;

    // ─── Scalar values ────────────────────────────────────────────────────

    JsonWriter& null_value() {
        pre_value();
        write_raw("null", 4);
        post_value();
        return *this;
    }

    JsonWriter& bool_value(bool v) {
        pre_value();
        if (v) write_raw("true", 4);
        else write_raw("false", 5);
        post_value();
        return *this;
    }

    JsonWriter& int_value(int64_t v) {
        pre_value();
        char buf[21];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (JSON_LIKELY(ec == std::errc{}))
            write_raw(buf, static_cast<size_t>(ptr - buf));
        else
            write_raw("0", 1); // Fallback (should not happen for int64_t)
        post_value();
        return *this;
    }

    JsonWriter& uint_value(uint64_t v) {
        pre_value();
        char buf[21];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (JSON_LIKELY(ec == std::errc{}))
            write_raw(buf, static_cast<size_t>(ptr - buf));
        else
            write_raw("0", 1);
        post_value();
        return *this;
    }

    JsonWriter& float_value(double v) {
        pre_value();
        if (JSON_UNLIKELY(std::isnan(v) || std::isinf(v))) {
            write_raw("null", 4);
        } else {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
            size_t len = static_cast<size_t>(ptr - buf);
            write_raw(buf, len);
            // Ensure it looks like a float (contains '.' or 'e')
            bool has_dot = false;
            for (size_t i = 0; i < len; ++i) {
                if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
                    has_dot = true;
                    break;
                }
            }
            if (!has_dot) write_raw(".0", 2);
        }
        post_value();
        return *this;
    }

    JsonWriter& string_value(std::string_view sv) {
        pre_value();
        write_escaped_string(sv);
        post_value();
        return *this;
    }

    // ─── Containers ───────────────────────────────────────────────────

    JsonWriter& begin_object() {
        pre_value();
        write_raw("{", 1);
        stack_.push_back(State::ObjectStart);
        ++depth_;
        return *this;
    }

    JsonWriter& end_object() {
        if (JSON_UNLIKELY(stack_.empty() ||
                          (stack_.back() != State::ObjectStart &&
                           stack_.back() != State::ObjectValue))) {
            throw TypeError("JsonWriter: end_object() without matching begin_object()");
        }
        bool empty_obj = (stack_.back() == State::ObjectStart);
        stack_.pop_back();
        --depth_;
        if (!empty_obj) write_newline();
        write_raw("}", 1);
        post_value();
        return *this;
    }

    JsonWriter& begin_array() {
        pre_value();
        write_raw("[", 1);
        stack_.push_back(State::ArrayStart);
        ++depth_;
        return *this;
    }

    JsonWriter& end_array() {
        if (JSON_UNLIKELY(stack_.empty() ||
                          (stack_.back() != State::ArrayStart &&
                           stack_.back() != State::ArrayValue))) {
            throw TypeError("JsonWriter: end_array() without matching begin_array()");
        }
        bool empty_arr = (stack_.back() == State::ArrayStart);
        stack_.pop_back();
        --depth_;
        if (!empty_arr) write_newline();
        write_raw("]", 1);
        post_value();
        return *this;
    }

    // ─── Object keys ────────────────────────────────────────────────────

    JsonWriter& key(std::string_view k) {
        if (JSON_UNLIKELY(stack_.empty() ||
                          (stack_.back() != State::ObjectStart &&
                           stack_.back() != State::ObjectValue))) {
            throw TypeError("JsonWriter: key() outside of object context");
        }
        if (stack_.back() == State::ObjectValue) {
            write_raw(",", 1);
        }
        write_newline();
        write_escaped_string(k);
        write_raw(":", 1);
        if (indent_ >= 0) write_raw(" ", 1);
        stack_.back() = State::ObjectKey;
        return *this;
    }

    // ─── Raw JSON insertion ────────────────────────────────────────────

    /// Insert pre-serialized, valid JSON directly into the output.
    /// WARNING: No validation of the raw JSON is performed.
    JsonWriter& raw_json(std::string_view json) {
        pre_value();
        write_raw(json.data(), json.size());
        post_value();
        return *this;
    }

    // ─── Flush buffer ───────────────────────────────────────────────────

    void flush() {
        if (os_) os_->flush();
    }

    /// Check whether the writer is in a valid final state (all containers closed).
    [[nodiscard]] bool is_complete() const noexcept {
        return stack_.empty() && value_written_;
    }

    /// Current nesting depth.
    [[nodiscard]] size_t depth() const noexcept { return depth_; }

private:
    std::string* sbuf_;
    std::ostream* os_;
    int indent_;
    size_t depth_ = 0;
    bool value_written_ = false;

    enum class State : uint8_t {
        ObjectStart,  // After '{', expecting key or '}'
        ObjectKey,    // After key+colon, expecting value
        ObjectValue,  // After value in object, expecting ',' or '}'
        ArrayStart,   // After '[', expecting value or ']'
        ArrayValue    // After value in array, expecting ',' or ']'
    };

    std::vector<State> stack_;

    void write_raw(const char* data, size_t len) {
        if (sbuf_) sbuf_->append(data, len);
        else if (os_) os_->write(data, static_cast<std::streamsize>(len));
    }

    void write_newline() {
        if (indent_ < 0) return;
        write_raw("\n", 1);
        size_t total = depth_ * static_cast<size_t>(indent_);
        // Write indentation with spaces
        static constexpr char spaces[] =
            "                                                                ";
        while (total > 0) {
            size_t chunk = (total < 64) ? total : 64;
            write_raw(spaces, chunk);
            total -= chunk;
        }
    }

    void write_escaped_string(std::string_view sv) {
        write_raw("\"", 1);
        const char* start = sv.data();
        const char* end = start + sv.size();
        const char* seg = start;
        for (const char* p = start; p < end; ++p) {
            const char c = *p;
            const char* esc = nullptr;
            size_t esc_len = 0;
            switch (c) {
                case '"':  esc = "\\\""; esc_len = 2; break;
                case '\\': esc = "\\\\"; esc_len = 2; break;
                case '\b': esc = "\\b";  esc_len = 2; break;
                case '\f': esc = "\\f";  esc_len = 2; break;
                case '\n': esc = "\\n";  esc_len = 2; break;
                case '\r': esc = "\\r";  esc_len = 2; break;
                case '\t': esc = "\\t";  esc_len = 2; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // \uXXXX for other control characters
                        if (seg < p) write_raw(seg, static_cast<size_t>(p - seg));
                        char ubuf[7];
                        ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
                        static constexpr char hex[] = "0123456789abcdef";
                        ubuf[4] = hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                        ubuf[5] = hex[static_cast<unsigned char>(c) & 0xF];
                        write_raw(ubuf, 6);
                        seg = p + 1;
                    }
                    continue;
            }
            if (seg < p) write_raw(seg, static_cast<size_t>(p - seg));
            write_raw(esc, esc_len);
            seg = p + 1;
        }
        if (seg < end) write_raw(seg, static_cast<size_t>(end - seg));
        write_raw("\"", 1);
    }

    void pre_value() {
        if (!stack_.empty()) {
            auto& top = stack_.back();
            switch (top) {
                case State::ObjectKey:
                    // After key, expecting value — no comma needed
                    break;
                case State::ArrayValue:
                    write_raw(",", 1);
                    write_newline();
                    break;
                case State::ArrayStart:
                    write_newline();
                    top = State::ArrayValue;
                    break;
                case State::ObjectStart:
                case State::ObjectValue:
                    // Value in object must be preceded by key()
                    break;
            }
        }
    }

    void post_value() {
        value_written_ = true;
        if (!stack_.empty()) {
            auto& top = stack_.back();
            if (top == State::ObjectKey) {
                top = State::ObjectValue;
            } else if (top == State::ArrayStart) {
                top = State::ArrayValue;
            }
        }
    }
};

} // namespace yajson
