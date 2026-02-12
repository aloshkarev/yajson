#pragma once

/// @file json_pointer.hpp
/// @author Aleksandr Loshkarev
/// @brief JSON Pointer (RFC 6901) implementation.
///
/// Provides navigation into JSON documents via standardized pointer syntax:
///   "" -> root document
///   "/foo" -> key "foo"
///   "/foo/0" -> first element of array "foo"
///   "/a~1b" -> key "a/b" (~ encoding: ~0 = ~, ~1 = /)
///
/// Optimization: tokens that contain no ~ escapes are stored as string_view
/// into the original pointer string, avoiding heap allocations. Only tokens
/// with escapes allocate a std::string for the unescaped version.

#include "error.hpp"
#include "value.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace yajson {

/// @brief JSON Pointer (RFC 6901) for navigating JSON documents.
///
/// Internally stores the original pointer string and a vector of tokens.
/// Each token is either a string_view into the original (zero-copy, no escapes)
/// or an index into an auxiliary vector of unescaped strings.
class JsonPointer {
public:
    /// Construct empty pointer (references root document).
    JsonPointer() = default;

    /// Construct from RFC 6901 string (e.g. "/foo/bar/0").
    explicit JsonPointer(std::string_view ptr) {
        if (ptr.empty()) return;
        if (ptr[0] != '/')
            throw ParseError("JSON pointer must start with '/' or be empty",
                             SourceLocation{}, errc::unexpected_character);

        // Store the original string for string_view tokens to reference.
        source_ = std::string(ptr);
        std::string_view src(source_);
        src.remove_prefix(1);

        // Two-pass approach to avoid dangling string_views from vector reallocation.
        // Pass 1: collect segments and count escapes needed.
        struct SegInfo {
            std::string_view segment;
            bool needs_unescape;
        };
        std::vector<SegInfo> segs;
        {
            std::string_view scan = src;
            do {
                auto pos = scan.find('/');
                auto seg = (pos == std::string_view::npos) ? scan : scan.substr(0, pos);
                segs.push_back({seg, seg.find('~') != std::string_view::npos});
                if (pos == std::string_view::npos) break;
                scan.remove_prefix(pos + 1);
            } while (true);
        }

        // Reserve unescaped_ so no reallocation happens during push_back.
        size_t esc_count = 0;
        for (const auto& s : segs) if (s.needs_unescape) ++esc_count;
        unescaped_.reserve(esc_count);

        // Pass 2: build unescaped strings and token views.
        tokens_.reserve(segs.size());
        for (const auto& s : segs) {
            if (s.needs_unescape) {
                unescaped_.push_back(unescape(s.segment));
                tokens_.push_back(std::string_view(unescaped_.back()));
            } else {
                tokens_.push_back(s.segment);
            }
        }
    }

    /// Copy constructor — must fixup string_view pointers.
    JsonPointer(const JsonPointer& o)
        : source_(o.source_), unescaped_(o.unescaped_) {
        rebuild_tokens(o);
    }

    JsonPointer& operator=(const JsonPointer& o) {
        if (this != &o) {
            source_ = o.source_;
            unescaped_ = o.unescaped_;
            tokens_.clear();
            rebuild_tokens(o);
        }
        return *this;
    }

    JsonPointer(JsonPointer&& o) noexcept = default;
    JsonPointer& operator=(JsonPointer&& o) noexcept = default;

    /// Resolve pointer against a JSON value (const). Throws on failure.
    const JsonValue& resolve(const JsonValue& root) const {
        const JsonValue* cur = &root;
        for (size_t i = 0; i < tokens_.size(); ++i) {
            const auto& tok = tokens_[i];
            if (cur->is_object()) {
                auto* p = cur->find(tok);
                if (!p)
                    throw OutOfRangeError("JSON pointer: key not found \"" +
                                          std::string(tok) + "\" at depth " +
                                          std::to_string(i));
                cur = p;
            } else if (cur->is_array()) {
                size_t idx = parse_index(tok, cur->as_array().size());
                cur = &cur->as_array()[idx];
            } else {
                throw TypeError("JSON pointer: cannot index into " +
                                std::string(type_name(cur->type())) +
                                " at depth " + std::to_string(i));
            }
        }
        return *cur;
    }

    /// Resolve pointer against a JSON value (mutable).
    JsonValue& resolve(JsonValue& root) const {
        return const_cast<JsonValue&>(
            static_cast<const JsonPointer*>(this)->resolve(
                static_cast<const JsonValue&>(root)));
    }

    /// Try to resolve without exceptions; returns nullptr if path doesn't exist.
    const JsonValue* try_resolve(const JsonValue& root) const noexcept {
        const JsonValue* cur = &root;
        for (size_t i = 0; i < tokens_.size(); ++i) {
            const auto& tok = tokens_[i];
            if (cur->is_object()) {
                auto* p = cur->find(tok);
                if (!p) return nullptr;
                cur = p;
            } else if (cur->is_array()) {
                const auto& arr = cur->as_array();
                if (tok.empty()) return nullptr;
                if (tok.size() > 1 && tok[0] == '0') return nullptr;
                size_t idx = 0;
                for (char c : tok) {
                    if (c < '0' || c > '9') return nullptr;
                    idx = idx * 10 + static_cast<size_t>(c - '0');
                    if (idx >= arr.size() + 1) return nullptr;
                }
                if (idx >= arr.size()) return nullptr;
                cur = &arr[idx];
            } else {
                return nullptr;
            }
        }
        return cur;
    }

    JsonValue* try_resolve(JsonValue& root) const noexcept {
        return const_cast<JsonValue*>(
            try_resolve(static_cast<const JsonValue&>(root)));
    }

    /// Set a value at this pointer location, creating intermediate objects.
    void set(JsonValue& root, JsonValue value) const {
        if (tokens_.empty()) {
            root = std::move(value);
            return;
        }
        JsonValue* cur = &root;
        for (size_t i = 0; i + 1 < tokens_.size(); ++i) {
            const auto& tok = tokens_[i];
            if (cur->is_object()) {
                auto* p = cur->find(tok);
                if (!p) {
                    cur->insert(std::string(tok), JsonValue::object());
                    p = cur->find(tok);
                }
                cur = p;
            } else if (cur->is_array()) {
                size_t idx = parse_index(tok, cur->as_array().size());
                cur = &cur->as_array()[idx];
            } else {
                throw TypeError("JSON pointer: cannot traverse " +
                                std::string(type_name(cur->type())));
            }
        }
        const auto& last = tokens_.back();
        if (cur->is_object()) {
            cur->insert(std::string(last), std::move(value));
        } else if (cur->is_array()) {
            if (last == "-") {
                cur->push_back(std::move(value));
            } else {
                size_t idx = parse_index(last, cur->as_array().size());
                cur->as_array()[idx] = std::move(value);
            }
        } else {
            throw TypeError("JSON pointer: cannot set in " +
                            std::string(type_name(cur->type())));
        }
    }

    /// Erase the value at this pointer location. Returns true if erased.
    bool erase(JsonValue& root) const {
        if (tokens_.empty()) return false;
        JsonPointer par = parent();
        auto* container = par.try_resolve(root);
        if (!container) return false;
        const auto& last = tokens_.back();
        if (container->is_object()) {
            return container->erase(last);
        }
        if (container->is_array()) {
            size_t idx = 0;
            auto [p, ec] = std::from_chars(last.data(), last.data() + last.size(), idx);
            if (ec != std::errc{} || p != last.data() + last.size()) return false;
            auto& arr = container->as_array();
            if (idx >= arr.size()) return false;
            arr.erase(arr.begin() + static_cast<ptrdiff_t>(idx));
            return true;
        }
        return false;
    }

    /// Append a token (returns new pointer).
    [[nodiscard]] JsonPointer append(std::string_view token) const {
        // Build the new pointer string and re-parse
        std::string new_ptr = to_string();
        new_ptr += '/';
        new_ptr += escape(token);
        return JsonPointer(new_ptr);
    }

    [[nodiscard]] JsonPointer append(size_t index) const {
        return append(std::to_string(index));
    }

    /// Get parent pointer (empty if already root).
    [[nodiscard]] JsonPointer parent() const {
        if (tokens_.empty()) return {};
        // Reconstruct the parent pointer string
        std::string parent_str;
        for (size_t i = 0; i + 1 < tokens_.size(); ++i) {
            parent_str += '/';
            parent_str += escape(tokens_[i]);
        }
        if (parent_str.empty()) return {};
        return JsonPointer(parent_str);
    }

    [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }
    [[nodiscard]] size_t depth() const noexcept { return tokens_.size(); }

    /// Serialize back to RFC 6901 string.
    [[nodiscard]] std::string to_string() const {
        std::string result;
        for (const auto& tok : tokens_) {
            result += '/';
            result += escape(tok);
        }
        return result;
    }

    [[nodiscard]] const std::vector<std::string_view>& tokens() const noexcept {
        return tokens_;
    }

    bool operator==(const JsonPointer& o) const { return tokens_ == o.tokens_; }
    bool operator!=(const JsonPointer& o) const { return tokens_ != o.tokens_; }

private:
    /// Original pointer string — string_view tokens reference into this.
    std::string source_;

    /// Unescaped tokens (only for segments that contained '~').
    std::vector<std::string> unescaped_;

    /// Token views: either into source_ or into unescaped_ strings.
    std::vector<std::string_view> tokens_;

    /// Rebuild tokens_ after copy, adjusting string_view pointers.
    void rebuild_tokens(const JsonPointer& o) {
        tokens_.reserve(o.tokens_.size());
        for (const auto& tok : o.tokens_) {
            // Determine if this token points into source_ or unescaped_
            if (!o.source_.empty() &&
                tok.data() >= o.source_.data() &&
                tok.data() < o.source_.data() + o.source_.size()) {
                // Token is a view into source_ — adjust to our source_
                size_t offset = static_cast<size_t>(tok.data() - o.source_.data());
                tokens_.push_back(std::string_view(source_.data() + offset, tok.size()));
            } else {
                // Token points into unescaped_ — find the matching string
                for (size_t j = 0; j < o.unescaped_.size(); ++j) {
                    if (tok.data() == o.unescaped_[j].data()) {
                        tokens_.push_back(std::string_view(unescaped_[j]));
                        break;
                    }
                }
            }
        }
    }

    /// Unescape RFC 6901: ~1 -> /, ~0 -> ~
    static std::string unescape(std::string_view sv) {
        std::string result;
        result.reserve(sv.size());
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '~' && i + 1 < sv.size()) {
                if (sv[i + 1] == '1') { result += '/'; ++i; continue; }
                if (sv[i + 1] == '0') { result += '~'; ++i; continue; }
            }
            result += sv[i];
        }
        return result;
    }

    /// Escape for RFC 6901: ~ -> ~0, / -> ~1
    static std::string escape(std::string_view s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            if (c == '~') result += "~0";
            else if (c == '/') result += "~1";
            else result += c;
        }
        return result;
    }

    /// Parse array index token, validate.
    static size_t parse_index(std::string_view tok, size_t arr_size) {
        if (tok.empty() || (tok.size() > 1 && tok[0] == '0'))
            throw OutOfRangeError("JSON pointer: invalid array index \"" +
                                  std::string(tok) + "\"");
        size_t idx = 0;
        auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), idx);
        if (ec != std::errc{} || p != tok.data() + tok.size())
            throw OutOfRangeError("JSON pointer: invalid array index \"" +
                                  std::string(tok) + "\"");
        if (idx >= arr_size)
            throw OutOfRangeError("JSON pointer: array index " +
                                  std::to_string(idx) + " >= size " +
                                  std::to_string(arr_size));
        return idx;
    }
};

/// Convenience: resolve pointer string against a value.
inline const JsonValue& resolve(const JsonValue& root, std::string_view pointer) {
    return JsonPointer(pointer).resolve(root);
}

inline JsonValue& resolve(JsonValue& root, std::string_view pointer) {
    return JsonPointer(pointer).resolve(root);
}

} // namespace yajson
