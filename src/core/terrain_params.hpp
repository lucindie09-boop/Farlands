#ifndef FARLANDS_TERRAIN_PARAMS_HPP
#define FARLANDS_TERRAIN_PARAMS_HPP
#include <cstdint>

namespace godot {
class String;
}

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Terrain generation parameters — kept in its own header so that
// world scheduling code (WorldUpdater, ChunkWorld) does not have to
// include the heavy chunk_generator.hpp / noise.hpp transitively.
//
// The macro-surface tuning fields load from data/terrain_config.json at
// startup (see load_from_json); the persisted fields below it (seed, sea
// level, bedrock height, cave/sub-surface params, climate scales, biome_size)
// round-trip through world.meta.
// -------------------------------------------------------------------------
struct TerrainParams {
    int32_t seed = 12345;
    float sea_level = 200.0f;
    int32_t bedrock_height = 5;

    float cave_threshold = 0.4f;
    float cave_scale = 0.05f;

    int32_t subsurface_cover_depth = 4;

    // Climate noise scales (lower = broader regions). Consumed by
    // WorldUpdater::set_biome_size (climate_*_scale = base / biome_size);
    // the resulting scales feed the climate samplers in chunk_generator.
    float climate_temp_scale = 0.00015f;
    float climate_humidity_scale = 0.00020f;

    // Fixed base-frequency climate scales for the macro surface height blend.
    // Kept separate so biome boundaries (climate_temp_scale, scaled by
    // biome_size) can move without blurring the height field.
    float climate_temp_base_scale = 0.00015f;
    float climate_humidity_base_scale = 0.00020f;

    // Macro surface base height (sea_level + margin).
    float height_base_y = 512.0f;

    // Domain warp amplitudes (blocks) for the macro height field: two warp
    // octaves, x/z warped by different amounts so landforms get a directional
    // grain. Frequencies are fixed in chunk_generator (0.002 / 0.0018).
    float macro_warp_amp_x1 = 18.0f;
    float macro_warp_amp_z1 = 30.0f;
    float macro_warp_amp_x2 = 10.0f;
    float macro_warp_amp_z2 = 16.0f;

    // 3D shape field domain warp amplitudes (blocks, horizontal plane only).
    float shape_warp_amp_x = 8.0f;
    float shape_warp_amp_z = 12.0f;

    // Medium-scale (~500-block) relief field: coarse world-anchored lattice
    // with nodes every mid_lattice_spacing blocks, bilinearly interpolated.
    float mid_lattice_spacing = 8.0f;
    float mid_frequency = 0.002f;      // ~1/freq = 500-block features
    float mid_amplitude = 90.0f;       // vertical relief, blocks

    // Small-scale (~150-block) relief field, same scheme.
    float small_lattice_spacing = 2.0f;
    float small_frequency = 0.0066667f; // ~1/freq = 150-block features
    float small_amplitude = 25.0f;      // vertical relief, blocks

    // 3D shape strength range: strength = lerp(min, max, weirdness).
    float shape_strength_min = 5.0f;
    float shape_strength_max = 50.0f;

    // Weirdness mask: 2D fBm at weirdness_scale (~1/scale block base octave),
    // smoothstepped between LOW and HIGH so most of the world maps to the
    // minimum strength and only the upper tail of each lobe rises through
    // the ramp.
    float weirdness_scale = 0.024f;
    float weirdness_low = 0.10f;
    float weirdness_high = 0.75f;

    // Biome size multiplier (1.0 = default, >1 = larger biomes)
    float biome_size = 1.0f;

    // Loads macro-surface tuning from JSON. Missing file or keys keep the
    // existing values; returns false only if the file could not load.
    bool load_from_json(const godot::String& json_path) noexcept;
};

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_PARAMS_HPP