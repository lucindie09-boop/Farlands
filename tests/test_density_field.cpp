#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include <vector>
#include <cmath>

using namespace VoxelEngine;

static bool is_water_block(BlockID b) {
    return b == BlockIDs::WATER || b == BlockIDs::SURFACE_WATER;
}

// =========================================================================
// Signed density field + find_surface_y
// =========================================================================

TEST_CASE("density field: signed surface reproduces the macro terrain") {
    TerrainParams params;
    ChunkGenerator gen(params);

    for (int32_t z = -64; z <= 64; z += 8) {
        for (int32_t x = -64; x <= 64; x += 8) {
            auto col = gen.sample_column_debug(x, z);
            int32_t surface = gen.find_surface_y(x, z);

            // Surface must stay within the deformation band around the macro
            // height: find_surface_y scans +/-DENSITY_MARGIN (30), which is
            // beyond SURFACE_BAND_OUTER (28) — the hard envelope of the 3D
            // displacement no matter how large SHAPE_STRENGTH_MAX gets.
            CHECK(surface >= static_cast<int32_t>(std::ceil(col.height - 30.0f)));
            CHECK(surface <= static_cast<int32_t>(std::ceil(col.height + 30.0f)));

            // The found surface is a real air->solid boundary of the density field.
            CHECK(gen.sample_terrain_density(x, surface, z, col) > 0.0f);
            CHECK(gen.sample_terrain_density(x, surface + 1, z, col) <= 0.0f);

            // Far below / above the surface the field is safely outside the 3D
            // shaping band and behaves like the old heightmap.
            CHECK(gen.sample_terrain_density(x, surface - 45, z, col) > 0.0f);
            CHECK(gen.sample_terrain_density(x, surface + 45, z, col) < 0.0f);
        }
    }
}

// =========================================================================
// generate_chunk: geometry pass must agree with the density function
// =========================================================================

TEST_CASE("generate_chunk: solid geometry matches the density field") {
    TerrainParams params;
    ChunkGenerator gen(params);
    ChunkData chunk;

    int failures = 0;

    for (int32_t cz = -2; cz <= 2; cz++) {
        for (int32_t cx = -2; cx <= 2; cx++) {
            // cy 4..9 covers world y 128..320, spanning sea level and mountains.
            for (int32_t cy = 4; cy <= 9; cy++) {
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, false);

                const int32_t wy_start = cy * CHUNK_HEIGHT;
                const int32_t wy_end = wy_start + CHUNK_HEIGHT;

                for (int32_t lz = 0; lz < CHUNK_DEPTH; lz += 4) {
                    for (int32_t lx = 0; lx < CHUNK_WIDTH; lx += 4) {
                        const int32_t wx = cx * CHUNK_WIDTH + lx;
                        const int32_t wz = cz * CHUNK_DEPTH + lz;
                        auto col = gen.sample_column_debug(wx, wz);

                        // Highest density-solid voxel inside this chunk (scan top-down).
                        int32_t dtop = -1;
                        for (int32_t ly = CHUNK_HEIGHT - 1; ly >= 0; ly--) {
                            if (gen.sample_terrain_density(wx, wy_start + ly, wz, col) > 0.0f) {
                                dtop = wy_start + ly;
                                break;
                            }
                        }

                        // Highest generated solid voxel inside this chunk (scan top-down).
                        int32_t topmost = -1;
                        for (int32_t ly = CHUNK_HEIGHT - 1; ly >= 0; ly--) {
                            BlockID b = chunk.get_block(lx, ly, lz);
                            if (b != BlockIDs::AIR && !is_water_block(b)) {
                                topmost = wy_start + ly;
                                break;
                            }
                        }

                        if (dtop < 0) {
                            // No density solid in the chunk: everything must be air/water.
                            for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                                BlockID b = chunk.get_block(lx, ly, lz);
                                if (b != BlockIDs::AIR && !is_water_block(b)) {
                                    failures++;
                                }
                            }
                            continue;
                        }

                        // Generation must never add solid above the density boundary.
                        CHECK(topmost <= dtop);
                        if (topmost > dtop) failures++;

                        if (topmost < 0) continue;

                        // The topmost solid voxel is density-solid.
                        CHECK(gen.sample_terrain_density(wx, topmost, wz, col) > 0.0f);
                        if (gen.sample_terrain_density(wx, topmost, wz, col) <= 0.0f) failures++;

                        // Every voxel between topmost and the density boundary that the
                        // density field marks solid must have been carved by a cave.
                        for (int32_t y = topmost + 1; y <= dtop; y++) {
                            if (gen.sample_terrain_density(wx, y, wz, col) > 0.0f) {
                                BlockID b = chunk.get_block(lx, y - wy_start, lz);
                                if (b == BlockIDs::AIR || is_water_block(b)) {
                                    // Carved: the cave check must agree.
                                    if (!gen.is_cave(wx, y, wz)) {
                                        failures++;
                                    }
                                } else {
                                    failures++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    CHECK(failures == 0);
}

// =========================================================================
// generate_chunk: no isolated single-block artifacts
// =========================================================================

TEST_CASE("generate_chunk: isolated single blocks are removed") {
    TerrainParams params;
    ChunkGenerator gen(params);
    ChunkData chunk;

    int artifacts = 0;
    for (int32_t cz = -3; cz <= 3; cz++) {
        for (int32_t cx = -3; cx <= 3; cx++) {
            for (int32_t cy = 4; cy <= 9; cy++) {
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, false);

                // Interior only: boundary-layer neighbors may live in adjacent
                // (possibly ungenerated) chunks, so we don't check those.
                for (int32_t lz = 1; lz < CHUNK_DEPTH - 1; lz++) {
                    for (int32_t lx = 1; lx < CHUNK_WIDTH - 1; lx++) {
                        for (int32_t ly = 1; ly < CHUNK_HEIGHT - 1; ly++) {
                            if (chunk.get_block(lx, ly, lz) == BlockIDs::AIR) continue;
                            bool has_neighbor =
                                chunk.get_block(lx + 1, ly, lz)     != BlockIDs::AIR ||
                                chunk.get_block(lx - 1, ly, lz)     != BlockIDs::AIR ||
                                chunk.get_block(lx, ly + 1, lz)     != BlockIDs::AIR ||
                                chunk.get_block(lx, ly - 1, lz)     != BlockIDs::AIR ||
                                chunk.get_block(lx, ly, lz + 1)     != BlockIDs::AIR ||
                                chunk.get_block(lx, ly, lz - 1)     != BlockIDs::AIR;
                            if (!has_neighbor) artifacts++;
                        }
                    }
                }
            }
        }
    }

    CHECK(artifacts == 0);
}

// =========================================================================
// generate_chunk: materials (topsoil / water / bedrock) are sane
// =========================================================================

TEST_CASE("generate_chunk: material pass produces valid blocks") {
    TerrainParams params;
    ChunkGenerator gen(params);
    ChunkData chunk;

    // Surface chunks around the origin.
    for (int32_t cz = -1; cz <= 1; cz++) {
        for (int32_t cx = -1; cx <= 1; cx++) {
            for (int32_t cy = 5; cy <= 7; cy++) {
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, false);

                for (int32_t lz = 0; lz < CHUNK_DEPTH; lz += 8) {
                    for (int32_t lx = 0; lx < CHUNK_WIDTH; lx += 8) {
                        for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                            BlockID b = chunk.get_block(lx, ly, lz);
                            CHECK(b <= BlockIDs::CACTUS); // no out-of-range ids
                        }
                    }
                }
            }
        }
    }

    // Water only ever appears where the column actually floods above its macro
    // ground, and never inside bedrock.
    gen.generate_chunk(chunk, 0, 5, 0, nullptr, false);
    for (int32_t lz = 0; lz < CHUNK_DEPTH; lz++) {
        for (int32_t lx = 0; lx < CHUNK_WIDTH; lx++) {
            auto col = gen.sample_column_debug(lx, lz);
            if (col.water_level < 0.0f) {
                for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                    BlockID b = chunk.get_block(lx, ly, lz);
                    CHECK(!is_water_block(b));
                }
            }
        }
    }
}
