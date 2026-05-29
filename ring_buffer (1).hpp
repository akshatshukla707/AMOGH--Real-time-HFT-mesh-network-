#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <cstddef>
#include <cassert>

// ─────────────────────────────────────────────────────────────
// Single-Producer Single-Consumer lock-free ring buffer.
// Zero mutexes. Zero condition variables. Zero syscalls.
// Uses release/acquire memory ordering for correct cache coherency.
// Head and tail on SEPARATE cache lines to prevent false sharing.
// N must be a power of 2 for fast bitwise wraparound.
// ─────────────────────────────────────────────────────────────
template <typename T, size_t N>
class SPSCRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2, "N must be at least 2");

public:
    SPSCRingBuffer() : head_(0), tail_(0) {}

    // Called by PRODUCER thread only
    // Returns true if item was pushed, false if buffer is full (drop)
    bool push(const T& item) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t next_t = advance(t);
        if (next_t == head_.load(std::memory_order_acquire)) {
            return false; // full — caller decides to drop or spin
        }
        buffer_[t] = item;
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    // Called by CONSUMER thread only
    // Returns item if available, nullopt if empty
    std::optional<T> pop() noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T item = buffer_[h];
        head_.store(advance(h), std::memory_order_release);
        return item;
    }

    size_t size() const noexcept {
        size_t t = tail_.load(std::memory_order_acquire);
        size_t h = head_.load(std::memory_order_acquire);
        return (t - h + N) & (N - 1);
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    constexpr size_t capacity() const noexcept { return N - 1; }

private:
    static constexpr size_t advance(size_t idx) noexcept {
        return (idx + 1) & (N - 1);
    }

    alignas(64) std::atomic<size_t> head_;  // owned by CONSUMER
    char _pad[64 - sizeof(std::atomic<size_t>)];
    alignas(64) std::atomic<size_t> tail_;  // owned by PRODUCER
    std::array<T, N> buffer_;
};
