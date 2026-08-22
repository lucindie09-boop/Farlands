#include "doctest.h"
#include "core/crafting.hpp"
#include "core/block_types.hpp"

using namespace VoxelEngine;

namespace {

// Deterministic ids from initialize_default_blocks: 1=stone, 3=dirt...
// Resolve by name so the tests don't hardcode numeric ids.
BlockID id(const char* name) {
    return BlockRegistry::get_instance().get_block_id_by_name(name);
}

// oak_planks exists only in block_definitions.json (Godot-side loading), so
// standalone tests register an equivalent full-cube default lazily.
BlockID ensure_oak_planks() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    BlockID existing = registry.get_block_id_by_name("oak_planks");
    if (existing != BlockIDs::AIR) return existing;
    BlockType bt{};
    bt.name = "oak_planks";
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.slipperiness = 0.6f;
    bt.full_cube_ = true;
    return registry.register_block(bt);
}

CraftingRecipe shapeless_recipe(std::vector<BlockID> items, BlockID result,
                                int count) {
    CraftingRecipe r;
    r.type = CraftingRecipe::Type::Shapeless;
    r.shapeless_items = std::move(items);
    r.result = {result, count};
    return r;
}

CraftingRecipe shaped_recipe(std::vector<std::vector<BlockID>> rows,
                             BlockID result, int count) {
    CraftingRecipe r;
    r.type = CraftingRecipe::Type::Shaped;
    r.shape_height = static_cast<int32_t>(rows.size());
    r.shape_width = static_cast<int32_t>(rows[0].size());
    for (const auto& row : rows) {
        r.shaped_cells.insert(r.shaped_cells.end(), row.begin(), row.end());
    }
    r.result = {result, count};
    return r;
}

} // namespace

TEST_CASE("shapeless matching is order independent") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID log = id("stone");
    const BlockID planks = ensure_oak_planks();

    RecipeBook book;
    book.add_recipe(shapeless_recipe({log}, planks, 4));

    SUBCASE("single ingredient anywhere in the grid") {
        for (int32_t cell = 0; cell < 4; ++cell) {
            BlockID grid[4] = {0, 0, 0, 0};
            grid[cell] = log;
            CHECK(book.match(grid, 2) != nullptr);
        }
    }

    SUBCASE("empty grid does not match") {
        BlockID grid[4] = {0, 0, 0, 0};
        CHECK(book.match(grid, 2) == nullptr);
    }

    SUBCASE("wrong ingredient does not match") {
        BlockID grid[4] = {id("dirt"), 0, 0, 0};
        CHECK(book.match(grid, 2) == nullptr);
    }

    SUBCASE("extra ingredients do not match") {
        BlockID grid[4] = {log, log, 0, 0};
        CHECK(book.match(grid, 2) == nullptr);
    }
}

TEST_CASE("shaped matching trims and mirrors") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID stone = id("stone");
    const BlockID dirt = id("dirt");

    RecipeBook book;
    // 2x2 checker stored trimmed; grid placements may sit anywhere.
    book.add_recipe(shaped_recipe(
        {{stone, dirt}, {dirt, stone}}, id("oak_planks"), 1));

    SUBCASE("exact placement") {
        BlockID grid[4] = {stone, dirt, dirt, stone};
        CHECK(book.match(grid, 2) != nullptr);
    }

    SUBCASE("mirrored placement matches") {
        BlockID grid[4] = {dirt, stone, stone, dirt};
        CHECK(book.match(grid, 2) != nullptr);
    }

    SUBCASE("offset placement inside a larger grid matches") {
        BlockID grid[9] = {0};
        // Rows 1-2, columns 1-2 of a 3x3.
        grid[4] = stone; grid[5] = dirt;
        grid[7] = dirt;  grid[8] = stone;
        CHECK(book.match(grid, 3) != nullptr);
    }

    SUBCASE("rotated pattern does not match") {
        BlockID grid[4] = {stone, stone, dirt, dirt};
        CHECK(book.match(grid, 2) == nullptr);
    }

    SUBCASE("partial pattern does not match") {
        BlockID grid[4] = {stone, dirt, dirt, 0};
        CHECK(book.match(grid, 2) == nullptr);
    }
}

TEST_CASE("craft_item consumes ingredients and adds result atomically") {
    BlockRegistry::get_instance().initialize_default_blocks();
    // NOTE: defaults have no "oak_log"; stone is always registered.
    const BlockID log = id("stone");
    const BlockID planks = ensure_oak_planks();

    RecipeBook book;
    book.add_recipe(shapeless_recipe({log}, planks, 4));
    const CraftingRecipe& recipe = book.recipes()[0];

    SUBCASE("successful craft") {
        Inventory inv;
        inv.add_block(log, 2);
        CHECK(craft_item(recipe, inv));
        CHECK(inv.get_total_count(log) == 1);
        CHECK(inv.get_total_count(planks) == 4);
    }

    SUBCASE("insufficient ingredients leaves inventory untouched") {
        Inventory inv;
        inv.add_block(log, 0);  // empty
        CHECK_FALSE(craft_item(recipe, inv));
        CHECK(inv.get_total_count(log) == 0);
        CHECK(inv.get_total_count(planks) == 0);
    }

    SUBCASE("full inventory rejects craft without consuming") {
        Inventory inv;
        // Fill every slot except hotbar[0] with distinct full stacks so
        // nothing stacks together, then park the logs in hotbar[0].
        for (int slot = 1; slot < Inventory::HOTBAR_SIZE; ++slot) {
            inv.set_hotbar_slot(slot, static_cast<BlockID>(300 + slot), 64);
        }
        for (int slot = 0; slot < Inventory::INVENTORY_SIZE; ++slot) {
            inv.set_inventory_slot(slot, static_cast<BlockID>(200 + slot), 64);
        }
        inv.set_hotbar_slot(0, log, 64);
        CHECK(inv.get_total_count(log) == 64);
        CHECK_FALSE(inv.can_add_block(planks, 4));
        CHECK_FALSE(craft_item(recipe, inv));
        CHECK(inv.get_total_count(log) == 64);
        CHECK(inv.get_total_count(planks) == 0);
    }
}
