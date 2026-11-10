#ifndef FARLANDS_TERRAIN_CACHE_READER_HPP
#define FARLANDS_TERRAIN_CACHE_READER_HPP

#include "core/terrain_cache_format.hpp"
#include "core/terrain_cache_manifest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {

// Counters are read from generation workers, so the live class keeps atomics and
// this struct is the main-thread snapshot. Times are microseconds accumulated
// over reads; callers convert for display.
struct TerrainCacheReaderStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t corrupt = 0;
    uint64_t read_errors = 0;
    uint64_t payload_bytes = 0;
    uint64_t read_us = 0;
    uint64_t decode_us = 0;
    uint64_t pack_count = 0;
    uint64_t index_entries = 0;
    uint64_t index_bytes = 0;
    uint64_t manifest_bytes = 0;
    uint64_t open_failures = 0;

    [[nodiscard]] double hit_rate() const noexcept {
        const uint64_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// Read-only generated-terrain cache. The manifest and every pack's trailing
// index are loaded once into RAM; a chunk read is then one seek and one read of
// that chunk's payload. There is deliberately no pack-payload cache here.
//
// `ReadCallback` is called from the thread that reads a chunk, including several
// workers at once. A production callback must therefore keep one handle per
// thread; the pure reader itself only mutates counters and never the loaded
// indexes.
class TerrainCacheReader {
public:
    using ReadCallback = std::function<bool(const std::string& path, uint64_t offset,
                                            size_t size, std::vector<uint8_t>& out)>;

    TerrainCacheReader() = default;

    // Discovers nothing itself: `pack_paths` is the adapter's list of `.ftc`
    // files. On any manifest, pack-header, index or key problem the cache stays
    // closed and the caller treats every chunk as a miss.
    bool open(const std::string& cache_root, uint64_t key,
              const std::vector<std::string>& pack_paths, ReadCallback read_callback);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] uint64_t key() const noexcept { return key_; }
    [[nodiscard]] const std::string& root() const noexcept { return root_; }
    [[nodiscard]] TerrainCacheReaderStats snapshot_stats() const;
    void reset_stats();

    bool read_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                    const BlockRegistry& registry, ChunkData& out_chunk,
                    std::vector<TerrainCacheSpill>& out_spill);

private:
    struct PackFile {
        std::string path;
        TerrainCachePackHeader header;
    };

    struct PackLocation {
        size_t pack_index = 0;
        TerrainCacheChunkRecord record;
    };

    std::string root_;
    uint64_t key_ = 0;
    std::vector<PackFile> packs_;
    std::unordered_map<uint64_t, PackLocation> chunks_;
    ReadCallback read_;
    bool open_ = false;

    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> corrupt_{0};
    mutable std::atomic<uint64_t> read_errors_{0};
    mutable std::atomic<uint64_t> payload_bytes_{0};
    mutable std::atomic<uint64_t> read_us_{0};
    mutable std::atomic<uint64_t> decode_us_{0};
    mutable std::atomic<uint64_t> index_bytes_{0};
    mutable std::atomic<uint64_t> manifest_bytes_{0};
    mutable std::atomic<uint64_t> open_failures_{0};
};

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_READER_HPP