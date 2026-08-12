#ifndef FUK_MINECRAFT_VEGETATION_CONFIG_HPP
#define FUK_MINECRAFT_VEGETATION_CONFIG_HPP

#include <cstdint>

namespace godot {
class String;
}

namespace VoxelEngine {

// ---------------------------------------------------------------------------
// Vegetation generation tuning — loaded from data/vegetation.json.
//
// Which feature set applies to a column is decided by its biome (see
// BiomeConfig::vegetation + BiomeType): Forest gets clustered trees with
// boulders, Plains gets a sparse single tree per qualifying chunk, Desert
// gets cacti. All numeric knobs below are configurable; defaults preserve the
// historical behavior.
// ---------------------------------------------------------------------------

// Dense clustered forests: most forest chunks are filled with trees that obey
// a minimum Chebyshev spacing, plus occasional stone boulders.
struct ForestVegConfig {
    int32_t chunk_chance_pct      = 80;  // % of forest chunks that contain trees
    int32_t min_trees             = 15;  // per-chunk tree target bounds
    int32_t max_trees             = 25;
    int32_t column_chance_pct     = 50;  // % of candidate surface columns per chunk
    int32_t spacing_radius        = 3;   // Chebyshev distance kept between trees
    int32_t boulder_chance_per_10000 = 3; // boulders per 10000 columns
    int32_t boulder_radius        = 3;   // base radius (grows by a seed bit)
};

// Sparse plains: only a fraction of chunks carry a single isolated tree.
struct PlainsVegConfig {
    int32_t chunk_chance_pct = 25;   // % of plains chunks that get one tree
    int32_t spacing_radius   = 3;
};

// Desert cacti.
struct DesertVegConfig {
    int32_t cactus_chance_per_1000 = 3;  // cacti per 1000 sand columns
    int32_t min_height             = 2;
    int32_t max_height             = 3;
};

struct VegetationConfig {
    int32_t tree_trunk_height = 5;   // oak/spruce trunk height in blocks

    ForestVegConfig forest;
    PlainsVegConfig plains;
    DesertVegConfig desert;

    // Applies defaults first, then overrides from the JSON file.
    bool load(const godot::String& json_path);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_VEGETATION_CONFIG_HPP
