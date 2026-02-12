# ─── Unit tests (Google Test) ──────────────────────────────────────────────────

set(TEST_SOURCES
    test_value.cpp
    test_parser.cpp
    test_serializer.cpp
    test_thread_safety.cpp
    test_utf8.cpp
    test_new_features.cpp
    test_conformance.cpp
    test_uint64_pointer_writer.cpp
    test_arena.cpp
    test_simd.cpp
)

add_executable(json_tests ${TEST_SOURCES})

target_link_libraries(json_tests PRIVATE
    yajson
    gtest
    gtest_main
)

# Aggressive warnings for maximum code quality
target_compile_options(json_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Optimization for Release test builds
target_compile_options(json_tests PRIVATE
    $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O2>>
)

include(GoogleTest)
gtest_discover_tests(json_tests)

add_executable(json_tests_simd_native test_simd.cpp)
target_link_libraries(json_tests_simd_native PRIVATE
    yajson
    gtest
    gtest_main
)
target_compile_options(json_tests_simd_native PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -O2 -march=native>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /arch:AVX2>
)
gtest_discover_tests(json_tests_simd_native TEST_PREFIX "native/")

# ─── clang-tidy with hl-tidy-ext plugin ──────────────────────────────────────
#
# Adds two targets:
#   hl_tidy_build  — builds the hl-tidy-ext plugin (HlTidyModule.so) from the git submodule
#   clang_tidy_report — runs clang-tidy with the plugin on all library headers, generates a report
#
# Usage:
#   cmake --build . --target clang_tidy_report
#
# The report is written to ${CMAKE_BINARY_DIR}/clang-tidy-report.txt
#

# Detect LLVM version used by hl-tidy-ext to pick the matching clang-tidy.
# The plugin must be loaded by the same LLVM version it was compiled against.
set(HL_TIDY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/hl-tidy-ext")
set(HL_TIDY_BUILD_DIR  "${CMAKE_BINARY_DIR}/hl-tidy-ext-build")
set(HL_TIDY_MODULE     "${HL_TIDY_BUILD_DIR}/HlTidyModule.so")
set(CLANG_TIDY_REPORT  "${CMAKE_BINARY_DIR}/clang-tidy-report.txt")

# User-overridable LLVM version for the plugin (default: 20)
set(HL_TIDY_LLVM_VERSION "20" CACHE STRING "LLVM version for hl-tidy-ext plugin (must match installed clang-tidy-N)")

# Prefer versioned clang-tidy-N to avoid ABI mismatches with the plugin
find_program(CLANG_TIDY_EXE NAMES
    "clang-tidy-${HL_TIDY_LLVM_VERSION}"
    clang-tidy
)

if(CLANG_TIDY_EXE AND EXISTS "${HL_TIDY_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
    message(STATUS "hl-tidy-ext source: ${HL_TIDY_SOURCE_DIR}")
    message(STATUS "hl-tidy-ext LLVM version: ${HL_TIDY_LLVM_VERSION}")

    # --- Build the plugin ---
    add_custom_command(
        OUTPUT  "${HL_TIDY_MODULE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${HL_TIDY_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -S "${HL_TIDY_SOURCE_DIR}"
            -B "${HL_TIDY_BUILD_DIR}"
            -DCMAKE_BUILD_TYPE=Release
            -DLLVM_VERSION=${HL_TIDY_LLVM_VERSION}
        COMMAND ${CMAKE_COMMAND}
            --build "${HL_TIDY_BUILD_DIR}"
            -- -j4
        COMMENT "Building hl-tidy-ext plugin (HlTidyModule.so, LLVM ${HL_TIDY_LLVM_VERSION})"
        VERBATIM
    )

    add_custom_target(hl_tidy_build DEPENDS "${HL_TIDY_MODULE}")

    # --- Generate a .cpp that includes the umbrella header (clang-tidy needs a TU) ---
    set(TIDY_TU "${CMAKE_BINARY_DIR}/clang_tidy_tu.cpp")
    file(WRITE "${TIDY_TU}" "// Auto-generated translation unit for clang-tidy analysis\n#include <json/json.hpp>\n")

    # --- Run clang-tidy via a wrapper script ---
    # The generated TU is not in compile_commands.json, so we pass compiler flags
    # explicitly via --extra-arg. A wrapper script avoids CMake VERBATIM escaping
    # issues with shell-special characters in --header-filter and --checks regexes.
    set(TIDY_SCRIPT "${CMAKE_BINARY_DIR}/run_clang_tidy.sh")
    file(WRITE "${TIDY_SCRIPT}"
"#!/bin/bash
set -euo pipefail
echo 'Running clang-tidy-${HL_TIDY_LLVM_VERSION} with hl-tidy-ext on yajson headers...'
echo ''
${CLANG_TIDY_EXE} \\
  -load '${HL_TIDY_MODULE}' \\
  --checks='-*,hl-perf-*,hl-modernize-*' \\
  --header-filter='.*include/json/.*' \\
  --extra-arg=-std=c++17 \\
  --extra-arg=-I${CMAKE_SOURCE_DIR}/include \\
  --extra-arg=-DYAJSON_SIMD_ENABLED \\
  '${TIDY_TU}' \\
  > '${CLANG_TIDY_REPORT}' 2>&1 || true
echo ''
echo '=== clang-tidy report (hl-tidy-ext): ${CLANG_TIDY_REPORT} ==='
echo ''
cat '${CLANG_TIDY_REPORT}'
echo ''
TOTAL=$(grep -cE 'warning:|error:' '${CLANG_TIDY_REPORT}' || true)
echo \"=== Total findings: $TOTAL ===\"
")

    add_custom_target(clang_tidy_report
        DEPENDS hl_tidy_build
        COMMAND bash "${TIDY_SCRIPT}"
        COMMENT "Generating clang-tidy report with hl-tidy-ext plugin"
    )
else()
    if(NOT CLANG_TIDY_EXE)
        message(STATUS "clang-tidy not found — clang_tidy_report target disabled")
    endif()
    if(NOT EXISTS "${HL_TIDY_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "hl-tidy-ext submodule not found — clang_tidy_report target disabled")
    endif()
endif()

# ─── Leak / stress tests (standalone, no gtest) ─────────────────────────────
add_executable(test_leak_stress test_leak_stress.cpp)
target_link_libraries(test_leak_stress PRIVATE yajson Threads::Threads)
target_compile_options(test_leak_stress PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CONFIG:Release>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O2>>
)

# Reduced-iteration version for ASan/UBSan testing
add_executable(test_leak_asan test_leak_asan.cpp)
target_link_libraries(test_leak_asan PRIVATE yajson Threads::Threads)
target_compile_options(test_leak_asan PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
