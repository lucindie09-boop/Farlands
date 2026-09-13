#ifndef FARLANDS_NAV_CHUNK_SOURCE_HPP
#define FARLANDS_NAV_CHUNK_SOURCE_HPP

// -----------------------------------------------------------------------------
// Planner cell source over the live chunk map.
//
// Two read paths, and the difference between them is lock scope:
//
//  * sample()      — one cell, one locked accessor call. Correct and simple, but
//                    a column scan of thirty cells takes thirty shared_lock
//                    acquisitions, each contending with chunk generation and
//                    meshing on the same shard.
//  * read_column() — a whole column range in one pass: the shards of the chunks
//                    the range touches are locked once (lock_keys, ascending
//                    order), every cell is classified through the
//                    caller-holds-the-lock accessor, and the lock is dropped
//                    before returning. NavView calls this when it is handed a
//                    reader, so a column scan costs one acquisition instead of
//                    thirty, and the chunk-map hash lookup drops to one per
//                    column.
//
// Neither path ever holds a lock between calls. That is deliberate: a lock held
// across calls must be released before the same thread takes another (or it can
// acquire shards out of order), and while it is held a writer on that shard —
// chunk generation inserting a chunk, an edit erasing one — blocks until the
// search gets around to reading another column. Scoping every lock to a single
// call is what makes this safe to run on a worker while the world keeps moving.
//
// Residency is sticky and negative-only: a chunk that is missing reports Unknown
// — never AIR — so a route can never be plotted through ungenerated space, and
// terrain that streams in mid-search stays Unknown so a search cannot validate a
// leg against ground it had already ruled out. Because the verdict lives in a
// cache rather than in a held lock, a chunk arriving mid-search is observed
// safely: the reader just keeps answering Unknown for it.
//
// An instance belongs to exactly one search and is not thread-safe; each
// PathService job owns its own.
// -----------------------------------------------------------------------------

#include "core/chunk_coords.hpp"
#include "core/chunk_map.hpp"
#include "pathfinding/block_class.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace nav {

class ChunkMapNavSource {
public:
    explicit ChunkMapNavSource(const ChunkMap& map) : map_(map) {}

    // One cell. Takes and releases a shared shard lock for that cell alone.
    [[nodiscard]] Cell sample(int32_t x, int32_t y, int32_t z) {
        if (y < 0 || y >= WORLD_HEIGHT_Y) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        ++cells_read_;
        ++lock_acquisitions_;  // the locked accessor below takes one

        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(x, y, z, cx, cy, cz, lx, ly, lz);
        if (!resident(cx, cy, cz)) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        return classify_block(static_cast<BlockID>(map_.get_block_world(x, y, z)));
    }

    // Every cell of (x, z) in [y_lo, y_hi], written to out[y - y_lo]. The whole
    // range is read under one lock per chunk slice, released before this
    // returns, so the caller gets a snapshot of the column with nothing held.
    void read_column(int32_t x, int32_t z, int32_t y_lo, int32_t y_hi, Cell* out) {
        if (out == nullptr || y_hi < y_lo) return;

        // The caller indexes out[y - y_lo] for the range it asked for, so cells
        // outside the world's vertical extent are filled as Unknown rather than
        // left to chance.
        const Cell unknown{CellClass::Unknown, 0.0f, 0.0f};
        for (int32_t y = y_lo; y <= y_hi; ++y) out[y - y_lo] = unknown;

        const int32_t lo = std::max(0, y_lo);
        const int32_t hi = std::min(WORLD_HEIGHT_Y - 1, y_hi);
        if (hi < lo) return;

        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(x, lo, z, cx, cy, cz, lx, ly, lz);

        for (int32_t cy_lo = lo / CHUNK_HEIGHT; cy_lo <= hi / CHUNK_HEIGHT;
             cy_lo += static_cast<int32_t>(kMaxSlices)) {
            const int32_t cy_hi = std::min(hi / CHUNK_HEIGHT, cy_lo + static_cast<int32_t>(kMaxSlices) - 1);

            keys_.clear();
            for (int32_t cy_it = cy_lo; cy_it <= cy_hi; ++cy_it) {
                keys_.push_back(map_.get_chunk_key(cx, cy_it, cz));
            }
            ++lock_acquisitions_;

            auto lock = map_.lock_keys(keys_);
            for (size_t i = 0; i < keys_.size(); ++i) {
                resident_[i] = resident_while_locked(keys_[i]) ? 1 : 0;
            }
            const int32_t slice_lo = std::max(lo, cy_lo * CHUNK_HEIGHT);
            const int32_t slice_hi = std::min(hi, (cy_hi + 1) * CHUNK_HEIGHT - 1);
            for (int32_t y = slice_lo; y <= slice_hi; ++y) {
                ++cells_read_;
                const size_t slot = static_cast<size_t>(y / CHUNK_HEIGHT - cy_lo);
                if (resident_[slot] == 0) continue;  // already Unknown from the prefill
                out[y - y_lo] = classify_block(static_cast<BlockID>(
                    map_.get_block_world_fast(x, y, z)));
            }
        }
    }

    // Cells classified, and how many times the map was locked to classify them:
    // one per cell through sample(), one per column (or per chunk slice of one)
    // through read_column(). The gap between the two counters is the point of the
    // ranged read, and the tests assert it.
    [[nodiscard]] size_t cells_read() const noexcept { return cells_read_; }
    [[nodiscard]] size_t lock_acquisitions() const noexcept { return lock_acquisitions_; }

private:
    // Chunk slices locked in one read_column pass. A range taller than this is
    // read in successive passes; NavView always asks for one column scan, which
    // spans at most four.
    static constexpr size_t kMaxSlices = 8;

    // The verdict for one chunk, decided once and never upgraded: a chunk that
    // was missing stays missing for the life of this source, so terrain that
    // streams in mid-search cannot appear underneath a leg the search already
    // validated. Called with the chunk's shard already locked.
    [[nodiscard]] bool resident_while_locked(uint64_t key) {
        const auto it = residency_.find(key);
        if (it != residency_.end()) return it->second != 0;
        const bool loaded = map_.contains_fast(key);
        residency_.emplace(key, loaded ? 1 : 0);
        return loaded;
    }

    // Same verdict for the per-cell path, which does not hold a lock yet.
    [[nodiscard]] bool resident(int32_t cx, int32_t cy, int32_t cz) {
        const uint64_t key = map_.get_chunk_key(cx, cy, cz);
        const auto it = residency_.find(key);
        if (it != residency_.end()) return it->second != 0;
        const bool loaded = map_.has_loaded_chunk(cx, cy, cz);
        residency_.emplace(key, loaded ? 1 : 0);
        return loaded;
    }

    const ChunkMap& map_;

    std::vector<uint64_t> keys_;
    std::array<uint8_t, kMaxSlices> resident_{};
    std::unordered_map<uint64_t, uint8_t> residency_;
    size_t cells_read_ = 0;
    size_t lock_acquisitions_ = 0;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_CHUNK_SOURCE_HPP
