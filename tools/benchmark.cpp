#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "lighting/block_light_region.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "world/sweep_band.hpp"
#include <algorithm>
#include <chrono>
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

// The squish A/B (worldgen/terrain_squish.hpp), done where nothing else can
// confound it: for `columns` columns, walk exactly the work a column costs — its
// band of slices, each one either the uniform fast path (all air / all bedrock /
// all solid rock) or a full generate_chunk — and report ms and chunks per column,
// split by kind. The unsquished band is the sweep's real window (pad 32, the
// constants in world/sweep_band.hpp); the squished band is the one slice it
// compresses into (pad 0, as WorldUpdater::band_pad() builds it). Same terrain,
// same column positions, same process: the only difference is the vertical axis.
// The two shipped macro knobs (data/terrain_config.json: height_base_y 312, sea
// level 200) are set explicitly so the terrain sits well above the cave band and
// the chunks under its surface take the solid fast path, as they do in the game;
// with the struct's default base the terrain would sit in the cave band and the
// normal side would full-generate a configuration the game never runs.
struct SquishBench {
    uint64_t full = 0;
    uint64_t fast = 0;
    uint64_t slices = 0;  // candidate slices over all columns (the band's size)
    double ms = 0.0;
};

// `span` is the squish's kept height in slices; 0 means not squished at all
// (the normal 1024-tall world), which is the reference every span is read
// against. The band comes from the generator's own height range, so a span
// measurement prices exactly the chunks that span would generate.
static SquishBench measure_column_band(int span, int columns) {
    VoxelEngine::TerrainParams params;
    params.height_base_y = 312.0f;
    params.squish_enabled = span > 0;
    params.squish_slice = 1;
    params.squish_span = span > 0 ? span : 1;
    VoxelEngine::ChunkGenerator gen(params);
    VoxelEngine::ChunkData chunk;
    constexpr int32_t kSlices = VoxelEngine::WORLD_HEIGHT_Y / VoxelEngine::CHUNK_HEIGHT;
    const float pad = params.squish_enabled ? 0.0f : 32.0f;

    SquishBench out;
    for (int i = 0; i < columns; ++i) {
        const int32_t cx = i * 7 + 3;
        const int32_t cz = i * 5 + 1;
        const VoxelEngine::ChunkGenerator::HeightRange range = gen.get_chunk_height_range(cx, cz);
        const float land_h = range.min_h;
        const float top_h = std::max(range.max_h, range.max_water_h);
        const VoxelEngine::sweep::ChunkBand band =
            VoxelEngine::sweep::band_for_column(land_h, top_h, false, kSlices, pad);
        out.slices += static_cast<uint64_t>(VoxelEngine::sweep::count(band));
        const auto started = std::chrono::steady_clock::now();
        for (int32_t cy = band.lo; cy <= band.hi; ++cy) {
            if (gen.generate_fast_path(chunk, cx, cy, cz)) {
                ++out.fast;
                continue;
            }
            ++out.full;
            gen.generate_chunk(chunk, cx, cy, cz, nullptr, false);
        }
        out.ms += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - started).count();
    }
    return out;
}

// The span sweep: how the per-column cost grows with the height the terrain is
// allowed to occupy. Read against the A/B above, whose span is 1. Printed, not
// checked, for the same reason as that one.
static void bench_squish_spans(int n) {
    printf("  squish spans:   span 0 = the normal 1024-tall world\n");
    for (int span : {0, 1, 2, 4, 8, 16, 32}) {
        measure_column_band(span, 16);  // warmup, so the first span is not cold
        const SquishBench b = measure_column_band(span, n);
        const double inv = 1.0 / static_cast<double>(n);
        char label[8];
        if (span == 0) {
            std::snprintf(label, sizeof(label), "off");
        } else {
            std::snprintf(label, sizeof(label), "%d", span);
        }
        printf("    span %-3s    %6.2f slices/col   %8.3f ms/col   %6.2f chunks/col\n",
               label, static_cast<double>(b.slices) * inv, b.ms * inv,
               static_cast<double>(b.full + b.fast) * inv);
    }
}

// Printed, deliberately NOT added to the results vector: the baseline check
// flags a metric whose value rose, and this one is better when it rises.
static void bench_squish_comparison(int n) {
    // Warmup both configurations so neither pays first-call cold caches inside
    // the measured window.
    measure_column_band(0, 16);
    measure_column_band(1, 16);

    const SquishBench normal = measure_column_band(0, n);
    const SquishBench squished = measure_column_band(1, n);
    const double inv = 1.0 / static_cast<double>(n);
    printf("  squish band:    normal   %.3f ms/column, %.2f chunks (%.2f full, %.2f fast)\n",
           normal.ms * inv, static_cast<double>(normal.full + normal.fast) * inv,
           static_cast<double>(normal.full) * inv, static_cast<double>(normal.fast) * inv);
    printf("                  squished %.3f ms/column, %.2f chunks (%.2f full, %.2f fast)  speedup=%.2fx\n",
           squished.ms * inv, static_cast<double>(squished.full + squished.fast) * inv,
           static_cast<double>(squished.full) * inv, static_cast<double>(squished.fast) * inv,
           (normal.ms > 0.0 ? normal.ms / squished.ms : 0.0));
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
    bench_squish_comparison(200);
    bench_squish_spans(200);
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
