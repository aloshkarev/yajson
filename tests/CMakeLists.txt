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
