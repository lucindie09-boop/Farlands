// The worker half of the seed-grid far mode: one tile's surface, sampled from the
// generator on a thread-pool task and handed back to the main thread to upload.
//
// This is the file that decides what the mode costs, so the shape of it is the
// cost model. `find_surface_y` and `sample_column_debug` are the generator's own
// public answers, and between them a column costs about a millisecond
// (tools/lod_sample_bench.cpp measures the split); the price of a tile is
// therefore its node count times that, which is why the spacing doubles with
// distance and why the innermost ring is the only expensive one.
//
// What is NOT here, deliberately: any attempt to march the density field
// separately. `lod::march_surface` is exact and cheaper per block, but the public
// density query re-derives the column's blended biome amplification on every call
// (~0.53 ms of a 0.58 ms column), so eight of those per column costs six times
// what one rigorous search does. The march is kept, tested and unused until that
// per-call cost has an answer that is measured rather than assumed.
#include "core/chunk_coords.hpp"
#include "lod/lod_grid.hpp"
#include "lod/lod_surface.hpp"
#include "worldgen/chunk_generator.hpp"

#include <chrono>
#include <vector>

namespace VoxelEngine {

using namespace godot;

namespace {

struct ColumnContext {
    const uint8_t* biome_layers = nullptr;
    uint8_t underwater_layer = 0;
    const ChunkGenerator* generator = nullptr;
    int32_t* counter = nullptr;
};

lod::SurfaceSample sample_column(const ColumnContext& ctx, int32_t x, int32_t z) {
    lod::SurfaceSample out;
    if (ctx.counter) ++(*ctx.counter);
    const ChunkGenerator::ColumnSample column = ctx.generator->sample_column_debug(x, z);
    const int32_t surface = ctx.generator->find_surface_y(x, z);
    if (surface <= 0 || surface >= WORLD_HEIGHT_Y) {
        // Outside the world's vertical extent: a hole, never invented terrain.
        return out;
    }
    out.valid = true;
    out.height = static_cast<float>(surface);
    const bool flooded = column.water_level > 0.0f;
    if (flooded) {
        out.water = column.water_level;
        out.layer = ctx.underwater_layer;
    } else {
        out.water = lod::kNoWater;
        const size_t biome = static_cast<size_t>(column.biome);
        out.layer = ctx.biome_layers[biome];
    }
    return out;
}

} // namespace

// Runs on a pool thread. It reads only its own copies of the configuration (the
// params, the biome config and the layer table were copied in by the caller), so
// there is nothing to lock and nothing to race with a world edit: this task never
// touches chunk data at all, which is the point of the mode.
void LodGrid::build_tile(const std::shared_ptr<CompletedTile>& result) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    ChunkGenerator generator(terrain_params);
    generator.set_biome_config(biome_config);

    ColumnContext ctx;
    ctx.biome_layers = biome_layers;
    // `underwater_layer`, NOT `water_layer`: the flooded columns this names are
    // TERRAIN (a sea floor shallow enough that its floor quad is kept), and the
    // water block's layer is the LIQUID quad's own -- the one build_tile_mesh is
    // handed below. Handing them the same byte painted every shallow sea floor and
    // every shelf at a coastline with the water texture, which is the blue the far
    // field's shallows wore.
    ctx.underwater_layer = underwater_layer;
    ctx.generator = &generator;
    int32_t sampled = 0;
    ctx.counter = &sampled;

    const int32_t origin_x = result->tx * kTileBlocks;
    const int32_t origin_z = result->tz * kTileBlocks;
    // One column per world column, shared with every tile that wants it. A tile of
    // the outer level is ONE cell, so three of its four corners are repeats of a
    // neighbour's, and the occlusion ring below asks for four more nodes per node --
    // all of them its neighbours' own. Without the table a 27 km reach samples
    // 185,017 columns where 46,656 exist, and `sampled` counts the misses, so the
    // stats keep reporting the sampler's own work rather than everybody's asks.
    lod::SurfaceSampler raw = [&ctx](int32_t x, int32_t z) {
        return sample_column(ctx, x, z);
    };
    lod::NodeCache& cache = sink->cache;
    lod::SurfaceSampler sampler = [&cache, &raw](int32_t x, int32_t z) {
        return cache.get(x, z, raw);
    };
    // The far field's occlusion: how much lower this node sits than the four nodes a
    // spacing away from it, which are the neighbouring tiles' own corners. A node the
    // sampler refuses is a hole, not a wall, so it occludes nothing.
    const int32_t spacing = result->spacing;
    lod::NodeShade node_shade = [&cache, &raw, spacing](int32_t x, int32_t z) {
        const lod::SurfaceSample self = cache.get(x, z, raw);
        if (!self.valid) return 1.0f;
        auto at = [&](int32_t nx, int32_t nz) {
            const lod::SurfaceSample s = cache.get(nx, nz, raw);
            return s.valid ? s.height : self.height;
        };
        return lod::concavity_shade(self.height, at(x, z - spacing), at(x, z + spacing),
                                    at(x + spacing, z), at(x - spacing, z), spacing);
    };
    result->mesh = lod::build_tile_mesh(origin_x, origin_z, kTileBlocks, result->spacing, sampler,
                                        water_layer, 8.0f, result->neighbour_spacing, node_shade);
    result->columns_sampled = sampled;
    result->build_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();

    sink->columns_sampled.fetch_add(sampled, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(sink->mutex);
    sink->completed.push(std::move(*result));
}

} // namespace VoxelEngine
