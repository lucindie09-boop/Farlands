#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "core/terrain_params.hpp"
#include <cstdint>
#include <cmath>

using namespace VoxelEngine;

namespace {

// Scans a grid for any column of the given biome. Returns false if the biome
// does not occur anywhere inside the probe window.
bool find_seed_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    for (int32_t z = -4096; z <= 4096; z += 64) {
        for (int32_t x = -4096; x <= 4096; x += 64) {
            if (gen.get_biome(x, z) == target) {
                wx = x;
                wz = z;
                return true;
            }
        }
    }
    return false;
}

} // namespace

// =========================================================================
// Biome name helpers (biome_name / biome_from_name)
// =========================================================================

TEST_CASE("biome names: round-trip through biome_name / biome_from_name") {
    for (int i = 0; i < static_cast<int>(BiomeType::Count); ++i) {
        const BiomeType b = static_cast<BiomeType>(i);
        const char* name = biome_name(b);
        CHECK(name != nullptr);
        if (name != nullptr) {
            CHECK(name[0] != '\0');
            BiomeType parsed;
            CHECK(biome_from_name(name, parsed));
            CHECK(parsed == b);
        }
    }

    // Case-insensitive lookup.
    BiomeType parsed;
    CHECK(biome_from_name("OCEAN", parsed));
    CHECK(parsed == BiomeType::Ocean);
    CHECK(biome_from_name("Hills", parsed));
    CHECK(parsed == BiomeType::Hills);

    // Unknown names are rejected.
    CHECK_FALSE(biome_from_name("mushroom", parsed));
    CHECK_FALSE(biome_from_name("", parsed));
    CHECK_FALSE(biome_from_name(nullptr, parsed));
}

// =========================================================================
// find_nearest_biome — deterministic single-biome worlds
// =========================================================================

TEST_CASE("locate biome: all-land world has land biomes, no ocean") {
    TerrainParams params;
    params.sea_level = -100000.0f;  // every column sits above sea level -> no Ocean
    ChunkGenerator gen(params);

    // The climate grid splits land into Hills and Plains, so no fixed point is
    // guaranteed to be Hills: scan for one to use as the search center.
    int32_t hx = 0, hz = 0;
    bool found_hills = false;
    for (int32_t z = -24000; z <= 24000 && !found_hills; z += 500) {
        for (int32_t x = -24000; x <= 24000 && !found_hills; x += 500) {
            if (gen.get_biome(x, z) == BiomeType::Hills) {
                hx = x;
                hz = z;
                found_hills = true;
            }
        }
    }
    if (!found_hills) {
        MESSAGE("No hills column found in the all-land probe window; skipping");
        return;
    }

    int32_t x = 0, z = 0;
    float h = 0.0f;

    // Center is already the target biome: found at distance zero.
    CHECK(gen.find_nearest_biome(BiomeType::Hills, hx, hz, 1000, x, z, h));
    CHECK(x == hx);
    CHECK(z == hz);
    CHECK(gen.get_biome(x, z) == BiomeType::Hills);
    CHECK(std::abs(h - gen.get_terrain_height(x, z)) < 0.01f);

    // Plains also exist on land and are locatable.
    CHECK(gen.find_nearest_biome(BiomeType::Plains, hx, hz, 100000, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Plains);

    // Oceans do not exist in an all-land world.
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Ocean, 0, 0, 1000, x, z, h));
}

TEST_CASE("locate biome: all-ocean world is entirely ocean") {
    TerrainParams params;
    params.sea_level = 100000.0f;  // every column sits below sea level -> Ocean
    ChunkGenerator gen(params);

    int32_t x = 0, z = 0;
    float h = 0.0f;

    CHECK(gen.find_nearest_biome(BiomeType::Ocean, 77, 88, 1000, x, z, h));
    CHECK(x == 77);
    CHECK(z == 88);
    CHECK(gen.get_biome(x, z) == BiomeType::Ocean);

    // Hills do not exist in an all-ocean world.
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Hills, 0, 0, 1000, x, z, h));
}

// =========================================================================
// find_nearest_biome — mixed land/ocean in the default world
// =========================================================================

TEST_CASE("locate biome: default world crosses land <-> ocean boundaries") {
    TerrainParams params;
    ChunkGenerator gen(params);

    int32_t land_x = 0, land_z = 0;
    int32_t ocean_x = 0, ocean_z = 0;
    if (!find_seed_column(gen, BiomeType::Hills, land_x, land_z) ||
        !find_seed_column(gen, BiomeType::Ocean, ocean_x, ocean_z)) {
        MESSAGE("Default world lacks both biomes inside the probe window; skipping cross-search");
        return;
    }

    int32_t x = 0, z = 0;
    float h = 0.0f;
    const int64_t radius = 8192;

    // From the middle of the ocean, the nearest hills must be found and the
    // result must actually be hills.
    CHECK(gen.find_nearest_biome(BiomeType::Hills, ocean_x, ocean_z, radius, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Hills);

    // From the middle of the land, the nearest ocean must be found and the
    // result must actually be ocean. It can be no farther than a loose bound
    // over the known ocean seed column.
    CHECK(gen.find_nearest_biome(BiomeType::Ocean, land_x, land_z, radius, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Ocean);
    const int64_t dx = static_cast<int64_t>(x) - land_x;
    const int64_t dz = static_cast<int64_t>(z) - land_z;
    const int64_t d2 = dx * dx + dz * dz;
    const int64_t seed_dx = static_cast<int64_t>(ocean_x) - land_x;
    const int64_t seed_dz = static_cast<int64_t>(ocean_z) - land_z;
    const int64_t seed_d2 = seed_dx * seed_dx + seed_dz * seed_dz;
    // Found distance <= 2 * known distance + 64 (ring-grid slack), and always
    // inside the requested radius.
    CHECK(d2 <= 4 * seed_d2 + 8192);
    CHECK(d2 <= radius * radius);
    CHECK(std::abs(h - gen.get_terrain_height(x, z)) < 0.01f);
}