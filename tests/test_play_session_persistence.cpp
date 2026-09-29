#include "doctest.h"

#include "play_session_test_support.hpp"

// =========================================================================
// Note: the inventory INVE byte format (serialize_inventory /
// deserialize_inventory) and the edit-map apply step (apply_edit_map_to_chunk)
// live in src/core/ and are the SAME functions ChunkWorld uses. These tests
// therefore exercise the production format and apply logic, not a mirrored
// copy.
// =========================================================================

#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/edit_map.hpp"
#include "core/crc32.hpp"
#include "core/inventory.hpp"
#include "core/crafting.hpp"
#include "worldgen/chunk_generator.hpp"

#include <functional>
#include <unordered_map>
#include <vector>

using namespace VoxelEngine;
using namespace play_session_test;

// =========================================================================
// Test 3: Cumulative edits — edit → save → reload → more edits → save →
//         reload → verify (tests that the reload-apply-save cycle is stable)
// =========================================================================

TEST_CASE("Cumulative edits: edit, save, reload, more edits, save, reload, verify") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID sand  = id("sand");

    TerrainParams params;
    params.seed = 777;
    ChunkGenerator gen(params);
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;

    // -- Round 1: generate, edit, save, reload --
    ChunkData chunk1;
    chunk1.clear();
    gen.generate_chunk(chunk1, 0, 0, 0, no_cross, false);

    EditMap edits1;
    edits1.set_block(10, 20, 10, sand);
    edits1.set_block(15, 25, 15, BlockIDs::AIR);

    std::vector<uint8_t> buf1;
    serialize_edit_map(edits1, buf1);

    // Reload round 1
    EditMap loaded1;
    CHECK(deserialize_edit_map(buf1.data(), buf1.size(), loaded1, BlockRegistry::get_instance()));

    ChunkData chunk2;
    chunk2.clear();
    gen.generate_chunk(chunk2, 0, 0, 0, no_cross, false);
    apply_edit_map_to_chunk(loaded1, chunk2);

    CHECK(chunk2.get_block(10, 20, 10) == sand);
    CHECK(chunk2.get_block(15, 25, 15) == BlockIDs::AIR);

    // -- Round 2: accumulate more edits on the reloaded chunk --
    EditMap edits2;
    // Re-record the round-1 edits (as if the edit map is cumulative)
    edits2.set_block(10, 20, 10, sand);
    edits2.set_block(15, 25, 15, BlockIDs::AIR);
    // New edits
    edits2.set_block(20, 30, 20, stone);
    edits2.set_block(5, 10, 5, BlockIDs::AIR);

    std::vector<uint8_t> buf2;
    serialize_edit_map(edits2, buf2);

    // Reload round 2 into a fresh chunk
    EditMap loaded2;
    CHECK(deserialize_edit_map(buf2.data(), buf2.size(), loaded2, BlockRegistry::get_instance()));

    ChunkData chunk3;
    chunk3.clear();
    gen.generate_chunk(chunk3, 0, 0, 0, no_cross, false);
    apply_edit_map_to_chunk(loaded2, chunk3);

    SUBCASE("all round-1 edits survive into round 2") {
        CHECK(chunk3.get_block(10, 20, 10) == sand);
        CHECK(chunk3.get_block(15, 25, 15) == BlockIDs::AIR);
    }

    SUBCASE("round-2 additions are correct") {
        CHECK(chunk3.get_block(20, 30, 20) == stone);
        CHECK(chunk3.get_block(5, 10, 5) == BlockIDs::AIR);
    }

    SUBCASE("un-edited regions remain generated terrain") {
        CHECK(chunk3.get_block(16, 16, 16) == chunk2.get_block(16, 16, 16));
    }
}

// =========================================================================
// Test 4: Inventory round-trip through full play session
//         Tests that exact slot positions, counts, and selected slot survive
// =========================================================================

TEST_CASE("Inventory round-trip: slot layout preserved through save/load") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID dirt  = id("dirt");
    const BlockID sand  = id("sand");

    Inventory inv;
    inv.set_hotbar_slot(0, stone, 32);
    inv.set_hotbar_slot(3, dirt, 17);
    inv.set_hotbar_slot(8, sand, 64);
    inv.set_inventory_slot(5, stone, 8);
    inv.set_inventory_slot(26, dirt, 1);
    inv.select_slot(3);

    std::vector<uint8_t> data;
    serialize_inventory(inv, data);

    Inventory loaded;
    CHECK(deserialize_inventory(data.data(), data.size(), loaded));

    SUBCASE("hotbar slot positions and counts match") {
        CHECK(loaded.get_hotbar_slot(0).block_id == stone);
        CHECK(loaded.get_hotbar_slot(0).count == 32);
        CHECK(loaded.get_hotbar_slot(3).block_id == dirt);
        CHECK(loaded.get_hotbar_slot(3).count == 17);
        CHECK(loaded.get_hotbar_slot(8).block_id == sand);
        CHECK(loaded.get_hotbar_slot(8).count == 64);
    }

    SUBCASE("main inventory slot positions and counts match") {
        CHECK(loaded.get_inventory_slot(5).block_id == stone);
        CHECK(loaded.get_inventory_slot(5).count == 8);
        CHECK(loaded.get_inventory_slot(26).block_id == dirt);
        CHECK(loaded.get_inventory_slot(26).count == 1);
    }

    SUBCASE("empty slots remain empty") {
        CHECK(loaded.get_hotbar_slot(1).is_empty());
        CHECK(loaded.get_hotbar_slot(7).is_empty());
        CHECK(loaded.get_inventory_slot(0).is_empty());
        CHECK(loaded.get_inventory_slot(25).is_empty());
    }

    SUBCASE("selected slot preserved") {
        CHECK(loaded.get_selected_slot() == 3);
    }
}

// =========================================================================
// Test 5: Full crafting session — gather materials → craft tool → use it
//         → save → reload → verify
// =========================================================================

TEST_CASE("Crafting session: gather, craft, place, save, reload, verify") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID planks = ensure_oak_planks();

    TerrainParams params;
    params.seed = 314;
    ChunkGenerator gen(params);
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;

    // Generate terrain
    ChunkData chunk;
    chunk.clear();
    gen.generate_chunk(chunk, 0, 0, 0, no_cross, false);

    // Set up crafting recipe: 2 stone → 1 stone_block (a "tool")
    BlockID stone_block = id("dirt"); // reuse dirt as a stand-in for a crafted block
    RecipeBook book;
    book.add_recipe(shapeless_recipe({stone, stone}, stone_block, 1));

    Inventory inv;
    EditMap edit_map;

    // Gather 8 stone by breaking blocks
    int gathered = 0;
    for (int32_t y = 31; y >= 0 && gathered < 8; --y) {
        for (int32_t x = 0; x < 32 && gathered < 8; ++x) {
            for (int32_t z = 0; z < 32 && gathered < 8; ++z) {
                if (chunk.get_block(x, y, z) == stone) {
                    chunk.set_block(x, y, z, BlockIDs::AIR);
                    edit_map.set_block(x, y, z, BlockIDs::AIR);
                    inv.add_block(stone, 1);
                    ++gathered;
                }
            }
        }
    }
    CHECK(gathered == 8);
    CHECK(inv.get_total_count(stone) == 8);

    // Craft 4 stone_blocks (consuming 8 stone)
    for (int i = 0; i < 4; ++i) {
        CHECK(craft_item(book.recipes()[0], inv));
    }
    CHECK(inv.get_total_count(stone) == 0);
    CHECK(inv.get_total_count(stone_block) == 4);

    // Place 2 crafted stone_blocks
    inv.consume_block(stone_block, 1);
    chunk.set_block(16, 31, 16, stone_block);
    edit_map.set_block(16, 31, 16, stone_block);

    inv.consume_block(stone_block, 1);
    chunk.set_block(17, 31, 16, stone_block);
    edit_map.set_block(17, 31, 16, stone_block);

    CHECK(inv.get_total_count(stone_block) == 2);

    // Save everything
    std::vector<uint8_t> edit_data;
    serialize_edit_map(edit_map, edit_data);

    std::vector<uint8_t> inv_data;
    serialize_inventory(inv, inv_data);

    // Reload into fresh objects
    EditMap loaded_edits;
    CHECK(deserialize_edit_map(edit_data.data(), edit_data.size(),
                                 loaded_edits, BlockRegistry::get_instance()));

    Inventory loaded_inv;
    CHECK(deserialize_inventory(inv_data.data(), inv_data.size(), loaded_inv));

    ChunkData reloaded;
    reloaded.clear();
    gen.generate_chunk(reloaded, 0, 0, 0, no_cross, false);
    apply_edit_map_to_chunk(loaded_edits, reloaded);

    SUBCASE("crafted blocks placed in world survived") {
        CHECK(reloaded.get_block(16, 31, 16) == stone_block);
        CHECK(reloaded.get_block(17, 31, 16) == stone_block);
    }

    SUBCASE("broken blocks are AIR after reload") {
        // Verify a few of the broken positions are AIR
        int air_count = 0;
        for (int32_t y = 0; y < 32; ++y)
            for (int32_t x = 0; x < 32; ++x)
                for (int32_t z = 0; z < 32; ++z)
                    if (loaded_edits.has_edit(x, y, z) &&
                        loaded_edits.get_block(x, y, z, BlockIDs::AIR) == BlockIDs::AIR)
                        ++air_count;
        CHECK(air_count == 8);
    }

    SUBCASE("inventory crafting result survived") {
        CHECK(loaded_inv.get_total_count(stone_block) == 2);
        CHECK(loaded_inv.get_total_count(stone) == 0);
    }
}

// =========================================================================
// Test 6: Edit map coalescing across break/place cycles on same coordinate
//         Tests that the production last-write-wins behavior is preserved
//         through serialization
// =========================================================================

TEST_CASE("Break and replace same block: last-write-wins through save/load") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID sand  = id("sand");

    TerrainParams params;
    params.seed = 555;
    ChunkGenerator gen(params);
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;

    ChunkData chunk;
    chunk.clear();
    gen.generate_chunk(chunk, 0, 0, 0, no_cross, false);

    // Ensure (16, 30, 16) is solid
    chunk.set_block(16, 30, 16, stone);

    EditMap edit_map;

    // Break it
    chunk.set_block(16, 30, 16, BlockIDs::AIR);
    edit_map.set_block(16, 30, 16, BlockIDs::AIR);
    CHECK(chunk.get_block(16, 30, 16) == BlockIDs::AIR);

    // Place sand in same spot
    chunk.set_block(16, 30, 16, sand);
    edit_map.set_block(16, 30, 16, sand);
    CHECK(chunk.get_block(16, 30, 16) == sand);

    // Break again
    chunk.set_block(16, 30, 16, BlockIDs::AIR);
    edit_map.set_block(16, 30, 16, BlockIDs::AIR);

    // Edit map should have exactly 1 entry (last-write-wins)
    CHECK(edit_map.size() == 1);

    // Save and reload
    std::vector<uint8_t> data;
    serialize_edit_map(edit_map, data);

    EditMap loaded;
    CHECK(deserialize_edit_map(data.data(), data.size(), loaded, BlockRegistry::get_instance()));
    CHECK(loaded.size() == 1);

    // Apply to fresh chunk
    ChunkData reloaded;
    reloaded.clear();
    gen.generate_chunk(reloaded, 0, 0, 0, no_cross, false);
    apply_edit_map_to_chunk(loaded, reloaded);

    CHECK(reloaded.get_block(16, 30, 16) == BlockIDs::AIR);
}

// =========================================================================
// Test 7: Verify that edits don't corrupt neighboring blocks across
//         save/load — the kind of off-by-one that bites at chunk edges
// =========================================================================

TEST_CASE("Edge-of-chunk edits don't corrupt neighbors through save/load") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID sand  = id("sand");

    TerrainParams params;
    params.seed = 111;
    ChunkGenerator gen(params);
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;

    ChunkData chunk;
    chunk.clear();
    gen.generate_chunk(chunk, 0, 0, 0, no_cross, false);

    // Snapshot neighbors of (31, 30, 31) before editing
    BlockID before_30_30_31 = chunk.get_block(30, 30, 31);
    BlockID before_31_29_31 = chunk.get_block(31, 29, 31);
    BlockID before_31_30_30 = chunk.get_block(31, 30, 30);

    EditMap edit_map;
    chunk.set_block(31, 30, 31, sand);
    edit_map.set_block(31, 30, 31, sand);

    // Save and reload
    std::vector<uint8_t> data;
    serialize_edit_map(edit_map, data);

    EditMap loaded;
    CHECK(deserialize_edit_map(data.data(), data.size(), loaded, BlockRegistry::get_instance()));

    ChunkData reloaded;
    reloaded.clear();
    gen.generate_chunk(reloaded, 0, 0, 0, no_cross, false);
    apply_edit_map_to_chunk(loaded, reloaded);

    SUBCASE("edited block changed") {
        CHECK(reloaded.get_block(31, 30, 31) == sand);
    }

    SUBCASE("neighbors are untouched") {
        CHECK(reloaded.get_block(30, 30, 31) == before_30_30_31);
        CHECK(reloaded.get_block(31, 29, 31) == before_31_29_31);
        CHECK(reloaded.get_block(31, 30, 30) == before_31_30_30);
    }

    SUBCASE("corner blocks are untouched") {
        CHECK(reloaded.get_block(30, 29, 30) == chunk.get_block(30, 29, 30));
        CHECK(reloaded.get_block(31, 31, 31) == chunk.get_block(31, 31, 31));
    }
}
