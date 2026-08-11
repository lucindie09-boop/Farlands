#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
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
                    mb.build_mesh(chunk);
                    v_greedy += mb.get_vertex_count();
                    i_greedy += mb.get_index_count();
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
    printf("\n");
    printf("greedy savings (verts): %.2f%%\n", 100.0 * (1.0 - (double)v_greedy / (double)v_plain));
    printf("smooth-off vs smooth-on verts: %lld (%+.2f%%)\n",
           v_greedy - v_smooth, 100.0 * ((double)v_greedy / (double)v_smooth - 1.0));
    return 0;
}
