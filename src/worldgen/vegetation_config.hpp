#ifndef FARLANDS_VEGETATION_CONFIG_HPP
#define FARLANDS_VEGETATION_CONFIG_HPP

#include <cstdint>

namespace godot {
class String;
}

namespace VoxelEngine {

// ---------------------------------------------------------------------------
// Vegetation generation tuning — loaded from data/vegetation.json.
//
// Which feature set applies to a column is decided by its biome (see
// BiomeConfig::vegetation + BiomeType): Hills carries sparse trees (a single
// isolated tree per qualifying chunk). All numeric knobs below are
// configurable; defaults preserve the historical behavior.
// ---------------------------------------------------------------------------

// Sparse hills: only a fraction of chunks carry a single isolated tree.
struct HillsVegConfig {
    int32_t chunk_chance_pct = 25;   // % of hills chunks that get one tree
    int32_t spacing_radius   = 3;
};

struct VegetationConfig {
    int32_t tree_trunk_height = 5;   // oak/spruce trunk height in blocks

    HillsVegConfig hills;

    // Applies defaults first, then overrides from the JSON file.
    bool load(const godot::String& json_path);
};

} // namespace VoxelEngine

#endif // FARLANDS_VEGETATION_CONFIG_HPP