#pragma once

/// @file config.hpp
/// @author Aleksandr Loshkarev
/// @brief Configuration macros for yajson library.
///
/// Controls:
///   - Header-only vs compiled mode
///   - SIMD support
///   - Platform detection
///   - Branch prediction hints
///   - PMR allocator availability

// =====================================================================
// Header-only / compiled mode
// =====================================================================
// By default the library is header-only.
// Define YAJSON_SEPARATE_COMPILATION to compile as a static/shared lib.
// In exactly one .cpp file, define YAJSON_IMPLEMENTATION before including.

#if defined(YAJSON_SEPARATE_COMPILATION)
    #define YAJSON_DECL
    #if defined(YAJSON_IMPLEMENTATION)
        #define YAJSON_INLINE
    #else
        #define YAJSON_INLINE extern
    #endif
#else
    #define YAJSON_DECL   inline
    #define YAJSON_INLINE inline
#endif

// =====================================================================
// Branch prediction hints
// =====================================================================

#if defined(__GNUC__) || defined(__clang__)
    #define JSON_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define JSON_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define JSON_NOINLINE    __attribute__((noinline))
    #define JSON_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define JSON_LIKELY(x)   (x)
    #define JSON_UNLIKELY(x) (x)
    #define JSON_NOINLINE    __declspec(noinline)
    #define JSON_ALWAYS_INLINE __forceinline
#else
    #define JSON_LIKELY(x)   (x)
    #define JSON_UNLIKELY(x) (x)
    #define JSON_NOINLINE
    #define JSON_ALWAYS_INLINE inline
#endif

// =====================================================================
// Platform detection
// =====================================================================

#if defined(_MSC_VER)
    #define YAJSON_MSVC 1
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    #define YAJSON_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define YAJSON_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
    #define YAJSON_ARM32 1
#endif

// =====================================================================
// PMR availability (C++17 <memory_resource>)
// =====================================================================
// Most C++17 compilers support PMR. Disable with YAJSON_NO_PMR.

#if !defined(YAJSON_NO_PMR)
    #if __has_include(<memory_resource>)
        #define YAJSON_HAS_PMR 1
    #else
        #define YAJSON_HAS_PMR 0
    #endif
#else
    #define YAJSON_HAS_PMR 0
#endif

// =====================================================================
// Recursion depth limit (stack overflow protection)
// =====================================================================

#if !defined(YAJSON_MAX_DEPTH)
    #define YAJSON_MAX_DEPTH 512
#endif

// =====================================================================
// Small object threshold for linear vs hash lookup
// =====================================================================

#if !defined(YAJSON_OBJECT_LINEAR_THRESHOLD)
    #define YAJSON_OBJECT_LINEAR_THRESHOLD 16
#endif
