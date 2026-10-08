// The far mode's two probe readouts (declared in lod/lod_grid.hpp): the sampler's
// answer for one column, and where the water quads that have been built actually
// are.
//
// Both exist because a frame cannot answer either question, and the report this
// mode keeps coming back with is one question asked twice: a light blue over the
// far field's OWN land. Over water that colour is invisible, so it is either the
// mode's water sheet somewhere other than the sea level, or the land's own quads
// wearing the water's texture layer -- and "is the water at the world's sea level"
// is the one number that separates them. A frame cannot supply it: which of the two
// surfaces drew a pixel is a depth-test answer (probes/probe_lod_grid_overlap.gd
// measures exactly that), and the sheet's own height is not in the picture at all.
//
// So both answers are read from the data instead. `debug_column` goes through the
// same generator every tile build makes, from the same copied params, and reports
// what a cell at that column would be given; `debug_water_heights` reads the water
// vertices back out of the tiles that have been built and says where they are, in
// world Y, beside the world's own sea level.
//
// probes/probe_lod_grid_water.gd is the caller.
#include "lod/lod_grid.hpp"

#include "worldgen/chunk_generator.hpp"
#include "worldgen/terrain_squish.hpp"

#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace VoxelEngine {

using namespace godot;

Dictionary LodGrid::debug_column(int32_t x, int32_t z) const {
    Dictionary out;
    // A generator per call, exactly as build_tile makes one: the params and the biome
    // config are copies here, so this reads the configuration the tiles were sampled
    // from and not a second, differently-configured world.
    ChunkGenerator generator(terrain_params);
    generator.set_biome_config(biome_config);
    const ChunkGenerator::ColumnSample column = generator.sample_column_debug(x, z);
    const int32_t surface = generator.find_surface_y(x, z);
    // The mesh builder's own rule (lod_grid_build.cpp::sample_column), spelled out
    // rather than left to the reader: a column is flooded when its water level is
    // positive, and a flooded column's cell is handed that level as its water.
    const bool flooded = column.water_level > 0.0f;
    const size_t biome = static_cast<size_t>(column.biome);

    out["x"] = x;
    out["z"] = z;
    out["biome"] = static_cast<int32_t>(column.biome);
    out["column_height"] = column.height;
    out["water_level"] = column.water_level;
    out["flooded"] = flooded;
    out["surface_y"] = surface;
    out["sample_height"] = static_cast<float>(surface);
    out["sample_water"] = flooded ? column.water_level : lod::kNoWater;
    out["sample_layer"] =
        static_cast<int32_t>(flooded ? underwater_layer : biome_layers[biome]);
    // The three numbers that decide the water level above, side by side with the
    // answer they produced: the configured level, whether the squish is mapping it,
    // and what those two come to (worldgen/terrain_squish.hpp::sea_level).
    out["sea_level"] = terrain_params.sea_level;
    out["effective_sea_level"] = squish::sea_level(terrain_params);
    out["squish_enabled"] = terrain_params.squish_enabled;
    out["squish_slice"] = terrain_params.squish_slice;
    out["squish_span"] = terrain_params.squish_span;
    return out;
}

Dictionary LodGrid::debug_water_heights() const {
    Dictionary out;
    int32_t tiles_with_geometry = 0;
    int32_t tiles_with_water = 0;
    int32_t water_vertices = 0;
    int32_t terrain_vertices = 0;
    float water_min = 1.0e30f;
    float water_max = -1.0e30f;
    float terrain_min = 1.0e30f;
    float terrain_max = -1.0e30f;
    // Where the water is, in Y, rounded to blocks: a sheet at one level is one bar,
    // and a sheet that has taken the terrain's own heights is a spread as wide as
    // the terrain.
    std::map<int32_t, int32_t> histogram;
    // ...and the same figures per level, because the levels sample at different
    // spacings and a difference between them is a difference in the sampler.
    std::map<int32_t, std::array<float, 3>> levels;

    for (const auto& [key, tile] : tiles) {
        (void)key;
        if (tile.raw.vertices.empty()) continue;
        ++tiles_with_geometry;
        bool water_here = false;
        for (const lod::LodVertex& v : tile.raw.vertices) {
            // The sheet's own flag (see build_tile_mesh): 1.0 on the water quads and
            // 0.0 on the terrain's, so the two can be told apart in a merged mesh.
            if (v.water > 0.5f) {
                water_here = true;
                ++water_vertices;
                water_min = std::min(water_min, v.y);
                water_max = std::max(water_max, v.y);
                ++histogram[static_cast<int32_t>(std::lround(v.y))];
            } else {
                ++terrain_vertices;
                terrain_min = std::min(terrain_min, v.y);
                terrain_max = std::max(terrain_max, v.y);
            }
        }
        if (water_here) {
            ++tiles_with_water;
            auto& level = levels[tile.level];
            level[0] += static_cast<float>(tile.raw.water_quads);
            level[1] = level[1] == 0.0f ? tile.raw.min_y : std::min(level[1], tile.raw.min_y);
            level[2] = std::max(level[2], tile.raw.max_y);
        }
    }

    out["tiles_with_geometry"] = tiles_with_geometry;
    out["tiles_with_water"] = tiles_with_water;
    out["water_vertices"] = water_vertices;
    out["terrain_vertices"] = terrain_vertices;
    out["water_min_y"] = water_vertices > 0 ? water_min : 0.0f;
    out["water_max_y"] = water_vertices > 0 ? water_max : 0.0f;
    out["terrain_min_y"] = terrain_vertices > 0 ? terrain_min : 0.0f;
    out["terrain_max_y"] = terrain_vertices > 0 ? terrain_max : 0.0f;
    Dictionary bars;
    for (const auto& [y, count] : histogram) {
        bars[y] = count;
    }
    out["histogram"] = bars;
    Dictionary per_level;
    for (const auto& [level, figure] : levels) {
        Dictionary entry;
        entry["water_quads"] = static_cast<int32_t>(figure[0]);
        entry["min_y"] = figure[1];
        entry["max_y"] = figure[2];
        per_level[level] = entry;
    }
    out["levels"] = per_level;
    // The world's own side of the comparison: the sea level the sampler answers with,
    // the eye the frame would be read from, and the two layers the mode resolved for
    // terrain (underwater) and for the liquid sheet.
    out["sea_level"] = terrain_params.sea_level;
    out["effective_sea_level"] = squish::sea_level(terrain_params);
    out["squish_enabled"] = terrain_params.squish_enabled;
    out["underwater_layer"] = static_cast<int32_t>(underwater_layer);
    out["water_layer"] = static_cast<int32_t>(water_layer);
    out["player_y"] = player_position.y;
    out["inner_radius_blocks"] = inner_radius_blocks;
    out["outer_radius_blocks"] = get_outer_radius_blocks();
    return out;
}

} // namespace VoxelEngine
