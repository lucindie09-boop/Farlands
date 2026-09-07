#include "worldgen/chunk_generator.hpp"
#include "core/chunk_data.hpp"
#include "worldgen/vegetation_generator.hpp"
#include <vector>
#include <cmath>
#include <limits>

namespace VoxelEngine {

PerformanceTimer ChunkGenerator::perf_timer;

bool ChunkGenerator::find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                                        int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                                        float& out_height) const {
    // Phase 1: concentric Chebyshev rings at 16-block step, walked from a side
    // midpoint so hits come back roughly nearest-first. Early-exit on the first
    // matching ring.
    constexpr int32_t STEP = 16;
    const int32_t max_cells = std::max(0, max_radius_blocks / STEP);
    bool found = false;
    int32_t cand_x = center_x;
    int32_t cand_z = center_z;

    auto match = [&](int32_t wx, int32_t wz) -> bool {
        if (get_biome(wx, wz) != target) return false;
        found = true;
        cand_x = wx;
        cand_z = wz;
        return true;
    };

    for (int32_t r = 0; r <= max_cells && !found; ++r) {
        if (r == 0) {
            match(center_x, center_z);
            continue;
        }
        for (int32_t i = -r; i <= r && !found; ++i) {
            match(center_x + i * STEP, center_z - r * STEP);
            match(center_x + i * STEP, center_z + r * STEP);
        }
        for (int32_t j = -r + 1; j <= r - 1 && !found; ++j) {
            match(center_x - r * STEP, center_z + j * STEP);
            match(center_x + r * STEP, center_z + j * STEP);
        }
    }
    if (!found) return false;

    // Phase 2: fine 4-block scan of the area around the coarse hit, keeping the
    // closest matching column to the requested center.
    constexpr int32_t FINE = 4;
    constexpr int32_t FINE_SPAN = 16;  // blocks on each side of the coarse hit
    int64_t best_dist2 = std::numeric_limits<int64_t>::max();
    int32_t best_x = cand_x;
    int32_t best_z = cand_z;
    for (int32_t wx = cand_x - FINE_SPAN; wx <= cand_x + FINE_SPAN; wx += FINE) {
        for (int32_t wz = cand_z - FINE_SPAN; wz <= cand_z + FINE_SPAN; wz += FINE) {
            if (get_biome(wx, wz) != target) continue;
            const int64_t dx = static_cast<int64_t>(wx) - center_x;
            const int64_t dz = static_cast<int64_t>(wz) - center_z;
            const int64_t d2 = dx * dx + dz * dz;
            if (d2 < best_dist2) {
                best_dist2 = d2;
                best_x = wx;
                best_z = wz;
            }
        }
    }

    out_x = best_x;
    out_z = best_z;
    out_height = sample_column(best_x, best_z).height;
    return true;
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column(int32_t world_x, int32_t world_z) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float t = sample_temperature(x, z);
    const float h = sample_humidity(x, z);
    return sample_column_with_climate(world_x, world_z, t, h,
                                      sample_land_shape(x, z, t, h),
                                      blend_amplification_at(world_x, world_z));
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column_with_climate(
        int32_t world_x, int32_t world_z, float temperature, float humidity,
        float land_height, const BiomeAmplification& blended) const {
    float x = static_cast<float>(world_x);
    float z = static_cast<float>(world_z);

    float cont = sample_continentalness(x, z);

    // Height comes purely from the noise stack — the full macro surface is
    // evaluated everywhere, with no continentalness gating and no sea-level
    // flattening while the noise runs.
    float saved_land_height = land_height;
    float height = land_height;

    // Oceans are a post-pass over the completed height field, not a terrain
    // input: any column whose surface ended up below sea level simply fills
    // with water up to sea level. The noisy height is kept as the sea bed, so
    // land and ocean floor are one continuous surface (no shelf logic, no
    // continentalness involvement).
    const bool is_land = land_height >= params.sea_level;
    float water_level = -1.0f;
    BiomeType biome;
    if (is_land) {
        biome = biome_from_climate(temperature, humidity, cont);
    } else {
        biome = BiomeType::Ocean;
        water_level = params.sea_level;
    }

    // Height amplification scales the column's displacement around sea level
    // (1.0 = neutral): the blended field on land (ramps across biome borders),
    // the ocean biome's own knob on the seabed. Applied AFTER biome
    // classification so the land/ocean split stays exactly where the raw
    // height put it.
    const BiomeAmplification& amp = amplification_for(biome, blended);
    height = params.sea_level + (height - params.sea_level) * amp.height;

    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);
    if (water_level >= 0.0f) {
        water_level = std::max(params.sea_level, water_level);
    }

    return ColumnSample{biome, height, water_level, false, saved_land_height, cont, temperature, humidity};
}

void ChunkGenerator::build_climate_lattice(int32_t chunk_x, int32_t chunk_z,
                                           float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                           float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t wx0 = chunk_x * CHUNK_WIDTH;
    const int32_t wz0 = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
            const float x = static_cast<float>(wx0 + i * CLIMATE_LATTICE_SPACING);
            const float z = static_cast<float>(wz0 + j * CLIMATE_LATTICE_SPACING);
            temp_lat[i][j] = sample_temperature_raw(x, z);
            hum_lat[i][j] = sample_humidity_raw(x, z);
        }
    }
}

void ChunkGenerator::build_land_shape_lattice(int32_t chunk_x, int32_t chunk_z,
                                              float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t wx0 = chunk_x * CHUNK_WIDTH;
    const int32_t wz0 = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
            const float x = static_cast<float>(wx0 + i * CLIMATE_LATTICE_SPACING);
            const float z = static_cast<float>(wz0 + j * CLIMATE_LATTICE_SPACING);
            land_lat[i][j] = sample_land_shape_raw(x, z);
        }
    }
}

float ChunkGenerator::interp_climate_lattice(const float lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                             int32_t wx, int32_t wz, int32_t wx0, int32_t wz0) {
    const int32_t ix = (wx - wx0) / CLIMATE_LATTICE_SPACING;
    const int32_t iz = (wz - wz0) / CLIMATE_LATTICE_SPACING;
    const float fx = static_cast<float>(wx - wx0 - ix * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float fz = static_cast<float>(wz - wz0 - iz * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float v00 = lat[ix][iz],     v10 = lat[ix + 1][iz];
    const float v01 = lat[ix][iz + 1], v11 = lat[ix + 1][iz + 1];
    return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
}

void ChunkGenerator::build_amp_lattice(int32_t chunk_x, int32_t chunk_z,
                                       const float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                       const float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                       BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t world_x_start = chunk_x * CHUNK_WIDTH;
    const int32_t world_z_start = chunk_z * CHUNK_DEPTH;

    // Blend windows extend up to climate_blend_radius_nodes nodes beyond the
    // chunk; biome values inside come from the cached climate lattice, just
    // outside from the raw samplers (bit-identical node values).
    const int32_t R = std::min(std::max(params.climate_blend_radius_nodes, 0),
                               CLIMATE_BLEND_MAX_RADIUS);
    const int32_t EXT = CLIMATE_LATTICE_NODES + 2 * R;
    BiomeType bio_grid[2 * CLIMATE_BLEND_MAX_RADIUS + CLIMATE_LATTICE_NODES]
                      [2 * CLIMATE_BLEND_MAX_RADIUS + CLIMATE_LATTICE_NODES];
    for (int32_t j = 0; j < EXT; ++j) {
        for (int32_t i = 0; i < EXT; ++i) {
            const int32_t nxi = i - R;   // node index, 0..8 inside the chunk
            const int32_t nzj = j - R;
            const int32_t wx = world_x_start + nxi * CLIMATE_LATTICE_SPACING;
            const int32_t wz = world_z_start + nzj * CLIMATE_LATTICE_SPACING;
            if (nxi >= 0 && nxi < CLIMATE_LATTICE_NODES &&
                nzj >= 0 && nzj < CLIMATE_LATTICE_NODES) {
                bio_grid[i][j] = biome_from_climate(
                    temp_lat[nxi][nzj], hum_lat[nxi][nzj],
                    sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
            } else {
                bio_grid[i][j] = biome_from_climate(
                    sample_temperature_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    sample_humidity_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
            }
        }
    }

    for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
        for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
            amp_lat[i][j] = blend_amplitudes([&](int32_t di, int32_t dj) {
                return bio_grid[i + R + di][j + R + dj];
            });
        }
    }
}

BiomeAmplification ChunkGenerator::interp_amp_lattice(
        const BiomeAmplification lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
        int32_t wx, int32_t wz, int32_t wx0, int32_t wz0) {
    const int32_t ix = (wx - wx0) / CLIMATE_LATTICE_SPACING;
    const int32_t iz = (wz - wz0) / CLIMATE_LATTICE_SPACING;
    const float fx = static_cast<float>(wx - wx0 - ix * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float fz = static_cast<float>(wz - wz0 - iz * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const BiomeAmplification& v00 = lat[ix][iz];
    const BiomeAmplification& v10 = lat[ix + 1][iz];
    const BiomeAmplification& v01 = lat[ix][iz + 1];
    const BiomeAmplification& v11 = lat[ix + 1][iz + 1];
    BiomeAmplification out;
    out.height = lerp(lerp(v00.height, v10.height, fx), lerp(v01.height, v11.height, fx), fz);
    out.weirdness = lerp(lerp(v00.weirdness, v10.weirdness, fx),
                         lerp(v01.weirdness, v11.weirdness, fx), fz);
    out.min_weirdness = lerp(lerp(v00.min_weirdness, v10.min_weirdness, fx),
                             lerp(v01.min_weirdness, v11.min_weirdness, fx), fz);
    return out;
}

BlockID ChunkGenerator::get_surface_block(BiomeType biome, int32_t y, bool has_surface_water, bool near_water) const {
    if (has_surface_water) {
        return biome_config.underwater_surface;
    }
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_surface : s.surface;
}

BlockID ChunkGenerator::get_subsurface_block(BiomeType biome, bool near_water) const {
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_subsurface : s.subsurface;
}

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
    // shaping can still push the real surface up to DENSITY_MARGIN above or
    // below the macro heightmap, so the range is padded by that.
    float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_land_shape_lattice(chunk_x, chunk_z, land_lat);
    build_climate_lattice(chunk_x, chunk_z, temp_lat, hum_lat);
    BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_amp_lattice(chunk_x, chunk_z, temp_lat, hum_lat, amp_lat);

    const BiomeAmplification& ocean_amp =
        biome_config.amplification[static_cast<size_t>(BiomeType::Ocean)];
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
    return HeightRange{min_h - DENSITY_MARGIN, max_h + DENSITY_MARGIN, max_water_h};
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

    // The density surface can only exist within DENSITY_MARGIN of the macro
    // heightmap (see sample_terrain_density), so scan exactly that band.
    const int32_t start_y =
        static_cast<int32_t>(std::ceil(column.height + DENSITY_MARGIN));

    for (int32_t y = start_y;
         y >= static_cast<int32_t>(std::ceil(column.height - DENSITY_MARGIN));
         --y) {
        const float here = sample_terrain_density(world_x, y, world_z, column, weirdness);
        const float above = sample_terrain_density(world_x, y + 1, world_z, column, weirdness);
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

void ChunkGenerator::generate_chunk(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                                    const CrossChunkWriter& cross_writer, bool vegetation_enabled) {
    ScopedTimer timer(perf_timer, TimerID::GenerateChunk);
    chunk.clear();

    int32_t world_x_start = chunk_x * CHUNK_WIDTH;
    int32_t world_y_start = chunk_y * CHUNK_HEIGHT;
    int32_t world_z_start = chunk_z * CHUNK_DEPTH;
    int32_t world_y_end = world_y_start + CHUNK_HEIGHT;

    // Single struct-of-arrays for all per-column data (replaces 7 separate stack arrays).
    ChunkColumn columns[CHUNK_WIDTH][CHUNK_DEPTH];

    // ---- Geometry pass (1/3): macro columns + cached weirdness mask ----
    // The climate fields are evaluated once on the chunk's 4-block lattice
    // (~160 raw evaluations) and every column interpolates from it instead of
    // running the per-call samplers (bit-identical values at shared nodes).
    float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_climate_lattice(chunk_x, chunk_z, temp_lat, hum_lat);

    // Amplification blend lattice: effective per-node knobs after the
    // border blend (see blend_amplitudes). Cheap — the biome windows reuse
    // the cached climate lattice plus raw samples only just outside the chunk.
    BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_amp_lattice(chunk_x, chunk_z, temp_lat, hum_lat, amp_lat);

    // Land-shape lattice: the macro height is evaluated once per 4-block
    // node (81 evaluations) instead of 4 per column (~4096), mirroring the
    // climate lattice. Bit-identical to the per-call sampler.
    float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
    build_land_shape_lattice(chunk_x, chunk_z, land_lat);

    float min_height = 1e9f;
    float max_height = -1e9f;
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            int32_t wx = world_x_start + x;
            int32_t wz = world_z_start + z;
            const BiomeAmplification blended =
                interp_amp_lattice(amp_lat, wx, wz, world_x_start, world_z_start);
            ColumnSample col = sample_column_with_climate(
                wx, wz,
                interp_climate_lattice(temp_lat, wx, wz, world_x_start, world_z_start),
                interp_climate_lattice(hum_lat, wx, wz, world_x_start, world_z_start),
                interp_climate_lattice(land_lat, wx, wz, world_x_start, world_z_start),
                blended);
            columns[x][z].sample       = col;
            columns[x][z].height       = static_cast<int32_t>(std::round(col.height));
            columns[x][z].biome        = col.biome;
            columns[x][z].water_level  = col.water_level >= 0.0f
                ? static_cast<int32_t>(std::round(col.water_level))
                : -1;
            columns[x][z].temperature  = col.temperature;
            columns[x][z].humidity     = col.humidity;
            columns[x][z].weirdness    = amplified_weirdness(
                sample_weirdness(static_cast<float>(wx), static_cast<float>(wz)),
                amplification_for(col.biome, blended));
            min_height = std::min(min_height, col.height);
            max_height = std::max(max_height, col.height);
        }
    }

    // ---- Chunk-level fast path ----
    // The 3D density surface can only exist within DENSITY_MARGIN of the macro
    // heightmap (displacement = shape * strength * surface_band, and the band
    // is exactly zero beyond SURFACE_BAND_OUTER < DENSITY_MARGIN regardless of
    // strength). Chunks entirely outside that band need no lattice, density
    // buffer, or material pass.
    const bool above_terrain = static_cast<float>(world_y_start) >= max_height + DENSITY_MARGIN;
    const bool below_terrain = static_cast<float>(world_y_end) <= min_height - DENSITY_MARGIN;

    if (above_terrain) {
        // No solids possible. Shallow-ocean chunks still carry the sea surface
        // (water tops out at sea_level, well above the sea floor), so fill
        // per-column water; land-only chunks end up all-air.
        // Optimized: use dense buffer to avoid set_block overhead
        thread_local std::vector<BlockID> above_terrain_buffer;
        above_terrain_buffer.resize(static_cast<size_t>(CHUNK_WIDTH) * CHUNK_HEIGHT * CHUNK_DEPTH);
        std::fill(above_terrain_buffer.begin(), above_terrain_buffer.end(), BlockIDs::AIR);
        
        for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
            for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
                const int32_t water_top = columns[x][z].water_level;
                if (water_top < 0 || water_top < world_y_start) continue;
                const int32_t end = std::min(world_y_end - 1, water_top);
                for (int32_t wy = world_y_start; wy <= end; wy++) {
                    int32_t ly = wy - world_y_start;
                    above_terrain_buffer[static_cast<size_t>(x) + static_cast<size_t>(ly) * CHUNK_WIDTH + static_cast<size_t>(z) * CHUNK_WIDTH * CHUNK_HEIGHT] =
                        (wy == water_top) ? BlockIDs::SURFACE_WATER : BlockIDs::WATER;
                }
            }
        }
        chunk.set_data(above_terrain_buffer.data(), CHUNK_VOLUME);
        return;
    }

    if (below_terrain && !kCavesEnabled) {
        // All density is solid: plain stone over the bedrock base. Skipping the
        // material pass must NOT be done while cave carving is enabled — deep
        // chunks lie inside the cave band.
        // Optimized: use O(1) fill instead of 32K individual set_block calls
        chunk.fill_blocks(BlockIDs::STONE);
        
        const int32_t bed = params.bedrock_height;
        // Only overwrite the bedrock layer with individual calls
        int32_t bedrock_overlap_start = std::max(0, world_y_start);
        int32_t bedrock_overlap_end   = std::min(bed, world_y_end);
        if (bedrock_overlap_start < bedrock_overlap_end) {
            for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
                for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
                    for (int32_t ly = bedrock_overlap_start - world_y_start;
                         ly < bedrock_overlap_end - world_y_start; ly++) {
                        chunk.set_block(x, ly, z, BlockIDs::BEDROCK);
                    }
                }
            }
        }
        return;
    }

    // ---- Geometry pass (2/3): signed density field over the whole chunk ----
    // One extra row of density above the chunk so the top voxel layer can decide
    // air/solid for the voxel above it. Buffers are thread-local so generation
    // workers reuse them across chunks instead of reallocating every call;
    // every cell is fully rewritten each call, so no clearing is needed.
    const int32_t DENSITY_STRIDE_Y = CHUNK_HEIGHT + 1;
    thread_local std::vector<float> density_buf;
    density_buf.resize(static_cast<size_t>(CHUNK_WIDTH) * DENSITY_STRIDE_Y * CHUNK_DEPTH);
    auto dens = [&](int32_t x, int32_t ly, int32_t z) -> float& {
        return density_buf[(static_cast<size_t>(x) * DENSITY_STRIDE_Y + ly) * CHUNK_DEPTH + z];
    };

    // ---- Shape noise lattice ----
    // The 3D shape field is sampled once per world-aligned 4x4x4 lattice node
    // (CHUNK_WIDTH is a multiple of SPACING, so nodes land on identical world
    // coordinates across chunk boundaries) and trilinearly interpolated per
    // voxel below. This is bit-identical to sample_shape_3d_interp(), keeping
    // the chunk grid and any single-point density query in exact agreement.
    constexpr int32_t SPACING = SHAPE_LATTICE_SPACING;
    constexpr int32_t LATTICE_X = CHUNK_WIDTH / SPACING + 1;
    constexpr int32_t LATTICE_Y = DENSITY_STRIDE_Y / SPACING + 1;
    constexpr int32_t LATTICE_Z = CHUNK_DEPTH / SPACING + 1;
    thread_local std::vector<float> shape_lattice;
    shape_lattice.resize(static_cast<size_t>(LATTICE_X) * LATTICE_Y * LATTICE_Z);
    auto lat = [&](int32_t gx, int32_t gy, int32_t gz) -> float& {
        return shape_lattice[(static_cast<size_t>(gx) * LATTICE_Y + gy) * LATTICE_Z + gz];
    };

    for (int32_t gx = 0; gx < LATTICE_X; gx++) {
        for (int32_t gy = 0; gy < LATTICE_Y; gy++) {
            for (int32_t gz = 0; gz < LATTICE_Z; gz++) {
                lat(gx, gy, gz) = sample_shape_3d(
                    static_cast<float>(world_x_start + gx * SPACING),
                    static_cast<float>(world_y_start + gy * SPACING),
                    static_cast<float>(world_z_start + gz * SPACING));
            }
        }
    }

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const float shape_strength =
                lerp(params.shape_strength_min, params.shape_strength_max, col.weirdness);

            // Local lattice cell for this voxel. world_x_start is a multiple of
            // SPACING, so the fractional coordinates derived from local (x, ly,
            // z) are exactly the world ones used by sample_shape_3d_interp().
            const int32_t ix = x / SPACING;
            const int32_t iz = z / SPACING;
            const float fx = static_cast<float>(x - ix * SPACING) / static_cast<float>(SPACING);
            const float fz = static_cast<float>(z - iz * SPACING) / static_cast<float>(SPACING);

            for (int32_t ly = 0; ly < DENSITY_STRIDE_Y; ly++) {
                // Clamp the top cell so the extra density row (ly == CHUNK_HEIGHT)
                // interpolates exactly onto the top lattice node (fy == 1.0).
                const int32_t iy = std::min(ly / SPACING, LATTICE_Y - 2);
                const float fy = static_cast<float>(ly - iy * SPACING) / static_cast<float>(SPACING);

                const float shape = trilinear_interp(
                    lat(ix,     iy,     iz),     lat(ix + 1, iy,     iz),
                    lat(ix,     iy + 1, iz),     lat(ix + 1, iy + 1, iz),
                    lat(ix,     iy,     iz + 1), lat(ix + 1, iy,     iz + 1),
                    lat(ix,     iy + 1, iz + 1), lat(ix + 1, iy + 1, iz + 1),
                    fx, fy, fz);

                dens(x, ly, z) = density_from_shape(
                    col.sample.height - static_cast<float>(world_y_start + ly),
                    shape_strength, shape);
            }
        }
    }

    // ---- Geometry pass (3/3): locate the topmost density surface per column ----
    // Vegetation and the material pass need the *actual* (density) surface, not
    // the macro heightmap value. Columns whose surface lies outside this chunk
    // get -1 and are skipped by vegetation.
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            int32_t surface = -1;
            for (int32_t ly = CHUNK_HEIGHT - 1; ly >= 0; ly--) {
                if (dens(x, ly, z) > 0.0f && dens(x, ly + 1, z) <= 0.0f) {
                    surface = world_y_start + ly;
                    break;
                }
            }
            columns[x][z].surface_y = surface;
        }
    }

    // ---- Near-water detection (post-density) ----
    // 2-pass scanline Manhattan distance transform.  Seeded from the actual
    // density surface_y so that columns whose 3D noise pushed terrain below
    // sea level are treated as water, and columns near *actual* water get
    // near_water = true (wet sand, etc.).  The old pre-density version used
    // macro height and missed shoreline detail.
    constexpr int32_t INF_DIST = 999;
    int32_t dist[CHUNK_WIDTH][CHUNK_DEPTH];

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t wt = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < params.sea_level
                   ? params.sea_level : -1);
            const bool is_water = wt >= 0 && col.surface_y >= 0 && col.surface_y < wt;
            dist[x][z] = is_water ? 0 : INF_DIST;
        }
    }

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            if (dist[x][z] == 0) continue;
            int32_t best = dist[x][z];
            if (x > 0)             best = std::min(best, dist[x-1][z] + 1);
            if (z > 0)             best = std::min(best, dist[x][z-1] + 1);
            dist[x][z] = best;
        }
    }
    for (int32_t x = CHUNK_WIDTH - 1; x >= 0; x--) {
        for (int32_t z = CHUNK_DEPTH - 1; z >= 0; z--) {
            if (dist[x][z] == 0) continue;
            int32_t best = dist[x][z];
            if (x < CHUNK_WIDTH - 1)  best = std::min(best, dist[x+1][z] + 1);
            if (z < CHUNK_DEPTH - 1)  best = std::min(best, dist[x][z+1] + 1);
            dist[x][z] = best;

            if (dist[x][z] > 0 && dist[x][z] <= 1) {
                columns[x][z].near_water = true;
            }
        }
    }

    // ---- Material pass: solid geometry first, materials second ----
    // Optimized: Build into a dense buffer first, then use build_from_dense()
    // to avoid palette upgrade thrashing from thousands of set_block() calls.
    thread_local std::vector<BlockID> dense_buffer;
    dense_buffer.resize(static_cast<size_t>(CHUNK_WIDTH) * CHUNK_HEIGHT * CHUNK_DEPTH);
    std::fill(dense_buffer.begin(), dense_buffer.end(), BlockIDs::AIR);
    auto dense = [&](int32_t x, int32_t y, int32_t z) -> BlockID& {
        return dense_buffer[static_cast<size_t>(x) + static_cast<size_t>(y) * CHUNK_WIDTH + static_cast<size_t>(z) * CHUNK_WIDTH * CHUNK_HEIGHT];
    };

    const int32_t bed = params.bedrock_height;

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t wx = world_x_start + x;
            const int32_t wz = world_z_start + z;
            const int32_t water_top = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < params.sea_level
                   ? params.sea_level : -1);
            const bool has_surface_water = col.water_level >= 0;
            const BlockID surface_block = get_surface_block(col.biome, col.height, has_surface_water, col.near_water);
            const BlockID subsurface_block = get_subsurface_block(col.biome, col.near_water);
            const float macro_height_f = static_cast<float>(col.height);

            // Bedrock occupies world y in [0, bed)
            int32_t bedrock_overlap_start = std::max(0, world_y_start);
            int32_t bedrock_overlap_end   = std::min(bed, world_y_end);
            if (bedrock_overlap_start < bedrock_overlap_end) {
                for (int32_t local_y = bedrock_overlap_start - world_y_start; local_y < bedrock_overlap_end - world_y_start; local_y++) {
                    dense(x, local_y, z) = BlockIDs::BEDROCK;
                }
            }

            for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                const int32_t wy = world_y_start + ly;

                // Bedrock region takes priority (also keeps cave noise out of it).
                if (wy < bed) {
                    dense(x, ly, z) = BlockIDs::AIR;
                    continue;
                }

                const float density = dens(x, ly, z);
                bool solid = density > 0.0f;

                // Caves carve the solid geometry (kept separate from the surface
                // density for now).
                if (solid && is_cave(wx, wy, wz)) {
                    solid = false;
                }

                if (!solid) {
                    // Water: fill air voxels between the terrain floor and the
                    // water surface.  Use whichever is lower — the macro
                    // heightmap or the actual density surface — so that 3D shape
                    // deformation doesn't leave dry gaps whether it pushes
                    // terrain down (surface_y < macro) or up (macro < surface_y
                    // peaks).  Caves below the reference stay dry.
                    const int32_t macro_h = static_cast<int32_t>(macro_height_f);
                    const int32_t ref_surface = col.surface_y >= 0
                        ? std::min(col.surface_y, macro_h)
                        : macro_h;
                    if (wy > ref_surface && wy <= water_top) {
                        BlockID water_block = (wy == water_top) ? BlockIDs::SURFACE_WATER : BlockIDs::WATER;
                        dense(x, ly, z) = water_block;
                    } else {
                        dense(x, ly, z) = BlockIDs::AIR;
                    }
                    continue;
                }

                // Material pass: biome topsoil only on upward surfaces that also
                // coincide with the macro surface; everything deeper stays stone.
                const float above_density = dens(x, ly + 1, z);
                const bool is_upward_surface = above_density <= 0.0f;
                const bool near_macro_surface = static_cast<float>(wy) >= macro_height_f - 14.0f;

                BlockID block;
                if (is_upward_surface && near_macro_surface) {
                    block = surface_block;
                } else if (near_macro_surface &&
                           static_cast<float>(wy) >= macro_height_f - static_cast<float>(params.subsurface_cover_depth)) {
                    block = subsurface_block;
                } else {
                    block = BlockIDs::STONE;
                }
                dense(x, ly, z) = block;
            }
        }
    }

    // Remove isolated single-block artifacts. A non-air voxel whose six
    // orthogonal neighbors are all air is a density-noise glitch (a lone
    // floating cube). Only interior blocks are touched — boundary-layer
    // neighbors may live in adjacent, not-yet-generated chunks.
    // Optimized: work directly on the dense buffer using dens() for detection.
    // Positive density = solid, negative density = air.
    for (int32_t ly = 1; ly < CHUNK_HEIGHT - 1; ly++) {
        for (int32_t x = 1; x < CHUNK_WIDTH - 1; x++) {
            for (int32_t z = 1; z < CHUNK_DEPTH - 1; z++) {
                if (dens(x, ly, z) <= 0.0f) continue;  // Skip air voxels
                if (dens(x + 1, ly, z) > 0.0f) continue;
                if (dens(x - 1, ly, z) > 0.0f) continue;
                if (dens(x, ly + 1, z) > 0.0f) continue;
                if (dens(x, ly - 1, z) > 0.0f) continue;
                if (dens(x, ly, z + 1) > 0.0f) continue;
                if (dens(x, ly, z - 1) > 0.0f) continue;
                dense(x, ly, z) = BlockIDs::AIR;
            }
        }
    }

    // Replace thin solid noise artifacts within water columns.  Near the macro
    // surface the 3D shape noise (density_from_shape) can flip a single voxel
    // to positive density while both Y-neighbours stay negative, creating a
    // 1-block-thick solid sheet inside what should be water.  Any solid voxel
    // with non-solid density both above AND below, and whose world Y is below
    // the column's water surface, is replaced with water.
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t wt = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < params.sea_level
                   ? params.sea_level : -1);
            if (wt < 0) continue;
            for (int32_t ly = 1; ly < CHUNK_HEIGHT - 1; ly++) {
                const int32_t wy = world_y_start + ly;
                if (wy >= wt) break;
                if (dens(x, ly, z) <= 0.0f) continue;
                if (dens(x, ly + 1, z) > 0.0f) continue;
                if (dens(x, ly - 1, z) > 0.0f) continue;
                dense(x, ly, z) = BlockIDs::WATER;
            }
        }
    }

    // Flood-fill water gaps below the water surface.  After the thin-layer
    // cleanup above, some AIR voxels may remain below water_top — e.g. when
    // a noise spike raised surface_y during the material pass, leaving AIR
    // voxels at the chunk bottom that the water fill missed.  Scan each water
    // column from water_top downward: any AIR becomes WATER until we hit solid
    // terrain.  Enclosed caves (surrounded by solid above) stay dry because the
    // scan stops at the first solid block.
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t wt = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < params.sea_level
                   ? params.sea_level : -1);
            if (wt < 0) continue;
            for (int32_t ly = CHUNK_HEIGHT - 1; ly >= 0; ly--) {
                const int32_t wy = world_y_start + ly;
                if (wy >= wt) continue;
                BlockID& bid = dense(x, ly, z);
                if (bid == BlockIDs::AIR) {
                    bid = BlockIDs::WATER;
                } else if (bid != BlockIDs::WATER && bid != BlockIDs::SURFACE_WATER) {
                    break;
                }
            }
        }
    }

    // Bulk build from dense buffer - avoids palette upgrade thrashing
    chunk.set_data(dense_buffer.data(), CHUNK_VOLUME);

    // Place vegetation
    if (vegetation_enabled) {
        VegetationGenerator veg;
        veg.generate_vegetation(chunk, columns, chunk_x, chunk_z,
                                world_y_start, world_y_end,
                                biome_config, vegetation_config, cross_writer);
    }

    // NOTE: compute_section_flags() removed - section_block_count is already correct
    // from set_block calls during generation. No need to rescan all sections.
}

} // namespace VoxelEngine