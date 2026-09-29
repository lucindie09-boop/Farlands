// The cheap questions about a chunk that do not need it generated: its height
// range, the surface and subsurface block under a column, and whether the chunk
// is uniformly solid or empty -- the last of which is what lets the writer take
// the fast path in chunk_generator.cpp.

#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Fast chunk content estimation (for surface-aware generation)
// -------------------------------------------------------------------------
ChunkGenerator::HeightRange ChunkGenerator::get_chunk_height_range(int32_t chunk_x, int32_t chunk_z) const {
    // Lattice-based: the land-shape and blended-amplification fields are
    // evaluated once on the chunk's 4-block nodes (81 each) and combined per
    // cell. This is ~2x cheaper than the old 5 per-column queries (which each
    // paid the border-blend neighborhood) and covers every column instead of
    // just the corners, so the fast-path classification is both faster and
    // tighter. The final column height is sea + (raw - sea) * amp with both
    // fields bilinearly interpolated between nodes, so within each cell the
    // product is bounded by every (raw x amp) combo of the cell's four
    // corners; ocean columns use the fixed ocean amp. The 3D density
    // shaping can still push the real surface up to density_margin() above or
    // below the macro heightmap (the widest surface band any biome's
    // weirdness_size can produce), so the range is padded by that.
    float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_land_shape_lattice(chunk_x, chunk_z, land_lat);
    build_climate_lattice(chunk_x, chunk_z, temp_lat, hum_lat);
    BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_amp_lattice(chunk_x, chunk_z, temp_lat, hum_lat, amp_lat);

    const BiomeAmplification& ocean_amp =
        biome_config.amplification[static_cast<size_t>(BiomeType::Ocean)];
    // With blending disabled every column uses its own biome's knobs (no
    // field interpolation), so a land biome can appear anywhere inside a
    // cell, not only at its corners. The column knobs then range over all
    // land biomes; include those extremes per cell so the range stays a safe
    // bound (slightly looser, never tighter than reality).
    const bool zero_blend = params.climate_blend_radius_nodes <= 0;
    float land_amp_lo = 1e9f, land_amp_hi = -1e9f;
    if (zero_blend) {
        for (int32_t b = 0; b < static_cast<int32_t>(BiomeType::Count); ++b) {
            if (static_cast<BiomeType>(b) == BiomeType::Ocean) continue;
            const float h = biome_config.amplification[static_cast<size_t>(b)].height;
            land_amp_lo = std::min(land_amp_lo, h);
            land_amp_hi = std::max(land_amp_hi, h);
        }
    }
    float min_h = 1e9f;
    float max_h = -1e9f;
    bool any_ocean_node = false;
    for (int32_t i = 0; i < CLIMATE_LATTICE_NODES - 1; ++i) {
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES - 1; ++j) {
            const float r0 = land_lat[i][j],         r1 = land_lat[i + 1][j];
            const float r2 = land_lat[i][j + 1],     r3 = land_lat[i + 1][j + 1];
            const float a0 = amp_lat[i][j].height,   a1 = amp_lat[i + 1][j].height;
            const float a2 = amp_lat[i][j + 1].height, a3 = amp_lat[i + 1][j + 1].height;
            const float r[4] = {r0, r1, r2, r3};
            const float a[4] = {a0, a1, a2, a3};
            if (zero_blend) {
                const float rmin = std::min(std::min(r0, r1), std::min(r2, r3));
                const float rmax = std::max(std::max(r0, r1), std::max(r2, r3));
                const float dlo = rmin - params.sea_level;
                const float dhi = rmax - params.sea_level;
                if (dhi >= 0.0f) {
                    max_h = std::max(max_h, params.sea_level + dhi * land_amp_hi);
                }
                if (dlo >= 0.0f) {
                    min_h = std::min(min_h, params.sea_level + dlo * land_amp_lo);
                }
            }
            for (int ci = 0; ci < 4; ++ci) {
                const float d = r[ci] - params.sea_level;
                // Land columns: interpolated amp lies within the corner amps.
                for (int cj = 0; cj < 4; ++cj) {
                    const float h = params.sea_level + d * a[cj];
                    min_h = std::min(min_h, h);
                    max_h = std::max(max_h, h);
                }
                // Ocean columns (raw below sea level): fixed ocean amp.
                const float ho = params.sea_level + d * ocean_amp.height;
                min_h = std::min(min_h, ho);
                max_h = std::max(max_h, ho);
            }
            if (std::min(std::min(r0, r1), std::min(r2, r3)) < params.sea_level) {
                any_ocean_node = true;
            }
        }
    }
    const float max_water_h = any_ocean_node ? params.sea_level : -1.0f;
    const float margin = density_margin();
    return HeightRange{min_h - margin, max_h + margin, max_water_h};
}

// Real topmost air-to-solid transition for a column, scanning down from above
// the maximum possible density displacement.
int32_t ChunkGenerator::find_surface_y(int32_t world_x, int32_t world_z) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float t = sample_temperature(x, z);
    const float h = sample_humidity(x, z);
    const BiomeAmplification blended = blend_amplification_at(world_x, world_z);
    ColumnSample column = sample_column_with_climate(
        world_x, world_z, t, h, sample_land_shape(x, z, t, h), blended);
    const BiomeAmplification& amp = amplification_for(column.biome, blended);
    const float weirdness = amplified_weirdness(sample_weirdness(x, z), amp);
    const ShapeEnvelope env = shape_envelope(weirdness, amp.weirdness_size);

    // The density surface can only exist within this column's surface band of
    // the macro heightmap (see sample_terrain_density), so scan exactly that
    // band — plus the slack, which keeps a size-0 envelope (band of zero)
    // scannable instead of collapsing to a single sample.
    const float scan_margin = env.band_outer + DENSITY_MARGIN_SLACK;
    const int32_t start_y =
        static_cast<int32_t>(std::ceil(column.height + scan_margin));

    for (int32_t y = start_y;
         y >= static_cast<int32_t>(std::ceil(column.height - scan_margin));
         --y) {
        const float here = sample_terrain_density(world_x, y, world_z, column, weirdness,
                                                  amp.weirdness_size);
        const float above = sample_terrain_density(world_x, y + 1, world_z, column, weirdness,
                                                   amp.weirdness_size);
        if (here > 0.0f && above <= 0.0f) {
            return y;
        }
    }

    return static_cast<int32_t>(column.height);
}

BlockID ChunkGenerator::get_chunk_subsurface_block(int32_t chunk_x, int32_t chunk_z) const {
    int32_t wx = chunk_x * CHUNK_WIDTH;
    int32_t wz = chunk_z * CHUNK_DEPTH;
    // Sample center column for biome
    ColumnSample col = sample_column(wx + CHUNK_WIDTH / 2, wz + CHUNK_DEPTH / 2);
    return get_subsurface_block(col.biome, false);
}

bool ChunkGenerator::generate_fast_path(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    const int32_t world_y_start = chunk_y * CHUNK_HEIGHT;
    const int32_t world_y_end = world_y_start + CHUNK_HEIGHT;

    if (world_y_start >= WORLD_HEIGHT_Y || world_y_end <= 0) {
        chunk.clear();
        chunk.propagate_sky_light(nullptr);
        chunk.compute_fully_solid();
        return true;
    }

    // Fast estimation: skip chunks that are entirely air or entirely solid.
    // Runs against this generator's configured params / biome config — no
    // per-call construction (callers keep a configured instance).
    auto height_range = get_chunk_height_range(chunk_x, chunk_z);
    float margin = 3.0f; // safety margin for intra-chunk height variation
    float top_content_h = std::max(height_range.max_h, height_range.max_water_h);

    // Entirely above surface: all air
    if (world_y_start > static_cast<int32_t>(top_content_h + margin)) {
        chunk.clear();
        chunk.propagate_sky_light(nullptr); // sky light = 15 for all air
        chunk.compute_fully_solid();
        return true;
    }

    // Entirely below surface (and below bedrock): all bedrock
    if (world_y_end <= params.bedrock_height) {
        chunk.fill_blocks(BlockIDs::BEDROCK);
        chunk.propagate_sky_light(nullptr); // first block is opaque → all light = 0
        // Without this flag every underground chunk gets a box mesh — the
        // buried-chunk culling relies on fully_solid() to skip invisible chunks.
        chunk.compute_fully_solid();
        return true;
    }

    // Caves only form inside [bedrock_height+3, sea_level+10]
    // (see ChunkGenerator::is_cave). A chunk overlapping that range is
    // not automatically solid even when it sits below the surface.
    const int32_t cave_min_y = params.bedrock_height + 3;
    const int32_t cave_max_y = static_cast<int32_t>(params.sea_level) + 10;
    const bool may_contain_caves =
        world_y_end > cave_min_y && world_y_start < cave_max_y;

    // Entirely below surface but above bedrock: all solid subsurface block.
    if (!may_contain_caves && world_y_end < static_cast<int32_t>(height_range.min_h - margin)) {
        BlockID solid_block = get_chunk_subsurface_block(chunk_x, chunk_z);
        chunk.fill_blocks(solid_block);
        chunk.propagate_sky_light(nullptr); // first block is opaque → all light = 0
        // Without this flag every underground chunk gets a box mesh.
        chunk.compute_fully_solid();
        return true;
    }

    return false;
}

bool chunk_would_be_fully_solid(const ChunkGenerator& gen, int32_t cx, int32_t cy, int32_t cz) {
    const int32_t world_y_start = cy * CHUNK_HEIGHT;
    const int32_t world_y_end = world_y_start + CHUNK_HEIGHT;
    // Out-of-world chunks are never treated as opaque so boundary faces at
    // the world edges still render.
    if (world_y_start >= WORLD_HEIGHT_Y || world_y_end <= 0) {
        return false;
    }
    // Entirely below the bedrock layer: bedrock, which is opaque.
    if (world_y_end <= gen.get_params().bedrock_height) {
        return true;
    }
    // The chunk is entirely below every column's lowest possible surface
    // (macro height − density margin), so full generation would produce
    // solid blocks throughout (the density band is exactly zero there).
    const ChunkGenerator::HeightRange range = gen.get_chunk_height_range(cx, cz);
    return static_cast<float>(world_y_end) < range.min_h;
}

} // namespace VoxelEngine
