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
// Test 1: Full play session — generate → break → collect → place → craft
//         → save → reload → verify
// =========================================================================

TEST_CASE("Full play session: generate, break, collect, place, craft, save, reload, verify") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID dirt  = id("dirt");
    const BlockID sand  = id("sand");
    const BlockID planks = ensure_oak_planks();

    // -- Phase 1: Generate terrain, then set up known blocks for the test --
    TerrainParams params;
    params.seed = 42;
    ChunkGenerator gen(params);

    ChunkData chunk;
    chunk.clear();
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;
    gen.generate_chunk(chunk, 0, 0, 0, no_cross, false);

    // Place known stone blocks at specific positions for reliable testing
    int32_t break_lx = 30, break_ly = 5, break_lz = 30;
    int32_t break2_x = 29, break2_y = 5, break2_z = 29;
    chunk.set_block(break_lx, break_ly, break_lz, stone);
    chunk.set_block(break2_x, break2_y, break2_z, stone);
    BlockID broken_block = stone;

    // -- Phase 2: Break block → add to inventory → record in edit map --
    Inventory inventory;
    EditMap edit_map;

    chunk.set_block(break_lx, break_ly, break_lz, BlockIDs::AIR);
    edit_map.set_block(break_lx, break_ly, break_lz, BlockIDs::AIR);
    inventory.add_block(broken_block, 1);

    CHECK(chunk.get_block(break_lx, break_ly, break_lz) == BlockIDs::AIR);
    CHECK(inventory.get_total_count(broken_block) == 1);
    CHECK(edit_map.size() == 1);

    // -- Phase 3: Place a different block (consume from inventory, set on chunk) --
    // Place sand at a known empty spot in a completely different column
    int32_t place_lx = 1, place_ly = 31, place_lz = 1;
    chunk.set_block(place_lx, place_ly, place_lz, BlockIDs::AIR);
    inventory.add_block(sand, 10);
    CHECK(inventory.get_total_count(sand) == 10);

    inventory.consume_block(sand, 1);
    chunk.set_block(place_lx, place_ly, place_lz, sand);
    edit_map.set_block(place_lx, place_ly, place_lz, sand);

    CHECK(chunk.get_block(place_lx, place_ly, place_lz) == sand);
    CHECK(inventory.get_total_count(sand) == 9);

    // -- Phase 4: Craft — stone → oak_planks (shapeless, 1:4) --
    RecipeBook book;
    book.add_recipe(shapeless_recipe({stone}, planks, 4));
    const CraftingRecipe& recipe = book.recipes()[0];

    // Break the second stone block we placed
    chunk.set_block(break2_x, break2_y, break2_z, BlockIDs::AIR);
    edit_map.set_block(break2_x, break2_y, break2_z, BlockIDs::AIR);
    inventory.add_block(stone, 1);

    CHECK(craft_item(recipe, inventory));
    CHECK(inventory.get_total_count(stone) == 1);
    CHECK(inventory.get_total_count(planks) == 4);

    // -- Phase 5: Place crafted block --
    inventory.consume_block(planks, 1);
    int32_t craft_place_x = 5, craft_place_y = 31, craft_place_z = 5;
    chunk.set_block(craft_place_x, craft_place_y, craft_place_z, planks);
    edit_map.set_block(craft_place_x, craft_place_y, craft_place_z, planks);
    CHECK(chunk.get_block(craft_place_x, craft_place_y, craft_place_z) == planks);
    CHECK(inventory.get_total_count(planks) == 3);
    CHECK(inventory.get_total_count(stone) == 1);

    // -- Phase 6: Save — serialize edit map and inventory to memory --
    std::vector<uint8_t> edit_data;
    serialize_edit_map(edit_map, edit_data);

    std::vector<uint8_t> inv_data;
    serialize_inventory(inventory, inv_data);

    // Record expected inventory state before "reloading"
    int saved_selected_slot = inventory.get_selected_slot();
    int saved_planks = inventory.get_total_count(planks);
    int saved_sand = inventory.get_total_count(sand);
    int saved_stone = inventory.get_total_count(stone);

    // Record expected chunk state
    BlockID expected_break = chunk.get_block(break_lx, break_ly, break_lz);
    BlockID expected_place = chunk.get_block(place_lx, place_ly, place_lz);
    BlockID expected_craft  = chunk.get_block(craft_place_x, craft_place_y, craft_place_z);

    // -- Phase 7: Reload — deserialize into fresh objects --
    EditMap reloaded_edits;
    bool ok1 = deserialize_edit_map(edit_data.data(), edit_data.size(),
                                    reloaded_edits, BlockRegistry::get_instance());
    CHECK(ok1);

    Inventory reloaded_inv;
    bool ok2 = deserialize_inventory(inv_data.data(), inv_data.size(), reloaded_inv);
    CHECK(ok2);

    // Re-generate chunk from scratch (simulating fresh world load)
    ChunkData reloaded_chunk;
    reloaded_chunk.clear();
    gen.generate_chunk(reloaded_chunk, 0, 0, 0, no_cross, false);

    // Apply reloaded edits (simulating ChunkWorld::apply_edit_map_to_chunk)
    apply_edit_map_to_chunk(reloaded_edits, reloaded_chunk);

    // -- Phase 8: Verify everything survived the full loop --
    SUBCASE("chunk block state persisted correctly") {
        CHECK(reloaded_chunk.get_block(break_lx, break_ly, break_lz) == BlockIDs::AIR);
        CHECK(reloaded_chunk.get_block(place_lx, place_ly, place_lz) == sand);
        CHECK(reloaded_chunk.get_block(craft_place_x, craft_place_y, craft_place_z) == planks);
    }

    SUBCASE("inventory contents persisted correctly") {
        CHECK(reloaded_inv.get_total_count(planks) == saved_planks);
        CHECK(reloaded_inv.get_total_count(sand) == saved_sand);
        CHECK(reloaded_inv.get_total_count(stone) == saved_stone);
        CHECK(reloaded_inv.get_selected_slot() == saved_selected_slot);
    }

    SUBCASE("un-edited chunk regions remain unchanged") {
        // A block far from any edit should still be the generated terrain
        CHECK(reloaded_chunk.get_block(16, 15, 16) == chunk.get_block(16, 15, 16));
    }
}

// =========================================================================
// Test 2: Multi-chunk play session — cross-chunk break and place
//         → save → reload → verify
// =========================================================================

TEST_CASE("Multi-chunk play session: cross-chunk edits, save, reload, verify") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID dirt  = id("dirt");

    TerrainParams params;
    params.seed = 99;
    ChunkGenerator gen(params);
    std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross;

    // Generate 2x2 chunks
    ChunkData chunks[2][2];
    for (int cx = 0; cx < 2; ++cx)
        for (int cz = 0; cz < 2; ++cz) {
            chunks[cx][cz].clear();
            gen.generate_chunk(chunks[cx][cz], cx, 0, cz, no_cross, false);
        }

    EditMap edits[2][2];
    Inventory inventory;

    // Break blocks at chunk boundary: chunk (0,0,0) local (31, 30, 16)
    // and chunk (1,0,0) local (0, 30, 16)
    BlockID b1 = chunks[0][0].get_block(31, 30, 16);
    if (b1 == BlockIDs::AIR || b1 == BlockIDs::WATER || b1 == BlockIDs::SURFACE_WATER) b1 = stone;
    chunks[0][0].set_block(31, 30, 16, BlockIDs::AIR);
    edits[0][0].set_block(31, 30, 16, BlockIDs::AIR);
    inventory.add_block(b1, 1);

    BlockID b2 = chunks[1][0].get_block(0, 30, 16);
    if (b2 == BlockIDs::AIR || b2 == BlockIDs::WATER || b2 == BlockIDs::SURFACE_WATER) b2 = dirt;
    chunks[1][0].set_block(0, 30, 16, BlockIDs::AIR);
    edits[1][0].set_block(0, 30, 16, BlockIDs::AIR);
    inventory.add_block(b2, 1);

    // Place blocks at the opposite chunk boundaries
    inventory.consume_block(b1, 1);
    chunks[1][0].set_block(31, 29, 16, b1);
    edits[1][0].set_block(31, 29, 16, b1);

    inventory.consume_block(b2, 1);
    chunks[0][0].set_block(0, 29, 16, b2);
    edits[0][0].set_block(0, 29, 16, b2);

    // Save each chunk's edit map
    std::vector<uint8_t> data[2][2];
    for (int cx = 0; cx < 2; ++cx)
        for (int cz = 0; cz < 2; ++cz)
            serialize_edit_map(edits[cx][cz], data[cx][cz]);

    // Reload: re-generate fresh chunks, then apply deserialized edits
    ChunkData reloaded[2][2];
    for (int cx = 0; cx < 2; ++cx)
        for (int cz = 0; cz < 2; ++cz) {
            reloaded[cx][cz].clear();
            gen.generate_chunk(reloaded[cx][cz], cx, 0, cz, no_cross, false);

            EditMap re;
            bool ok = deserialize_edit_map(data[cx][cz].data(), data[cx][cz].size(),
                                           re, BlockRegistry::get_instance());
            CHECK(ok);
            apply_edit_map_to_chunk(re, reloaded[cx][cz]);
        }

    SUBCASE("break at chunk (0,0) boundary persisted") {
        CHECK(reloaded[0][0].get_block(31, 30, 16) == BlockIDs::AIR);
    }

    SUBCASE("break at chunk (1,0) boundary persisted") {
        CHECK(reloaded[1][0].get_block(0, 30, 16) == BlockIDs::AIR);
    }

    SUBCASE("cross-chunk placement in chunk (1,0) persisted") {
        CHECK(reloaded[1][0].get_block(31, 29, 16) == b1);
    }

    SUBCASE("cross-chunk placement in chunk (0,0) persisted") {
        CHECK(reloaded[0][0].get_block(0, 29, 16) == b2);
    }

    SUBCASE("unchanged regions in each chunk are preserved") {
        CHECK(reloaded[0][0].get_block(16, 15, 16) == chunks[0][0].get_block(16, 15, 16));
        CHECK(reloaded[1][0].get_block(16, 15, 16) == chunks[1][0].get_block(16, 15, 16));
    }

    SUBCASE("inventory survived") {
        CHECK(inventory.get_total_count(b1) == 0);
        CHECK(inventory.get_total_count(b2) == 0);
    }
}
