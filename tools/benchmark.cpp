#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "lighting/block_light_region.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

struct BenchResult {
    const char* name;
    double value;
    const char* unit;
};

static BenchResult bench_generation(int n) {
    VoxelEngine::TerrainParams params;
    VoxelEngine::ChunkGenerator gen(params);
    VoxelEngine::ChunkData chunk;

    for (int i = 0; i < 50; i++)
        gen.generate_chunk(chunk, i * 10, 0, i * 10);

    VoxelEngine::PerformanceTimer perf;
    for (int i = 0; i < n; i++) {
        VoxelEngine::ScopedTimer timer(perf, VoxelEngine::TimerID::GenerateChunk);
        gen.generate_chunk(chunk, i * 10, 0, i * 10);
    }

    double avg = perf.get_avg(VoxelEngine::TimerID::GenerateChunk);
    printf("  generate_chunk: avg=%.3f ms  min=%.3f ms  max=%.3f ms  (n=%d)\n",
           avg, perf.get_min(VoxelEngine::TimerID::GenerateChunk),
           perf.get_max(VoxelEngine::TimerID::GenerateChunk), n);
    return {"generate_chunk_avg_ms", avg, "ms"};
}

static BenchResult bench_meshing(int n) {
    VoxelEngine::TerrainParams params;
    VoxelEngine::ChunkGenerator gen(params);
    VoxelEngine::ChunkData chunk;

    gen.generate_chunk(chunk, 0, 0, 0);

    VoxelEngine::MeshBuilder mb;
    VoxelEngine::PerformanceTimer perf;

    for (int i = 0; i < 50; i++) {
        mb.clear();
        mb.build_mesh(chunk);
    }

    for (int i = 0; i < n; i++) {
        mb.clear();
        VoxelEngine::ScopedTimer timer(perf, VoxelEngine::TimerID::BuildMesh);
        mb.build_mesh(chunk);
    }

    double avg = perf.get_avg(VoxelEngine::TimerID::BuildMesh);
    printf("  build_mesh:     avg=%.3f ms  min=%.3f ms  max=%.3f ms  (n=%d)\n",
           avg, perf.get_min(VoxelEngine::TimerID::BuildMesh),
           perf.get_max(VoxelEngine::TimerID::BuildMesh), n);
    return {"build_mesh_avg_ms", avg, "ms"};
}

static BenchResult bench_incremental_meshing(int n) {
    VoxelEngine::BlockRegistry::get_instance().initialize_default_blocks();
    VoxelEngine::ChunkData chunk;
    chunk.fill_blocks(VoxelEngine::BlockIDs::AIR);
    for (int y = 0; y <= 16; y++)
        for (int z = 0; z < VoxelEngine::CHUNK_DEPTH; z++)
            for (int x = 0; x < VoxelEngine::CHUNK_WIDTH; x++)
                chunk.set_block(x, y, z, VoxelEngine::BlockIDs::STONE);
    for (int y = 17; y < 24; y++) {
        chunk.set_block(6, y, 6, VoxelEngine::BlockIDs::STONE);
        chunk.set_block(24, y, 24, VoxelEngine::BlockIDs::STONE);
    }
    chunk.set_block(16, 20, 16, VoxelEngine::BlockIDs::STONE);
    chunk.set_light_rgb(16, 21, 16, 10, 10, 10);
    chunk.compute_section_flags();
    chunk.compute_fully_solid();

    VoxelEngine::MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.build_mesh(chunk);

    // Dirty bbox = the single edited block (16,20,16), [min,max]+1 exclusive —
    // what the manager now snapshots for a block edit. build_mesh_incremental
    // expands it by the AO/light ring.
    const VoxelEngine::MeshBuilder::SubChunkBounds bounds{16, 17, 20, 21, 16, 17};

    // Warmup (the incremental build feeds its own output back as the next
    // iteration's previous-quad cache, mirroring the live edit loop).
    for (int i = 0; i < 50; i++) {
        chunk.set_block(16, 20, 16, (i % 2) ? VoxelEngine::BlockIDs::STONE : VoxelEngine::BlockIDs::AIR);
        chunk.compute_section_flags();
        mb.build_mesh_incremental(chunk, mb.get_quads(), mb.get_light_checksums(), bounds);
    }

    VoxelEngine::PerformanceTimer perf;
    VoxelEngine::MeshBuilder::get_perf_timer().reset_all();
    for (int i = 0; i < n; i++) {
        chunk.set_block(16, 20, 16, (i % 2) ? VoxelEngine::BlockIDs::STONE : VoxelEngine::BlockIDs::AIR);
        chunk.compute_section_flags();
        VoxelEngine::ScopedTimer timer(perf, VoxelEngine::TimerID::BuildMesh);
        mb.build_mesh_incremental(chunk, mb.get_quads(), mb.get_light_checksums(), bounds);
    }

    auto& pt = VoxelEngine::MeshBuilder::get_perf_timer();
    printf("  [incr] solid_cache=%.3f h=%.3f v=%.3f build_total=%.3f n=%llu\n",
           pt.get_avg(VoxelEngine::TimerID::SolidCachePopulation),
           pt.get_avg(VoxelEngine::TimerID::GreedyMeshHorizontal),
           pt.get_avg(VoxelEngine::TimerID::GreedyMeshVertical),
           pt.get_avg(VoxelEngine::TimerID::BuildMesh),
           pt.get_count(VoxelEngine::TimerID::BuildMesh));

    double avg = perf.get_avg(VoxelEngine::TimerID::BuildMesh);
    printf("  incremental:    avg=%.3f ms  min=%.3f ms  max=%.3f ms  (n=%d, 1-block edit)\n",
           avg, perf.get_min(VoxelEngine::TimerID::BuildMesh),
           perf.get_max(VoxelEngine::TimerID::BuildMesh), n);
    return {"incremental_mesh_avg_ms", avg, "ms"};
}

static BenchResult bench_palette_ops(int n) {
    VoxelEngine::ChunkData chunk;
    chunk.clear();

    VoxelEngine::PerformanceTimer perf;

    for (int i = 0; i < 50; i++) {
        for (int x = 0; x < VoxelEngine::CHUNK_WIDTH; x++)
            for (int z = 0; z < VoxelEngine::CHUNK_DEPTH; z++)
                for (int y = 0; y < VoxelEngine::CHUNK_HEIGHT; y++)
                    chunk.set_block(x, y, z, VoxelEngine::BlockIDs::AIR);
    }

    for (int i = 0; i < n; i++) {
        VoxelEngine::ScopedTimer timer(perf, VoxelEngine::TimerID::PaletteWrite);
        for (int x = 0; x < VoxelEngine::CHUNK_WIDTH; x++)
            for (int z = 0; z < VoxelEngine::CHUNK_DEPTH; z++)
                for (int y = 0; y < VoxelEngine::CHUNK_HEIGHT; y++)
                    chunk.set_block(x, y, z, VoxelEngine::BlockIDs::STONE);
    }

    double avg = perf.get_avg(VoxelEngine::TimerID::PaletteWrite);
    printf("  palette_write:  avg=%.3f ms  min=%.3f ms  max=%.3f ms  (n=%d full chunk fills)\n",
           avg, perf.get_min(VoxelEngine::TimerID::PaletteWrite),
           perf.get_max(VoxelEngine::TimerID::PaletteWrite), n);
    return {"palette_write_avg_ms", avg, "ms"};
}

static BenchResult bench_light_propagation(int n) {
    VoxelEngine::BlockRegistry::get_instance().initialize_default_blocks();

    // 3×3×3 grid of chunks with an emissive block in the center
    VoxelEngine::ChunkData region[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                region[dz][dy][dx].clear();
    region[1][1][1].set_block(16, 16, 16, VoxelEngine::BlockIDs::LIGHT_BLOCK);

    VoxelEngine::ChunkData* grid[3][3][3];
    for (int dz = 0; dz < 3; dz++)
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                grid[dz][dy][dx] = &region[dz][dy][dx];

    VoxelEngine::BlockLightRegion light_region(grid);
    std::vector<VoxelEngine::EmissiveSource> sources;

    // Warmup
    for (int i = 0; i < 50; i++) {
        light_region.clear_block_light();
        sources.clear();
        light_region.collect_emissive_sources(sources);
        light_region.propagate_additive(sources);
    }

    VoxelEngine::PerformanceTimer perf;
    for (int i = 0; i < n; i++) {
        light_region.clear_block_light();
        sources.clear();
        VoxelEngine::ScopedTimer timer(perf, VoxelEngine::TimerID::LightPropagation);
        light_region.collect_emissive_sources(sources);
        light_region.propagate_additive(sources);
    }

    double avg = perf.get_avg(VoxelEngine::TimerID::LightPropagation);
    printf("  light_prop:     avg=%.3f ms  min=%.3f ms  max=%.3f ms  (n=%d)\n",
           avg, perf.get_min(VoxelEngine::TimerID::LightPropagation),
           perf.get_max(VoxelEngine::TimerID::LightPropagation), n);
    return {"light_propagation_avg_ms", avg, "ms"};
}

static BenchResult bench_memory_usage() {
    VoxelEngine::TerrainParams params;
    VoxelEngine::ChunkGenerator gen(params);
    VoxelEngine::ChunkData chunk;

    gen.generate_chunk(chunk, 0, 0, 0);

    size_t bytes = chunk.memory_usage();
    printf("  memory_usage:  %zu bytes (%.1f KB)\n", bytes, bytes / 1024.0);
    return {"chunk_memory_bytes", static_cast<double>(bytes), "bytes"};
}

struct BaselineEntry {
    std::string name;
    double max_value;
};

static std::vector<BaselineEntry> load_baseline(const char* path) {
    std::vector<BaselineEntry> entries;
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Warning: could not open baseline file %s\n", path);
        return entries;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        char name[128];
        double val;
        if (sscanf(line, "%127s %lf", name, &val) == 2)
            entries.push_back({name, val});
    }
    fclose(f);
    return entries;
}

int main(int argc, char** argv) {
    bool check_mode = false;
    const char* baseline_path = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            check_mode = true;
            baseline_path = argv[++i];
        }
    }

    printf("=== VoxelEngine Benchmark ===\n");
    std::vector<BenchResult> results;
    results.push_back(bench_generation(1000));
    results.push_back(bench_meshing(1000));
    results.push_back(bench_incremental_meshing(1000));
    results.push_back(bench_palette_ops(100));
    results.push_back(bench_light_propagation(1000));
    results.push_back(bench_memory_usage());

    if (!check_mode || !baseline_path) return 0;

    auto baseline = load_baseline(baseline_path);
    if (baseline.empty()) {
        fprintf(stderr, "No baseline entries loaded, skipping check.\n");
        return 0;
    }

    printf("\n--- Baseline check (%s) ---\n", baseline_path);
    int regressions = 0;

    for (auto& r : results) {
        for (auto& b : baseline) {
            if (r.name == b.name) {
                if (r.value > b.max_value) {
                    printf("  REGRESSION %s: %.3f %s > baseline %.3f %s\n",
                           r.name, r.value, r.unit, b.max_value, r.unit);
                    regressions++;
                } else {
                    printf("  OK %s: %.3f %s <= baseline %.3f %s\n",
                           r.name, r.value, r.unit, b.max_value, r.unit);
                }
            }
        }
    }

    if (regressions > 0) {
        printf("\nFAILED: %d metric(s) regressed beyond baseline.\n", regressions);
        return 1;
    }
    printf("\nAll metrics within baseline.\n");
    return 0;
}
