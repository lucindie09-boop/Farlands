#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <algorithm>

using namespace VoxelEngine;

struct Zone {
    long long chunks = 0;
    long long meshed = 0;
    long long verts = 0;
    long long water_verts = 0;
    long long idx = 0;
    long long water_idx = 0;
    long long chunk_bytes = 0;
    long long packed_bytes = 0;   // 24 B/vert + 4 B/index (both solid + water)
    double verts_per_meshed = 0.0;
};

static void tally(Zone& z, ChunkData& chunk, MeshBuilder& mb) {
    const long long v = mb.get_vertex_count();
    const long long wv = mb.get_water_vertices().size();
    const long long idx = mb.get_index_count();
    const long long widx = mb.get_water_indices().size();
    z.chunks++;
    if (v + wv > 0) z.meshed++;
    z.verts += v;
    z.water_verts += wv;
    z.idx += idx;
    z.water_idx += widx;
    z.chunk_bytes += chunk.memory_usage();
    z.packed_bytes += (v + wv) * 24 + (idx + widx) * 4;
}

static void print_zone(const char* label, const Zone& z) {
    printf("  %-20s chunks=%-6lld meshed=%-6lld verts=%-10lld water=%-8lld packed=%.1f MB  avg_verts/meshed=%.0f\n",
           label, z.chunks, z.meshed, z.verts, z.water_verts, z.packed_bytes / 1048576.0,
           z.meshed ? (double)(z.verts + z.water_verts) / (double)z.meshed : 0.0);
}

int main() {
    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainParams params;
    ChunkGenerator gen(params);

    const int RD = 32;
    const int LOD_DIST = 6;
    const int pcy = gen.find_surface_y(0, 0) / CHUNK_HEIGHT;

    long long columns = 0;
    for (int x = -RD; x <= RD; x++)
        for (int z = -RD; z <= RD; z++)
            if (x * x + z * z <= RD * RD) columns++;

    printf("render distance 32: columns=%lld, LOD ring=dist<=%d full-detail, player surface chunk cy=%d\n", columns, LOD_DIST + 1, pcy);
    fflush(stdout);
    printf("generating world...\n");
    fflush(stdout);

    Zone near_z;  // dist <= 7: full detail, greedy
    Zone far_z;   // dist > 7: stride 2, greedy

    for (int cx = -RD; cx <= RD; cx++) {
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
                ChunkData chunk;
                gen.generate_chunk(chunk, cx, cy, cz, nullptr, true);
                chunk.compute_section_flags();
                chunk.compute_fully_solid();

                int d = std::max({std::abs(cx), std::abs(cy - pcy), std::abs(cz)});
                bool far = d > LOD_DIST + 1;

                MeshBuilder mb;
                mb.set_smooth_lighting(false);
                mb.set_greedy_enabled(true);
                if (far) mb.set_detail_level(0.5f);
                mb.build_mesh(chunk);
                tally(far ? far_z : near_z, chunk, mb);
            }
        }
    }

    const double near_packed = near_z.packed_bytes;
    const double far_packed = far_z.packed_bytes;
    const double near_meshed = near_z.meshed;
    const double far_meshed = far_z.meshed;

    // Far zone keeps: per-chunk GPU mesh + far CPU cache (2 copies in active use).
    const double vram = near_packed + 2.0 * far_packed;
    const double cpu_chunk = near_z.chunk_bytes + far_z.chunk_bytes;
    const double cpu_far_cache = far_packed;
    const double cpu_overhead = (near_z.chunks + far_z.chunks) * 4400.0; // ChunkRenderData + map + RIDs

    const double total_cpu = cpu_chunk + cpu_far_cache + cpu_overhead;

    printf("render distance 32: columns=%lld, LOD ring=dist<=%d full-detail, player surface chunk cy=%d\n\n", columns, LOD_DIST + 1, pcy);
    printf("--- real terrain, default seed, smooth OFF + greedy (actual runtime) ---\n");
    print_zone("near zone (dist<=7)", near_z);
    print_zone("far zone (dist 8..32)", far_z);

    printf("\n--- estimate at render distance 32 ---\n");
    printf("  VRAM (server-side mesh buffers, 24B/vert + 4B/index):\n");
    printf("    avg (real greedy):        %.0f MB  (near %.0f + far-chunk %.0f + far-cache %.0f)\n",
           vram / 1048576.0, near_packed / 1048576.0, far_packed / 1048576.0, far_packed / 1048576.0);
    printf("    max (no-greedy full-res): ~%.0f MB  (near %.0f + 2x far no-greedy %.0f)\n",
           (near_packed + 2.0 * 4.0 * far_packed) / 1048576.0,
           near_packed / 1048576.0, (4.0 * far_packed) / 1048576.0);
    printf("    min (flat plains, greedy): ~%.0f MB  (stride-2 far quads ~4x fewer, ~15%% of avg)\n",
           vram / 1048576.0 * 0.15);
    printf("\n  CPU RAM (voxel system only, not engine/assets):\n");
    printf("    avg (real greedy):        %.0f MB  (chunk data %.0f + far cache %.0f + overhead %.0f)\n",
           total_cpu / 1048576.0, cpu_chunk / 1048576.0, cpu_far_cache / 1048576.0, cpu_overhead / 1048576.0);
    printf("    max:                       ~%.0f MB  (chunk data %.0f + far no-greedy cache %.0f + overhead %.0f)\n",
           (cpu_chunk + 4.0 * far_packed + cpu_overhead) / 1048576.0,
           cpu_chunk / 1048576.0, (4.0 * far_packed) / 1048576.0, cpu_overhead / 1048576.0);
    printf("    min (flat plains, greedy): ~%.0f MB  (chunk data %.0f + small far cache)\n",
           (cpu_chunk + far_packed * 0.15) / 1048576.0, cpu_chunk / 1048576.0);

    printf("\n  per-chunk averages: near=%.0f verts (full detail), far=%.0f verts (stride-2)\n",
           near_meshed ? (near_z.verts + near_z.water_verts) / near_meshed : 0.0,
           far_meshed ? (far_z.verts + far_z.water_verts) / far_meshed : 0.0);
    return 0;
}
