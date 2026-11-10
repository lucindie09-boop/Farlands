#ifndef FARLANDS_TERRAIN_CACHE_WRITER_HPP
#define FARLANDS_TERRAIN_CACHE_WRITER_HPP

#include "core/terrain_cache_format.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VoxelEngine {

// Phase 3 of the generated-terrain cache: staging and pack assembly, with no
// file I/O of its own. The adapter owns the mutex (stage runs on generation
// workers, take_ready on the main thread) and hands the ready regions to the
// pool, which merges them with the region's existing pack and writes it.
inline constexpr double kTerrainCacheRegionIdleSeconds = 1.5;
inline constexpr size_t kTerrainCacheRegionFlushChunks = 192;
inline constexpr size_t kTerrainCacheStagedChunkLimit = 4096;

struct TerrainCacheWriterStats {
    uint64_t chunks_staged = 0;
    uint64_t chunks_written = 0;
    uint64_t chunks_merged = 0;
    uint64_t packs_written = 0;
    uint64_t encode_failures = 0;
    uint64_t write_failures = 0;
    uint64_t bytes_written = 0;
    uint64_t encode_us = 0;
    uint64_t write_us = 0;
    size_t staged_chunks = 0;
    size_t staged_regions = 0;
};

// The pack-grid region a chunk belongs to, shared by the adapter's in-flight
// set and the writer's own ready check.
inline uint64_t terrain_cache_region_key(int32_t region_x, int32_t region_z) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(region_x)) << 32) |
           static_cast<uint32_t>(region_z);
}

// Holds encoded chunks per 8x8-column region until the region is worth a pack.
// Not thread-safe by design: the adapter serialises stage/take_ready with one
// mutex, which is also why the class has no clock of its own (the caller passes
// monotonic seconds, so the idle rule is testable).
class TerrainCacheWriter {
public:
    struct ReadyRegion {
        int32_t region_x = 0;
        int32_t region_z = 0;
        std::vector<TerrainCacheEncodedChunk> chunks;
    };

    // Adds one encoded chunk. A repeated coordinate replaces the staged payload,
    // so a region that is touched again after a flush still ends with the newest
    // answer. Returns false for an empty or oversized payload (the cache simply
    // omits that chunk).
    bool stage(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
               std::vector<uint8_t> payload, double now_seconds);

    // Moves out regions that are full or have been idle for
    // `kTerrainCacheRegionIdleSeconds`, then the oldest regions while the
    // staged total is above `kTerrainCacheStagedChunkLimit`. A region in `busy`
    // is left alone: its previous pack is still being written, and flushing it
    // now would race that write.
    void take_ready(double now_seconds, std::vector<ReadyRegion>& out,
                    const std::unordered_set<uint64_t>* busy = nullptr);

    // Builds the pack for one ready region. `existing` is the pack currently on
    // disk (empty when there is none, or when it could not be read); its records
    // are carried over unless a fresh chunk has the same coordinate. A corrupt
    // or wrong-key existing pack is ignored and the fresh chunks are written
    // alone, which heals that region's cache file. `out_merged_chunks` counts
    // the carried records.
    static bool build_pack(uint64_t key, const ReadyRegion& region,
                           const std::vector<uint8_t>& existing,
                           std::vector<uint8_t>& out, size_t& out_merged_chunks);

    void note_pack_written(size_t fresh_chunks, size_t merged_chunks, size_t bytes,
                           uint64_t encode_us, uint64_t write_us);
    void note_encode_time(uint64_t encode_us);
    void note_encode_failure();
    void note_write_failure();

    [[nodiscard]] TerrainCacheWriterStats snapshot_stats() const;
    void reset_stats();
    void clear();

    [[nodiscard]] size_t staged_region_count() const noexcept { return regions_.size(); }

private:
    struct RegionBuffer {
        std::vector<TerrainCacheEncodedChunk> chunks;
        double last_stage = 0.0;
    };

    std::map<std::pair<int32_t, int32_t>, RegionBuffer> regions_;
    TerrainCacheWriterStats stats_;
};

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_WRITER_HPP
