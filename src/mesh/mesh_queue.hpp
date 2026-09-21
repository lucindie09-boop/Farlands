#ifndef FARLANDS_MESH_QUEUE_HPP
#define FARLANDS_MESH_QUEUE_HPP
#include "core/chunk_map.hpp"
#include "core/frustum.hpp"
#include <cstdint>
#include <algorithm>
#include <queue>
#include <deque>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Dirty mesh queue entry (priority by distance)
// -------------------------------------------------------------------------
struct DirtyChunkEntry {
    uint64_t key = 0;
    int32_t dist_sq = 0;
    bool urgent = false;
    // View-first meshing: a rebuild you can SEE jumps the queue. This is the
    // one remaining frustum priority, kept deliberately after an A/B (see
    // ARCHITECTURE.md) — generation is chain/distance-ordered and frustum-free,
    // but the MESH pass runs view-first, which is what makes terrain appear
    // where the player is looking before it appears at their back.
    // It is no longer an unbounded tier: entries are routed into one of two
    // heaps by this flag, and every frame a slice of the budget is spent on the
    // non-visible heap in plain distance order. See kBacklogReserveDivisor.
    bool in_frustum = false;
    uint32_t priority_revision = 0;
    bool operator>(const DirtyChunkEntry& other) const {
        if (urgent != other.urgent) return !urgent && other.urgent;
        if (in_frustum != other.in_frustum) return !in_frustum && other.in_frustum;
        return dist_sq > other.dist_sq;
    }
};

// -------------------------------------------------------------------------
// Mesh queue — manages the priority queue of chunks that need mesh rebuilds.
// -------------------------------------------------------------------------
class MeshQueue {
public:
    // Fraction of the per-frame rebuild budget spent strictly in distance order,
    // ignoring the frustum tier.
    //
    // Why this is needed: during streaming the dirty queue is permanently
    // saturated — every installed chunk dirties itself and its six neighbours,
    // which is far more than the 16/frame active mesh budget can drain — so an
    // unbounded view-first tier means a chunk that is merely *not currently
    // visible* (behind the player, or at the edge of the frustum) never reaches
    // the front at all. It stays un-meshed until the player turns round and
    // looks at it, at which point it jumps the queue and appears. That is the
    // "forgets to mesh until you look at it" artifact. Reserving a quarter of
    // the budget for the distance-ordered backlog bounds the delay a visible
    // chunk can impose on an invisible one; the far-region LOD tiers (also
    // distance-ordered, no frustum term) behave this way already.
    static constexpr int32_t kBacklogReserveDivisor = 4;

    void clear() {
        while (!dirty_mesh_queue.empty()) dirty_mesh_queue.pop();
        while (!backlog_queue.empty()) backlog_queue.pop();
        dirty_mesh_pending.clear();
        immediate_dirty_mesh_queue.clear();
        immediate_dirty_mesh_pending.clear();
        skip_next_dirty_mesh_rebuild.clear();
        urgent_mesh_chunks.clear();
        player_chunk_x_ = INT32_MIN;
        player_chunk_y_ = INT32_MIN;
        player_chunk_z_ = INT32_MIN;
        frustum_ = nullptr;
        priority_revision_ = 0;
    }

    void queue_dirty_chunk(uint64_t key, int32_t dist_sq, bool urgent) {
        if (!dirty_mesh_pending.insert(key).second) {
            return;
        }
        DirtyChunkEntry entry;
        entry.key = key;
        entry.dist_sq = dist_sq;
        entry.urgent = urgent || is_urgent(key);
        entry.priority_revision = priority_revision_;
        if (frustum_ && player_chunk_x_ != INT32_MIN) {
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            entry.in_frustum = frustum_->is_chunk_visible(cx, cy, cz);
        }
        push_entry(entry);
    }

    void queue_immediate_dirty_chunk(uint64_t key, bool is_already_dirty) {
        if (is_already_dirty) {
            skip_next_dirty_mesh_rebuild.insert(key);
        }
        if (!immediate_dirty_mesh_pending.insert(key).second) {
            return;
        }
        immediate_dirty_mesh_queue.push_back(key);
    }

    void mark_urgent(uint64_t key) {
        urgent_mesh_chunks.insert(key);
    }

    bool is_urgent(uint64_t key) const {
        return urgent_mesh_chunks.find(key) != urgent_mesh_chunks.end();
    }

    bool erase_urgent(uint64_t key) {
        return urgent_mesh_chunks.erase(key) > 0;
    }

    void reprioritize(int32_t player_chunk_x, int32_t player_chunk_y, int32_t player_chunk_z,
                      const Frustum* frustum = nullptr) {
        player_chunk_x_ = player_chunk_x;
        player_chunk_y_ = player_chunk_y;
        player_chunk_z_ = player_chunk_z;
        frustum_ = frustum;
        ++priority_revision_;
    }

    // Process the mesh queues. Calls the provided callback for each chunk that
    // needs rebuilding; the callback returns false when the entry resolved to no
    // work (beyond mesh range), so the frame's budget is not spent on it.
    // Returns the number of rebuilds processed.
template<typename RebuildCallback>
    int32_t process(RebuildCallback&& rebuild_callback,
                    int32_t max_immediate_rebuilds,
                    int32_t max_rebuilds,
                    double budget_ms) {
        int32_t total_processed = 0;
        int32_t immediate_processed = 0;

        while (!immediate_dirty_mesh_queue.empty() &&
               immediate_processed < max_immediate_rebuilds) {
            const uint64_t key = immediate_dirty_mesh_queue.front();
            immediate_dirty_mesh_queue.pop_front();
            immediate_dirty_mesh_pending.erase(key);

            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            rebuild_callback(cx, cy, cz);
            immediate_processed++;
        }
        total_processed += immediate_processed;

        const auto start_time = std::chrono::high_resolution_clock::now();
        const auto elapsed_ms = [&start_time]() {
            const auto now = std::chrono::high_resolution_clock::now();
            return std::chrono::duration<double, std::milli>(now - start_time).count();
        };
        // Bound the number of heap operations per frame. Entries that resolve to
        // no work are consumed silently, so without a cap a queue of thousands of
        // out-of-range entries could spin until the time budget expired.
        int32_t pops = 0;
        const int32_t pop_cap = std::max(64, max_rebuilds * 8);

        const int32_t backlog_reserve =
            (frustum_ && max_rebuilds > 1) ? std::max(1, max_rebuilds / kBacklogReserveDivisor) : 0;
        const int32_t view_budget = std::max(0, max_rebuilds - backlog_reserve);

        const int32_t view_done =
            drain(dirty_mesh_queue, view_budget, pops, pop_cap, budget_ms, elapsed_ms, rebuild_callback);
        // Whatever the view pass could not spend (an empty or thin visible set) is
        // handed to the backlog, so a frame with nothing visible to build still
        // drains a full budget instead of idling at the reserve.
        const int32_t backlog_budget = backlog_reserve + (view_budget - view_done);
        const int32_t backlog_done =
            drain(backlog_queue, backlog_budget, pops, pop_cap, budget_ms, elapsed_ms, rebuild_callback);
        total_processed += view_done + backlog_done;

        return total_processed;
    }

    [[nodiscard]] bool is_pending(uint64_t key) const {
        return dirty_mesh_pending.find(key) != dirty_mesh_pending.end();
    }

    [[nodiscard]] bool is_immediate_pending(uint64_t key) const {
        return immediate_dirty_mesh_pending.find(key) != immediate_dirty_mesh_pending.end();
    }

    [[nodiscard]] size_t size() const { return dirty_mesh_queue.size() + backlog_queue.size(); }
    [[nodiscard]] size_t immediate_size() const { return immediate_dirty_mesh_queue.size(); }

private:
    using EntryHeap = std::priority_queue<DirtyChunkEntry, std::vector<DirtyChunkEntry>, std::greater<DirtyChunkEntry>>;

    void push_entry(const DirtyChunkEntry& entry) {
        // Without a frustum there is no view to prioritise, so everything goes
        // into the distance-ordered heap and the whole budget drains it.
        if (!frustum_ || entry.in_frustum) {
            dirty_mesh_queue.push(entry);
        } else {
            backlog_queue.push(entry);
        }
    }

    // Drain one heap. `pops` and `pop_cap` bound the total heap operations for
    // the whole frame across both heaps. Entries whose priority revision is
    // stale are recomputed (position, urgency, visibility) and re-routed into
    // whichever heap now matches — that is how entries migrate between the
    // view-first tier and the distance-ordered backlog as the player turns.
    template<typename ElapsedFn, typename RebuildCallback>
    int32_t drain(EntryHeap& heap, int32_t budget, int32_t& pops, int32_t pop_cap,
                  double budget_ms, ElapsedFn&& elapsed_ms, RebuildCallback&& rebuild_callback) {
        int32_t done = 0;
        while (done < budget && pops < pop_cap && !heap.empty()) {
            if (elapsed_ms() >= budget_ms) break;

            DirtyChunkEntry entry = heap.top();
            heap.pop();
            ++pops;

            if (entry.priority_revision != priority_revision_ && player_chunk_x_ != INT32_MIN) {
                int32_t cx, cy, cz;
                ChunkMap::decode_chunk_key(entry.key, cx, cy, cz);
                const int32_t dx = cx - player_chunk_x_;
                const int32_t dy = cy - player_chunk_y_;
                const int32_t dz = cz - player_chunk_z_;
                entry.dist_sq = dx * dx + dy * dy + dz * dz;
                entry.urgent = entry.urgent || is_urgent(entry.key);
                entry.in_frustum = frustum_ ? frustum_->is_chunk_visible(cx, cy, cz) : false;
                entry.priority_revision = priority_revision_;
                push_entry(entry);
                continue;
            }

            dirty_mesh_pending.erase(entry.key);
            if (skip_next_dirty_mesh_rebuild.erase(entry.key) > 0) {
                continue;
            }

            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(entry.key, cx, cy, cz);
            if (rebuild_callback(cx, cy, cz)) {
                ++done;
            }
        }
        return done;
    }

    EntryHeap dirty_mesh_queue;
    EntryHeap backlog_queue;
    std::unordered_set<uint64_t> dirty_mesh_pending;
    std::deque<uint64_t> immediate_dirty_mesh_queue;
    std::unordered_set<uint64_t> immediate_dirty_mesh_pending;
    std::unordered_set<uint64_t> skip_next_dirty_mesh_rebuild;
    std::unordered_set<uint64_t> urgent_mesh_chunks;
    int32_t player_chunk_x_ = INT32_MIN;
    int32_t player_chunk_y_ = INT32_MIN;
    int32_t player_chunk_z_ = INT32_MIN;
    const Frustum* frustum_ = nullptr;
    uint32_t priority_revision_ = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_MESH_QUEUE_HPP
