#pragma once

/// @file fwd.hpp
/// @author Aleksandr Loshkarev
/// @brief Forward declarations and type aliases for yajson.

#include "detail/hash.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yajson {

// ─── Forward declarations ───────────────────────────────────────────────
class JsonValue;
class MonotonicArena;
namespace detail { class Parser; } // Forward for friend access

/// JSON value types
enum class Type : uint8_t {
    Null    = 0,
    Bool    = 1,
    Integer = 2,
    Float   = 3,
    String  = 4,
    Array   = 5,
    Object   = 6,
    UInteger = 7
};

/// @brief Returns the string representation of a type.
inline const char* type_name(Type t) noexcept {
    switch (t) {
        case Type::Null:    return "null";
        case Type::Bool:    return "bool";
        case Type::Integer: return "integer";
        case Type::Float:   return "float";
        case Type::String:  return "string";
        case Type::Array:   return "array";
        case Type::Object:   return "object";
        case Type::UInteger: return "uinteger";
    }
    return "unknown";
}

// ─── Type aliases ───────────────────────────────────────────────────────

/// JSON array: ordered collection of values.
/// Uses pmr::vector so that when a MonotonicArena is active, the vector's
/// backing store is allocated from the arena instead of the global heap.
using Array = std::pmr::vector<JsonValue>;

/// @brief JSON object: ordered key-value pairs with O(1) lookup.
///
/// Uses pmr::vector for entry storage and pmr::unordered_map for the hash
/// index, both routed through the arena when one is active.
///
/// The keys remain std::string (not pmr::string) to keep the public API
/// simple. Keys shorter than SSO threshold (~15 chars on most platforms)
/// don't allocate from the heap regardless.
struct Object {
    using storage_type = std::pmr::vector<std::pair<std::string, JsonValue>>;
    using size_type = size_t;
    /// Hash index stores string_view keys pointing into entries[].first.
    /// This avoids heap-allocating a std::string on every find() call.
    /// The index is always rebuilt from scratch (invalidated on mutation),
    /// so the views always point to valid entry keys.
    using index_type = std::pmr::unordered_map<std::string_view, size_type,
                                                detail::StringHash,
                                                detail::StringEqual>;

    storage_type entries;

    /// Lazily created hash index: key -> offset in entries.
    /// Stored as unique_ptr to keep sizeof(Object) compact.
    /// Created only when the object grows above kIndexThreshold.
    mutable std::unique_ptr<index_type> index_;

    // ─── Constructors (declared here, defined in value.hpp) ──────────

    /// Default constructor: uses new_delete_resource.
    Object() : entries(std::pmr::new_delete_resource()) {}

    /// Constructor with explicit memory_resource (for arena allocation).
    explicit Object(std::pmr::memory_resource* mr) : entries(mr) {}

    ~Object();
    Object(const Object&);
    Object(Object&&) noexcept;
    Object& operator=(const Object&);
    Object& operator=(Object&&) noexcept;

    /// Initializer-list constructor: {{"key", value}, ...}
    Object(std::initializer_list<std::pair<std::string, JsonValue>> init);

    // ─── Capacity ────────────────────────────────────────────────────────
    bool empty() const noexcept { return entries.empty(); }
    size_type size() const noexcept { return entries.size(); }
    void reserve(size_type n) { entries.reserve(n); }

    /// Get the memory_resource used by this object's storage.
    std::pmr::memory_resource* get_resource() const noexcept {
        return entries.get_allocator().resource();
    }

    // ─── Iterators ──────────────────────────────────────────────────────
    auto begin() noexcept { return entries.begin(); }
    auto end()   noexcept { return entries.end(); }
    auto begin()  const noexcept { return entries.begin(); }
    auto end()    const noexcept { return entries.end(); }
    auto cbegin() const noexcept { return entries.cbegin(); }
    auto cend()   const noexcept { return entries.cend(); }

    // ─── Methods (declared here, defined after JsonValue in value.hpp) ──

    /// O(1) key lookup for large objects, linear for small ones.
    JsonValue* find(std::string_view key) noexcept;
    const JsonValue* find(std::string_view key) const noexcept;

    /// Check whether a key exists.
    bool contains(std::string_view key) const noexcept;

    /// Access or create an element by key.
    JsonValue& operator[](std::string_view key);

    /// Const access by key. Throws an exception if not found.
    const JsonValue& at(std::string_view key) const;

    /// Insert or update a key-value pair (amortized O(1)).
    void insert(std::string key, JsonValue value);

    /// Append to the end. If the hash index exists, update it incrementally
    /// instead of destroying and rebuilding from scratch.
    /// (Template body is in value.hpp where JsonValue is complete.)
    template <typename K, typename V>
    void emplace_back(K&& key, V&& value);

    /// Called after emplace_back when index_ exists to keep it in sync.
    /// Defined in value.hpp.
    void update_index_after_push(const void* old_data);

    /// Erase by key.
    bool erase(std::string_view key);

    /// Clear all entries and release the index.
    void clear() noexcept {
        entries.clear();
        index_.reset();
    }

    /// Comparison.
    bool operator==(const Object& other) const;
    bool operator!=(const Object& other) const { return !(*this == other); }

    /// Direct access to the underlying storage.
    const storage_type& storage() const noexcept { return entries; }
    storage_type& storage() noexcept { return entries; }

    /// @brief Rebuild the hash index from entries.
    /// Public to allow the parser to build the index once after batch insertion.
    void rebuild_index() const;

private:
    friend class detail::Parser;  // Allow parser to call rebuild_index / kIndexThreshold

    // Threshold: below this value linear search is used (cache-friendly)
    static constexpr size_type kIndexThreshold = 16;

    bool use_index() const noexcept {
        return entries.size() >= kIndexThreshold;
    }

    void ensure_index() const;
    void invalidate_index() const noexcept { index_.reset(); }
};

/// The string_view type used in the API
using string_view = std::string_view;

} // namespace yajson
