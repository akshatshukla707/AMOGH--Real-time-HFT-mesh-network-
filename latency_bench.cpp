#include <iostream>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <vector>
#include <thread>
#include "../include/ring_buffer.hpp"
#include "../include/types.hpp"

// Benchmark SPSC ring buffer throughput and latency
void benchmarkRingBuffer() {
    using Buf = SPSCRingBuffer<MarketTick, 65536>;
    Buf buf;

    constexpr int N = 1'000'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(N);

    std::atomic<bool> done{false};

    // Consumer thread
    std::thread consumer([&]() {
        uint64_t count = 0;
        while (count < N) {
            auto tick = buf.pop();
            if (tick) {
                uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                latencies.push_back(now - tick->timestamp_ns);
                ++count;
            }
        }
        done = true;
    });

    // Producer
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        MarketTick tick{};
        tick.timestamp_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        while (!buf.push(tick)) {} // spin on full
    }

    consumer.join();
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();

    std::sort(latencies.begin(), latencies.end());
    double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    std::cout << "\n═══ SPSC Ring Buffer Benchmark ═══\n"
              << "Messages   : " << N << "\n"
              << "Throughput : " << (int)(N / elapsed_s / 1e6) << " M msg/s\n"
              << "Latency p50: " << latencies[N * 50 / 100] << " ns\n"
              << "Latency p95: " << latencies[N * 95 / 100] << " ns\n"
              << "Latency p99: " << latencies[N * 99 / 100] << " ns\n"
              << "Latency max: " << latencies.back()         << " ns\n"
              << "Mean       : " << (int)mean                << " ns\n"
              << "══════════════════════════════════\n";
}

void benchmarkMemoryPool() {
    MemoryPool<BookOrder, 100000> pool;

    constexpr int N = 1'000'000;
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<BookOrder*> ptrs;
    ptrs.reserve(1000);
    for (int i = 0; i < N; ++i) {
        BookOrder* p = pool.alloc();
        ptrs.push_back(p);
        if (ptrs.size() == 1000) {
            for (auto* x : ptrs) pool.free(x);
            ptrs.clear();
        }
    }
    for (auto* x : ptrs) pool.free(x);

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();

    std::cout << "\n═══ Memory Pool Benchmark ═══\n"
              << "Operations   : " << N << " alloc+free pairs\n"
              << "Time per op  : " << (int)(elapsed_ns / N) << " ns\n"
              << "Throughput   : " << (int)(N / (elapsed_ns / 1e9) / 1e6) << " M ops/s\n"
              << "═════════════════════════════\n";
}

int main() {
    std::cout << "HFT Mesh — Latency Benchmarks\n\n";
    benchmarkRingBuffer();
    benchmarkMemoryPool();
    return 0;
}
