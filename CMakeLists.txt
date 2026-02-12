cmake_minimum_required(VERSION 3.10)
project(yajson
    VERSION 1.0.0
    DESCRIPTION "High-performance thread-safe C++17 JSON library"
    LANGUAGES CXX
)

# ─── Standard and general settings ───────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Options ─────────────────────────────────────────────────────────────────
option(YAJSON_BUILD_TESTS        "Build unit tests (Google Test)"                      ON)
option(YAJSON_BUILD_BENCHMARKS   "Build benchmarks (Google Benchmark)"                 ON)
option(YAJSON_ENABLE_SIMD        "Enable SIMD optimizations"                           ON)
option(YAJSON_NATIVE_ARCH        "Add -march=native to interface (enables AVX2/SSE4.2 for consumers)" OFF)
option(YAJSON_BENCH_BOOST_JSON   "Build comparative benchmarks vs Boost.JSON (fetches ~90MB)" OFF)
option(YAJSON_BENCH_COMPARE      "Build full comparative benchmarks (yajson, Boost.JSON, RapidJSON, nlohmann, simdjson)" OFF)

# ─── Header-only library (INTERFACE) ─────────────────────────────────────────
add_library(yajson INTERFACE)
target_include_directories(yajson INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(yajson INTERFACE cxx_std_17)

# SIMD: enable native instructions for current platform
if(YAJSON_ENABLE_SIMD)
    target_compile_definitions(yajson INTERFACE YAJSON_SIMD_ENABLED)
endif()

if(YAJSON_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" YAJSON_HAS_MARCH_NATIVE)
    if(YAJSON_HAS_MARCH_NATIVE)
        target_compile_options(yajson INTERFACE -march=native)
        message(STATUS "yajson: -march=native enabled (AVX2/SSE4.2/NEON auto-detected)")
    elseif(MSVC)
        target_compile_options(yajson INTERFACE /arch:AVX2)
        message(STATUS "yajson: /arch:AVX2 enabled (MSVC)")
    else()
        message(WARNING "yajson: YAJSON_NATIVE_ARCH requested but -march=native not supported by compiler")
    endif()
endif()

# Thread safety — requires pthread on POSIX systems
find_package(Threads REQUIRED)
target_link_libraries(yajson INTERFACE Threads::Threads)

# ─── Test and benchmark dependencies (FetchContent) ─────────────────────────
include(FetchContent)

if(YAJSON_BUILD_TESTS OR YAJSON_BUILD_BENCHMARKS)
    # Google Test
    if(YAJSON_BUILD_TESTS)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    # Google Benchmark
    if(YAJSON_BUILD_BENCHMARKS)
        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.8.3
            GIT_SHALLOW    TRUE
        )
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    if(YAJSON_BENCH_COMPARE)
        set(YAJSON_BENCH_BOOST_JSON ON CACHE BOOL "" FORCE)
    endif()

    # Boost (for comparative benchmarks vs Boost.JSON)
    if(YAJSON_BENCH_BOOST_JSON)
        # Try system-installed Boost first (>= 1.75 for Boost.JSON)
        find_package(Boost 1.75 QUIET COMPONENTS json)
        if(Boost_FOUND)
            message(STATUS "Found system Boost.JSON ${Boost_VERSION}")
            set(YAJSON_BOOST_JSON_AVAILABLE TRUE)
        else()
            message(STATUS "System Boost.JSON not found, fetching Boost via FetchContent...")
            FetchContent_Declare(
                Boost
                URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz
            )
            # Only build the JSON library and its dependencies
            set(BOOST_INCLUDE_LIBRARIES json CACHE STRING "" FORCE)
            set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(Boost)
            set(YAJSON_BOOST_JSON_AVAILABLE TRUE)
        endif()
    endif()

    if(YAJSON_BENCH_COMPARE)
        # RapidJSON — header-only, fetch source only (avoid its complex CMake)
        FetchContent_Declare(
            rapidjson
            GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
            GIT_TAG        master
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(rapidjson)
        if(NOT rapidjson_POPULATED)
            FetchContent_Populate(rapidjson)
        endif()
        message(STATUS "RapidJSON source: ${rapidjson_SOURCE_DIR}")

        # nlohmann/json — header-only, clean CMake
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3
            GIT_SHALLOW    TRUE
        )
        set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
        set(JSON_Install OFF CACHE BOOL "" FORCE)
        set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(nlohmann_json)

        # simdjson — compiled library, clean CMake
        FetchContent_Declare(
            simdjson
            GIT_REPOSITORY https://github.com/simdjson/simdjson.git
            GIT_TAG        v3.10.1
            GIT_SHALLOW    TRUE
        )
        set(SIMDJSON_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(simdjson)

        set(YAJSON_COMPARE_AVAILABLE TRUE)
    endif()
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
if(YAJSON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Benchmarks ──────────────────────────────────────────────────────────────
if(YAJSON_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
install(DIRECTORY include/json DESTINATION include)
install(TARGETS yajson EXPORT yajson-targets)
install(EXPORT yajson-targets
    NAMESPACE yajson::
    DESTINATION lib/cmake/yajson
)
