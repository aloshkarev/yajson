#pragma once

/// @file thread_safe.hpp
/// @author Aleksandr Loshkarev
/// @brief Thread-safe wrapper around JsonValue.
///
/// Uses std::shared_mutex for shared/exclusive access:
///   - Multiple concurrent readers (shared lock)
///   - Exclusive access for writing (unique lock)
///
/// Provides high throughput for read-heavy scenarios.

#include "value.hpp"

#include <mutex>
#include <shared_mutex>
#include <string_view>

namespace yajson {

/// @brief Thread-safe wrapper around JsonValue.
///
/// Usage example:
/// @code
///   ThreadSafeJson tsj(yajson::parse(input));
///
///   // Reading (multiple threads concurrently):
///   tsj.read([](const JsonValue& v) {
///       std::cout << v["name"].as_string() << std::endl;
///   });
///
///   // Writing (exclusive access):
///   tsj.write([](JsonValue& v) {
///       v["count"] = JsonValue(42);
///   });
/// @endcode
class ThreadSafeJson {
public:
    /// @brief Constructor from an existing value (copy).
    explicit ThreadSafeJson(const JsonValue& value)
        : value_(value) {}

    /// @brief Constructor from an existing value (move).
    explicit ThreadSafeJson(JsonValue&& value) noexcept
        : value_(std::move(value)) {}

    /// @brief Default constructor: null value.
    ThreadSafeJson() = default;

    // Non-copyable (mutex is not copyable)
    ThreadSafeJson(const ThreadSafeJson&) = delete;
    ThreadSafeJson& operator=(const ThreadSafeJson&) = delete;

    // Move: not thread-safe, use from a single thread
    ThreadSafeJson(ThreadSafeJson&& other) noexcept {
        std::unique_lock lock(other.mutex_);
        value_ = std::move(other.value_);
    }

    ThreadSafeJson& operator=(ThreadSafeJson&& other) noexcept {
        if (this != &other) {
            // Lock both mutexes, avoiding deadlock via std::lock
            std::unique_lock lk1(mutex_, std::defer_lock);
            std::unique_lock lk2(other.mutex_, std::defer_lock);
            std::lock(lk1, lk2);
            value_ = std::move(other.value_);
        }
        return *this;
    }

    // ─── Read access (shared lock) ──────────────────────────────────────

    /// @brief Perform a read operation (shared lock).
    /// @param fn  Callable accepting const JsonValue&.
    /// @return Result of calling fn.
    template <typename Fn>
    auto read(Fn&& fn) const -> decltype(fn(std::declval<const JsonValue&>())) {
        std::shared_lock lock(mutex_);
        return fn(value_);
    }

    /// @brief Get a snapshot copy of the value (shared lock).
    [[nodiscard]] JsonValue snapshot() const {
        std::shared_lock lock(mutex_);
        return value_;
    }

    /// @brief Get a serialized copy (shared lock).
    [[nodiscard]] std::string dump(int indent = -1) const {
        std::shared_lock lock(mutex_);
        return value_.dump(indent);
    }

    /// @brief Check the value type (shared lock).
    [[nodiscard]] Type type() const {
        std::shared_lock lock(mutex_);
        return value_.type();
    }

    /// @brief Get the size (shared lock).
    [[nodiscard]] size_t size() const {
        std::shared_lock lock(mutex_);
        return value_.size();
    }

    // ─── Write access (unique lock) ─────────────────────────────────────

    /// @brief Perform a write operation (unique lock).
    /// @param fn  Callable accepting JsonValue&.
    /// @return Result of calling fn.
    template <typename Fn>
    auto write(Fn&& fn) -> decltype(fn(std::declval<JsonValue&>())) {
        std::unique_lock lock(mutex_);
        return fn(value_);
    }

    /// @brief Replace the entire value (unique lock).
    void assign(const JsonValue& value) {
        std::unique_lock lock(mutex_);
        value_ = value;
    }

    /// @brief Replace the entire value (move, unique lock).
    void assign(JsonValue&& value) {
        std::unique_lock lock(mutex_);
        value_ = std::move(value);
    }

    // ─── Atomic operations ─────────────────────────────────────────────

    /// @brief Atomic read-modify-write operation.
    /// @param fn  Callable: JsonValue -> JsonValue.
    template <typename Fn>
    void update(Fn&& fn) {
        std::unique_lock lock(mutex_);
        value_ = fn(std::move(value_));
    }

    /// @brief Atomic insertion into an object.
    void insert(std::string_view key, JsonValue value) {
        std::unique_lock lock(mutex_);
        value_.insert(std::string(key), std::move(value));
    }

    /// @brief Atomic append to an array.
    void push_back(JsonValue value) {
        std::unique_lock lock(mutex_);
        value_.push_back(std::move(value));
    }

    /// @brief Atomic removal of a key from an object.
    bool erase(std::string_view key) {
        std::unique_lock lock(mutex_);
        return value_.erase(key);
    }

    // ─── Scoped access (for complex scenarios) ──────────────────────────

    /// @brief RAII wrapper for read access with a held lock.
    class ReadGuard {
    public:
        explicit ReadGuard(const ThreadSafeJson& tsj)
            : lock_(tsj.mutex_), value_(tsj.value_) {}

        const JsonValue& operator*()  const noexcept { return value_; }
        const JsonValue* operator->() const noexcept { return &value_; }

    private:
        std::shared_lock<std::shared_mutex> lock_;
        const JsonValue& value_;
    };

    /// @brief RAII wrapper for write access with a held lock.
    class WriteGuard {
    public:
        explicit WriteGuard(ThreadSafeJson& tsj)
            : lock_(tsj.mutex_), value_(tsj.value_) {}

        JsonValue& operator*()  noexcept { return value_; }
        JsonValue* operator->() noexcept { return &value_; }

    private:
        std::unique_lock<std::shared_mutex> lock_;
        JsonValue& value_;
    };

    /// @brief Get a RAII read guard.
    [[nodiscard]] ReadGuard read_guard() const { return ReadGuard(*this); }

    /// @brief Get a RAII write guard.
    [[nodiscard]] WriteGuard write_guard() { return WriteGuard(*this); }

private:
    JsonValue value_;
    mutable std::shared_mutex mutex_;
};

} // namespace yajson
