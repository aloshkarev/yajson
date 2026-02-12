#pragma once

/// @file allocator.hpp
/// @author Aleksandr Loshkarev
/// @brief PMR (Polymorphic Memory Resource) support for yajson.
///
/// Provides memory_resource integration for parser/serializer internal
/// buffers. Use monotonic_buffer_resource for fastest parsing.
///
/// @example
/// @code
///   char buf[4096];
///   std::pmr::monotonic_buffer_resource mr(buf, sizeof(buf));
///   auto val = yajson::parse(input, yajson::ParseOptions{}, &mr);
/// @endcode

#include "config.hpp"

#if YAJSON_HAS_PMR
#include <memory_resource>
#endif

namespace yajson {

#if YAJSON_HAS_PMR

/// @brief Alias for std::pmr::memory_resource.
using memory_resource = std::pmr::memory_resource;

/// @brief Get default memory resource for yajson.
/// Falls back to std::pmr::get_default_resource().
inline memory_resource* get_default_resource() noexcept {
    return std::pmr::get_default_resource();
}

/// @brief RAII helper to use a custom memory resource in a scope.
class ScopedResource {
public:
    explicit ScopedResource(memory_resource* mr) noexcept
        : prev_(std::pmr::get_default_resource()) {
        std::pmr::set_default_resource(mr);
    }

    ~ScopedResource() {
        std::pmr::set_default_resource(prev_);
    }

    ScopedResource(const ScopedResource&) = delete;
    ScopedResource& operator=(const ScopedResource&) = delete;

private:
    memory_resource* prev_;
};

/// @brief PMR string type.
using pmr_string = std::pmr::string;

/// @brief PMR vector type.
template <typename T>
using pmr_vector = std::pmr::vector<T>;

#else

// Fallback: no PMR support — provide type aliases that compile
using memory_resource = void;

inline void* get_default_resource() noexcept { return nullptr; }

#endif // YAJSON_HAS_PMR

} // namespace yajson
