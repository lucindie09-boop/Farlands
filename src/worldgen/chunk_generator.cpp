#include "worldgen/chunk_generator.hpp"
#include "core/chunk_data.hpp"
#include "worldgen/vegetation_generator.hpp"
#include <vector>

namespace VoxelEngine {

PerformanceTimer ChunkGenerator::perf_timer;

ChunkGenerator::ColumnSample ChunkGenerator::sample_column(int32_t world_x, int32_t world_z) const {
    float x = static_cast<float>(world_x);
    float z = static_cast<float>(world_z);

    float cont = sample_continentalness(x, z);
    // Warp continentalness with a higher-frequency noise to make the
    // shoreline wavy (bays, inlets, peninsulas) instead of straight.
    float cont_warp = terrain_noise.noise_2d(x * 0.003f + 10000.0f, z * 0.003f + 10000.0f) * 0.04f;
    cont = clamp01(cont + cont_warp);
    float temperature = sample_temperature(x, z);
    float humidity = sample_humidity(x, z);
    bool is_land = cont >= params.land_threshold;
    float height = 0.0f;
    float water_level = -1.0f;
    BiomeType biome = BiomeType::Plains;

    // Unified height: compute both ocean and land heights for every column,
    // then blend them through a single smoothstep.  No hard is_land split —
    // the height transitions continuously across the coastline.
    float land_height = sample_land_shape(x, z, temperature, humidity);
    float saved_land_height = land_height;

    float cont_from_coast = params.land_threshold - cont;
    float depth = 0.0f;
    if (cont_from_coast > 0.0f) {
        depth = cont_from_coast <= 0.05f
            ? lerp(0.0f, params.shelf_depth, cont_from_coast / 0.05f)
            : lerp(params.shelf_depth, params.deep_ocean_depth, (cont_from_coast - 0.05f) / 0.12f);
        depth = std::min(params.deep_ocean_depth, depth);
    }
    float bed_noise = terrain_noise.noise_2d(x * 0.002f, z * 0.002f) * 2.0f;
    float ocean_height = params.sea_level - depth + bed_noise;

    float coast_width = std::max(0.0001f, params.shelf_width * 0.4f);
    if (cont >= params.land_threshold) {
        float coast_t = smoothstep(params.land_threshold, params.land_threshold + coast_width, cont);
        height = lerp(params.sea_level, land_height, coast_t);
    } else {
        float coast_t = smoothstep(params.land_threshold, params.land_threshold - coast_width, cont);
        height = lerp(params.sea_level, ocean_height, coast_t);
    }

    biome = biome_from_climate(temperature, humidity, cont);
    if (!is_land) {
        water_level = params.sea_level;
    }

    // Sub-block surface jitter: break the uniform staircase. A gentle macro
    // slope rounds to perfectly regular 1-up steps; adding a small high-freq
    // vertical offset (before the final clamp/round) makes adjacent columns
    // land on irregular steps (1 up, flat, 2 up) instead of a machine-made
    // ramp. Land-only — water columns are placed from water_level, which is
    // left untouched, so the sea surface stays flat.
    // Gated to coastal areas only (continentalness near land_threshold) to
    // avoid unwanted blobby patterns inland where terrain is naturally rugged.
    if (water_level < 0.0f && params.surface_jitter_amplitude > 0.0f) {
        const float coast_distance = std::abs(cont - params.land_threshold);
        const float coast_range = 0.025f; // Apply jitter only very close to coastline
        if (coast_distance < coast_range) {
            // Fade jitter as we move inland
            const float fade = 1.0f - (coast_distance / coast_range);
            float jit = terrain_noise.noise_2d(x * params.surface_jitter_scale + 9000.0f,
                                               z * params.surface_jitter_scale + 9000.0f);
            height += jit * params.surface_jitter_amplitude * fade;
        }
    }

    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);
    if (water_level >= 0.0f) {
        water_level = std::max(params.sea_level, water_level);
    }

    return ColumnSample{biome, height, water_level, false, saved_land_height, cont, temperature, humidity};
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
    int32_t wx_start = chunk_x * CHUNK_WIDTH;
    int32_t wz_start = chunk_z * CHUNK_DEPTH;
    float min_h = 10000.0f;
    float max_h = -10000.0f;
    float max_water_h = -1.0f;
    // Sample corners and center for a good estimate
    for (int32_t x : {0, CHUNK_WIDTH - 1}) {
        for (int32_t z : {0, CHUNK_DEPTH - 1}) {
            ColumnSample col = sample_column(wx_start + x, wz_start + z);
            min_h = std::min(min_h, col.height);
            max_h = std::max(max_h, col.height);
            if (col.water_level > max_water_h) max_water_h = col.water_level;
        }
    }
    // Center sample
    ColumnSample center = sample_column(wx_start + CHUNK_WIDTH / 2, wz_start + CHUNK_DEPTH / 2);
    min_h = std::min(min_h, center.height);
    max_h = std::max(max_h, center.height);
    if (center.water_level > max_water_h) max_water_h = center.water_level;

    // 3D density shaping can push the real surface up to DENSITY_MARGIN above
    // or below the macro heightmap, so pad the range conservatively.
    return HeightRange{min_h - DENSITY_MARGIN, max_h + DENSITY_MARGIN, max_water_h};
}

// Real topmost air-to-solid transition for a column, scanning down from above
// the maximum possible density displacement.
int32_t ChunkGenerator::find_surface_y(int32_t world_x, int32_t world_z) const {
    ColumnSample column = sample_column(world_x, world_z);
    const float weirdness = sample_weirdness(
        static_cast<float>(world_x), static_cast<float>(world_z));

    const int32_t start_y =
        static_cast<int32_t>(std::ceil(column.height + 14.0f));

    for (int32_t y = start_y;
         y >= static_cast<int32_t>(column.height - 32.0f);
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
    float min_height = 1e9f;
    float max_height = -1e9f;
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            int32_t wx = world_x_start + x;
            int32_t wz = world_z_start + z;
            ColumnSample col = sample_column(wx, wz);
            columns[x][z].sample       = col;
            columns[x][z].height       = static_cast<int32_t>(std::round(col.height));
            columns[x][z].biome        = col.biome;
            columns[x][z].water_level  = col.water_level >= 0.0f
                ? static_cast<int32_t>(std::round(col.water_level))
                : -1;
            columns[x][z].temperature  = col.temperature;
            columns[x][z].humidity     = col.humidity;
            columns[x][z].weirdness    = sample_weirdness(static_cast<float>(wx), static_cast<float>(wz));
            min_height = std::min(min_height, col.height);
            max_height = std::max(max_height, col.height);
        }
    }

    // ---- Chunk-level fast path ----
    // The 3D density surface can only exist within DENSITY_MARGIN of the macro
    // heightmap (max displacement is SHAPE_STRENGTH_MAX < DENSITY_MARGIN).
    // Chunks entirely outside that band need no lattice, density buffer, or
    // material pass.
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
    // workers reuse them across chunks instead of malloc/zeroing ~4MB every call;
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
                lerp(SHAPE_STRENGTH_MIN, SHAPE_STRENGTH_MAX, col.weirdness);

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