#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "mesh/mesh_fluid.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>

using namespace VoxelEngine;

int main() {
    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainParams params;
    ChunkGenerator gen(params);

    long long v_plain = 0, i_plain = 0;   // greedy OFF, smooth OFF
    long long v_greedy = 0, i_greedy = 0; // greedy ON,  smooth OFF
    long long v_smooth = 0, i_smooth = 0; // smooth ON  (forces greedy OFF)
    long long v_water = 0, i_water = 0;   // liquids, drawn by the fluid surface pass
    long long liquid_cells = 0;
    double greedy_ms = 0.0;
    int chunk_count = 0;

    for (int cx = -2; cx <= 2; cx++) {
        for (int cz = -2; cz <= 2; cz++) {
            int32_t wx = cx * CHUNK_WIDTH + CHUNK_WIDTH / 2;
            int32_t wz = cz * CHUNK_DEPTH + CHUNK_DEPTH / 2;
            int32_t sy = gen.find_surface_y(wx, wz);
            int32_t cy0 = sy / CHUNK_HEIGHT;
            for (int cy = cy0 - 1; cy <= cy0 + 1; cy++) {
                ChunkData chunk;
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, true);
                chunk.compute_section_flags();
                chunk.compute_fully_solid();
                for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
                    for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                        for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                            const BlockID b = chunk.get_block_unsafe(x, y, z);
                            if (mesh_fluid::family_of(b, BlockRegistry::get_instance().get_block_fast(b))
                                != FluidKind::None) {
                                ++liquid_cells;
                            }
                        }
                    }
                }

                {
                    MeshBuilder mb;
                    mb.set_smooth_lighting(false);
                    mb.set_greedy_enabled(false);
                    mb.build_mesh(chunk);
                    v_plain += mb.get_vertex_count();
                    i_plain += mb.get_index_count();
                }
                {
                    MeshBuilder mb;
                    mb.set_smooth_lighting(false);
                    mb.set_greedy_enabled(true);
                    const auto t0 = std::chrono::steady_clock::now();
                    mb.build_mesh(chunk);
                    const auto t1 = std::chrono::steady_clock::now();
                    greedy_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                    v_greedy += mb.get_vertex_count();
                    i_greedy += mb.get_index_count();
                    v_water += mb.get_water_vertices().size();
                    i_water += mb.get_water_indices().size();
                }
                {
                    MeshBuilder mb;
                    mb.set_smooth_lighting(true);
                    mb.build_mesh(chunk);
                    v_smooth += mb.get_vertex_count();
                    i_smooth += mb.get_index_count();
                }
                chunk_count++;
            }
        }
    }

    printf("chunks meshed: %d\n", chunk_count);
    printf("\n");
    printf("%-34s %12s %12s\n", "mode", "vertices", "indices");
    printf("%-34s %12lld %12lld\n", "plain (greedy OFF, smooth OFF)", v_plain, i_plain);
    printf("%-34s %12lld %12lld\n", "greedy (greedy ON,  smooth OFF)", v_greedy, i_greedy);
    printf("%-34s %12lld %12lld\n", "smooth (smooth ON,  forces greedy OFF)", v_smooth, i_smooth);
    printf("%-34s %12lld %12lld\n", "liquids (fluid surface pass)", v_water, i_water);
    printf("%-34s %12lld\n", "liquid cells generated", liquid_cells);

    // A synthetic ocean chunk: every cell up to the waterline is sea, so the
    // fluid pass meets its worst case (a whole surface that must be measured) and
    // the flat-run merge is what keeps the vertex count sane.
    {
        const int32_t sea_top = CHUNK_HEIGHT / 2;
        ChunkData ocean;
        ocean.fill_blocks(BlockIDs::AIR);
        for (int32_t y = 0; y <= sea_top; ++y) {
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    const BlockID b = (y == 0) ? BlockIDs::STONE : BlockIDs::SURFACE_WATER;
                    ocean.set_block(x, y, z, b);
                }
            }
        }
        ocean.compute_section_flags();

        MeshBuilder mb;
        mb.set_greedy_enabled(true);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kReps = 10;
        for (int i = 0; i < kReps; ++i) mb.build_mesh(ocean);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;
        printf("\nfull ocean chunk (32x%d sea, no neighbours): %lld liquid verts, %.3f ms/build\n",
               sea_top + 1, static_cast<long long>(mb.get_water_vertices().size()), ms);
    }
    printf("%-34s %12.3f ms  (%.3f ms/chunk)\n", "greedy build wall clock", greedy_ms,
           chunk_count > 0 ? greedy_ms / chunk_count : 0.0);
    printf("\n");
    printf("greedy savings (verts): %.2f%%\n", 100.0 * (1.0 - (double)v_greedy / (double)v_plain));
    printf("smooth-off vs smooth-on verts: %lld (%+.2f%%)\n",
           v_greedy - v_smooth, 100.0 * ((double)v_greedy / (double)v_smooth - 1.0));
    return 0;
}
