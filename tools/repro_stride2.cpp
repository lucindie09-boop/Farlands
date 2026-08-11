#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace VoxelEngine;

int main() {
    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainParams params;
    ChunkGenerator gen(params);

    const int RD = 32;
    const int LOD_DIST = 6;
    const int pcy = gen.find_surface_y(0, 0) / CHUNK_HEIGHT;
    long long count = 0;

    for (int cx = 24; cx <= RD; cx++) {
        for (int cz = -RD; cz <= RD; cz++) {
            if (cx * cx + cz * cz > RD * RD) continue;
            int32_t wx = cx * CHUNK_WIDTH + CHUNK_WIDTH / 2;
            int32_t wz = cz * CHUNK_DEPTH + CHUNK_DEPTH / 2;
            int32_t surface = gen.find_surface_y(wx, wz);
            bool near_player = std::abs(cx) <= 1 && std::abs(cz) <= 1;
            int32_t cy_min = near_player ? pcy - 10 : (surface - 32) / CHUNK_HEIGHT - 2;
            int32_t cy_max = near_player ? pcy + 10 : (surface + 32) / CHUNK_HEIGHT + 2;
            for (int32_t cy = cy_min; cy <= cy_max; cy++) {
                int32_t bottom = cy * CHUNK_HEIGHT;
                int32_t top = (cy + 1) * CHUNK_HEIGHT;
                if (!near_player) {
                    if (top < surface - 32.0f) continue;
                    if (bottom > surface + 32.0f) continue;
                }
                count++;
                ChunkData chunk;
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, true);
                chunk.compute_section_flags();
                chunk.compute_fully_solid();
                int d = std::max({std::abs(cx), std::abs(cy - pcy), std::abs(cz)});
                bool far = d > LOD_DIST + 1;
                printf("#%lld (%d,%d,%d) surface=%d far=%d ... ", count, cx, cy, cz, surface, far ? 1 : 0);
                fflush(stdout);
                MeshBuilder mb;
                mb.set_smooth_lighting(false);
                mb.set_greedy_enabled(true);
                if (far) mb.set_detail_level(0.5f);
                mb.build_mesh(chunk);
                printf("OK v=%zu i=%zu\n", mb.get_vertex_count(), mb.get_index_count());
                fflush(stdout);
            }
        }
    }
    printf("all done, %lld chunks\n", count);
    return 0;
}
