#include "doctest.h"
#include "lighting/light_propagation.hpp"
#include "lighting/block_light_region.hpp"
#include "core/chunk_data.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;

TEST_CASE("emissive block lights neighbor") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(16, 16, 16) > 0);
    CHECK(chunk.get_light_unsafe(16, 17, 16) > 0);
}

TEST_CASE("no emissive -> no light") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::STONE);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(16, 16, 16) == 0);
}

TEST_CASE("LIGHT_BLOCK channel values") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_r_unsafe(16, 16, 16) == 15);
    CHECK(chunk.get_light_g_unsafe(16, 16, 16) == 15);
    CHECK(chunk.get_light_b_unsafe(16, 16, 16) == 15);
}

TEST_CASE("colored light channels") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_RED);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_r_unsafe(16, 16, 16) > 0);
    CHECK(chunk.get_light_g_unsafe(16, 16, 16) == 0);
    CHECK(chunk.get_light_b_unsafe(16, 16, 16) == 0);
}

TEST_CASE("wrap_local_to_region inside bounds") {
    int16_t nx = 5, ny = 10, nz = 15;
    int8_t rdx = 0, rdy = 0, rdz = 0;
    bool ok = wrap_local_to_region(nx, ny, nz, rdx, rdy, rdz);
    CHECK(ok);
    CHECK(nx == 5);
    CHECK(ny == 10);
    CHECK(nz == 15);
    CHECK(rdx == 0);
    CHECK(rdy == 0);
    CHECK(rdz == 0);
}

TEST_CASE("wrap_local_to_region wraps negative") {
    int16_t nx = -1, ny = 0, nz = 0;
    int8_t rdx = 0, rdy = 0, rdz = 0;
    bool ok = wrap_local_to_region(nx, ny, nz, rdx, rdy, rdz);
    CHECK(ok);
    CHECK(nx == 31);
    CHECK(rdx == -1);
}

// =========================================================================
// Cross-chunk BFS edge case tests
// =========================================================================

TEST_CASE("wrap_local_to_world crosses positive x boundary") {
    int32_t cx = 0, cy = 0, cz = 0;
    int16_t x = 32, y = 16, z = 16; // Out of bounds
    wrap_local_to_world(x, y, z, cx, cy, cz);
    CHECK(x == 0);
    CHECK(y == 16);
    CHECK(z == 16);
    CHECK(cx == 1);
    CHECK(cy == 0);
    CHECK(cz == 0);
}

TEST_CASE("wrap_local_to_world crosses negative x boundary") {
    int32_t cx = 0, cy = 0, cz = 0;
    int16_t x = -1, y = 16, z = 16; // Out of bounds
    wrap_local_to_world(x, y, z, cx, cy, cz);
    CHECK(x == 31);
    CHECK(y == 16);
    CHECK(z == 16);
    CHECK(cx == -1);
    CHECK(cy == 0);
    CHECK(cz == 0);
}

TEST_CASE("wrap_local_to_world crosses positive y boundary") {
    int32_t cx = 0, cy = 0, cz = 0;
    int16_t x = 16, y = 32, z = 16; // Out of bounds (CHUNK_HEIGHT is 32)
    wrap_local_to_world(x, y, z, cx, cy, cz);
    CHECK(x == 16);
    CHECK(y == 0);
    CHECK(z == 16);
    CHECK(cx == 0);
    CHECK(cy == 1);
    CHECK(cz == 0);
}

TEST_CASE("wrap_local_to_world crosses positive z boundary") {
    int32_t cx = 0, cy = 0, cz = 0;
    int16_t x = 16, y = 16, z = 32; // Out of bounds
    wrap_local_to_world(x, y, z, cx, cy, cz);
    CHECK(x == 16);
    CHECK(y == 16);
    CHECK(z == 0);
    CHECK(cx == 0);
    CHECK(cy == 0);
    CHECK(cz == 1);
}

TEST_CASE("wrap_local_to_world stays in bounds") {
    int32_t cx = 0, cy = 0, cz = 0;
    int16_t x = 16, y = 16, z = 16;
    wrap_local_to_world(x, y, z, cx, cy, cz);
    CHECK(x == 16);
    CHECK(y == 16);
    CHECK(z == 16);
    CHECK(cx == 0);
    CHECK(cy == 0);
    CHECK(cz == 0);
}

// =========================================================================
// Single-chunk BFS edge cases
// =========================================================================

TEST_CASE("BFS attenuation reaches exactly 14 cells") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(16, 16, 16) == 15);
    CHECK(chunk.get_light_unsafe(16, 30, 16) > 0);
    CHECK(chunk.get_light_unsafe(16, 0, 16) == 0);
}

TEST_CASE("opaque block occludes light") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    for (int y = 0; y < CHUNK_HEIGHT; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            chunk.set_block(17, y, z, BlockIDs::STONE);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(16, 16, 16) > 0);
    CHECK(chunk.get_light_unsafe(18, 16, 16) == 0);
}

TEST_CASE("cross-section boundary propagation") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(8, 15, 8, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(8, 15, 8) == 15);
    CHECK(chunk.get_light_unsafe(8, 16, 8) > 0);
}

TEST_CASE("colored light mixing at overlap") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.set_block(14, 16, 16, BlockIDs::LIGHT_RED);
    chunk.set_block(18, 16, 16, BlockIDs::LIGHT_BLUE);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_r_unsafe(16, 16, 16) > 0);
    CHECK(chunk.get_light_b_unsafe(16, 16, 16) > 0);
}

// =========================================================================
// Cross-chunk-boundary tests via 3x3x3 BlockLightRegion grid
// =========================================================================

static void init_grid_3x3x3(ChunkData (&grid)[3][3][3]) {
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx].clear();
}

TEST_CASE("cross-chunk-boundary propagation via 3x3x3 grid") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData region[3][3][3];
    init_grid_3x3x3(region);
    region[1][1][1].set_block(31, 16, 16, BlockIDs::LIGHT_BLOCK);
    ChunkData* grid[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx] = &region[dz][dy][dx];
    BlockLightRegion light_region(grid);
    std::vector<EmissiveSource> sources;
    light_region.collect_emissive_sources(sources);
    light_region.propagate_additive(sources);
    CHECK(region[1][1][1].get_light_r_unsafe(31, 16, 16) == 15);
    CHECK(region[2][1][1].get_light_r_unsafe(0, 16, 16) > 0);
}

TEST_CASE("light blocked by opaque neighbor chunk") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData region[3][3][3];
    init_grid_3x3x3(region);
    region[1][1][1].set_block(31, 16, 16, BlockIDs::LIGHT_BLOCK);
    for (int y = 0; y < CHUNK_HEIGHT; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            region[2][1][1].set_block(0, y, z, BlockIDs::STONE);
    ChunkData* grid[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx] = &region[dz][dy][dx];
    BlockLightRegion light_region(grid);
    std::vector<EmissiveSource> sources;
    light_region.collect_emissive_sources(sources);
    light_region.propagate_additive(sources);
    CHECK(region[1][1][1].get_light_r_unsafe(31, 16, 16) > 0);
    CHECK(region[2][1][1].get_light_r_unsafe(0, 16, 16) == 0);
}

TEST_CASE("null neighbor chunk stops propagation") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData region[3][3][3];
    init_grid_3x3x3(region);
    region[1][1][1].set_block(31, 16, 16, BlockIDs::LIGHT_BLOCK);
    ChunkData* grid[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx] = &region[dz][dy][dx];
    grid[2][1][1] = nullptr;
    BlockLightRegion light_region(grid);
    std::vector<EmissiveSource> sources;
    light_region.collect_emissive_sources(sources);
    light_region.propagate_additive(sources);
    CHECK(region[1][1][1].get_light_r_unsafe(31, 16, 16) > 0);
}

TEST_CASE("multi-source light propagation across chunks") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData region[3][3][3];
    init_grid_3x3x3(region);
    region[1][1][1].set_block(31, 16, 16, BlockIDs::LIGHT_RED);
    region[1][1][1].set_block(0, 16, 16, BlockIDs::LIGHT_BLUE);
    ChunkData* grid[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx] = &region[dz][dy][dx];
    BlockLightRegion light_region(grid);
    std::vector<EmissiveSource> sources;
    light_region.collect_emissive_sources(sources);
    light_region.propagate_additive(sources);
    CHECK(region[1][1][1].get_light_r_unsafe(31, 16, 16) > 0);
    CHECK(region[1][1][1].get_light_b_unsafe(0, 16, 16) > 0);
}


TEST_CASE("sky light dies with depth in water") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    // A 10-deep pool of source water: sky light enters the surface cell at 15
    // and pays water's opacity (3) per cell — 15, 12, 9, 6, 3, then dark.
    for (int y = 20; y < 30; ++y) {
        chunk.set_block(16, y, 16, BlockIDs::WATER);
    }
    chunk.set_block(16, 19, 16, BlockIDs::STONE);  // pool floor
    chunk.propagate_sky_light(nullptr);
    CHECK(chunk.get_sky_light(16, 29, 16) == 12);  // first water cell below open sky
    CHECK(chunk.get_sky_light(16, 28, 16) == 9);
    CHECK(chunk.get_sky_light(16, 27, 16) == 6);
    CHECK(chunk.get_sky_light(16, 26, 16) == 3);
    CHECK(chunk.get_sky_light(16, 25, 16) == 0);
    CHECK(chunk.get_sky_light(16, 20, 16) == 0);   // deep water is dark
}

TEST_CASE("block light pays water opacity crossing it") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    // A torch in a sealed stone room with a 2-deep pool between it and the
    // probe cell. The stone cap matters: without it light hops over the pool
    // through the air above and the direct-crossing value never wins.
    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    chunk.set_block(17, 16, 16, BlockIDs::WATER);
    chunk.set_block(18, 16, 16, BlockIDs::WATER);
    chunk.set_block(17, 17, 16, BlockIDs::WATER);
    chunk.set_block(18, 17, 16, BlockIDs::WATER);
    for (int x = 15; x <= 19; ++x) chunk.set_block(x, 18, 16, BlockIDs::STONE);
    chunk.set_block(19, 16, 16, BlockIDs::STONE);
    chunk.set_block(19, 17, 16, BlockIDs::STONE);
    for (int y = 15; y <= 17; ++y) {
        chunk.set_block(17, y, 15, BlockIDs::STONE);
        chunk.set_block(18, y, 15, BlockIDs::STONE);
        chunk.set_block(17, y, 17, BlockIDs::STONE);
        chunk.set_block(18, y, 17, BlockIDs::STONE);
        chunk.set_block(17, y, 16, y == 16 || y == 17 ? BlockIDs::WATER : BlockIDs::STONE);
        chunk.set_block(18, y, 16, y == 16 || y == 17 ? BlockIDs::WATER : BlockIDs::STONE);
    }
    propagate_chunk_block_light_additive(chunk);
    // source 15 -> water: 15-1-3 = 11 -> next water: 11-1-3 = 7
    CHECK(chunk.get_light_r_unsafe(17, 16, 16) == 11);
    CHECK(chunk.get_light_r_unsafe(18, 16, 16) == 7);
}
