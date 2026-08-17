#ifndef FUK_MINECRAFT_TERRAIN_PARAMS_HPP
#define FUK_MINECRAFT_TERRAIN_PARAMS_HPP
#include <array>
#include <cstdint>

namespace godot {
class String;
}

namespace VoxelEngine {

// One land-biome height center used by the Voronoi-weighted macro surface
// blend. `temp`/`hum` locate the center in climate space; `base_off` shifts
// the surface height and `scale_m` scales terrain amplitude near it.
struct HeightCenter {
    float temp    = 0.5f;
    float hum     = 0.5f;
    float base_off = 0.0f;
    float scale_m = 1.0f;
};

// -------------------------------------------------------------------------
// Terrain generation parameters — kept in its own header so that
// world scheduling code (WorldUpdater, ChunkWorld) does not have to
// include the heavy chunk_generator.hpp / noise.hpp transitively.
//
// Fields not persisted to world.meta (macro surface tuning) load from
// data/terrain_config.json at startup.
// -------------------------------------------------------------------------
struct TerrainParams {
    int32_t seed = 12345;
    float sea_level = 200.0f;
    int32_t bedrock_height = 5;

    float cave_threshold = 0.4f;
    float cave_scale = 0.05f;

    float continentalness_scale = 0.00010f;
    float ocean_threshold = 0.48f;
    float land_threshold = 0.48f;
    float shelf_width = 0.025f;
    float shelf_depth = 18.0f;
    float deep_ocean_depth = 48.0f;
    float beach_width = 0.003f;
    int32_t subsurface_cover_depth = 4;

    // Climate noise scales (lower = broader regions).
    float climate_temp_scale = 0.00015f;
    float climate_humidity_scale = 0.00020f;

    // Fixed base-frequency climate scales for the macro surface height blend.
    // Kept separate so biome boundaries (climate_temp_scale, scaled by
    // biome_size) can move without blurring the height field.
    float climate_temp_base_scale = 0.00015f;
    float climate_humidity_base_scale = 0.00020f;

    // Macro surface base height (sea_level + margin).
    float height_base_y = 208.0f;

    // Voronoi height centers over land biomes (indexed with the same order as
    // the hardcoded table: plains, forest, desert).
    std::array<HeightCenter, 3> height_centers{
        HeightCenter{0.50f, 0.35f,   6.0f, 0.12f},
        HeightCenter{0.50f, 0.78f,   4.0f, 1.00f},
        HeightCenter{0.78f, 0.22f, -12.0f, 0.37f}
    };

    // Biome size multiplier (1.0 = default, >1 = larger biomes)
    float biome_size = 1.0f;

    // Loads non-persisted macro-surface tuning from JSON. Missing file or keys
    // keep the existing values; returns false only if the file could not load.
    bool load_from_json(const godot::String& json_path) noexcept;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_TERRAIN_PARAMS_HPP