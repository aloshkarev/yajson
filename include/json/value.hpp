#pragma once

/// @file value.hpp
/// @author Aleksandr Loshkarev
/// @brief Library core: JsonValue — a 24-byte tagged union with SSO strings.
///
/// Implementation:
///   - Compact 24-byte tagged union (like boost::json::value)
///   - Small String Optimization (SSO) for strings up to 15 characters
///   - Manual resource management (copy/move/destroy)
///   - Support for all JSON types: null, bool, int64_t, uint64_t, double, string, array, object
///   - O(1) object key lookup via a lazy hash index
///   - Arena-aware allocation: when a MonotonicArena is active (via ArenaScope),
///     strings, arrays, and objects are allocated from the arena instead of the heap
///   - PMR containers: Array (pmr::vector) and Object (pmr::vector + pmr::unordered_map)
///     route their internal storage through the arena when active

#include "arena.hpp"
#include "config.hpp"
#include "error.hpp"
#include "fwd.hpp"

#include <cmath>
#include <cstring>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace yajson {
namespace detail { class Parser; } // forward declaration

class JsonValue {
    friend class detail::Parser;  // Zero-copy arena string construction
public:
    JsonValue() noexcept : kind_(Type::Null), sso_len_(0) { u_.i = 0; }
    JsonValue(std::nullptr_t) noexcept : kind_(Type::Null), sso_len_(0) { u_.i = 0; }
    JsonValue(bool v) noexcept : kind_(Type::Bool), sso_len_(0) { u_.i = 0; u_.b = v; }
    JsonValue(int v) noexcept : kind_(Type::Integer), sso_len_(0) { u_.i = static_cast<int64_t>(v); }
    JsonValue(int64_t v) noexcept : kind_(Type::Integer), sso_len_(0) { u_.i = v; }
    JsonValue(unsigned v) noexcept : kind_(Type::Integer), sso_len_(0) { u_.i = static_cast<int64_t>(v); }
    JsonValue(uint64_t v) noexcept : kind_(Type::UInteger), sso_len_(0) { u_.u = v; }
    JsonValue(double v) noexcept : kind_(Type::Float), sso_len_(0) { u_.d = v; }
    JsonValue(const char* v) : kind_(Type::Null), sso_len_(0) {
        u_.i = 0;
        if (JSON_UNLIKELY(!v)) return;
        kind_ = Type::String;
        init_string(v, std::strlen(v));
    }
    JsonValue(std::string_view v) : kind_(Type::String) { init_string(v.data(), v.size()); }
    JsonValue(const std::string& v) : kind_(Type::String) { init_string(v.data(), v.size()); }
    JsonValue(std::string&& v) : kind_(Type::String) { init_string_move(std::move(v)); }

    JsonValue(const Array& v) : kind_(Type::Array), sso_len_(0) {
        auto* arena = detail::current_arena;
        if (JSON_UNLIKELY(arena != nullptr)) {
            u_.arr = arena->construct<Array>(v, arena);
            pad_[0] |= kArenaFlag;
        } else {
            u_.arr = new Array(v);
        }
    }
    JsonValue(Array&& v) : kind_(Type::Array), sso_len_(0) {
        auto* arena = detail::current_arena;
        if (JSON_UNLIKELY(arena != nullptr)) {
            u_.arr = arena->construct<Array>(std::move(v), arena);
            pad_[0] |= kArenaFlag;
        } else {
            u_.arr = new Array(std::move(v));
        }
    }
    JsonValue(const Object& v) : kind_(Type::Object), sso_len_(0) {
        auto* arena = detail::current_arena;
        if (JSON_UNLIKELY(arena != nullptr)) {
            u_.obj = arena->construct<Object>(v);
            pad_[0] |= kArenaFlag;
        } else {
            u_.obj = new Object(v);
        }
    }
    JsonValue(Object&& v) : kind_(Type::Object), sso_len_(0) {
        auto* arena = detail::current_arena;
        if (JSON_UNLIKELY(arena != nullptr)) {
            u_.obj = arena->construct<Object>(std::move(v));
            pad_[0] |= kArenaFlag;
        } else {
            u_.obj = new Object(std::move(v));
        }
    }

    /// @brief Parser-private constructors: accept a pre-cached arena pointer
    /// to avoid repeated TLS lookups (detail::current_arena) during parsing.
    /// These are used exclusively by the Parser class via make_array/make_object.
    JsonValue(Array&& v, MonotonicArena* arena) : kind_(Type::Array), sso_len_(0) {
        if (arena != nullptr) {
            u_.arr = arena->construct<Array>(std::move(v), arena);
            pad_[0] |= kArenaFlag;
        } else {
            u_.arr = new Array(std::move(v));
        }
    }
    JsonValue(Object&& v, MonotonicArena* arena) : kind_(Type::Object), sso_len_(0) {
        if (arena != nullptr) {
            u_.obj = arena->construct<Object>(std::move(v));
            pad_[0] |= kArenaFlag;
        } else {
            u_.obj = new Object(std::move(v));
        }
    }

    JsonValue(const JsonValue& o) : kind_(o.kind_), sso_len_(o.sso_len_) {
        copy_payload(o);
    }
    JsonValue(JsonValue&& o) noexcept : kind_(o.kind_), sso_len_(o.sso_len_) {
        std::memcpy(pad_, o.pad_, sizeof(pad_));
        std::memcpy(&u_, &o.u_, sizeof(u_));
        o.kind_ = Type::Null;  // Only this is needed for destroy() to be a no-op
    }
    JsonValue& operator=(const JsonValue& o) {
        if (this != &o) { JsonValue tmp(o); swap(tmp); }
        return *this;
    }
    JsonValue& operator=(JsonValue&& o) noexcept {
        if (this != &o) {
            destroy();
            kind_ = o.kind_;
            sso_len_ = o.sso_len_;
            std::memcpy(pad_, o.pad_, sizeof(pad_));
            std::memcpy(&u_, &o.u_, sizeof(u_));
            o.kind_ = Type::Null;  // Only this is needed for destroy() to be a no-op
        }
        return *this;
    }
    ~JsonValue() { destroy(); }

    void swap(JsonValue& o) noexcept {
        constexpr auto S = sizeof(JsonValue);
        alignas(alignof(JsonValue)) char tmp[S];
        auto* a = reinterpret_cast<char*>(this);
        auto* b = reinterpret_cast<char*>(&o);
        std::memcpy(tmp, a, S);
        std::memcpy(a, b, S);
        std::memcpy(b, tmp, S);
    }

    [[nodiscard]] static JsonValue array() { return JsonValue(Array(detail::current_resource())); }
    [[nodiscard]] static JsonValue object() { return JsonValue(Object(detail::current_resource())); }

    [[nodiscard]] Type type() const noexcept { return kind_; }
    [[nodiscard]] bool is_null()    const noexcept { return kind_ == Type::Null; }
    [[nodiscard]] bool is_bool()    const noexcept { return kind_ == Type::Bool; }
    [[nodiscard]] bool is_integer() const noexcept { return kind_ == Type::Integer; }
    [[nodiscard]] bool is_float()   const noexcept { return kind_ == Type::Float; }
    [[nodiscard]] bool is_string()  const noexcept { return kind_ == Type::String; }
    [[nodiscard]] bool is_array()   const noexcept { return kind_ == Type::Array; }
    [[nodiscard]] bool is_object()   const noexcept { return kind_ == Type::Object; }
    [[nodiscard]] bool is_uinteger() const noexcept { return kind_ == Type::UInteger; }
    [[nodiscard]] bool is_number()   const noexcept { return is_integer() || is_uinteger() || is_float(); }

    bool as_bool() const {
        if (JSON_UNLIKELY(!is_bool()))
            throw TypeError("expected bool, got " + std::string(type_name(type())));
        return u_.b;
    }
    int64_t as_integer() const {
        if (is_integer()) return u_.i;
        if (is_uinteger() && u_.u <= static_cast<uint64_t>(INT64_MAX))
            return static_cast<int64_t>(u_.u);
        throw TypeError("expected integer, got " + std::string(type_name(type())));
    }
    uint64_t as_uinteger() const {
        if (is_uinteger()) return u_.u;
        if (is_integer() && u_.i >= 0) return static_cast<uint64_t>(u_.i);
        throw TypeError("expected uinteger, got " + std::string(type_name(type())));
    }
    double as_float() const {
        if (is_float()) return u_.d;
        if (is_integer()) return static_cast<double>(u_.i);
        if (is_uinteger()) return static_cast<double>(u_.u);
        throw TypeError("expected number, got " + std::string(type_name(type())));
    }
    double as_number() const { return as_float(); }

    [[nodiscard]] std::string_view as_string_view() const {
        if (JSON_UNLIKELY(!is_string()))
            throw TypeError("expected string, got " + std::string(type_name(type())));
        return str_view();
    }
    [[nodiscard]] std::string as_string() const {
        return std::string(as_string_view());
    }

    [[nodiscard]] const Array& as_array() const {
        if (JSON_UNLIKELY(!is_array()))
            throw TypeError("expected array, got " + std::string(type_name(type())));
        return *u_.arr;
    }
    Array& as_array() {
        if (JSON_UNLIKELY(!is_array()))
            throw TypeError("expected array, got " + std::string(type_name(type())));
        return *u_.arr;
    }
    [[nodiscard]] const Object& as_object() const {
        if (JSON_UNLIKELY(!is_object()))
            throw TypeError("expected object, got " + std::string(type_name(type())));
        return *u_.obj;
    }
    Object& as_object() {
        if (JSON_UNLIKELY(!is_object()))
            throw TypeError("expected object, got " + std::string(type_name(type())));
        return *u_.obj;
    }

    template <typename T>
    [[nodiscard]] T get() const {
        if constexpr (std::is_same_v<T, bool>) return as_bool();
        else if constexpr (std::is_same_v<T, uint64_t>) return as_uinteger();
        else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, int> || std::is_same_v<T, long>)
            return static_cast<T>(as_integer());
        else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>)
            return static_cast<T>(as_float());
        else if constexpr (std::is_same_v<T, std::string>) return as_string();
        else if constexpr (std::is_same_v<T, std::string_view>) return as_string_view();
        else static_assert(sizeof(T) == 0, "Unsupported type for get<T>()");
    }
    /// Type-safe value access with fallback — no exceptions, no overhead.
    template <typename T>
    [[nodiscard]] T get_or(const T& dv) const noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            return is_bool() ? u_.b : dv;
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            if (is_uinteger()) return u_.u;
            if (is_integer() && u_.i >= 0) return static_cast<uint64_t>(u_.i);
            return dv;
        } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, int> || std::is_same_v<T, long>) {
            if (is_integer()) return static_cast<T>(u_.i);
            if (is_uinteger() && u_.u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return static_cast<T>(u_.u);
            return dv;
        } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>) {
            if (is_float()) return static_cast<T>(u_.d);
            if (is_integer()) return static_cast<T>(u_.i);
            if (is_uinteger()) return static_cast<T>(u_.u);
            return dv;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return is_string() ? std::string(str_view()) : dv;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return is_string() ? str_view() : dv;
        } else {
            static_assert(sizeof(T) == 0, "Unsupported type for get_or<T>()");
        }
    }

    JsonValue& operator[](size_t index) {
        auto& a = as_array();
        if (JSON_UNLIKELY(index >= a.size()))
            throw OutOfRangeError("array index " + std::to_string(index) + " out of range (size=" + std::to_string(a.size()) + ")");
        return a[index];
    }
    const JsonValue& operator[](size_t index) const {
        const auto& a = as_array();
        if (JSON_UNLIKELY(index >= a.size()))
            throw OutOfRangeError("array index " + std::to_string(index) + " out of range (size=" + std::to_string(a.size()) + ")");
        return a[index];
    }
    JsonValue& operator[](int index) { return operator[](static_cast<size_t>(index)); }
    const JsonValue& operator[](int index) const { return operator[](static_cast<size_t>(index)); }

    JsonValue& operator[](const std::string& key) {
        if (JSON_UNLIKELY(!is_object()))
            throw TypeError("expected object, got " + std::string(type_name(type())));
        return (*u_.obj)[key];
    }
    const JsonValue& operator[](const std::string& key) const {
        const auto& obj = as_object();
        const auto* p = obj.find(key);
        if (JSON_UNLIKELY(!p)) throw OutOfRangeError("key not found: \"" + key + "\"");
        return *p;
    }
    JsonValue& operator[](const char* key) { return operator[](std::string_view(key)); }
    const JsonValue& operator[](const char* key) const { return operator[](std::string_view(key)); }
    JsonValue& operator[](std::string_view key) {
        if (JSON_UNLIKELY(!is_object()))
            throw TypeError("expected object, got " + std::string(type_name(type())));
        return (*u_.obj)[key];
    }
    const JsonValue& operator[](std::string_view key) const {
        const auto& obj = as_object();
        const auto* p = obj.find(key);
        if (JSON_UNLIKELY(!p)) throw OutOfRangeError("key not found: \"" + std::string(key) + "\"");
        return *p;
    }

    [[nodiscard]] bool contains(std::string_view key) const {
        return is_object() && u_.obj->contains(key);
    }
    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        return is_object() ? u_.obj->find(key) : nullptr;
    }
    [[nodiscard]] JsonValue* find(std::string_view key) {
        return is_object() ? u_.obj->find(key) : nullptr;
    }

    [[nodiscard]] size_t size() const noexcept {
        if (is_array())  return u_.arr->size();
        if (is_object()) return u_.obj->size();
        return 0;
    }
    [[nodiscard]] bool empty() const noexcept {
        if (is_null()) return true;
        if (is_array())  return u_.arr->empty();
        if (is_object()) return u_.obj->empty();
        return false;
    }

    void push_back(const JsonValue& v) { as_array().push_back(v); }
    void push_back(JsonValue&& v)      { as_array().push_back(std::move(v)); }
    template <typename... Args>
    JsonValue& emplace_back(Args&&... args) {
        return as_array().emplace_back(std::forward<Args>(args)...);
    }

    void insert(std::string_view key, const JsonValue& v) {
        as_object().insert(std::string(key), JsonValue(v));
    }
    void insert(std::string key, JsonValue&& v) {
        as_object().insert(std::move(key), std::move(v));
    }
    bool erase(std::string_view key) { return as_object().erase(key); }
    void clear() {
        if (is_array())  { u_.arr->clear(); return; }
        if (is_object()) { u_.obj->clear(); return; }
    }

    [[nodiscard]] bool operator==(const JsonValue& other) const {
        if (kind_ != other.kind_) {
            if (is_number() && other.is_number()) {
                // Exact int/uint comparison without double-precision loss
                if ((is_integer() && other.is_uinteger()) || (is_uinteger() && other.is_integer())) {
                    int64_t  sv = is_integer()  ? u_.i : other.u_.i;
                    uint64_t uv = is_uinteger() ? u_.u : other.u_.u;
                    return sv >= 0 && static_cast<uint64_t>(sv) == uv;
                }
                return as_float() == other.as_float();
            }
            return false;
        }
        switch (kind_) {
            case Type::Null:    return true;
            case Type::Bool:    return u_.b == other.u_.b;
            case Type::Integer:  return u_.i == other.u_.i;
            case Type::UInteger: return u_.u == other.u_.u;
            case Type::Float:   return u_.d == other.u_.d;
            case Type::String:  return str_view() == other.str_view();
            case Type::Array:   return *u_.arr == *other.u_.arr;
            case Type::Object:  return *u_.obj == *other.u_.obj;
        }
        return false;
    }
    [[nodiscard]] bool operator!=(const JsonValue& other) const { return !(*this == other); }

    [[nodiscard]] std::string dump(int indent = -1) const;
    [[nodiscard]] std::string dump(const struct SerializeOptions& opts) const;

private:
    Type kind_;
    uint8_t sso_len_;
    uint8_t pad_[6] = {};
    union Payload {
        bool b; int64_t i; uint64_t u; double d;
        char sso_buf[16];
        std::string* str_ptr;
        const char* arena_str;  ///< For arena-allocated strings: raw char* into arena
        Array* arr;
        Object* obj;
    } u_;

    static constexpr size_t kSsoMax = 15;
    static constexpr uint8_t kHeapTag = 0xFF;
    static constexpr uint8_t kArenaFlag = 0x01;

    bool is_sso() const noexcept { return sso_len_ != kHeapTag; }

    /// Check if this value has arena-allocated payload.
    bool is_arena() const noexcept { return pad_[0] & kArenaFlag; }

    /// Store a raw arena string: pointer + length packed into pad_[1..4].
    void set_arena_str(const char* p, uint32_t len) noexcept {
        pad_[0] |= kArenaFlag;
        std::memcpy(&pad_[1], &len, sizeof(uint32_t));
        u_.arena_str = p;
    }

    /// Read the length of an arena-allocated string from pad_[1..4].
    uint32_t arena_str_len() const noexcept {
        uint32_t len;
        std::memcpy(&len, &pad_[1], sizeof(uint32_t));
        return len;
    }

    std::string_view str_view() const noexcept {
        if (is_sso()) return {u_.sso_buf, sso_len_};
        if (JSON_UNLIKELY(is_arena()))
            return {u_.arena_str, arena_str_len()};
        return {u_.str_ptr->data(), u_.str_ptr->size()};
    }

    void init_string(const char* s, size_t len) {
        if (len <= kSsoMax) {
            sso_len_ = static_cast<uint8_t>(len);
            std::memcpy(u_.sso_buf, s, len);
            u_.sso_buf[len] = '\0';
        } else if (JSON_UNLIKELY(detail::current_arena != nullptr)) {
            sso_len_ = kHeapTag;
            auto* buf = static_cast<char*>(
                detail::current_arena->allocate(len, 1));
            std::memcpy(buf, s, len);
            set_arena_str(buf, static_cast<uint32_t>(len));
        } else {
            sso_len_ = kHeapTag;
            u_.str_ptr = new std::string(s, len);
        }
    }

    void init_string_move(std::string&& s) {
        const size_t len = s.size();
        if (len <= kSsoMax) {
            sso_len_ = static_cast<uint8_t>(len);
            std::memcpy(u_.sso_buf, s.data(), len);
            u_.sso_buf[len] = '\0';
        } else if (JSON_UNLIKELY(detail::current_arena != nullptr)) {
            sso_len_ = kHeapTag;
            auto* buf = static_cast<char*>(
                detail::current_arena->allocate(len, 1));
            std::memcpy(buf, s.data(), len);
            set_arena_str(buf, static_cast<uint32_t>(len));
        } else {
            sso_len_ = kHeapTag;
            u_.str_ptr = new std::string(std::move(s));
        }
    }

    void copy_payload(const JsonValue& o) {
        auto* arena = detail::current_arena;
        auto* mr = detail::current_resource();
        switch (o.kind_) {
            case Type::String:
                if (o.is_sso()) {
                    std::memcpy(u_.sso_buf, o.u_.sso_buf, sizeof(u_.sso_buf));
                } else {
                    auto sv = o.str_view();
                    if (JSON_UNLIKELY(arena != nullptr)) {
                        sso_len_ = kHeapTag;
                        auto* buf = static_cast<char*>(arena->allocate(sv.size(), 1));
                        std::memcpy(buf, sv.data(), sv.size());
                        set_arena_str(buf, static_cast<uint32_t>(sv.size()));
                    } else {
                        u_.str_ptr = new std::string(sv.data(), sv.size());
                    }
                }
                break;
            case Type::Array:
                if (JSON_UNLIKELY(arena != nullptr)) {
                    u_.arr = arena->construct<Array>(o.u_.arr->begin(), o.u_.arr->end(), mr);
                    pad_[0] |= kArenaFlag;
                } else {
                    u_.arr = new Array(*o.u_.arr);
                }
                break;
            case Type::Object:
                if (JSON_UNLIKELY(arena != nullptr)) {
                    u_.obj = arena->construct<Object>(*o.u_.obj);
                    pad_[0] |= kArenaFlag;
                } else {
                    u_.obj = new Object(*o.u_.obj);
                }
                break;
            default:
                std::memcpy(&u_, &o.u_, sizeof(u_));
                break;
        }
    }

    void destroy() noexcept {
        const bool arena = is_arena();
        switch (kind_) {
            case Type::String:
                // Arena strings: raw char* in arena, nothing to free.
                // Heap strings: delete the std::string object.
                if (!is_sso() && !arena) delete u_.str_ptr;
                break;
            case Type::Array:
                if (arena) {
                    // Skip destructor if vector is in moved-from state (avoids crash on reset).
                    if (u_.arr->data() != nullptr || u_.arr->size() == 0)
                        u_.arr->~Array();
                } else {
                    delete u_.arr;
                }
                break;
            case Type::Object:
                if (arena) u_.obj->~Object();
                else       delete u_.obj;
                break;
            default: break;
        }
    }
public:
    static_assert(sizeof(Type) == 1, "Type enum must be 1 byte");
};

static_assert(sizeof(JsonValue) == 24, "JsonValue must be exactly 24 bytes");

// ─── Object special member functions ─────────────────────────────────────

inline Object::~Object() = default;
inline Object::Object(const Object& o)
    : entries(o.entries, o.get_resource()) {}
inline Object::Object(Object&& o) noexcept
    : entries(std::move(o.entries)), index_(std::move(o.index_)) {}
inline Object& Object::operator=(const Object& o) {
    if (this != &o) { entries = o.entries; index_.reset(); }
    return *this;
}
inline Object& Object::operator=(Object&& o) noexcept {
    if (this != &o) { entries = std::move(o.entries); index_ = std::move(o.index_); }
    return *this;
}
inline Object::Object(std::initializer_list<std::pair<std::string, JsonValue>> init)
    : entries(init.begin(), init.end(), std::pmr::new_delete_resource()) {}

inline void Object::ensure_index() const { if (use_index() && !index_) rebuild_index(); }
inline void Object::rebuild_index() const {
    auto* mr = entries.get_allocator().resource();
    if (!index_) {
        index_ = std::make_unique<index_type>(
            entries.size() * 2,
            detail::StringHash{},
            detail::StringEqual{},
            mr);
    } else {
        index_->clear();
    }
    for (size_type i = 0; i < entries.size(); ++i)
        (*index_)[std::string_view(entries[i].first)] = i;
}
inline void Object::update_index_after_push(const void* old_data) {
    if (entries.data() != old_data) {
        // Reallocation happened — all string_view keys are dangling.
        // Rebuild index from scratch (reuses existing hash map allocation).
        index_->clear();
        for (size_type i = 0; i < entries.size(); ++i)
            (*index_)[std::string_view(entries[i].first)] = i;
    } else {
        // No reallocation — O(1) incremental update.
        (*index_)[std::string_view(entries.back().first)] = entries.size() - 1;
    }
}
template <typename K, typename V>
void Object::emplace_back(K&& key, V&& value) {
    const auto* old_data = entries.data();
    entries.emplace_back(std::forward<K>(key), std::forward<V>(value));
    if (index_) update_index_after_push(old_data);
}
inline JsonValue* Object::find(std::string_view key) noexcept {
    if (use_index()) {
        ensure_index();
        auto it = index_->find(key);  // O(1), zero allocations
        if (it != index_->end()) return &entries[it->second].second;
        return nullptr;
    }
    for (auto& [k, v] : entries) if (k == key) return &v;
    return nullptr;
}
inline const JsonValue* Object::find(std::string_view key) const noexcept {
    if (use_index()) {
        ensure_index();
        auto it = index_->find(key);  // O(1), zero allocations
        if (it != index_->end()) return &entries[it->second].second;
        return nullptr;
    }
    for (const auto& [k, v] : entries) if (k == key) return &v;
    return nullptr;
}
inline bool Object::contains(std::string_view key) const noexcept { return find(key) != nullptr; }
inline JsonValue& Object::operator[](std::string_view key) {
    auto* p = find(key);
    if (p) return *p;
    const auto* old_data = entries.data();
    entries.emplace_back(std::string(key), JsonValue{});
    if (index_) update_index_after_push(old_data);
    return entries.back().second;
}
inline const JsonValue& Object::at(std::string_view key) const {
    auto* p = find(key);
    if (JSON_UNLIKELY(!p)) throw OutOfRangeError("key not found: \"" + std::string(key) + "\"");
    return *p;
}
inline void Object::insert(std::string key, JsonValue value) {
    if (entries.size() < kIndexThreshold) {
        for (auto& [k, v] : entries) { if (k == key) { v = std::move(value); return; } }
        entries.emplace_back(std::move(key), std::move(value));
    } else {
        ensure_index();
        auto it = index_->find(std::string_view(key));
        if (it != index_->end()) { entries[it->second].second = std::move(value); }
        else {
            const auto* old_data = entries.data();
            const size_type idx = entries.size();
            entries.emplace_back(std::move(key), std::move(value));
            if (entries.data() != old_data) {
                // Reallocation — all string_view keys are dangling. Rebuild.
                rebuild_index();
            } else {
                // No reallocation — O(1) incremental update.
                index_->emplace(std::string_view(entries.back().first), idx);
            }
        }
    }
}
inline bool Object::erase(std::string_view key) {
    if (use_index() && index_) {
        // Use hash index for O(1) lookup of the position.
        auto map_it = index_->find(key);
        if (map_it == index_->end()) return false;
        size_type idx = map_it->second;
        entries.erase(entries.begin() + static_cast<ptrdiff_t>(idx));
        // Indices shifted — must rebuild. (Keeps the hash map allocation.)
        rebuild_index();
        return true;
    }
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == key) { entries.erase(it); return true; }
    }
    return false;
}
inline bool Object::operator==(const Object& other) const {
    if (size() != other.size()) return false;
    // Key order does not matter for semantic comparison of JSON objects.
    // For each entry, look up the corresponding key in the other object.
    for (const auto& [key, val] : entries) {
        const auto* p = other.find(key);
        if (!p || *p != val) return false;
    }
    return true;
}

} // namespace yajson
