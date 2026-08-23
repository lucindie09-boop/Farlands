#ifndef FARLANDS_CRAFTING_HPP
#define FARLANDS_CRAFTING_HPP

#include "core/block_types.hpp"
#include "core/inventory.hpp"

#include <cstdint>
#include <vector>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/variant/string.hpp>
#endif

namespace VoxelEngine {

struct CraftingRecipe {
    enum class Type : uint8_t { Shaped, Shapeless };

    Type type = Type::Shapeless;

    // Shaped: trimmed pattern cells, row-major, shape_height rows of
    // shape_width entries. AIR (0) = empty cell. Mirrored variants are
    // matched automatically at match time, not stored separately.
    int32_t shape_width = 0;
    int32_t shape_height = 0;
    std::vector<BlockID> shaped_cells;

    // Shapeless: one entry per required unit (duplicates allowed).
    std::vector<BlockID> shapeless_items;

    InventorySlot result;

    // Distinct ingredients with per-unit consumption counts, derived at
    // add_recipe time so crafting never re-walks the raw cells.
    std::vector<InventorySlot> ingredient_totals;
};

class RecipeBook {
public:
    // Default grid the GUI should present (loaded from recipes.json).
    int32_t grid_size = 2;

    void add_recipe(CraftingRecipe recipe);
    void clear() { recipes_.clear(); }
    const std::vector<CraftingRecipe>& recipes() const { return recipes_; }

    // grid: row-major size*size BlockIDs (AIR = empty). Returns nullptr when
    // nothing matches. Shaped patterns also match mirrored placements.
    [[nodiscard]] const CraftingRecipe* match(const BlockID* grid, int32_t size) const;

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // Parses res://data/recipes.json. Missing/unparseable file returns false
    // and leaves current recipes untouched; individual recipes referencing
    // unknown block names are skipped with a warning.
    [[nodiscard]] bool load_from_json(const godot::String& json_path) noexcept;
#endif

private:
    std::vector<CraftingRecipe> recipes_;
};

// Consumes the recipe's ingredients from inv and adds the result. Returns
// false (without mutating anything) when the inventory lacks ingredients or
// cannot fit the output.
[[nodiscard]] bool craft_item(const CraftingRecipe& recipe, Inventory& inv);

} // namespace VoxelEngine

#endif // FARLANDS_CRAFTING_HPP
