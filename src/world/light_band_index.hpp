#ifndef FARLANDS_LIGHT_BAND_INDEX_HPP
#define FARLANDS_LIGHT_BAND_INDEX_HPP

#include "core/chunk_coords.hpp"

#include <cstdint>
#include <unordered_map>

namespace VoxelEngine {

// Event-maintained answer to "is there anything light-relevant in this
// 3x3x3?". The install path used to answer it by walking 27 ChunkData pointers
// under a 28-key shared lock, once per installed chunk. This index is updated
// when a chunk is installed, unloaded or edited, and the query is nine column
// lookups: ChunkMap shards by column, and a 3x3x3 band spans exactly nine of
// them.
//
// "Light-relevant" is the same predicate the walk used: the chunk has an
// emissive block or its sky scan recorded shade. Two masks keep the channels
// separate because the worker needs to know which pass to run; combining them
// would either skip a real block-light pass or run a sky pass for free.
//
// A stale true is harmless (it buys one unnecessary pass); a false is not. The
// update points are installs, unloads and add_block_edit(s), each of which
// re-reads the chunk after its write, so an edit that creates an emitter or
// shade cannot leave the flag behind. Clearing a flag is best-effort: the shade
// flag is sticky in ChunkData anyway, and leaving a bit set only costs work.
class LightBandIndex {
public:
    static constexpr int32_t kChunkLevels = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    static_assert(kChunkLevels > 0 && kChunkLevels <= 32,
                  "LightBandIndex uses one 32-bit mask per column and channel");

    struct Flags {
        bool emissive = false;
        bool shade = false;

        [[nodiscard]] bool any() const noexcept { return emissive || shade; }
    };

    // Replaces this chunk's bits with the flags it currently holds. Install and
    // edit paths both call this after the chunk data is final.
    void set_chunk(int32_t cx, int32_t cy, int32_t cz, bool emissive, bool shade) {
        if (cy < 0 || cy >= kChunkLevels) {
            // The world has no such slice. Keep the conservative answer instead
            // of silently dropping a source that a test or a future taller world
            // put there.
            overflow_emissive_ = overflow_emissive_ || emissive;
            overflow_shade_ = overflow_shade_ || shade;
            return;
        }
        const uint64_t key = column_key(cx, cz);
        auto it = columns_.find(key);
        if (it == columns_.end()) {
            if (!emissive && !shade) return;
            it = columns_.emplace(key, Column{}).first;
        }
        const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(cy);
        if (emissive) it->second.emissive |= bit;
        else it->second.emissive &= ~bit;
        if (shade) it->second.shade |= bit;
        else it->second.shade &= ~bit;
        if (it->second.emissive == 0 && it->second.shade == 0) columns_.erase(it);
    }

    void erase_chunk(int32_t cx, int32_t cy, int32_t cz) {
        if (cy < 0 || cy >= kChunkLevels) return;
        auto it = columns_.find(column_key(cx, cz));
        if (it == columns_.end()) return;
        const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(cy);
        it->second.emissive &= ~bit;
        it->second.shade &= ~bit;
        if (it->second.emissive == 0 && it->second.shade == 0) columns_.erase(it);
    }

    // Only the 3x3x3's nine columns are touched. A chunk outside the world's
    // normal slice range makes the query conservative, never a false negative.
    [[nodiscard]] Flags query(int32_t cx, int32_t cy, int32_t cz) const {
        Flags flags{overflow_emissive_, overflow_shade_};
        if (cy < 0 || cy >= kChunkLevels) return {true, true};

        uint32_t levels = uint32_t{1} << static_cast<uint32_t>(cy);
        if (cy > 0) levels |= uint32_t{1} << static_cast<uint32_t>(cy - 1);
        if (cy + 1 < kChunkLevels) levels |= uint32_t{1} << static_cast<uint32_t>(cy + 1);

        for (int32_t dz = -1; dz <= 1; ++dz) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                auto it = columns_.find(column_key(cx + dx, cz + dz));
                if (it == columns_.end()) continue;
                flags.emissive = flags.emissive || (it->second.emissive & levels) != 0;
                flags.shade = flags.shade || (it->second.shade & levels) != 0;
                if (flags.emissive && flags.shade) return flags;
            }
        }
        return flags;
    }

    void clear() {
        columns_.clear();
        overflow_emissive_ = false;
        overflow_shade_ = false;
    }

    [[nodiscard]] size_t column_count() const noexcept { return columns_.size(); }

private:
    struct Column {
        uint32_t emissive = 0;
        uint32_t shade = 0;
    };

    static uint64_t column_key(int32_t cx, int32_t cz) noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
               static_cast<uint32_t>(cz);
    }

    std::unordered_map<uint64_t, Column> columns_;
    bool overflow_emissive_ = false;
    bool overflow_shade_ = false;
};

} // namespace VoxelEngine

#endif // FARLANDS_LIGHT_BAND_INDEX_HPP