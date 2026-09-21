// The generation sweep asks "is this chunk already being generated?" on every
// candidate it offers, and before this filter existed the only way to ask was
// `enqueue_generation`, which takes a GLOBAL mutex and then a shard lock on the
// chunk map (through its is_already_loaded callback) to be refused — measured at
// 46,576 refusals in one session, each of them a check spent on terrain that was
// already on its way rather than on terrain that needed generating.
//
// The filter answers the same question with three relaxed atomic loads. What is
// tested here is that it is wired to the ENQUEUE and COMPLETION paths, not just
// that a bitmap can be set and cleared: a filter that is never set would silently
// do nothing (harmless), and one that is never cleared would defer chunks forever
// (a hole), so both directions are asserted against a real worker.
#include "doctest.h"
#include "core/thread_pool.hpp"
#include "world/chunk_scheduler.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace VoxelEngine;

namespace {

// Blocks inside the generate callback until released, so "in flight" is a state
// the test can hold still and observe rather than a race it hopes to hit.
struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};

    void wait_entered() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void wait_released(std::function<bool()> done, int ms = 5000) {
        released.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

uint64_t key_of(int32_t x, int32_t y, int32_t z) {
    // Same packing ChunkMap uses, so the test's keys look like the real ones.
    constexpr uint32_t OFFSET = 1u << 20;
    constexpr uint32_t MASK = 0x1FFFFF;
    const uint64_t ux = (static_cast<uint32_t>(x) + OFFSET) & MASK;
    const uint64_t uy = (static_cast<uint32_t>(y) + OFFSET) & MASK;
    const uint64_t uz = (static_cast<uint32_t>(z) + OFFSET) & MASK;
    return (ux << 42) | (uy << 21) | uz;
}

bool enqueue(ChunkScheduler& scheduler, ThreadPool& pool, uint64_t key, Gate& gate, std::atomic<bool>& loaded) {
    return scheduler.enqueue_generation(
        &pool, 0, 0, 0, 1, key,
        [](uint64_t) { return false; },              // nothing is loaded yet
        [&gate, &loaded](int32_t, int32_t, int32_t, bool& was_loaded) {
            gate.entered.store(true);
            while (!gate.released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            was_loaded = false;
            loaded.store(true);
            return std::make_unique<ChunkData>();
        },
        []() { return uint64_t(1); });               // epoch stays current
}

} // namespace

TEST_CASE("a fresh scheduler reports nothing in flight") {
    ChunkScheduler scheduler;
    CHECK_FALSE(scheduler.may_be_generating(key_of(0, 0, 0)));
    CHECK_FALSE(scheduler.may_be_generating(key_of(12345, 7, -9)));
}

TEST_CASE("an enqueue sets the filter and the worker's completion clears it") {
    ChunkScheduler scheduler;
    ThreadPool pool(2);
    Gate gate;
    std::atomic<bool> loaded{false};
    const uint64_t key = key_of(64, 3, 96);
    const uint64_t other = key_of(-1000, 4, 2000);

    CHECK_FALSE(scheduler.may_be_generating(key));
    const bool queued = enqueue(scheduler, pool, key, gate, loaded);
    CHECK(queued);
    if (!queued) return;   // no worker to observe: nothing to assert against
    gate.wait_entered();

    // In flight: the filter must say so, because this is exactly the state the
    // sweep needs to skip without taking the lock.
    CHECK(scheduler.may_be_generating(key));
    // A second enqueue of the same key is still refused by the scheduler proper.
    Gate unused;
    std::atomic<bool> unused_loaded{false};
    CHECK_FALSE(enqueue(scheduler, pool, key, unused, unused_loaded));

    // Unrelated keys must still read as free, or the filter would defer real work.
    // Counted rather than asserted one by one: 3 probes over 64 unrelated keys can
    // collide by chance, and a single unlucky key is not a bug.
    int free_keys = 0;
    for (int i = 0; i < 64; ++i) {
        if (!scheduler.may_be_generating(key_of(i * 31 + 7, -i, i * 17))) ++free_keys;
    }
    CHECK(free_keys >= 32);
    CHECK(other != key);

    // Released: the worker finishes, clears its bits, and the chunk lands.
    gate.wait_released([&] { return !scheduler.may_be_generating(key); });
    CHECK_FALSE(scheduler.may_be_generating(key));
    CompletedChunk completed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.poll_completed_chunk(completed) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(completed.chunk_data != nullptr);
}

TEST_CASE("clear() empties the filter along with the in-flight set") {
    ChunkScheduler scheduler;
    ThreadPool pool(2);
    Gate gate;
    std::atomic<bool> loaded{false};
    const uint64_t key = key_of(7, 1, 7);

    const bool queued = enqueue(scheduler, pool, key, gate, loaded);
    CHECK(queued);
    if (!queued) return;
    gate.wait_entered();
    CHECK(scheduler.may_be_generating(key));

    // A reset (world clear / epoch change) empties the scheduler. Anything the
    // filter still claimed after that would defer chunks in the new world, so the
    // bits go with the set.
    scheduler.clear();
    CHECK_FALSE(scheduler.may_be_generating(key));
    CHECK(scheduler.generating_count() == 0);

    gate.released.store(true);
}
