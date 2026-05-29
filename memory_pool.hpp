#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <new>

// ─────────────────────────────────────────────────────────────
// Fixed-size memory pool — pre-allocates N objects at startup.
// Allocation: ~5ns (pop from free-list intrusive stack).
// Deallocation: ~3ns (push back to free-list).
// ZERO heap allocation after startup. ZERO fragmentation.
// NOT thread-safe — each thread gets its own pool.
// ─────────────────────────────────────────────────────────────
template <typename T, size_t N>
class MemoryPool {
public:
    MemoryPool() {
        // Build free-list: each slot's first bytes hold the index
        // of the next free slot (intrusive linked list).
        for (size_t i = 0; i < N - 1; ++i) {
            *reinterpret_cast<size_t*>(&storage_[i]) = i + 1;
        }
        *reinterpret_cast<size_t*>(&storage_[N - 1]) = SENTINEL;
        free_head_ = 0;
        used_ = 0;
    }

    // Allocate and construct an object
    template <typename... Args>
    T* alloc(Args&&... args) {
        if (free_head_ == SENTINEL) return nullptr; // pool exhausted
        size_t idx = free_head_;
        free_head_ = *reinterpret_cast<size_t*>(&storage_[idx]);
        ++used_;
        return new (&storage_[idx]) T(std::forward<Args>(args)...);
    }

    // Destruct and return object to pool
    void free(T* ptr) {
        if (!ptr) return;
        ptr->~T();
        size_t idx = reinterpret_cast<Slot*>(ptr) - &storage_[0];
        assert(idx < N);
        *reinterpret_cast<size_t*>(ptr) = free_head_;
        free_head_ = idx;
        --used_;
    }

    size_t used()      const noexcept { return used_; }
    size_t available() const noexcept { return N - used_; }
    size_t capacity()  const noexcept { return N; }

private:
    static constexpr size_t SENTINEL = SIZE_MAX;

    // Each slot is aligned and sized to hold T
    struct alignas(alignof(T)) Slot {
        uint8_t bytes[sizeof(T)];
    };

    std::array<Slot, N> storage_;
    size_t free_head_;
    size_t used_;
};
