#include "doctest.h"
#include "lighting/light_propagator.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;

static std::unique_ptr<ChunkRenderData> make_light_test_chunk(std::unique_ptr<ChunkData> data) {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::move(data));
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

// Place 3x3x3 chunks around (0,0,0) so BFS has room.
static void fill_light_test_grid(ChunkMap& cm) {
    BlockRegistry::get_instance().initialize_default_blocks();
    for (int cy = -1; cy <= 1; ++cy)
        for (int cz = -1; cz <= 1; ++cz)
            for (int cx = -1; cx <= 1; ++cx) {
                auto d = std::make_unique<ChunkData>();
                cm.insert(cm.get_chunk_key(cx, cy, cz), make_light_test_chunk(std::move(d)));
            }
}

static uint8_t max_light_in_grid(ChunkMap& cm) {
    uint8_t m = 0;
    for (int cy = -1; cy <= 1; ++cy)
        for (int cz = -1; cz <= 1; ++cz)
            for (int cx = -1; cx <= 1; ++cx) {
                auto lock = cm.lock_keys_exclusive({cm.get_chunk_key(cx, cy, cz)});
                ChunkData* c = cm.get_chunk_data_fast(cx, cy, cz);
                if (!c) continue;
                for (int y = 0; y < CHUNK_HEIGHT; ++y)
                    for (int z = 0; z < CHUNK_DEPTH; ++z)
                        for (int x = 0; x < CHUNK_WIDTH; ++x) {
                            uint8_t r = c->get_light_r(x, y, z);
                            uint8_t g = c->get_light_g(x, y, z);
                            uint8_t b = c->get_light_b(x, y, z);
                            m = std::max(m, std::max(r, std::max(g, b)));
                        }
            }
    return m;
}

static int count_lit_cells(ChunkMap& cm) {
    int n = 0;
    for (int cy = -1; cy <= 1; ++cy)
        for (int cz = -1; cz <= 1; ++cz)
            for (int cx = -1; cx <= 1; ++cx) {
                auto lock = cm.lock_keys_exclusive({cm.get_chunk_key(cx, cy, cz)});
                ChunkData* c = cm.get_chunk_data_fast(cx, cy, cz);
                if (!c) continue;
                for (int y = 0; y < CHUNK_HEIGHT; ++y)
                    for (int z = 0; z < CHUNK_DEPTH; ++z)
                        for (int x = 0; x < CHUNK_WIDTH; ++x) {
                            uint8_t r = c->get_light_r(x, y, z);
                            uint8_t g = c->get_light_g(x, y, z);
                            uint8_t b = c->get_light_b(x, y, z);
                            if (r || g || b) ++n;
                        }
            }
    return n;
}

// Reads the block light at a local position inside center chunk.
static void read_cell(ChunkMap& cm, int x, int y, int z, uint8_t& r, uint8_t& g, uint8_t& b) {
    auto lock = cm.lock_keys_exclusive({cm.get_chunk_key(0, 0, 0)});
    ChunkData* c = cm.get_chunk_data_fast(0, 0, 0);
    r = c->get_light_r(x, y, z);
    g = c->get_light_g(x, y, z);
    b = c->get_light_b(x, y, z);
}

TEST_CASE("incremental remove of single light block clears all light") {
    ChunkMap cm;
    fill_light_test_grid(cm);
    LightPropagator lp;
    lp.set_chunk_map(&cm);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 16, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_BLOCK, 0, 0, 0);
    CHECK(max_light_in_grid(cm) > 0);
    CHECK(count_lit_cells(cm) > 1);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 16, 16, 16, BlockIDs::LIGHT_BLOCK, BlockIDs::AIR, 15, 15, 15);
    CHECK(max_light_in_grid(cm) == 0);
    CHECK(count_lit_cells(cm) == 0);
}

TEST_CASE("incremental remove of single red light clears all light") {
    ChunkMap cm;
    fill_light_test_grid(cm);
    LightPropagator lp;
    lp.set_chunk_map(&cm);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 16, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);
    uint8_t r, g, b;
    read_cell(cm, 16, 17, 16, r, g, b);
    CHECK(r > 0);
    CHECK(g == 0);
    CHECK(b == 0);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 16, 16, 16, BlockIDs::LIGHT_RED, BlockIDs::AIR, 15, 0, 0);
    CHECK(max_light_in_grid(cm) == 0);
    CHECK(count_lit_cells(cm) == 0);
}

TEST_CASE("incremental remove of one of two red lights keeps other") {
    ChunkMap cm;
    fill_light_test_grid(cm);
    LightPropagator lp;
    lp.set_chunk_map(&cm);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 12, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 20, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);
    CHECK(max_light_in_grid(cm) == 15);
    CHECK(count_lit_cells(cm) > 1);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 12, 16, 16, BlockIDs::LIGHT_RED, BlockIDs::AIR, 15, 0, 0);

    // The other red source should keep lighting.
    uint8_t r, g, b;
    read_cell(cm, 20, 16, 16, r, g, b);
    CHECK(r == 15);
    read_cell(cm, 20, 17, 16, r, g, b);
    CHECK(r == 14);
    // The removed light location (12, 16, 16) is still lit by the other source at distance 8
    read_cell(cm, 12, 16, 16, r, g, b);
    CHECK(r == 7); // Distance 8 from (20, 16, 16), so level 15 - 8 = 7
}

TEST_CASE("incremental remove of red light keeps independent blue light") {
    ChunkMap cm;
    fill_light_test_grid(cm);
    LightPropagator lp;
    lp.set_chunk_map(&cm);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 12, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 12, 20, 16, BlockIDs::AIR, BlockIDs::LIGHT_BLUE, 0, 0, 0);
    CHECK(max_light_in_grid(cm) == 15);

    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 12, 16, 16, BlockIDs::LIGHT_RED, BlockIDs::AIR, 15, 0, 0);

    // Blue light at the removed-red location and above must persist.
    uint8_t r, g, b;
    read_cell(cm, 12, 16, 16, r, g, b);
    CHECK(r == 0);
    read_cell(cm, 12, 18, 16, r, g, b);
    CHECK(b > 0);
    // Cell at (12, 15, 16) is distance 5 from blue source at (12, 20, 16), so level 10
    read_cell(cm, 12, 15, 16, r, g, b);
    CHECK(b == 10);
}

TEST_CASE("incremental remove does not infinite loop on overlapping sources") {
    // This test specifically checks for the bug where multi-source removal
    // would re-add cells to the remove queue, causing an infinite loop.
    ChunkMap cm;
    fill_light_test_grid(cm);
    LightPropagator lp;
    lp.set_chunk_map(&cm);

    // Place two light sources that overlap at cell (16, 16, 16)
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 15, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 17, 16, 16, BlockIDs::AIR, BlockIDs::LIGHT_RED, 0, 0, 0);

    // Check that overlap cell is lit
    uint8_t r, g, b;
    read_cell(cm, 16, 16, 16, r, g, b);
    CHECK(r > 0);

    // Remove one source - this should complete without infinite loop
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 15, 16, 16, BlockIDs::LIGHT_RED, BlockIDs::AIR, 15, 0, 0);

    // Overlap cell should still be lit by the remaining source
    read_cell(cm, 16, 16, 16, r, g, b);
    CHECK(r > 0);

    // Remove the second source - all light should clear
    lp.update_block_light_incremental(0, 0, 0, 0, 0, 0, 17, 16, 16, BlockIDs::LIGHT_RED, BlockIDs::AIR, 15, 0, 0);
    CHECK(max_light_in_grid(cm) == 0);
}
