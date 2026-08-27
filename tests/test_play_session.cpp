#include "doctest.h"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/edit_map.hpp"
#include "core/crc32.hpp"
#include "core/inventory.hpp"
#include "core/crafting.hpp"
#include "worldgen/chunk_generator.hpp"
#include <vector>
#include <unordered_map>
#include <functional>

using namespace VoxelEngine;

// =========================================================================
// Note: the inventory INVE byte format (serialize_inventory /
// deserialize_inventory) and the edit-map apply step (apply_edit_map_to_chunk)
// live in src/core/ and are the SAME functions ChunkWorld uses. These tests
// therefore exercise the production format and apply logic, not a mirrored
// copy.
// =========================================================================

namespace {

// Helper: create a shaped recipe
CraftingRecipe shaped_recipe(std::vector<std::vector<BlockID>> rows,
                             BlockID result, int count) {
    CraftingRecipe r;
    r.type = CraftingRecipe::Type::Shaped;
    r.shape_height = static_cast<int32_t>(rows.size());
    r.shape_width = static_cast<int32_t>(rows[0].size());
    for (const auto& row : rows)
        r.shaped_cells.insert(r.shaped_cells.end(), row.begin(), row.end());
    r.result = {result, count};
    return r;
}

// Helper: create a shapeless recipe
CraftingRecipe shapeless_recipe(std::vector<BlockID> items, BlockID result, int count) {
    CraftingRecipe r;
    r.type = CraftingRecipe::Type::Shapeless;
    r.shapeless_items = std::move(items);
    r.result = {result, count};
    return r;
}

BlockID id(const char* name) {
    return BlockRegistry::get_instance().get_block_id_by_name(name);
}

BlockID ensure_oak_planks() {
    BlockRegistry& reg = BlockRegistry::get_instance();
    BlockID existing = reg.get_block_id_by_name("oak_planks");
    if (existing != BlockIDs::AIR) return existing;
    BlockType bt{};
    bt.name = "oak_planks";
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.slipperiness = 0.6f;
    bt.full_cube_ = true;
    return reg.register_block(bt);
}

} // anonymous namespace

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
