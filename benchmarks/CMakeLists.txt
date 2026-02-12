# ─── Benchmarks (Google Benchmark) ──────────────────────────────────────────────

add_executable(json_benchmarks bench_json.cpp bench_throughput.cpp bench_arena.cpp)

target_link_libraries(json_benchmarks PRIVATE
    yajson
    benchmark::benchmark
    benchmark::benchmark_main
)

# Maximum optimization + SIMD for benchmarks
target_compile_options(json_benchmarks PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -march=native -DNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/O2 /DNDEBUG>
)

# ─── Comparative benchmarks: yajson vs Boost.JSON ────────────────────────────
if(YAJSON_BENCH_BOOST_JSON AND YAJSON_BOOST_JSON_AVAILABLE AND NOT YAJSON_BENCH_COMPARE)
    add_executable(bench_vs_boost bench_vs_boost.cpp)

    target_link_libraries(bench_vs_boost PRIVATE
        yajson
        Boost::json
        benchmark::benchmark
        benchmark::benchmark_main
    )

    target_compile_options(bench_vs_boost PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -march=native -DNDEBUG>
        $<$<CXX_COMPILER_ID:MSVC>:/O2 /DNDEBUG>
    )

    message(STATUS "Building comparative benchmarks: yajson vs Boost.JSON")
elseif(YAJSON_BENCH_BOOST_JSON AND NOT YAJSON_BOOST_JSON_AVAILABLE)
    message(WARNING "Boost.JSON not available, skipping comparative benchmarks")
endif()

# ─── Full comparative benchmarks: 5 JSON libraries ──────────────────────────
if(YAJSON_BENCH_COMPARE AND YAJSON_COMPARE_AVAILABLE)
    add_executable(bench_compare bench_compare.cpp)

    target_link_libraries(bench_compare PRIVATE
        yajson
        Boost::json
        nlohmann_json::nlohmann_json
        simdjson
        benchmark::benchmark
        benchmark::benchmark_main
    )

    # RapidJSON is header-only, add include path directly
    target_include_directories(bench_compare PRIVATE ${rapidjson_SOURCE_DIR}/include)

    target_compile_options(bench_compare PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -march=native -DNDEBUG>
        $<$<CXX_COMPILER_ID:MSVC>:/O2 /DNDEBUG>
    )

    message(STATUS "Building full comparative benchmarks: yajson vs Boost.JSON vs RapidJSON vs nlohmann vs simdjson")
endif()
