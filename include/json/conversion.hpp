#pragma once

/// @file conversion.hpp
/// @author Aleksandr Loshkarev
/// @brief ADL-based conversion system for C++ types <-> JSON.
///
/// Provides:
///   - to_json() / from_json() for basic types and STL containers
///   - to_value<T>() / from_value<T>() helper wrappers
///   - JSON_DEFINE_TYPE_NON_INTRUSIVE() macro for struct mapping
///   - JSON_DEFINE_TYPE_INTRUSIVE() macro for friend mapping
///
/// @example
/// @code
///   struct User { std::string name; int age; bool active; };
///   JSON_DEFINE_TYPE_NON_INTRUSIVE(User, name, age, active)
///
///   User u{"Alice", 30, true};
///   yajson::JsonValue j = yajson::to_value(u);
///   User u2 = yajson::from_value<User>(j);
/// @endcode

#include "value.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace yajson {

// =====================================================================
// to_json: C++ type -> JsonValue
// =====================================================================

inline void to_json(JsonValue& j, std::nullptr_t) { j = JsonValue(nullptr); }
inline void to_json(JsonValue& j, bool v)          { j = JsonValue(v); }
inline void to_json(JsonValue& j, int v)           { j = JsonValue(v); }
inline void to_json(JsonValue& j, unsigned v)       { j = JsonValue(v); }
inline void to_json(JsonValue& j, int64_t v)       { j = JsonValue(v); }
inline void to_json(JsonValue& j, uint64_t v)      { j = JsonValue(v); }
inline void to_json(JsonValue& j, float v)         { j = JsonValue(static_cast<double>(v)); }
inline void to_json(JsonValue& j, double v)        { j = JsonValue(v); }
inline void to_json(JsonValue& j, const std::string& v) { j = JsonValue(v); }
inline void to_json(JsonValue& j, std::string_view v)   { j = JsonValue(v); }
inline void to_json(JsonValue& j, const char* v)        { j = JsonValue(v); }
inline void to_json(JsonValue& j, const JsonValue& v)   { j = v; }

template <typename T>
void to_json(JsonValue& j, const std::vector<T>& vec) {
    Array arr(detail::current_resource());
    arr.reserve(vec.size());
    for (const auto& elem : vec) {
        JsonValue tmp;
        to_json(tmp, elem);
        arr.push_back(std::move(tmp));
    }
    j = JsonValue(std::move(arr));
}

template <typename T>
void to_json(JsonValue& j, const std::map<std::string, T>& m) {
    Object obj(detail::current_resource());
    obj.reserve(m.size());
    for (const auto& [key, val] : m) {
        JsonValue tmp;
        to_json(tmp, val);
        obj.emplace_back(key, std::move(tmp));
    }
    j = JsonValue(std::move(obj));
}

template <typename T>
void to_json(JsonValue& j, const std::unordered_map<std::string, T>& m) {
    Object obj(detail::current_resource());
    obj.reserve(m.size());
    for (const auto& [key, val] : m) {
        JsonValue tmp;
        to_json(tmp, val);
        obj.emplace_back(key, std::move(tmp));
    }
    j = JsonValue(std::move(obj));
}

template <typename T>
void to_json(JsonValue& j, const std::optional<T>& opt) {
    if (opt.has_value()) {
        to_json(j, *opt);
    } else {
        j = JsonValue(nullptr);
    }
}

// =====================================================================
// from_json: JsonValue -> C++ type
// =====================================================================

inline void from_json(const JsonValue& j, bool& v)         { v = j.as_bool(); }
inline void from_json(const JsonValue& j, int& v)          { v = static_cast<int>(j.as_integer()); }
inline void from_json(const JsonValue& j, unsigned& v)      { v = static_cast<unsigned>(j.as_integer()); }
inline void from_json(const JsonValue& j, int64_t& v)      { v = j.as_integer(); }
inline void from_json(const JsonValue& j, uint64_t& v)     { v = j.as_uinteger(); }
inline void from_json(const JsonValue& j, float& v)        { v = static_cast<float>(j.as_float()); }
inline void from_json(const JsonValue& j, double& v)       { v = j.as_float(); }
inline void from_json(const JsonValue& j, std::string& v)  { v = j.as_string(); }
inline void from_json(const JsonValue& j, JsonValue& v)    { v = j; }

template <typename T>
void from_json(const JsonValue& j, std::vector<T>& vec) {
    const auto& arr = j.as_array();
    vec.clear();
    vec.reserve(arr.size());
    for (const auto& elem : arr) {
        T val{};
        from_json(elem, val);
        vec.push_back(std::move(val));
    }
}

template <typename T>
void from_json(const JsonValue& j, std::map<std::string, T>& m) {
    const auto& obj = j.as_object();
    m.clear();
    for (const auto& [key, val] : obj) {
        T v{};
        from_json(val, v);
        m.emplace(key, std::move(v));
    }
}

template <typename T>
void from_json(const JsonValue& j, std::unordered_map<std::string, T>& m) {
    const auto& obj = j.as_object();
    m.clear();
    for (const auto& [key, val] : obj) {
        T v{};
        from_json(val, v);
        m.emplace(key, std::move(v));
    }
}

template <typename T>
void from_json(const JsonValue& j, std::optional<T>& opt) {
    if (j.is_null()) {
        opt = std::nullopt;
    } else {
        T val{};
        from_json(j, val);
        opt = std::move(val);
    }
}

// =====================================================================
// Helper wrappers
// =====================================================================

/// C++ value -> JsonValue (ADL finds to_json).
template <typename T>
[[nodiscard]] JsonValue to_value(const T& val) {
    JsonValue j;
    to_json(j, val);
    return j;
}

/// JsonValue -> C++ value (T must be default-constructible).
template <typename T>
[[nodiscard]] T from_value(const JsonValue& j) {
    T val{};
    from_json(j, val);
    return val;
}

/// JsonValue -> C++ value with fallback on error.
template <typename T>
[[nodiscard]] T from_value_or(const JsonValue& j, const T& d) noexcept {
    try {
        T v{};
        from_json(j, v);
        return v;
    } catch (...) {
        return d;
    }
}

} // namespace yajson

// =====================================================================
// Preprocessor FOREACH utilities (support up to 20 fields)
// =====================================================================

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define JSON_PP_CAT_I(a, b) a##b
#define JSON_PP_CAT(a, b) JSON_PP_CAT_I(a, b)

#define JSON_PP_NARG_I(...) \
    JSON_PP_ARG_N(__VA_ARGS__, \
    20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define JSON_PP_ARG_N( \
    _1,_2,_3,_4,_5,_6,_7,_8,_9,_10, \
    _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, N,...) N

#define JSON_PP_FE_1(m,x) m(x)
#define JSON_PP_FE_2(m,x,...) m(x) JSON_PP_FE_1(m,__VA_ARGS__)
#define JSON_PP_FE_3(m,x,...) m(x) JSON_PP_FE_2(m,__VA_ARGS__)
#define JSON_PP_FE_4(m,x,...) m(x) JSON_PP_FE_3(m,__VA_ARGS__)
#define JSON_PP_FE_5(m,x,...) m(x) JSON_PP_FE_4(m,__VA_ARGS__)
#define JSON_PP_FE_6(m,x,...) m(x) JSON_PP_FE_5(m,__VA_ARGS__)
#define JSON_PP_FE_7(m,x,...) m(x) JSON_PP_FE_6(m,__VA_ARGS__)
#define JSON_PP_FE_8(m,x,...) m(x) JSON_PP_FE_7(m,__VA_ARGS__)
#define JSON_PP_FE_9(m,x,...) m(x) JSON_PP_FE_8(m,__VA_ARGS__)
#define JSON_PP_FE_10(m,x,...) m(x) JSON_PP_FE_9(m,__VA_ARGS__)
#define JSON_PP_FE_11(m,x,...) m(x) JSON_PP_FE_10(m,__VA_ARGS__)
#define JSON_PP_FE_12(m,x,...) m(x) JSON_PP_FE_11(m,__VA_ARGS__)
#define JSON_PP_FE_13(m,x,...) m(x) JSON_PP_FE_12(m,__VA_ARGS__)
#define JSON_PP_FE_14(m,x,...) m(x) JSON_PP_FE_13(m,__VA_ARGS__)
#define JSON_PP_FE_15(m,x,...) m(x) JSON_PP_FE_14(m,__VA_ARGS__)
#define JSON_PP_FE_16(m,x,...) m(x) JSON_PP_FE_15(m,__VA_ARGS__)
#define JSON_PP_FE_17(m,x,...) m(x) JSON_PP_FE_16(m,__VA_ARGS__)
#define JSON_PP_FE_18(m,x,...) m(x) JSON_PP_FE_17(m,__VA_ARGS__)
#define JSON_PP_FE_19(m,x,...) m(x) JSON_PP_FE_18(m,__VA_ARGS__)
#define JSON_PP_FE_20(m,x,...) m(x) JSON_PP_FE_19(m,__VA_ARGS__)

#define JSON_PP_FOREACH(m,...) \
    JSON_PP_CAT(JSON_PP_FE_, JSON_PP_NARG_I(__VA_ARGS__))(m, __VA_ARGS__)

// Field-level macros for struct mapping
#define JSON_DETAIL_TO_FIELD(fld) \
    { ::yajson::JsonValue _jt; to_json(_jt, v.fld); j[#fld] = std::move(_jt); }
#define JSON_DETAIL_FROM_FIELD(fld) \
    { from_json(j[#fld], v.fld); }

/// Non-intrusive: use in the same namespace as the type.
#define JSON_DEFINE_TYPE_NON_INTRUSIVE(Type, ...) \
    inline void to_json(::yajson::JsonValue& j, const Type& v) { \
        j = ::yajson::JsonValue::object(); \
        JSON_PP_FOREACH(JSON_DETAIL_TO_FIELD, __VA_ARGS__) \
    } \
    inline void from_json(const ::yajson::JsonValue& j, Type& v) { \
        JSON_PP_FOREACH(JSON_DETAIL_FROM_FIELD, __VA_ARGS__) \
    }

/// Intrusive: use inside the class/struct body.
#define JSON_DEFINE_TYPE_INTRUSIVE(Type, ...) \
    friend void to_json(::yajson::JsonValue& j, const Type& v) { \
        j = ::yajson::JsonValue::object(); \
        JSON_PP_FOREACH(JSON_DETAIL_TO_FIELD, __VA_ARGS__) \
    } \
    friend void from_json(const ::yajson::JsonValue& j, Type& v) { \
        JSON_PP_FOREACH(JSON_DETAIL_FROM_FIELD, __VA_ARGS__) \
    }

// NOLINTEND(cppcoreguidelines-macro-usage)
