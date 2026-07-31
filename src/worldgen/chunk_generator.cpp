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
    float temperature = sample_temperature(x, z);
    float humidity = sample_humidity(x, z);
    bool is_land = cont >= params.land_threshold;
    float coast_width = std::max(0.0001f, params.shelf_width * 0.4f);

    float height = 0.0f;
    float water_level = -1.0f;
    BiomeType biome = BiomeType::Plains;
    float saved_land_height = 0.0f;

    if (is_land) {
        float land_height = sample_land_shape(x, z, temperature, humidity);
        float coast_t = smoothstep(params.ocean_threshold, params.ocean_threshold + coast_width, cont);
        land_height = lerp(params.sea_level, land_height, coast_t);
        saved_land_height = land_height;
        height = land_height;

        biome = biome_from_climate(temperature, humidity, cont);
    } else {
        float cont_from_coast = params.land_threshold - cont;
        float depth = cont_from_coast <= 0.05f
            ? lerp(1.0f, params.shelf_depth, cont_from_coast / 0.05f)
            : lerp(params.shelf_depth, params.deep_ocean_depth, (cont_from_coast - 0.05f) / 0.12f);
        depth = std::min(params.deep_ocean_depth, depth);
        height = params.sea_level - depth;

        float bed_noise = terrain_noise.noise_2d(x * 0.002f, z * 0.002f) * 2.0f;
        height += bed_noise;
        height = std::min(height, params.sea_level - 1.0f);
        water_level = params.sea_level;

        biome = biome_from_climate(temperature, humidity, cont);
    }

    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);
    if (water_level >= 0.0f) {
        water_level = std::max(params.sea_level, water_level);
    }

    return ColumnSample{biome, height, water_level, false, saved_land_height, cont, temperature, humidity};
}

BlockID ChunkGenerator::get_surface_block(BiomeType biome, int32_t y, bool has_surface_water, bool near_water) const {
    if (has_surface_water) {
        return BlockIDs::SAND;
    }
    switch (biome) {
        case BiomeType::Ocean:       return BlockIDs::SAND;
        case BiomeType::Beach:        return near_water ? BlockIDs::WET_SAND : BlockIDs::SAND;
        case BiomeType::Desert:      return BlockIDs::SAND;
        default:                     return near_water ? BlockIDs::MUD : BlockIDs::GRASS;
    }
}

BlockID ChunkGenerator::get_subsurface_block(BiomeType biome, bool near_water) const {
    switch (biome) {
        case BiomeType::Ocean:       return BlockIDs::SAND;
        case BiomeType::Beach:        return near_water ? BlockIDs::WET_SAND_FULL : BlockIDs::SAND;
        case BiomeType::Desert:       return BlockIDs::SAND;
        default:                      return BlockIDs::DIRT;
    }
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
        }
    }

    // 2-pass scanline Manhattan distance transform for near_water detection.
    constexpr int32_t INF_DIST = 999;
    int32_t dist[CHUNK_WIDTH][CHUNK_DEPTH];

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            dist[x][z] = (columns[x][z].water_level > columns[x][z].height) ? 0 : INF_DIST;
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

            if (dist[x][z] > 0 && dist[x][z] <= 3) {
                columns[x][z].near_water = true;
            }
        }
    }

    // ---- Geometry pass (2/3): signed density field over the whole chunk ----
    // One extra row of density above the chunk so the top voxel layer can decide
    // air/solid for the voxel above it. Heap-allocated: generation runs on
    // worker threads with limited stacks.
    const int32_t DENSITY_STRIDE_Y = CHUNK_HEIGHT + 1;
    std::vector<float> density_buf(
        static_cast<size_t>(CHUNK_WIDTH) * DENSITY_STRIDE_Y * CHUNK_DEPTH);
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
    std::vector<float> shape_lattice(
        static_cast<size_t>(LATTICE_X) * LATTICE_Y * LATTICE_Z);
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

    // ---- Material pass: solid geometry first, materials second ----
    const int32_t bed = params.bedrock_height;

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t wx = world_x_start + x;
            const int32_t wz = world_z_start + z;
            const int32_t surface_y = col.height;
            const bool has_surface_water = col.water_level > col.height;
            const BlockID surface_block = get_surface_block(col.biome, surface_y, has_surface_water, col.near_water);
            const BlockID subsurface_block = get_subsurface_block(col.biome, col.near_water);
            const int32_t water_top = col.water_level;
            const float macro_height_f = static_cast<float>(col.height);

            // Bedrock occupies world y in [0, bed)
            int32_t bedrock_overlap_start = std::max(0, world_y_start);
            int32_t bedrock_overlap_end   = std::min(bed, world_y_end);
            if (bedrock_overlap_start < bedrock_overlap_end) {
                for (int32_t local_y = bedrock_overlap_start - world_y_start; local_y < bedrock_overlap_end - world_y_start; local_y++) {
                    chunk.set_block(x, local_y, z, BlockIDs::BEDROCK);
                }
            }

            for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                const int32_t wy = world_y_start + ly;

                // Bedrock region takes priority (also keeps cave noise out of it).
                if (wy < bed) continue;

                const float density = dens(x, ly, z);
                bool solid = density > 0.0f;

                // Caves carve the solid geometry (kept separate from the surface
                // density for now).
                if (solid && is_cave(wx, wy, wz)) {
                    solid = false;
                }

                if (!solid) {
                    // Water: preserve the column-based macro behaviour. Only fill
                    // above the original (macro) ground so newly carved cavities
                    // below sea level do not flood; overhangs above ocean water
                    // still produce arches and covered channels.
                    const bool above_original_ground = static_cast<float>(wy) > macro_height_f;
                    if (above_original_ground && wy <= water_top) {
                        BlockID water_block = (wy == water_top) ? BlockIDs::SURFACE_WATER : BlockIDs::WATER;
                        chunk.set_block(x, ly, z, water_block);
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
                chunk.set_block(x, ly, z, block);
            }
        }
    }

    // Remove isolated single-block artifacts. A non-air voxel whose six
    // orthogonal neighbors are all air is a density-noise glitch (a lone
    // floating cube). Only interior blocks are touched — boundary-layer
    // neighbors may live in adjacent, not-yet-generated chunks.
    for (int32_t ly = 1; ly < CHUNK_HEIGHT - 1; ly++) {
        for (int32_t x = 1; x < CHUNK_WIDTH - 1; x++) {
            for (int32_t z = 1; z < CHUNK_DEPTH - 1; z++) {
                if (chunk.get_block(x, ly, z) == BlockIDs::AIR) continue;
                if (chunk.get_block(x + 1, ly, z) != BlockIDs::AIR) continue;
                if (chunk.get_block(x - 1, ly, z) != BlockIDs::AIR) continue;
                if (chunk.get_block(x, ly + 1, z) != BlockIDs::AIR) continue;
                if (chunk.get_block(x, ly - 1, z) != BlockIDs::AIR) continue;
                if (chunk.get_block(x, ly, z + 1) != BlockIDs::AIR) continue;
                if (chunk.get_block(x, ly, z - 1) != BlockIDs::AIR) continue;
                chunk.set_block(x, ly, z, BlockIDs::AIR);
            }
        }
    }

    // Place vegetation
    if (vegetation_enabled) {
        VegetationGenerator veg;
        veg.generate_vegetation(chunk, columns, chunk_x, chunk_z,
                                world_y_start, world_y_end, cross_writer);
    }

    chunk.compute_section_flags();
}

void ChunkGenerator::render_continentalness_pgm(const char* filename, int img_w, int img_h,
                                float world_x_start, float world_z_start,
                                float step) const {
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", img_w, img_h);
    for (int py = 0; py < img_h; py++) {
        for (int px = 0; px < img_w; px++) {
            float wx = world_x_start + static_cast<float>(px) * step;
            float wz = world_z_start + static_cast<float>(py) * step;
            float cont = sample_continentalness(wx, wz);
            uint8_t byte = static_cast<uint8_t>(std::round(cont * 255.0f));
            fwrite(&byte, 1, 1, f);
        }
    }
    fclose(f);
}

void ChunkGenerator::render_biome_pgm(const char* filename, int img_w, int img_h,
                      float world_x_start, float world_z_start,
                      float step) const {
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", img_w, img_h);
    for (int py = 0; py < img_h; py++) {
        for (int px = 0; px < img_w; px++) {
            float wx = world_x_start + static_cast<float>(px) * step;
            float wz = world_z_start + static_cast<float>(py) * step;
            ColumnSample col = sample_column(static_cast<int32_t>(wx),
                                               static_cast<int32_t>(wz));
            uint8_t byte = 0;
            switch (col.biome) {
                case BiomeType::Ocean:         byte = 30;  break;
                case BiomeType::Beach:         byte = 220; break;
                case BiomeType::Plains:        byte = 150; break;
                case BiomeType::Forest:        byte = 100; break;
                case BiomeType::Desert:        byte = 200; break;
                default:                       byte = 128; break;
            }
            fwrite(&byte, 1, 1, f);
        }
    }
    fclose(f);
}

} // namespace VoxelEngine