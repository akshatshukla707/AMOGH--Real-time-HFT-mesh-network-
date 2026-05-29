#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <cstddef>

// Single Producer Single Consumer lock-free ring buffer
// Used to pass ticks from UDP listener thread -> router thread
// without any mutex locking (zero contention, cache-friendly)
template<typename T, size_t N>
class SPSCRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

public:
    SPSCRingBuffer() : head_(0), tail_(0) {}

    // Producer: push a tick (called from UDP listener thread only)
    bool push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (N - 1);
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // buffer full, drop tick
        }
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer: pop a tick (called from router thread only)
    std::optional<T> pop() {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt; // buffer empty
        }
        T item = buffer_[current_head];
        head_.store((current_head + 1) & (N - 1), std::memory_order_release);
        return item;
    }

    size_t size() const {
        size_t t = tail_.load(std::memory_order_acquire);
        size_t h = head_.load(std::memory_order_acquire);
        return (t - h + N) & (N - 1);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<size_t> head_; // cache line aligned
    alignas(64) std::atomic<size_t> tail_;
    std::array<T, N> buffer_;
};
