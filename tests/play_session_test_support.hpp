#ifndef FARLANDS_TESTS_PLAY_SESSION_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_PLAY_SESSION_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the play-session tests.
//
// `id` looks a block up by name and `ensure_oak_planks` registers the plank the
// crafting tests need if the data files did not, and the two recipe builders make
// a shaped and a shapeless recipe without spelling out every field each time.
//
// These were file-local to test_play_session.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "core/crafting.hpp"

#include <algorithm>
#include <utility>
#include <vector>

using namespace VoxelEngine;

namespace play_session_test {


// Helper: create a shaped recipe
inline CraftingRecipe shaped_recipe(std::vector<std::vector<BlockID>> rows,
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
inline CraftingRecipe shapeless_recipe(std::vector<BlockID> items, BlockID result, int count) {
    CraftingRecipe r;
    r.type = CraftingRecipe::Type::Shapeless;
    r.shapeless_items = std::move(items);
    r.result = {result, count};
    return r;
}

inline BlockID id(const char* name) {
    return BlockRegistry::get_instance().get_block_id_by_name(name);
}

inline BlockID ensure_oak_planks() {
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

} // namespace play_session_test

#endif // FARLANDS_TESTS_PLAY_SESSION_TEST_SUPPORT_HPP
