#include "core/crafting.hpp"

#include <algorithm>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include "core/item_registry.hpp"
#include "core/json_config.hpp"
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#endif

namespace VoxelEngine {

namespace {

bool is_occupied(BlockID id) { return id != BlockIDs::AIR; }

// Bounding box of non-empty cells in a row-major grid. Returns false when the
// grid has no occupied cells.
bool grid_bounds(const BlockID* grid, int32_t size, int32_t& min_row,
                 int32_t& min_col, int32_t& max_row, int32_t& max_col) {
    min_row = min_col = size;
    max_row = max_col = -1;
    for (int32_t r = 0; r < size; ++r) {
        for (int32_t c = 0; c < size; ++c) {
            if (is_occupied(grid[r * size + c])) {
                min_row = std::min(min_row, r);
                min_col = std::min(min_col, c);
                max_row = std::max(max_row, r);
                max_col = std::max(max_col, c);
            }
        }
    }
    return max_row >= 0;
}

bool shaped_matches(const CraftingRecipe& recipe, const BlockID* grid,
                    int32_t size, bool mirrored) {
    int32_t min_row, min_col, max_row, max_col;
    if (!grid_bounds(grid, size, min_row, min_col, max_row, max_col)) {
        return false;
    }
    const int32_t w = max_col - min_col + 1;
    const int32_t h = max_row - min_row + 1;
    if (w != recipe.shape_width || h != recipe.shape_height) {
        return false;
    }
    for (int32_t r = 0; r < h; ++r) {
        for (int32_t c = 0; c < w; ++c) {
            const BlockID cell =
                grid[(min_row + r) * size + (min_col + c)];
            const int32_t col = mirrored ? (w - 1 - c) : c;
            const BlockID want = recipe.shaped_cells[r * recipe.shape_width + col];
            if (cell != want) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void RecipeBook::add_recipe(CraftingRecipe recipe) {
    // Derive per-ingredient consumption totals once.
    std::vector<InventorySlot> totals;
    auto add_unit = [&totals](BlockID id) {
        for (auto& slot : totals) {
            if (slot.block_id == id) {
                ++slot.count;
                return;
            }
        }
        totals.push_back({id, 1});
    };
    if (recipe.type == CraftingRecipe::Type::Shaped) {
        for (BlockID id : recipe.shaped_cells) {
            if (is_occupied(id)) add_unit(id);
        }
    } else {
        for (BlockID id : recipe.shapeless_items) add_unit(id);
    }
    recipe.ingredient_totals = std::move(totals);
    recipes_.push_back(std::move(recipe));
}

const CraftingRecipe* RecipeBook::match(const BlockID* grid,
                                        int32_t size) const {
    for (const CraftingRecipe& recipe : recipes_) {
        if (recipe.type == CraftingRecipe::Type::Shapeless) {
            std::vector<BlockID> present;
            for (int32_t i = 0; i < size * size; ++i) {
                if (is_occupied(grid[i])) present.push_back(grid[i]);
            }
            if (present.size() != recipe.shapeless_items.size()) continue;
            std::vector<BlockID> wanted = recipe.shapeless_items;
            std::sort(present.begin(), present.end());
            std::sort(wanted.begin(), wanted.end());
            if (present == wanted) return &recipe;
        } else {
            if (shaped_matches(recipe, grid, size, false) ||
                shaped_matches(recipe, grid, size, true)) {
                return &recipe;
            }
        }
    }
    return nullptr;
}

bool craft_item(const CraftingRecipe& recipe, Inventory& inv) {
    // All-or-nothing: verify availability and output space before mutating.
    for (const InventorySlot& need : recipe.ingredient_totals) {
        if (inv.get_total_count(need.block_id) < need.count) return false;
    }
    if (!inv.can_add_block(recipe.result.block_id, recipe.result.count)) {
        return false;
    }
    for (const InventorySlot& need : recipe.ingredient_totals) {
        if (!inv.consume_block(need.block_id, need.count)) return false;
    }
    return inv.add_block(recipe.result.block_id, recipe.result.count);
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

namespace {

BlockID resolve_name(const godot::String& name) {
    const char* utf8 = name.utf8().get_data();
    // Items (sticks, tools, ...) share the recipe namespace with blocks.
    BlockID id = BlockRegistry::get_instance().get_block_id_by_name(utf8);
    if (id == BlockIDs::AIR) {
        id = ItemRegistry::get_instance().get_item_id_by_name(utf8);
    }
    return id;
}

} // namespace

bool RecipeBook::load_from_json(const godot::String& json_path) noexcept {
    std::optional<godot::Dictionary> root_opt = read_json_dictionary(json_path);
    if (!root_opt.has_value()) {
        return false;
    }
    const godot::Dictionary& root = *root_opt;

    if (root.has("grid_size")) {
        const int64_t gs = static_cast<int64_t>(root["grid_size"]);
        if (gs >= 1 && gs <= 3) grid_size = static_cast<int32_t>(gs);
    }

    if (!root.has("recipes")) {
        return true;
    }
    const godot::Array entries = root["recipes"];
    for (int64_t i = 0; i < entries.size(); ++i) {
        const godot::Dictionary entry = entries[static_cast<int>(i)];
        const godot::String type_str = entry.get("type", "shapeless");

        // Result (required)
        if (!entry.has("result")) {
            WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": missing result, skipped");
            continue;
        }
        const godot::Dictionary result_dict = entry["result"];
        CraftingRecipe recipe;
        recipe.result.block_id =
            resolve_name(result_dict.get("block", godot::String()));
        recipe.result.count =
            static_cast<int>(static_cast<int64_t>(result_dict.get("count", 1)));
        if (recipe.result.block_id == BlockIDs::AIR || recipe.result.count <= 0) {
            WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": unknown result block or bad count, skipped");
            continue;
        }

        if (type_str == "shaped") {
            const godot::Array pattern = entry.get("pattern", godot::Array());
            const godot::Dictionary key = entry.get("key", godot::Dictionary());
            if (pattern.size() == 0) {
                WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": empty shaped pattern, skipped");
                continue;
            }
            const int64_t raw_w = static_cast<godot::String>(pattern[0]).length();
            recipe.type = CraftingRecipe::Type::Shaped;
            recipe.shape_height = static_cast<int32_t>(pattern.size());
            recipe.shape_width = static_cast<int32_t>(raw_w);
            recipe.shaped_cells.reserve(static_cast<size_t>(raw_w * pattern.size()));
            bool ok = true;
            for (int64_t r = 0; r < pattern.size() && ok; ++r) {
                const godot::String row = pattern[static_cast<int>(r)];
                if (row.length() != raw_w) {
                    WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": ragged pattern row, skipped");
                    ok = false;
                    break;
                }
                for (int64_t c = 0; c < raw_w; ++c) {
                    const char ch = row.utf8().get_data()[c];
                    if (ch == ' ') {
                        recipe.shaped_cells.push_back(BlockIDs::AIR);
                        continue;
                    }
                    const godot::String name = key.get(
                        godot::String::chr(static_cast<char32_t>(ch)), godot::String());
                    const BlockID id = resolve_name(name);
                    if (id == BlockIDs::AIR) {
                        WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": unknown ingredient '" + name + "', skipped");
                        ok = false;
                        break;
                    }
                    recipe.shaped_cells.push_back(id);
                }
            }
            if (!ok) continue;

            // Trim empty border rows/columns so matching only sees the core.
            while (recipe.shape_height > 0) {
                bool empty = true;
                for (int32_t c = 0; c < recipe.shape_width; ++c) {
                    if (recipe.shaped_cells[c] != BlockIDs::AIR) empty = false;
                }
                if (!empty) break;
                recipe.shaped_cells.erase(
                    recipe.shaped_cells.begin(),
                    recipe.shaped_cells.begin() + recipe.shape_width);
                --recipe.shape_height;
            }
            while (recipe.shape_width > 0) {
                bool empty = true;
                for (int32_t r = 0; r < recipe.shape_height; ++r) {
                    if (recipe.shaped_cells[static_cast<size_t>(r) * recipe.shape_width] != BlockIDs::AIR) empty = false;
                }
                if (!empty) break;
                for (int32_t r = recipe.shape_height - 1; r >= 0; --r) {
                    recipe.shaped_cells.erase(
                        recipe.shaped_cells.begin() + static_cast<ptrdiff_t>(r) * recipe.shape_width);
                }
                --recipe.shape_width;
            }
        } else {
            recipe.type = CraftingRecipe::Type::Shapeless;
            const godot::Array ingredients = entry.get("ingredients", godot::Array());
            bool ok = true;
            for (int64_t k = 0; k < ingredients.size(); ++k) {
                const BlockID id =
                    resolve_name(static_cast<godot::String>(ingredients[static_cast<int>(k)]));
                if (id == BlockIDs::AIR) {
                    WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": unknown ingredient, skipped");
                    ok = false;
                    break;
                }
                recipe.shapeless_items.push_back(id);
            }
            if (!ok) continue;
            if (recipe.shapeless_items.empty()) {
                WARN_PRINT("recipes.json entry " + godot::String::num_int64(i) + ": no ingredients, skipped");
                continue;
            }
        }

        add_recipe(std::move(recipe));
    }
    return true;
}

#endif // !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

} // namespace VoxelEngine
