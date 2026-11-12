#include "mesh/mesh_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace VoxelEngine {

using namespace godot;

namespace {

// One region's worth of work, accumulated over the armed regions. Integer
// microseconds, not doubles: this build is not guaranteed to be C++20, where
// atomic<double>::fetch_add appears. Atomics because each region's build runs
// on its own worker while the main thread may read the counters.
struct RegionSpikeTotals {
    std::atomic<uint64_t> regions{0};
    std::atomic<uint64_t> regions_seen{0};
    std::atomic<uint64_t> max_members{0};
    std::atomic<uint64_t> chunks{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> vertices{0};
    std::atomic<uint64_t> snapshot_us{0};
    std::atomic<uint64_t> wait_total_us{0};
    std::atomic<uint64_t> wait_max_us{0};
    std::atomic<uint64_t> hold_total_us{0};
    std::atomic<uint64_t> hold_max_us{0};
    std::atomic<uint64_t> mesh_total_us{0};
    std::atomic<uint64_t> mesh_max_us{0};
};

RegionSpikeTotals& spike_totals() {
    static RegionSpikeTotals totals;
    return totals;
}

// FARLANDS_REGION_SPIKE=<regions>: how many far regions run the spike. Zero (or
// unset) is the shipping behaviour - the hook returns before touching anything.
int32_t spike_budget() {
    static const int32_t budget = [] {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        const char* value = std::getenv("FARLANDS_REGION_SPIKE");
        if (value == nullptr || *value == '\0') return 0;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end != value && *end == '\0' ? static_cast<int32_t>(parsed) : 0;
    }();
    return budget;
}

std::atomic<int32_t>& spike_remaining(int32_t budget) {
    static std::atomic<int32_t> remaining{budget};
    return remaining;
}

// FARLANDS_REGION_SPIKE_MIN=<members>: the smallest region the spike will
// sample. 64 asks for a region full of far caches; the default keeps the first
// populated region, which is what the lock measurement needs.
int32_t spike_min_members() {
    static const int32_t minimum = [] {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        const char* value = std::getenv("FARLANDS_REGION_SPIKE_MIN");
        int32_t parsed = 16;
        if (value != nullptr) {
            char* end = nullptr;
            const long requested = std::strtol(value, &end, 10);
            if (end != value && *end == '\0') parsed = static_cast<int32_t>(requested);
        }
        return parsed > 0 ? parsed : 1;
    }();
    return minimum;
}

uint64_t us_between(std::chrono::steady_clock::time_point from,
                    std::chrono::steady_clock::time_point to) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

void atomic_max(std::atomic<uint64_t>& slot, uint64_t value) {
    uint64_t current = slot.load(std::memory_order_relaxed);
    while (current < value &&
           !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

double to_ms(uint64_t microseconds) {
    return static_cast<double>(microseconds) / 1000.0;
}

} // namespace

void MeshManager::region_snapshot_spike(const std::vector<uint64_t>& chunk_keys,
                                        float detail_level, bool smooth_lighting) {
    const int32_t budget = spike_budget();
    if (budget <= 0 || !chunk_map) {
        return;
    }
    // Record the distribution on every region build: a threshold run that finds
    // no region big enough must still report the largest one the fill produced.
    RegionSpikeTotals& totals = spike_totals();
    totals.regions_seen.fetch_add(1, std::memory_order_relaxed);
    atomic_max(totals.max_members, static_cast<uint64_t>(chunk_keys.size()));
    // Only a populated region answers the design's question: the first regions to
    // rebuild during a fill have a handful of far caches, so they understate both
    // the copy and the mesh. A region below the bar leaves the budget untouched
    // and is measured later, if a bigger one arrives.
    if (chunk_keys.size() < static_cast<size_t>(spike_min_members())) {
        return;
    }
    // Claim one armed region. The budget counts regions, so a two-region run
    // costs ~128 snapshot+mesh pairs under live streaming and leaves the fill
    // otherwise untouched.
    std::atomic<int32_t>& remaining = spike_remaining(budget);
    int32_t left = remaining.load(std::memory_order_relaxed);
    while (left > 0) {
        if (remaining.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            break;
        }
    }
    if (left <= 0) {
        return;
    }

    totals.regions.fetch_add(1, std::memory_order_relaxed);

    // One chunk at a time, under its OWN shard only. The hold is the copy of one
    // chunk's palette storage; a band over the whole 8x8 region would hold every
    // shard an install needs exclusive for a 3x3x3 read, which is the question
    // this spike exists to answer. The copy is also the snapshot: the mesher
    // below reads it after the lock is released.
    thread_local MeshBuilder builder;
    for (uint64_t key : chunk_keys) {
        int32_t cx = 0, cy = 0, cz = 0;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);

        ChunkData snapshot;
        bool have_data = false;
        uint64_t wait_us = 0;
        uint64_t hold_us = 0;
        {
            const auto before_lock = std::chrono::steady_clock::now();
            auto lock = chunk_map->lock_keys(&key, 1);
            const auto after_lock = std::chrono::steady_clock::now();
            ChunkRenderData* render_data = chunk_map->get_chunk_render_data_fast(cx, cy, cz);
            if (render_data && render_data->data) {
                snapshot = *render_data->data;
                have_data = true;
            }
            const auto after_copy = std::chrono::steady_clock::now();
            wait_us = us_between(before_lock, after_lock);
            hold_us = us_between(after_lock, after_copy);
        }
        if (!have_data) {
            continue;
        }

        totals.chunks.fetch_add(1, std::memory_order_relaxed);
        totals.bytes.fetch_add(static_cast<uint64_t>(snapshot.memory_usage()),
                               std::memory_order_relaxed);
        totals.snapshot_us.fetch_add(wait_us + hold_us, std::memory_order_relaxed);
        totals.wait_total_us.fetch_add(wait_us, std::memory_order_relaxed);
        totals.hold_total_us.fetch_add(hold_us, std::memory_order_relaxed);
        atomic_max(totals.wait_max_us, wait_us);
        atomic_max(totals.hold_max_us, hold_us);

        // The far-detail build the real region path would run per chunk, on the
        // snapshot, with no neighbours (border faces are expected to differ) and
        // no upload. This checks the data path runs outside the chunk-map lock
        // and gives the spike its build cost.
        const auto before_build = std::chrono::steady_clock::now();
        builder.clear();
        builder.set_smooth_lighting(smooth_lighting);
        builder.set_detail_level(detail_level);
        builder.set_greedy_enabled(true);
        builder.set_quad_recording(false);
        builder.build_mesh(snapshot);
        const uint64_t build_us = us_between(before_build, std::chrono::steady_clock::now());
        totals.mesh_total_us.fetch_add(build_us, std::memory_order_relaxed);
        totals.vertices.fetch_add(static_cast<uint64_t>(builder.get_vertex_count()),
                                  std::memory_order_relaxed);
        atomic_max(totals.mesh_max_us, build_us);
    }
}

Dictionary MeshManager::get_region_spike_stats() const {
    const RegionSpikeTotals& totals = spike_totals();
    Dictionary out;
    out["regions"] = static_cast<int64_t>(totals.regions.load(std::memory_order_relaxed));
    out["regions_seen"] = static_cast<int64_t>(totals.regions_seen.load(std::memory_order_relaxed));
    out["max_members"] = static_cast<int64_t>(totals.max_members.load(std::memory_order_relaxed));
    out["chunks"] = static_cast<int64_t>(totals.chunks.load(std::memory_order_relaxed));
    out["bytes"] = static_cast<int64_t>(totals.bytes.load(std::memory_order_relaxed));
    out["vertices"] = static_cast<int64_t>(totals.vertices.load(std::memory_order_relaxed));
    out["snapshot_ms"] = to_ms(totals.snapshot_us.load(std::memory_order_relaxed));
    out["wait_total_ms"] = to_ms(totals.wait_total_us.load(std::memory_order_relaxed));
    out["wait_max_ms"] = to_ms(totals.wait_max_us.load(std::memory_order_relaxed));
    out["hold_total_ms"] = to_ms(totals.hold_total_us.load(std::memory_order_relaxed));
    out["hold_max_ms"] = to_ms(totals.hold_max_us.load(std::memory_order_relaxed));
    out["mesh_ms"] = to_ms(totals.mesh_total_us.load(std::memory_order_relaxed));
    out["mesh_max_ms"] = to_ms(totals.mesh_max_us.load(std::memory_order_relaxed));
    return out;
}

} // namespace VoxelEngine
