#ifndef FARLANDS_CHUNK_SCHEDULER_HPP
#define FARLANDS_CHUNK_SCHEDULER_HPP
#include "core/chunk_types.hpp"
#include "core/thread_pool.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_set>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Chunk scheduler ??? manages async chunk generation and mesh completion queues.
// This is a pure queue manager; the actual processing logic stays in ChunkManager.
// -------------------------------------------------------------------------
class ChunkScheduler {
public:
    void clear() {
        std::scoped_lock lock(generating_mutex, completed_mutex, completed_mesh_mutex, completed_light_mutex);
        generating_chunks.clear();
        for (auto& word : inflight_bits_) word.store(0, std::memory_order_relaxed);
        while (!completed_chunks.empty()) completed_chunks.pop();
        while (!completed_meshes.empty()) completed_meshes.pop_front();
        while (!completed_meshes_high_priority.empty()) completed_meshes_high_priority.pop_front();
        while (!completed_light_propagations.empty()) completed_light_propagations.pop();
        chunk_count_a.store(0, std::memory_order_relaxed);
        mesh_count_a.store(0, std::memory_order_relaxed);
        light_count_a.store(0, std::memory_order_relaxed);
    }

    // Returns true if the completed queue has room for more chunks.
    // Call this before enqueue_generation to prevent unbounded queue growth.
    // Lock-free fast-path: uses atomic counter so the generation loop never
    // acquires a mutex just to check queue depth.
    [[nodiscard]] bool can_enqueue(size_t max_completed) const noexcept {
        return chunk_count_a.load(std::memory_order_relaxed) < static_cast<int32_t>(max_completed);
    }

    // --- Lock-free "is this chunk already being generated?" -------------------
    //
    // Asking through enqueue_generation costs a GLOBAL mutex (contended with every
    // worker finishing a chunk) plus a shard lock on the chunk map inside the
    // is_already_loaded callback — three lock operations to learn that a chunk is
    // already on its way. The generation sweep offers plenty of such candidates
    // (46,576 in one observed session, 15% of its checks), and every one of those
    // checks is spent on terrain that does not need generating instead of on
    // terrain that does. This answers the same question with three relaxed atomic
    // loads and no lock at all.
    //
    // Approximate by construction, and safe in exactly the direction that matters.
    // A false POSITIVE defers a chunk to the walk's next cycle — the bits are
    // cleared when that generation finishes, and a bit shared with another
    // in-flight chunk is cleared no later than that chunk landing. A false
    // NEGATIVE only means the caller finds out the old way (enqueue_generation
    // refuses it). Neither can lose a chunk: the walk re-offers every candidate on
    // every cycle, so nothing is skipped permanently.
    static constexpr size_t kFilterBits = 1u << 16;   // 8 KB, several hundred in flight
    static constexpr size_t kFilterWords = kFilterBits / 64;
    static constexpr size_t kFilterProbes = 3;

    [[nodiscard]] bool may_be_generating(uint64_t chunk_key) const noexcept {
        for (size_t i = 0; i < kFilterProbes; ++i) {
            const size_t bit = filter_bit(chunk_key, i);
            const uint64_t word = inflight_bits_[bit >> 6].load(std::memory_order_relaxed);
            if ((word & (1ull << (bit & 63))) == 0) return false;
        }
        return true;
    }

    // Returns true if the chunk was enqueued for generation, false if already generating or loaded.
    template<typename IsLoaded, typename GenerateFn, typename EpochFn>
    bool enqueue_generation(ThreadPool* pool, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                            uint64_t chunk_key,
                            IsLoaded&& is_already_loaded,
                            GenerateFn&& generate_fn,
                            EpochFn&& epoch_provider) {
        if (!pool) return false;

        {
            std::lock_guard<std::mutex> lock(generating_mutex);
            if (is_already_loaded(chunk_key) || generating_chunks.find(chunk_key) != generating_chunks.end()) {
                return false;
            }
            generating_chunks.insert(chunk_key);
            filter_set(chunk_key);
        }

        pool->fire_and_forget([this, chunk_x, chunk_y, chunk_z, chunk_key, epoch,
                               generate_fn = std::forward<GenerateFn>(generate_fn),
                               epoch_provider = std::forward<EpochFn>(epoch_provider)]() mutable {
            bool loaded = false;
            auto chunk_data = generate_fn(chunk_x, chunk_y, chunk_z, loaded);

            CompletedChunk completed;
            completed.chunk_x = chunk_x;
            completed.chunk_y = chunk_y;
            completed.chunk_z = chunk_z;
            completed.epoch = epoch;
            completed.chunk_data = std::move(chunk_data);
            completed.was_loaded_from_disk = loaded;

            {
                std::lock_guard<std::mutex> lock(generating_mutex);
                generating_chunks.erase(chunk_key);
                // Cleared unconditionally, and BEFORE the epoch check below: a
                // generation dropped for a stale epoch still has to stop claiming
                // to be in flight, or the filter would defer that chunk forever.
                filter_clear(chunk_key);
            }

            if (epoch != epoch_provider()) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(completed_mutex);
                completed_chunks.push(std::move(completed));
                chunk_count_a.fetch_add(1, std::memory_order_relaxed);
            }
        });
        return true;
    }

    // Poll the next completed chunk. Returns false if the queue is empty.
    bool poll_completed_chunk(CompletedChunk& out) {
        std::lock_guard<std::mutex> lock(completed_mutex);
        if (completed_chunks.empty()) return false;
        out = std::move(completed_chunks.front());
        completed_chunks.pop();
        chunk_count_a.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    // Poll the next completed mesh. Returns false if both queues are empty.
    bool poll_completed_mesh(CompletedMesh& out, bool& high_priority) {
        std::lock_guard<std::mutex> lock(completed_mesh_mutex);
        if (!completed_meshes_high_priority.empty()) {
            out = std::move(completed_meshes_high_priority.front());
            completed_meshes_high_priority.pop_front();
            high_priority = true;
            mesh_count_a.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        if (!completed_meshes.empty()) {
            out = std::move(completed_meshes.front());
            completed_meshes.pop_front();
            high_priority = false;
            mesh_count_a.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Poll the completed mesh nearest to the player. Scans both queues for the
    // closest (by chunk distance) eligible completion and pops it, so a frame
    // budget that only allows a few uploads still spends them on the most
    // visible chunks. Entries whose epoch no longer matches are dropped while
    // scanning (they are stale — the chunk was regenerated or the world reset).
    bool poll_completed_mesh_nearest(int32_t pcx, int32_t pcy, int32_t pcz,
                                     uint64_t epoch, CompletedMesh& out, bool& high_priority) {
        std::lock_guard<std::mutex> lock(completed_mesh_mutex);

        auto scan_and_pop = [&](std::deque<CompletedMesh>& queue, bool is_high) -> bool {
            // Drop stale entries (wrong epoch) first so they cannot accumulate
            // and so mesh_count_a mirrors the queue depth. Swap-pop keeps each
            // removal O(1) instead of shifting the rest of the deque.
            for (size_t i = 0; i < queue.size();) {
                if (queue[i].epoch != epoch) {
                    queue[i] = std::move(queue.back());
                    queue.pop_back();
                    mesh_count_a.fetch_sub(1, std::memory_order_relaxed);
                    continue;  // re-check the swapped-in element
                }
                ++i;
            }
            int64_t best_dist = INT64_MAX;
            int best_idx = -1;
            for (size_t i = 0; i < queue.size(); i++) {
                const CompletedMesh& m = queue[i];
                int64_t dx = m.chunk_x - pcx;
                int64_t dy = m.chunk_y - pcy;
                int64_t dz = m.chunk_z - pcz;
                const int64_t dist = dx * dx + dy * dy + dz * dz;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = static_cast<int>(i);
                }
            }
            if (best_idx < 0) {
                return false;
            }
            out = std::move(queue[static_cast<size_t>(best_idx)]);
            queue[static_cast<size_t>(best_idx)] = std::move(queue.back());
            queue.pop_back();
            high_priority = is_high;
            mesh_count_a.fetch_sub(1, std::memory_order_relaxed);
            return true;
        };

        if (scan_and_pop(completed_meshes_high_priority, true)) {
            return true;
        }
        if (scan_and_pop(completed_meshes, false)) {
            return true;
        }
        return false;
    }

    void push_completed_mesh(CompletedMesh&& mesh, bool high_priority) {
        std::lock_guard<std::mutex> lock(completed_mesh_mutex);
        if (high_priority) {
            completed_meshes_high_priority.push_back(std::move(mesh));
        } else {
            completed_meshes.push_back(std::move(mesh));
        }
        mesh_count_a.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t generating_count() const {
        std::lock_guard<std::mutex> lock(generating_mutex);
        return generating_chunks.size();
    }

    // Lock-free count: uses atomic counter maintained by push/poll.
    // May be momentarily stale by ??1 ??? safe for the "is anything pending?" fast-path.
    [[nodiscard]] size_t completed_chunk_count() const noexcept {
        return static_cast<size_t>(std::max(0, chunk_count_a.load(std::memory_order_relaxed)));
    }

    [[nodiscard]] size_t completed_mesh_count() const noexcept {
        return static_cast<size_t>(std::max(0, mesh_count_a.load(std::memory_order_relaxed)));
    }

    [[nodiscard]] size_t completed_light_count() const noexcept {
        return static_cast<size_t>(std::max(0, light_count_a.load(std::memory_order_relaxed)));
    }

    void push_completed_light_propagation(CompletedLightPropagation&& prop) {
        std::lock_guard<std::mutex> lock(completed_light_mutex);
        completed_light_propagations.push(std::move(prop));
        light_count_a.fetch_add(1, std::memory_order_relaxed);
    }

    bool poll_completed_light_propagation(CompletedLightPropagation& out) {
        std::lock_guard<std::mutex> lock(completed_light_mutex);
        if (completed_light_propagations.empty()) return false;
        out = std::move(completed_light_propagations.front());
        completed_light_propagations.pop();
        light_count_a.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

private:
    // One bit position per probe. Splitmix-style mixing, because chunk keys are
    // built from packed coordinates and the low bits alone would cluster badly.
    [[nodiscard]] static size_t filter_bit(uint64_t chunk_key, size_t probe) noexcept {
        uint64_t h = chunk_key + 0x9E3779B97F4A7C15ull * (probe + 1);
        h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27; h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<size_t>(h) & (kFilterBits - 1);
    }

    void filter_set(uint64_t chunk_key) noexcept {
        for (size_t i = 0; i < kFilterProbes; ++i) {
            const size_t bit = filter_bit(chunk_key, i);
            inflight_bits_[bit >> 6].fetch_or(1ull << (bit & 63), std::memory_order_relaxed);
        }
    }

    // Note this clears bits, not entries: a bit shared with another chunk that is
    // still in flight is cleared early, which can only produce a false NEGATIVE
    // (the caller asks the slow way and is refused).
    void filter_clear(uint64_t chunk_key) noexcept {
        for (size_t i = 0; i < kFilterProbes; ++i) {
            const size_t bit = filter_bit(chunk_key, i);
            inflight_bits_[bit >> 6].fetch_and(~(1ull << (bit & 63)), std::memory_order_relaxed);
        }
    }

    std::array<std::atomic<uint64_t>, kFilterWords> inflight_bits_{};
    std::unordered_set<uint64_t> generating_chunks;
    std::queue<CompletedChunk> completed_chunks;
    std::deque<CompletedMesh> completed_meshes;
    std::deque<CompletedMesh> completed_meshes_high_priority;
    mutable std::mutex generating_mutex;
    mutable std::mutex completed_mutex;
    mutable std::mutex completed_mesh_mutex;
    mutable std::mutex completed_light_mutex;
    // Atomic counters mirror queue sizes for lock-free fast-path reads.
    std::atomic<int32_t> chunk_count_a{0};
    std::atomic<int32_t> mesh_count_a{0};
    std::atomic<int32_t> light_count_a{0};
    std::queue<CompletedLightPropagation> completed_light_propagations;
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_SCHEDULER_HPP