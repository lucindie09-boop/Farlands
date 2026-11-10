#include "core/terrain_cache_format.hpp"

#include <cstring>

namespace VoxelEngine {

void TerrainCacheKeyBuilder::add_byte(uint8_t value) noexcept {
    state_ ^= value;
    state_ *= 1099511628211ULL;
}

void TerrainCacheKeyBuilder::add_bytes(const void* data, size_t size) {
    if (!data) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) add_byte(bytes[i]);
}

void TerrainCacheKeyBuilder::add_u32(uint32_t value) {
    for (int i = 0; i < 4; ++i) add_byte(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void TerrainCacheKeyBuilder::add_i32(int32_t value) {
    add_u32(static_cast<uint32_t>(value));
}

void TerrainCacheKeyBuilder::add_u64(uint64_t value) {
    for (int i = 0; i < 8; ++i) add_byte(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void TerrainCacheKeyBuilder::add_float(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    add_u32(bits);
}

uint64_t terrain_cache_key(const TerrainParams& params,
                           const std::vector<uint8_t>& terrain_config,
                           const std::vector<uint8_t>& biomes,
                           const std::vector<uint8_t>& vegetation,
                           const std::vector<uint8_t>& block_definitions) {
    TerrainCacheKeyBuilder key;
    key.add_u32(kTerrainCacheDataVersion);
    key.add_i32(params.seed);
    key.add_float(params.sea_level);
    key.add_i32(params.bedrock_height);
    key.add_float(params.cave_threshold);
    key.add_float(params.cave_scale);
    key.add_i32(params.subsurface_cover_depth);
    key.add_float(params.climate_temp_scale);
    key.add_float(params.climate_humidity_scale);
    key.add_float(params.biome_size);

    const std::vector<uint8_t>* files[] = {
        &terrain_config, &biomes, &vegetation, &block_definitions,
    };
    for (const std::vector<uint8_t>* file : files) {
        key.add_u64(file->size());
        key.add_bytes(file->data(), file->size());
    }
    return key.finish();
}

} // namespace VoxelEngine
