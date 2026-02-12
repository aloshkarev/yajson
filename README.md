# yajson — Yet Another JSON Library


High-performance, header-only, thread-safe C++17 JSON library with SIMD acceleration and arena allocator.

## Features

| Category | Details |
|---|---|
| **Value type** | 24-byte tagged union, SSO for strings up to 15 chars, `uint64_t` support |
| **Parsing** | Recursive descent, SIMD whitespace/string scanning (SSE2/AVX2/NEON), inline float path |
| **Serialization** | Constexpr escape tables, buffered output (4 KiB string / 8 KiB stream), size-hint pre-alloc |
| **Key lookup** | O(1) via wyhash index (linear scan for objects with ≤16 keys) |
| **Memory** | `MonotonicArena` bump allocator with PMR integration, zero-malloc parsing path |
| **Thread safety** | `ThreadSafeJson` wrapper (`shared_mutex`: concurrent reads, exclusive writes) |
| **Standards** | JSON Pointer (RFC 6901), SAX-style `JsonWriter`, ADL `to_value`/`from_value` |
| **Extensions** | Comments, trailing commas, single quotes, unquoted keys, hex numbers, NaN/Infinity |
| **Error handling** | Exceptions, `error_code` via `try_parse()`, `get_or<T>(default)` |
| **Platforms** | x86_64 (SSE2/AVX2), ARM/ARM64 (NEON), any C++17 compiler |

## Quick Start

```cpp
#include <json/json.hpp>

auto val = yajson::parse(R"({"name":"Alice","age":30,"scores":[100,95]})");

std::string name = val["name"].as_string();   // "Alice"
int64_t age      = val["age"].as_integer();   // 30

val["active"] = yajson::JsonValue(true);
std::cout << val.dump(2) << std::endl;        // pretty-print

// Exception-free parsing
auto [v, ec] = yajson::try_parse(input);

// Arena-accelerated parsing (zero malloc)
char buf[8192];
yajson::MonotonicArena arena(buf, sizeof(buf));
{
    auto fast = yajson::parse(input, arena);
    process(fast);
}
arena.reset();  // O(1) reset for next document
```

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
./tests/json_tests           # 1012 unit tests
./benchmarks/json_benchmarks # internal benchmarks
```

| CMake Option | Default | Description |
|---|---|---|
| `YAJSON_BUILD_TESTS` | `ON` | Build unit tests (Google Test) |
| `YAJSON_BUILD_BENCHMARKS` | `ON` | Build benchmarks (Google Benchmark) |
| `YAJSON_NATIVE_ARCH` | `OFF` | Add `-march=native` (enables AVX2 for consumers) |
| `YAJSON_BENCH_COMPARE` | `OFF` | Build 5-library comparison (fetches Boost, RapidJSON, nlohmann, simdjson) |

## Performance

> 4-core x86_64 @ 3.8 GHz, GCC 11, `-O3 -march=native`, AVX2. Median of 3 runs.
> RapidJSON and simdjson reuse allocator/parser between iterations (their standard pattern).

### Parsing

Higher is better. Best result in each row is **bold**.

| Document | yajson | Boost.JSON | RapidJSON | nlohmann | simdjson |
|---|--:|--:|--:|--:|--:|
| Small (51 B) | 142 | 157 | 218 | 42 | **497** |
| Medium (1.7 KB) | 146 | 196 | 273 | 57 | **1,024** |
| Large (270 KB) | 271 | 283 | 337 | 74 | **1,743** |
| Int array 10K | 145 | 238 | 209 | 57 | **415** |
| Float array 1K | 269 | 395 | 282 | 44 | **586** |
| Strings 1K×256B | 2,140 | 2,800 | 404 | 130 | **5,740** |

All values in **MB/s**.

### Serialization

Lower is better.

| Document | yajson | Boost.JSON | RapidJSON | nlohmann |
|---|--:|--:|--:|--:|
| Small (51 B) | **177 ns** | 198 ns | 213 ns | 386 ns |
| Medium (1.7 KB) | 4.25 µs | **3.58 µs** | 4.26 µs | 8.94 µs |
| Large (270 KB) | 358 µs | **314 µs** | 551 µs | 1,414 µs |

> simdjson does not provide serialization.

### Roundtrip (parse + serialize, 270 KB)

| Library | Throughput | Latency |
|---|--:|--:|
| RapidJSON | **216 MB/s** | **1,194 µs** |
| Boost.JSON | 210 MB/s | 1,224 µs |
| yajson | 196 MB/s | 1,317 µs |
| nlohmann | 53 MB/s | 4,896 µs |

### Network Messages (100-msg batch, ~80 B each)

| Library | msg/s | MB/s |
|---|--:|--:|
| simdjson | **9.36M** | **739** |
| RapidJSON | 3.12M | 246 |
| **yajson** | **2.51M** | **198** |
| yajson (arena) | 2.41M | 190 |
| Boost.JSON | 2.23M | 176 |
| Boost.JSON (monotonic) | 2.63M | 208 |
| nlohmann | 0.66M | 52 |

yajson outperforms Boost.JSON on small-message throughput thanks to adaptive count-ahead (skipped for inputs < 256 B), inline whitespace fast-path, and cached TLS arena pointer.

### Arena Allocator Effect

| Document | Heap | Arena | Speedup |
|---|--:|--:|--:|
| Small (51 B) | 142 MB/s | 137 MB/s | — |
| Medium (1.7 KB) | 146 MB/s | 173 MB/s | +18% |
| Large (270 KB) | 271 MB/s | 430 MB/s | **+59%** |

Arena shows the largest gains on documents with many strings > 15 chars (beyond SSO) and deeply nested containers.

### Key Lookup & Deep Copy

| Operation | yajson | Boost.JSON | RapidJSON | nlohmann |
|---|--:|--:|--:|--:|
| Lookup in 1K-key object | 59 ns | **51 ns** | 2,114 ns | 100 ns |
| Deep copy (270 KB doc) | 470 µs | 645 µs | **101 µs** | 1,054 µs |

### At a Glance

| Scenario | Leader |
|---|---|
| Parse throughput | simdjson (read-only SIMD DOM) |
| Parse (mutable DOM) | RapidJSON > Boost.JSON > yajson |
| Serialize small objects | **yajson** |
| Network message batch | simdjson > RapidJSON > **yajson** > Boost.JSON |
| Key lookup | Boost.JSON ≈ yajson (O(1) hash) |
| Feature completeness | **yajson** (arena, Pointer, writer, thread-safe, PMR) |
| Ease of use | nlohmann/json |

## SIMD

Same core algorithm as simdjson: `cmpeq` + `movemask` (no SSE4.2 string instructions).

| Platform | Width | Intrinsics |
|---|---|---|
| x86_64 + AVX2 | 32 B | `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8` |
| x86_64 baseline | 16 B | `_mm_cmpeq_epi8` + `_mm_movemask_epi8` (SSE2) |
| AArch64 | 32 B | `vceqq_u8` + `vpadd_u8` bitmask (2×16 B) |
| ARMv7 | 16 B | `vceqq_u8` + `vpadd_u8` bitmask |
| Fallback | 1 B | Scalar |

Three hot functions: `skip_whitespace`, `find_string_delimiter`, `find_needs_escape`.

AVX2 is not enabled by default (header-only library, compile-time dispatch). Enable via:

```bash
cmake .. -DYAJSON_NATIVE_ARCH=ON     # propagates -march=native to consumers
```

## High-Load Recommendations

- **`-DYAJSON_NATIVE_ARCH=ON`** — enables AVX2 32-byte SIMD paths
- **Per-thread arena** — `thread_local MonotonicArena`, `parse(input, arena)`, zero malloc
- **Arena reuse** — `arena.reset()` between documents (O(1), no allocator pressure)
- **Batch processing** — single arena per batch of messages

## Arena Allocator

`MonotonicArena` implements `std::pmr::memory_resource`. All `JsonValue` allocations (strings, vectors, hash maps) route through it.

```cpp
char buf[16384];
yajson::MonotonicArena arena(buf, sizeof(buf));
{
    auto val = yajson::parse(json_input, arena);
    process(val);
}   // val must be destroyed before reset
arena.reset();

// Multi-threaded
void worker(std::string_view input) {
    thread_local yajson::MonotonicArena arena(32768);
    {
        auto val = yajson::parse(input, arena);
        process(val);
    }
    arena.reset();
}
```

## Error Handling

```cpp
// Exceptions (default)
try {
    auto val = yajson::parse(input);
} catch (const yajson::ParseError& e) {
    std::cerr << e.what() << " at line " << e.location().line << "\n";
}

// Error codes (no exceptions)
auto [val, ec] = yajson::try_parse(input);
if (ec) std::cerr << ec.message() << "\n";

// Safe accessors
int x = val["x"].get_or<int>(0);
```

## Project Structure

```
include/json/
├── json.hpp              # umbrella header
├── config.hpp            # macros, platform detection
├── fwd.hpp               # forward declarations, Type enum, Array/Object
├── error.hpp             # ParseError, error_code integration
├── value.hpp             # JsonValue (24-byte tagged union, SSO)
├── arena.hpp             # MonotonicArena, ArenaScope
├── parse_options.hpp     # non-standard extensions config
├── parser.hpp            # recursive descent parser (SIMD)
├── serializer.hpp        # buffered serializer (string + ostream)
├── stream_parser.hpp     # istream parser
├── json_pointer.hpp      # JSON Pointer (RFC 6901)
├── json_writer.hpp       # SAX-style incremental writer
├── thread_safe.hpp       # ThreadSafeJson
├── conversion.hpp        # ADL to_value / from_value
└── detail/
    ├── dtoa.hpp          # fast double-to-string
    ├── hash.hpp          # wyhash for O(1) key lookup
    ├── simd.hpp          # SSE2/AVX2/NEON utilities
    └── utf8.hpp          # UTF-8 encode/decode/validate
```

## Tests

1012 tests across two suites plus stress tests:

```bash
./tests/json_tests              # 691 tests (parser, serializer, Pointer, arena, UTF-8, ...)
./tests/json_tests_simd_native  # 321 tests (AVX2/NEON paths, -march=native)
./tests/test_leak_stress        # 10 stress tests (100K+ iterations, multi-threaded)
./tests/test_leak_asan          # same under AddressSanitizer
```

**Author:** Aleksandr Loshkarev