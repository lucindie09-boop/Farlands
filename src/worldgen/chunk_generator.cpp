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
    float erosion = sample_erosion(x, z);
    bool is_land = cont >= params.land_threshold;
    float height = 0.0f;
    float water_level = -1.0f;
    BiomeType biome = BiomeType::Plains;

    // Simplified height - single noise layer only
    float land_height = sample_land_shape(x, z, temperature, humidity);
    float saved_land_height = land_height;

    float cont_from_coast = params.land_threshold - cont;
    float depth = 0.0f;
    // Simplified ocean - flat sea level
    float ocean_height = params.sea_level;

    // Simplified blending - just use land height everywhere
    height = land_height;

    biome = biome_from_climate(temperature, humidity, cont);
    if (!is_land) {
        water_level = params.sea_level;
    }

    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);
    if (water_level >= 0.0f) {
        water_level = std::max(params.sea_level, water_level);
    }

    return ColumnSample{biome, height, water_level, false, saved_land_height, cont, temperature, humidity, erosion};
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

    // Simplified - no 3D density shaping, just use the raw height range
    return HeightRange{min_h, max_h, max_water_h};
}

// Real topmost air-to-solid transition for a column, scanning down from above
// the maximum possible density displacement.
int32_t ChunkGenerator::find_surface_y(int32_t world_x, int32_t world_z) const {
    ColumnSample column = sample_column(world_x, world_z);
    const float weirdness = 0.0f; // Disabled - no weirdness

    const int32_t start_y =
        static_cast<int32_t>(std::ceil(column.height + 1.0f));

    for (int32_t y = start_y;
         y >= static_cast<int32_t>(column.height - 4.0f);
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
            columns[x][z].erosion     = col.erosion;
            columns[x][z].weirdness    = 0.0f; // Disabled - no weirdness
            min_height = std::min(min_height, col.height);
            max_height = std::max(max_height, col.height);
        }
    }

    // ---- Chunk-level fast path ----
    // Simplified - no 3D density surface, just simple height check
    const bool above_terrain = static_cast<float>(world_y_start) >= max_height + 1.0f;
    const bool below_terrain = static_cast<float>(world_y_end) <= min_height - 1.0f;

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

    // ---- Simplified geometry pass - just use heightmap ----
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const int32_t surface_y = col.height;
            columns[x][z].surface_y = surface_y;
        }
    }

    // ---- Simplified near-water detection ----
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkColumn& col = columns[x][z];
            const bool is_water = col.water_level >= 0 && col.surface_y >= 0 && col.surface_y < col.water_level;
            columns[x][z].near_water = is_water;
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

                // Bedrock region takes priority
                if (wy < bed) {
                    dense(x, ly, z) = BlockIDs::AIR;
                    continue;
                }

                // Simplified solid check - just compare to height
                bool solid = wy <= col.height;

                // Caves carve the solid geometry
                if (solid && is_cave(wx, wy, wz)) {
                    solid = false;
                }

                if (!solid) {
                    // Water: fill air voxels between the terrain floor and the
                    // water surface.
                    if (wy > col.height && wy <= water_top) {
                        BlockID water_block = (wy == water_top) ? BlockIDs::SURFACE_WATER : BlockIDs::WATER;
                        dense(x, ly, z) = water_block;
                    } else {
                        dense(x, ly, z) = BlockIDs::AIR;
                    }
                    continue;
                }

                // Material pass: biome topsoil only on upward surfaces that also
                // coincide with the macro surface; everything deeper stays stone.
                const bool is_upward_surface = wy == col.height;
                const bool near_macro_surface = wy >= col.height - 14;

                BlockID block;
                if (is_upward_surface && near_macro_surface) {
                    block = surface_block;
                } else if (near_macro_surface && wy >= col.height - params.subsurface_cover_depth) {
                    block = subsurface_block;
                } else {
                    block = BlockIDs::STONE;
                }
                dense(x, ly, z) = block;
            }
        }
    }

    // Artifact removal disabled - no density field to clean up

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