#include "doctest.h"

#include "concurrency_test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace VoxelEngine;
using namespace concurrency_test;

TEST_CASE("cross-chunk writer concurrent pending placements") {
    CrossChunkWriter writer;
    constexpr int NUM_THREADS = 4;
    constexpr int PLACEMENTS_PER_THREAD = 500;

    auto worker = [&](int thread_id) {
        auto cross_writer = writer.make_cross_writer();
        for (int i = 0; i < PLACEMENTS_PER_THREAD; i++) {
            int32_t wx = thread_id * 1000 + i;
            int32_t wy = 10;
            int32_t wz = i;
            cross_writer(wx, wy, wz, i % 10);
        }
    };

    std::thread writers[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        writers[i] = std::thread(worker, i);
    for (auto& t : writers) t.join();

    CHECK(writer.total_pending_count() == static_cast<size_t>(NUM_THREADS * PLACEMENTS_PER_THREAD));
    CHECK(writer.cross_boundary_count() == static_cast<size_t>(NUM_THREADS * PLACEMENTS_PER_THREAD));
}

// =========================================================================
// Cross-chunk writer: concurrent push + drain under contention
//     Tests the actual production pattern from chunk_world.cpp apply_pending_placements
// =========================================================================
TEST_CASE("cross-chunk writer concurrent push and drain") {
    CrossChunkWriter writer;
    std::atomic<bool> stop_writers{false};

    constexpr int NUM_WRITERS = 4;
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_drained{0};

    auto pusher = [&](int base) {
        auto cross_writer = writer.make_cross_writer();
        int count = 0;
        while (!stop_writers.load(std::memory_order_acquire)) {
            cross_writer(base + count, 0, 0, 1);
            count++;
            total_pushed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Simulates process_completed_chunks draining cross_boundary_remesh
    auto drainer = [&]() {
        while (!stop_writers.load(std::memory_order_acquire)) {
            // Drain all keys currently present to simulate apply_pending_placements
            auto keys = writer.get_all_keys();
            for (auto key : keys) {
                auto placements = writer.dequeue_placements(key);
                total_drained.fetch_add(static_cast<int>(placements.size()), std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    std::thread drain_thread(drainer);
    std::thread writers[NUM_WRITERS];
    for (int i = 0; i < NUM_WRITERS; i++)
        writers[i] = std::thread(pusher, i * 10000);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_writers.store(true, std::memory_order_release);
    for (auto& t : writers) t.join();
    drain_thread.join();
    // After both writers and drainer are joined, do a final drain pass
    // to catch any entries that were pushed after the drainer's last snapshot
    while (true) {
        auto keys = writer.get_all_keys();
        if (keys.empty()) break;
        for (auto key : keys) {
            auto placements = writer.dequeue_placements(key);
            total_drained.fetch_add(static_cast<int>(placements.size()), std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }

    // Final verification: all pushed entries were drained
    CHECK(total_drained.load() == total_pushed.load());
    CHECK(writer.total_pending_count() == 0);
}
