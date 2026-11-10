#ifndef FARLANDS_TERRAIN_CACHE_MANIFEST_HPP
#define FARLANDS_TERRAIN_CACHE_MANIFEST_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VoxelEngine {

// `cache.meta` is written last by the producer, so finding it is what makes a
// cache directory complete. The packs it counts are named by region and are
// discovered by the adapter; the manifest carries the key that must match them.
inline constexpr uint32_t kTerrainCacheManifestMagic = 0x314D5446u; // "FTM1" in LE bytes
inline constexpr uint32_t kTerrainCacheManifestVersion = 1;
inline constexpr size_t kTerrainCacheManifestSize = 36;
inline constexpr uint32_t kTerrainCacheMaxPacks = 4096;

struct TerrainCacheManifest {
    uint32_t format_version = kTerrainCacheManifestVersion;
    uint64_t key = 0;
    uint32_t pack_count = 0;
    uint64_t total_bytes = 0;
    uint64_t created_unix = 0;
};

bool encode_terrain_cache_manifest(const TerrainCacheManifest& manifest,
                                   std::vector<uint8_t>& out);
bool decode_terrain_cache_manifest(const uint8_t* data, size_t size,
                                   TerrainCacheManifest& out_manifest);

// `cache.meta` inside a cache root, and the region-named pack files beside it.
std::string terrain_cache_manifest_path(const std::string& cache_root);
std::string terrain_cache_pack_filename(int32_t region_x, int32_t region_z);
bool parse_terrain_cache_pack_filename(const std::string& filename,
                                       int32_t& out_region_x, int32_t& out_region_z);

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_MANIFEST_HPP