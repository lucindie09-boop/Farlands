#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/biome_search_config.hpp"
#include "core/terrain_params.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>

using namespace VoxelEngine;

namespace {

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
// The radius the chat asks for is one the search can actually cover
// =========================================================================

TEST_CASE("locate biome: advertised radius fits the probe budget") {
    constexpr int32_t radius = biome_search::kRadiusBlocks;
    constexpr int32_t ring = radius / biome_search::kStep;
    CHECK(radius == 864);             // ring 54, not the old chat's 3,000
    CHECK(ring == 54);
    // Rings 0..54 cost 1 + 4*54*55 = 11,881 probes, inside the 12,000 cap;
    // the next ring starts beyond the advertised radius.
    CHECK(1 + 4 * ring * (ring + 1) <= biome_search::kMaxProbes);
    CHECK((ring + 1) * biome_search::kStep > radius);
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

    // The center is already Hills, and that is the patch underfoot rather than an
    // answer: the search skips the patch it is standing in and finds the nearest
    // Hills beyond it.
    CHECK(gen.find_nearest_biome(BiomeType::Hills, hx, hz, 1000, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Hills);
    const bool underfoot = (x == hx && z == hz);
    CHECK_FALSE(underfoot);
    CHECK(std::abs(h - gen.get_terrain_height(x, z)) < 0.01f);

    // Plains also exist on land and are locatable.
    CHECK(gen.find_nearest_biome(BiomeType::Plains, hx, hz, 100000, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Plains);

    // Oceans do not exist in an all-land world. A radius the budget covers is a
    // complete answer: the walk covered it and did not give up.
    ChunkGenerator::BiomeSearchReport radius_report;
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Ocean, 0, 0, 512, x, z, h, &radius_report));
    CHECK_FALSE(radius_report.gave_up);

    // The old chat's 3,000-block request is the unbounded case: the walk stops
    // at its probe/time budget and says so instead of proving the absence of
    // ocean out to a horizon it cannot reach. Chat now asks for 864 instead.
    ChunkGenerator::BiomeSearchReport rejected_report;
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Ocean, 0, 0, 3000, x, z, h, &rejected_report));
    CHECK(rejected_report.gave_up);
    CHECK(rejected_report.probes > 1000);
    CHECK(rejected_report.reached_blocks > 500);

    // A cancel (what quitting or a world change does) stops it at the first probe.
    std::atomic<bool> cancel{true};
    ChunkGenerator::BiomeSearchReport cancelled_report;
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Ocean, 0, 0, 100000, x, z, h, &cancelled_report,
                                       &cancel));
    CHECK(cancelled_report.gave_up);
    CHECK(cancelled_report.probes == 0);
}

TEST_CASE("locate biome: all-ocean world is entirely ocean") {
    TerrainParams params;
    params.sea_level = 100000.0f;  // every column sits below sea level -> Ocean
    ChunkGenerator gen(params);

    int32_t x = 0, z = 0;
    float h = 0.0f;

    // (77, 88) is ocean itself, so the flood skips the water underfoot and the
    // walk answers another ocean column inside the radius.
    CHECK(gen.find_nearest_biome(BiomeType::Ocean, 77, 88, 1000, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Ocean);
    const bool underfoot = (x == 77 && z == 88);
    CHECK_FALSE(underfoot);
    const int32_t reach = std::max(std::abs(x - 77), std::abs(z - 88));
    CHECK(reach <= 1000);

    // A radius the patch underfoot already fills has no second ocean in it.
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Ocean, 77, 88, 8, x, z, h));

    // Hills do not exist in an all-ocean world.
    ChunkGenerator::BiomeSearchReport report;
    CHECK_FALSE(gen.find_nearest_biome(BiomeType::Hills, 0, 0, 512, x, z, h, &report));
    CHECK_FALSE(report.gave_up);
}

// =========================================================================
// find_nearest_biome — mixed land/ocean in the default world
// =========================================================================

TEST_CASE("locate biome: default world crosses land <-> ocean boundaries") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // An adjacent pair of grid columns of different biomes: a hills column and an
    // ocean column 64 blocks apart, so each is the other's answer well inside the
    // probe budget. A pair picked from anywhere in the world scan can be far
    // enough that a bounded search gives up before reaching it, which is what the
    // give-up case above pins; this one pins the crossing itself.
    int32_t land_x = 0, land_z = 0;
    int32_t ocean_x = 0, ocean_z = 0;
    bool paired = false;
    for (int32_t z = -4096; z <= 4096 && !paired; z += 64) {
        for (int32_t x = -4096; x <= 4096; x += 64) {
            const BiomeType a = gen.get_biome(x, z);
            const BiomeType b = gen.get_biome(x + 64, z);
            if (a == b) continue;
            if ((a != BiomeType::Hills && a != BiomeType::Ocean) ||
                (b != BiomeType::Hills && b != BiomeType::Ocean)) {
                continue;
            }
            land_x = (a == BiomeType::Hills) ? x : x + 64;
            ocean_x = (a == BiomeType::Ocean) ? x : x + 64;
            land_z = z;
            ocean_z = z;
            paired = true;
            break;
        }
    }
    if (!paired) {
        MESSAGE("Default world has no adjacent Hills/Ocean pair; skipping cross-search");
        return;
    }

    int32_t x = 0, z = 0;
    float h = 0.0f;
    const int32_t radius = 512;
    // The pair is 64 apart; the first ring with a hit can add a step, and the
    // refine scans 16 blocks around it.
    const int32_t bound = 64 + 16;

    // From the ocean side, the nearest hills must be found and be hills.
    CHECK(gen.find_nearest_biome(BiomeType::Hills, ocean_x, ocean_z, radius, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Hills);
    CHECK(std::max(std::abs(x - ocean_x), std::abs(z - ocean_z)) <= bound);

    // From the land side, the nearest ocean must be found and be ocean.
    CHECK(gen.find_nearest_biome(BiomeType::Ocean, land_x, land_z, radius, x, z, h));
    CHECK(gen.get_biome(x, z) == BiomeType::Ocean);
    CHECK(std::max(std::abs(x - land_x), std::abs(z - land_z)) <= bound);
    CHECK(std::abs(h - gen.get_terrain_height(x, z)) < 0.01f);
}