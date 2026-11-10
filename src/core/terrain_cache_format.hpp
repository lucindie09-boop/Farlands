#ifndef FARLANDS_TERRAIN_CACHE_FORMAT_HPP
#define FARLANDS_TERRAIN_CACHE_FORMAT_HPP

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/terrain_params.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VoxelEngine {

// Phase 1 of the generated-terrain cache: a pure byte format and key. There is
// no file I/O here. A cache record is the generator's block output plus the
// CrossChunkWriter spill list; light, meshes and edit maps are deliberately not
// part of it.
inline constexpr uint32_t kTerrainCacheFormatVersion = 1;
inline constexpr uint32_t kTerrainCacheDataVersion = 1;
inline constexpr uint32_t kTerrainCachePackMagic = 0x31435446u; // "FTC1" in LE bytes
inline constexpr int32_t kTerrainCacheRegionChunks = 8;
inline constexpr size_t kTerrainCacheHeaderSize = 40;
inline constexpr size_t kTerrainCacheIndexEntrySize = 24;
inline constexpr size_t kTerrainCacheMaxChunkBytes = 1u << 20;
inline constexpr size_t kTerrainCacheMaxPackBytes = 64u << 20;
inline constexpr uint32_t kTerrainCacheMaxChunksPerPack =
    static_cast<uint32_t>(kTerrainCacheRegionChunks * kTerrainCacheRegionChunks *
                          (WORLD_HEIGHT_Y / CHUNK_HEIGHT));
inline constexpr uint32_t kTerrainCacheMaxSpills = 65536;

struct TerrainCacheSpill {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    BlockID block = 0;
};

struct TerrainCacheChunkRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t payload_offset = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc = 0;
};

struct TerrainCachePackHeader {
    uint32_t format_version = kTerrainCacheFormatVersion;
    uint64_t key = 0;
    int32_t region_x = 0;
    int32_t region_z = 0;
    uint32_t chunk_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_size = 0;
    uint32_t index_crc = 0;
};

struct TerrainCacheEncodedChunk {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::vector<uint8_t> payload;
};

// Encodes the eight block sections followed by the spill list. The result has
// no header or CRC; the pack index supplies those around it.
bool encode_terrain_cache_chunk(const ChunkData& chunk,
                                const std::vector<TerrainCacheSpill>& spill,
                                std::vector<uint8_t>& out);

// Decodes and validates a chunk payload. Unknown block IDs, a bad palette or
// index, a truncated section or an oversized spill list all return false; the
// caller treats that as a cache miss.
bool decode_terrain_cache_chunk(const uint8_t* data, size_t size,
                                const BlockRegistry& registry,
                                ChunkData& out_chunk,
                                std::vector<TerrainCacheSpill>& out_spill);

// Builds a complete pack: 40-byte header, payloads, then the index. Chunks are
// ordered by the caller; duplicate coordinates or coordinates outside the 8x8
// region are rejected. A valid pack fits entirely in the returned buffer.
bool encode_terrain_cache_pack(uint64_t key, int32_t region_x, int32_t region_z,
                               const std::vector<TerrainCacheEncodedChunk>& chunks,
                               std::vector<uint8_t>& out);

// Reads and validates the header and index. The expected key is the immutable
// cache key; a mismatch is a miss. This does not read any chunk payload.
bool decode_terrain_cache_pack_header(const uint8_t* data, size_t size,
                                      TerrainCachePackHeader& out_header);

// Decodes an index block already read from a pack by seek. `data` must be
// exactly `header.index_size` bytes. This is the reader's path: it never needs
// the payload half of a pack in memory.
bool decode_terrain_cache_pack_index_bytes(const uint8_t* data, size_t size,
                                           const TerrainCachePackHeader& header,
                                           std::vector<TerrainCacheChunkRecord>& out_index);

bool decode_terrain_cache_pack_index(const uint8_t* data, size_t size,
                                     uint64_t expected_key,
                                     TerrainCachePackHeader& out_header,
                                     std::vector<TerrainCacheChunkRecord>& out_index);

// Decodes one payload that was read by offset, checking the record's CRC first.
bool decode_terrain_cache_payload(const uint8_t* data, size_t size,
                                  const TerrainCacheChunkRecord& record,
                                  const BlockRegistry& registry,
                                  ChunkData& out_chunk,
                                  std::vector<TerrainCacheSpill>& out_spill);

// Decodes one record from a pack whose index has already been validated.
bool decode_terrain_cache_pack_chunk(const uint8_t* data, size_t size,
                                     const TerrainCacheChunkRecord& record,
                                     const BlockRegistry& registry,
                                     ChunkData& out_chunk,
                                     std::vector<TerrainCacheSpill>& out_spill);

// FNV-1a 64 over typed fields and length-prefixed raw config bytes. No Godot
// types and no file access, so the key can be tested on the core target.
class TerrainCacheKeyBuilder {
public:
    void add_bytes(const void* data, size_t size);
    void add_u32(uint32_t value);
    void add_i32(int32_t value);
    void add_u64(uint64_t value);
    void add_float(float value);
    [[nodiscard]] uint64_t finish() const noexcept { return state_; }

private:
    void add_byte(uint8_t value) noexcept;
    uint64_t state_ = 14695981039346656037ULL; // FNV-1a 64 offset basis
};

// The key covers the generator's persisted inputs and the raw data files that
// define biomes, vegetation and block properties. Macro-surface tuning is
// covered by the raw terrain_config bytes.
uint64_t terrain_cache_key(const TerrainParams& params,
                           const std::vector<uint8_t>& terrain_config,
                           const std::vector<uint8_t>& biomes,
                           const std::vector<uint8_t>& vegetation,
                           const std::vector<uint8_t>& block_definitions);

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_FORMAT_HPP
