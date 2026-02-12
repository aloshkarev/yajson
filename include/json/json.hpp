#pragma once

/// @file json.hpp
/// @author Aleksandr Loshkarev
/// @brief Main header file for the yajson library.

#include "config.hpp"
#include "fwd.hpp"
#include "error.hpp"
#include "arena.hpp"
#include "value.hpp"
#include "parse_options.hpp"
#include "serializer.hpp"
#include "parser.hpp"
#include "stream_parser.hpp"
#include "thread_safe.hpp"
#include "conversion.hpp"
#include "json_pointer.hpp"
#include "json_writer.hpp"
#include "allocator.hpp"

// Arena-aware parse overloads (after parser.hpp to avoid circular deps)
namespace yajson {

/// Parse JSON using a monotonic arena for allocations.
[[nodiscard]] inline JsonValue parse(std::string_view input,
                                     MonotonicArena& arena,
                                     const ParseOptions& opts = {}) {
    ArenaScope scope(arena);
    return detail::Parser::parse(input, opts);
}

/// Parse JSON using a monotonic arena (exception-free).
[[nodiscard]] inline result<JsonValue> try_parse(std::string_view input,
                                                  MonotonicArena& arena,
                                                  const ParseOptions& opts = {}) noexcept {
    ArenaScope scope(arena);
    return detail::Parser::try_parse(input, opts);
}

// ─── ArenaDocument: zero-config arena parsing ─────────────────────────────

/// @brief Document that owns a MonotonicArena and the root JsonValue.
///
/// Use when you want arena-allocated parsing without managing the arena
/// yourself. The document holds both the arena and the parsed root;
/// all allocations for the tree come from the internal arena.
///
/// @code
///   yajson::ArenaDocument doc;
///   doc.parse(R"({"a":1,"b":[2,3]})");
///   assert(doc.root()["a"].as_integer() == 1);
///   doc.reset();  // reuse for next parse
///   doc.parse("[1,2,3]");
/// @endcode
///
/// The root value and all its descendants are valid only until reset()
/// or the next parse(), or until the ArenaDocument is destroyed.
class ArenaDocument {
public:
    /// @brief Construct with default arena size (4096 bytes initial).
    explicit ArenaDocument(size_t initial_arena_size = 4096)
        : arena_(initial_arena_size)
        , root_() {}

    /// @brief Parse input into the document; root is updated.
    /// @throws ParseError on invalid JSON.
    void parse(std::string_view input, const ParseOptions& opts = {}) {
        ArenaScope scope(arena_);
        root_ = detail::Parser::parse(input, opts);
    }

    /// @brief Parse input (no exceptions); returns error code on failure.
    /// On success, root is updated; on failure, root is unchanged.
    result<JsonValue> try_parse(std::string_view input,
                                const ParseOptions& opts = {}) noexcept {
        ArenaScope scope(arena_);
        auto res = detail::Parser::try_parse(input, opts);
        if (res) root_ = std::move(res.value);
        return res;
    }

    /// @brief Access the parsed root value (null if never parsed or after reset).
    [[nodiscard]] JsonValue& root() noexcept { return root_; }
    [[nodiscard]] const JsonValue& root() const noexcept { return root_; }

    /// @brief Reset arena and root; reuse for another parse.
    /// Root is cleared first so that arena-allocated data is not accessed after reset.
    void reset() noexcept {
        root_ = JsonValue();
        arena_.reset();
    }

    /// @brief Arena used by this document (e.g. for bytes_used() / block_count()).
    [[nodiscard]] MonotonicArena& arena() noexcept { return arena_; }
    [[nodiscard]] const MonotonicArena& arena() const noexcept { return arena_; }

private:
    MonotonicArena arena_;
    JsonValue root_;
};

} // namespace yajson
