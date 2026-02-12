#pragma once

/// @file arena.hpp
/// @author Aleksandr Loshkarev
/// @brief Monotonic arena allocator for high-performance JSON parsing.
///
/// MonotonicArena is both a raw bump allocator and a std::pmr::memory_resource,
/// so it can back pmr containers (pmr::vector, pmr::unordered_map) directly.
///
/// Design:
///   - Initial buffer (stack or heap) + overflow blocks with geometric growth
///   - Implements std::pmr::memory_resource for use with pmr containers
///   - Thread-local arena context via ArenaScope RAII guard
///   - JsonValue constructors automatically route allocations through the arena
///   - Arena-allocated values must not outlive the arena
///   - Copying an arena-allocated JsonValue outside of arena scope produces
///     a normal heap-allocated copy
///
/// Typical usage:
/// @code
///   char buf[8192];
///   yajson::MonotonicArena arena(buf, sizeof(buf));
///   auto val = yajson::parse(input, arena);
///   // use val...
///   arena.reset();  // O(1) — reuse for next document
/// @endcode
///
/// Thread safety:
///   Each thread should use its own MonotonicArena instance.
///   ArenaScope uses thread_local storage, safe for concurrent use.

#include "config.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <new>
#include <utility>

namespace yajson {

/// @brief High-performance monotonic (bump) arena allocator.
///
/// Derives from std::pmr::memory_resource so it can be used as the allocator
/// for pmr::vector, pmr::unordered_map, and other pmr containers.
///
/// Allocations are O(1) pointer bumps. Deallocation is a no-op; all memory
/// is released at once via reset() or the destructor.
///
/// Memory layout: initial buffer (optional stack/external) followed by a
/// linked list of heap-allocated overflow blocks with geometric growth.
class MonotonicArena : public std::pmr::memory_resource {
public:
    /// @brief Construct with an external (e.g. stack-allocated) buffer.
    /// Overflow allocations use the heap with geometric growth.
    /// @param buf       Pointer to external buffer (must remain valid for arena lifetime).
    /// @param buf_size  Size of the external buffer in bytes.
    explicit MonotonicArena(void* buf, size_t buf_size) noexcept
        : ptr_(static_cast<char*>(buf))
        , end_(static_cast<char*>(buf) + buf_size)
        , initial_buf_(static_cast<char*>(buf))
        , initial_size_(buf_size)
        , blocks_(nullptr)
        , total_allocated_(buf_size)
        , next_block_size_(buf_size < 4096 ? 4096 : buf_size * 2) {}

    /// @brief Construct with a heap-allocated initial buffer.
    /// @param initial_size  Initial buffer size in bytes (default: 4096).
    explicit MonotonicArena(size_t initial_size = 4096)
        : ptr_(nullptr)
        , end_(nullptr)
        , initial_buf_(nullptr)
        , initial_size_(0)
        , blocks_(nullptr)
        , total_allocated_(0)
        , next_block_size_(initial_size < 256 ? 256 : initial_size)
    {
        grow(next_block_size_);
    }

    ~MonotonicArena() override { free_blocks(); }

    // Non-copyable, non-movable
    MonotonicArena(const MonotonicArena&) = delete;
    MonotonicArena& operator=(const MonotonicArena&) = delete;
    MonotonicArena(MonotonicArena&&) = delete;
    MonotonicArena& operator=(MonotonicArena&&) = delete;

    /// @brief Allocate size bytes from the arena with given alignment.
    /// @param size  Number of bytes to allocate.
    /// @param align Required alignment (default: max_align_t alignment).
    /// @return Pointer to aligned memory, or nullptr on failure (extremely unlikely).
    void* allocate(size_t size, size_t align = alignof(std::max_align_t)) noexcept {
        uintptr_t cur = reinterpret_cast<uintptr_t>(ptr_);
        uintptr_t aligned = (cur + align - 1) & ~(align - 1);
        char* result = reinterpret_cast<char*>(aligned);

        if (JSON_LIKELY(result + size <= end_)) {
            ptr_ = result + size;
            return result;  // Hot path: no statistics overhead
        }
        return allocate_slow(size, align);
    }

    /// @brief Allocate and construct an object of type T in the arena.
    /// @tparam T     Type to construct.
    /// @tparam Args  Constructor argument types.
    /// @return Pointer to the constructed object.
    /// @throws std::bad_alloc on allocation failure (extremely unlikely).
    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        if (JSON_UNLIKELY(!mem)) throw std::bad_alloc();
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /// @brief Reset the arena, releasing all overflow blocks.
    /// After reset, the initial buffer is reused. O(1) if only using
    /// the initial buffer (no overflow blocks allocated).
    /// @warning All pointers obtained from this arena become invalid.
    void reset() noexcept {
        free_blocks();
        blocks_ = nullptr;
        if (initial_buf_) {
            ptr_ = initial_buf_;
            end_ = initial_buf_ + initial_size_;
            total_allocated_ = initial_size_;
        } else {
            // Heap-only arena: allocate a fresh block
            total_allocated_ = 0;
            ptr_ = nullptr;
            end_ = nullptr;
            grow(next_block_size_ > 0 ? next_block_size_ / 2 : 4096);
        }
    }

    /// @brief Total bytes allocated (initial buffer + all heap blocks).
    [[nodiscard]] size_t bytes_allocated() const noexcept { return total_allocated_; }

    /// @brief Approximate total bytes used (total data capacity minus remaining in current block).
    /// Note: for heap-only arenas, this includes capacity from past (filled) blocks.
    /// Computed lazily — no overhead on the allocation fast path.
    [[nodiscard]] size_t bytes_used() const noexcept {
        return data_capacity() - bytes_remaining();
    }

    /// @brief Total data capacity across all blocks (excludes block headers).
    [[nodiscard]] size_t data_capacity() const noexcept {
        size_t cap = initial_size_;
        for (Block* b = blocks_; b; b = b->next) cap += b->capacity;
        return cap;
    }

    /// @brief Bytes remaining in the current block before overflow.
    [[nodiscard]] size_t bytes_remaining() const noexcept {
        return ptr_ && end_ > ptr_ ? static_cast<size_t>(end_ - ptr_) : 0;
    }

    /// @brief Number of heap overflow blocks allocated.
    [[nodiscard]] size_t block_count() const noexcept {
        size_t n = 0;
        for (Block* b = blocks_; b; b = b->next) ++n;
        return n;
    }

protected:
    // ─── std::pmr::memory_resource interface ────────────────────────────

    /// Allocate from the arena (called by pmr containers).
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* p = allocate(bytes, alignment);
        if (JSON_UNLIKELY(!p)) throw std::bad_alloc();
        return p;
    }

    /// Deallocation is a no-op for monotonic arenas.
    /// Memory is released only via reset() or destructor.
    void do_deallocate(void* /*p*/, size_t /*bytes*/, size_t /*alignment*/) override {
        // Intentional no-op: monotonic allocator
    }

    /// Two MonotonicArenas are equal only if they are the same object.
    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    /// Overflow block header. Data follows immediately after this struct.
    struct Block {
        Block* next;
        size_t capacity;
        char* data() noexcept { return reinterpret_cast<char*>(this + 1); }
    };

    char* ptr_;            ///< Current allocation pointer
    char* end_;            ///< End of current block
    char* initial_buf_;    ///< Pointer to the initial (external) buffer, nullptr for heap-only
    size_t initial_size_;  ///< Size of the initial buffer
    Block* blocks_;        ///< Linked list of heap-allocated overflow blocks (newest first)
    size_t total_allocated_;
    size_t next_block_size_;

    /// Slow path: current block exhausted, allocate a new heap block and retry.
    JSON_NOINLINE void* allocate_slow(size_t size, size_t align) noexcept {
        size_t needed = size + align - 1; // worst-case alignment padding
        size_t block_size = next_block_size_;
        if (block_size < needed) block_size = needed;

        grow(block_size);

        // Retry allocation in the fresh block
        uintptr_t cur = reinterpret_cast<uintptr_t>(ptr_);
        uintptr_t aligned_addr = (cur + align - 1) & ~(align - 1);
        char* result = reinterpret_cast<char*>(aligned_addr);

        if (JSON_LIKELY(result + size <= end_)) {
            ptr_ = result + size;
            return result;
        }
        return nullptr; // Should not happen with correct grow()
    }

    /// Allocate a new heap block and make it the current allocation target.
    void grow(size_t data_size) noexcept {
        size_t total = sizeof(Block) + data_size;
        auto* block = static_cast<Block*>(std::malloc(total));
        if (JSON_UNLIKELY(!block)) return; // OOM — extremely rare

        block->next = blocks_;
        block->capacity = data_size;
        blocks_ = block;

        ptr_ = block->data();
        end_ = ptr_ + data_size;
        total_allocated_ += total;

        // Geometric growth: each subsequent block is 2x larger
        next_block_size_ = data_size * 2;
    }

    /// Free all heap-allocated overflow blocks.
    void free_blocks() noexcept {
        Block* b = blocks_;
        while (b) {
            Block* next = b->next;
            std::free(b);
            b = next;
        }
    }
};

// ─── Thread-local arena context ─────────────────────────────────────────────

namespace detail {

/// Thread-local pointer to the active MonotonicArena (nullptr when inactive).
/// Accessed by JsonValue constructors to route heap allocations through the arena.
inline thread_local MonotonicArena* current_arena = nullptr;

/// @brief Get the pmr memory_resource for the current context.
/// Returns the active arena if one is set, otherwise new_delete_resource.
inline std::pmr::memory_resource* current_resource() noexcept {
    if (auto* arena = current_arena)
        return static_cast<std::pmr::memory_resource*>(arena);
    return std::pmr::new_delete_resource();
}

} // namespace detail

/// @brief RAII guard that activates a MonotonicArena for the current thread.
///
/// While the guard is alive, all JsonValue heap allocations on this thread
/// are routed through the arena. Nesting is supported: the previous arena
/// is restored when the guard is destroyed.
class ArenaScope {
public:
    explicit ArenaScope(MonotonicArena& arena) noexcept
        : prev_(detail::current_arena)
    {
        detail::current_arena = &arena;
    }

    ~ArenaScope() noexcept {
        detail::current_arena = prev_;
    }

    // Non-copyable
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    MonotonicArena* prev_;
};

} // namespace yajson
