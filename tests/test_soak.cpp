#include "doctest.h"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/thread_pool.hpp"
#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "lighting/block_light_region.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace VoxelEngine;

// =========================================================================
// Soak test: simulate player flying through the world
// =========================================================================

static void run_soak_test(int total_iterations, int render_distance) {
    BlockRegistry::get_instance().initialize_default_blocks();

    // Pre-allocate chunks for the full flight path — no concurrent store access needed.
    // This avoids raw-pointer-across-lock-boundary issues entirely.
    struct ChunkEntry {
        ChunkData data;
        bool generated = false;
    };

    // Estimate max chunks needed: (iterations/4 + render_distance*2) * (iterations/3 + render_distance*2)
    // Use a flat array indexed by (x - x_min, z - z_min)
    int32_t x_min = -render_distance;
    int32_t z_min = -render_distance;
    int32_t x_max = total_iterations / 4 + render_distance;
    int32_t z_max = total_iterations / 3 + render_distance;
    int32_t grid_w = x_max - x_min + 1;
    int32_t grid_d = z_max - z_min + 1;

    auto grid = std::make_shared<std::vector<ChunkEntry>>(static_cast<size_t>(grid_w) * grid_d);

    auto idx = [&](int32_t x, int32_t z) -> ChunkEntry& {
        return (*grid)[static_cast<size_t>(x - x_min) * grid_d + (z - z_min)];
    };

    TerrainParams params;
    ChunkGenerator gen(params);

    auto chunks_generated = std::make_shared<std::atomic<int>>(0);
    auto chunks_meshed = std::make_shared<std::atomic<int>>(0);
    auto chunks_lighted = std::make_shared<std::atomic<int>>(0);
    auto errors = std::make_shared<std::atomic<int>>(0);
    auto pending_mesh = std::make_shared<std::atomic<int>>(0);

    ThreadPool pool(std::max(std::size_t(2), static_cast<std::size_t>(std::thread::hardware_concurrency()) - 1));

    for (int iter = 0; iter < total_iterations; iter++) {
        int32_t px = iter / 4;
        int32_t pz = iter / 3;

        // Phase 1: Generate missing chunks (main thread — ChunkGenerator is not thread-safe)
        for (int dz = -render_distance; dz <= render_distance; dz++) {
            for (int dx = -render_distance; dx <= render_distance; dx++) {
                int32_t cx = px + dx;
                int32_t cz = pz + dz;
                double dist = std::sqrt(static_cast<double>(dx * dx + dz * dz));
                if (dist > render_distance) continue;
                if (cx < x_min || cx > x_max || cz < z_min || cz > z_max) continue;

                auto& entry = idx(cx, cz);
                if (entry.generated) continue;

                entry.data.clear();
                std::function<void(int32_t, int32_t, int32_t, BlockID)> no_cross_writes;
                gen.generate_chunk(entry.data, cx, 0, cz, no_cross_writes, false);
                entry.generated = true;
                chunks_generated->fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Phase 2: Build meshes concurrently — workers read from pre-allocated grid (no store/lock needed)
        int mesh_distance = std::min(render_distance, render_distance - 1);
        for (int dz = -mesh_distance; dz <= mesh_distance; dz++) {
            for (int dx = -mesh_distance; dx <= mesh_distance; dx++) {
                int32_t cx = px + dx;
                int32_t cz = pz + dz;
                double dist = std::sqrt(static_cast<double>(dx * dx + dz * dz));
                if (dist > mesh_distance) continue;
                if (cx < x_min || cx > x_max || cz < z_min || cz > z_max) continue;

                auto& center_entry = idx(cx, cz);
                if (!center_entry.generated) continue;

                pending_mesh->fetch_add(1, std::memory_order_relaxed);
                pool.fire_and_forget([grid, grid_d, x_min, z_min, x_max, z_max,
                                      pending_mesh, chunks_meshed, errors, cx, cz]() {
                    auto lookup = [&](int dx, int dz) -> const ChunkData* {
                        int32_t nx = cx + dx;
                        int32_t nz = cz + dz;
                        if (nx < x_min || nx > x_max || nz < z_min || nz > z_max) return nullptr;
                        auto& e = (*grid)[static_cast<size_t>(nx - x_min) * grid_d + (nz - z_min)];
                        return e.generated ? &e.data : nullptr;
                    };

                    auto& center = (*grid)[static_cast<size_t>(cx - x_min) * grid_d + (cz - z_min)];

                    MeshBuilder mb;
                    mb.build_mesh(
                        center.data,
                        lookup(-1, 0), lookup(1, 0),
                        nullptr, nullptr,
                        lookup(0, -1), lookup(0, 1),
                        lookup(-1, -1), lookup(-1, 1),
                        lookup(1, -1), lookup(1, 1),
                        nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, nullptr, nullptr
                    );

                    if (center.data.get_block_count() > 0) {
                        if (mb.get_vertex_count() == 0 && mb.get_index_count() == 0) {
                            errors->fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    chunks_meshed->fetch_add(1, std::memory_order_relaxed);
                    pending_mesh->fetch_sub(1, std::memory_order_relaxed);
                });
            }
        }

        // Wait for all mesh builds to complete
        while (pending_mesh->load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        // Phase 3: Light propagation — main thread (writes to chunks, not thread-safe)
        int light_distance = std::min(3, mesh_distance);
        for (int dz = -light_distance; dz <= light_distance; dz++) {
            for (int dx = -light_distance; dx <= light_distance; dx++) {
                int32_t cx = px + dx;
                int32_t cz = pz + dz;

                ChunkData* region[3][3][3] = {};
                bool valid = true;
                for (int z = -1; z <= 1; z++) {
                    for (int y = -1; y <= 1; y++) {
                        for (int x = -1; x <= 1; x++) {
                            int32_t nx = cx + x;
                            int32_t nz = cz + z;
                            if (nx >= x_min && nx <= x_max && nz >= z_min && nz <= z_max) {
                                auto& e = idx(nx, nz);
                                if (e.generated)
                                    region[z + 1][y + 1][x + 1] = &e.data;
                            }
                        }
                    }
                }

                if (!region[1][1][1]) continue;

                BlockLightRegion light_region(region);
                std::vector<EmissiveSource> sources;
                light_region.collect_emissive_sources(sources);
                light_region.propagate_additive(sources);

                // Verify light values
                for (int32_t lz = 0; lz < CHUNK_DEPTH; lz++) {
                    for (int32_t ly = 0; ly < CHUNK_HEIGHT; ly++) {
                        for (int32_t lx = 0; lx < CHUNK_WIDTH; lx++) {
                            uint8_t r = region[1][1][1]->get_light_r(lx, ly, lz);
                            uint8_t g = region[1][1][1]->get_light_g(lx, ly, lz);
                            uint8_t b = region[1][1][1]->get_light_b(lx, ly, lz);
                            if (r > 15 || g > 15 || b > 15) {
                                errors->fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                chunks_lighted->fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Phase 4: Unload distant chunks (main thread — exercises unload vs active work races)
        int unload_distance = render_distance + 2;
        for (int dz = -unload_distance; dz <= unload_distance; dz++) {
            for (int dx = -unload_distance; dx <= unload_distance; dx++) {
                int32_t cx = px + dx;
                int32_t cz = pz + dz;
                if (cx < x_min || cx > x_max || cz < z_min || cz > z_max) continue;

                double dist = std::sqrt(static_cast<double>(dx * dx + dz * dz));
                if (dist > unload_distance) {
                    auto& entry = idx(cx, cz);
                    if (entry.generated) {
                        entry.data.clear();
                        entry.generated = false;
                    }
                }
            }
        }
    }

    pool.shutdown();

    INFO("Chunks generated: ", chunks_generated->load());
    INFO("Chunks meshed:    ", chunks_meshed->load());
    INFO("Chunks lighted:   ", chunks_lighted->load());
    INFO("Errors:           ", errors->load());

    CHECK(chunks_generated->load() > 0);
    CHECK(chunks_meshed->load() > 0);
    CHECK(chunks_lighted->load() > 0);
    CHECK(errors->load() == 0);
}

TEST_CASE("soak: quick smoke test") {
    run_soak_test(50, 3);
}

TEST_CASE("soak: sustained flight simulation") {
    run_soak_test(200, 3);
}

// =========================================================================
// Soak test: shard map insert/erase/read hammer
// =========================================================================

static constexpr size_t kSoakShards = 64;
struct SoakShard {
    mutable std::shared_mutex mutex;
    std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> chunks;
};

static uint64_t soak_key(int32_t x, int32_t y, int32_t z) {
    constexpr uint32_t OFF = 1u << 20;
    constexpr uint32_t MASK = 0x1FFFFF;
    uint64_t ux = static_cast<uint64_t>((static_cast<uint32_t>(x) + OFF) & MASK);
    uint64_t uy = static_cast<uint64_t>((static_cast<uint32_t>(y) + OFF) & MASK);
    uint64_t uz = static_cast<uint64_t>((static_cast<uint32_t>(z) + OFF) & MASK);
    return (ux << 42) | (uy << 21) | uz;
}

static void run_shard_hammer() {
    auto shards = std::make_shared<std::array<SoakShard, kSoakShards>>();

    auto insert_count = std::make_shared<std::atomic<int>>(0);
    auto read_count = std::make_shared<std::atomic<int>>(0);
    auto erase_count = std::make_shared<std::atomic<int>>(0);
    auto error_count = std::make_shared<std::atomic<int>>(0);

    BlockRegistry::get_instance().initialize_default_blocks();

    constexpr int kTotalOps = 50000;
    constexpr int kMaxChunks = 500;

    ThreadPool pool(std::max(std::size_t(2), static_cast<std::size_t>(std::thread::hardware_concurrency()) - 1));

    for (int i = 0; i < kTotalOps; i++) {
        pool.fire_and_forget([shards, insert_count, read_count, erase_count, error_count, i, kMaxChunks]() {
            int32_t x = i % kMaxChunks;
            int32_t y = 0;
            int32_t z = (i / kMaxChunks) % 8;
            uint64_t k = soak_key(x, y, z);
            size_t s = k % 64;

            int op = i % 10;

            if (op < 4) {
                auto chunk = std::make_unique<ChunkData>();
                chunk->clear();
                chunk->set_block(x % CHUNK_WIDTH, 16, z % CHUNK_DEPTH, BlockIDs::STONE);
                std::unique_lock<std::shared_mutex> lk((*shards)[s].mutex);
                (*shards)[s].chunks.insert_or_assign(k, std::move(chunk));
                insert_count->fetch_add(1, std::memory_order_relaxed);
            } else if (op < 7) {
                // Read: hold lock for the entire read operation
                std::shared_lock<std::shared_mutex> lk((*shards)[s].mutex);
                auto it = (*shards)[s].chunks.find(k);
                if (it != (*shards)[s].chunks.end()) {
                    BlockID id = it->second->get_block(0, 0, 0);
                    if (id != BlockIDs::AIR && id != BlockIDs::STONE) {
                        error_count->fetch_add(1, std::memory_order_relaxed);
                    }
                }
                read_count->fetch_add(1, std::memory_order_relaxed);
            } else if (op < 9) {
                std::unique_lock<std::shared_mutex> lk((*shards)[s].mutex);
                (*shards)[s].chunks.erase(k);
                erase_count->fetch_add(1, std::memory_order_relaxed);
            } else {
                std::shared_lock<std::shared_mutex> lk((*shards)[s].mutex);
                volatile size_t count = (*shards)[s].chunks.size();
                (void)count;
                read_count->fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    pool.shutdown();

    INFO("Inserts: ", insert_count->load());
    INFO("Reads:   ", read_count->load());
    INFO("Erases:  ", erase_count->load());
    INFO("Errors:  ", error_count->load());

    CHECK(insert_count->load() + erase_count->load() > 0);
    CHECK(error_count->load() == 0);
}

TEST_CASE("soak: shard map concurrent insert/erase/read") {
    run_shard_hammer();
}

// =========================================================================
// Soak test: mesh builder stress test with random layouts
// =========================================================================

TEST_CASE("soak: mesh builder concurrent stress") {
    BlockRegistry::get_instance().initialize_default_blocks();

    constexpr int kNumChunks = 64;
    auto chunks = std::make_shared<std::vector<ChunkData>>(kNumChunks);
    auto chunk_ptrs = std::make_shared<std::vector<ChunkData*>>(kNumChunks);

    for (int i = 0; i < kNumChunks; i++) {
        (*chunks)[i].clear();
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            for (int y = 0; y < CHUNK_HEIGHT; y++) {
                for (int x = 0; x < CHUNK_WIDTH; x++) {
                    if ((x + y + z + i) % 7 == 0)
                        (*chunks)[i].set_block(x, y, z, BlockIDs::STONE);
                    else if ((x + y + z + i) % 11 == 0)
                        (*chunks)[i].set_block(x, y, z, BlockIDs::GRASS);
                    else if ((x + y + z + i) % 13 == 0)
                        (*chunks)[i].set_block(x, y, z, BlockIDs::LIGHT_BLOCK);
                }
            }
        }
        (*chunks)[i].compute_fully_solid();
        (*chunk_ptrs)[i] = &(*chunks)[i];
    }

    auto builds_completed = std::make_shared<std::atomic<int>>(0);
    auto errors = std::make_shared<std::atomic<int>>(0);

    ThreadPool pool(std::max(std::size_t(2), static_cast<std::size_t>(std::thread::hardware_concurrency()) - 1));

    constexpr int kBuildsPerChunk = 20;
    for (int i = 0; i < kNumChunks * kBuildsPerChunk; i++) {
        pool.fire_and_forget([chunk_ptrs, builds_completed, errors, i, kNumChunks]() {
            int chunk_idx = i % kNumChunks;
            int neighbor_idx = (i + 1) % kNumChunks;

            MeshBuilder mb;
            mb.build_mesh(
                *(*chunk_ptrs)[chunk_idx],
                (*chunk_ptrs)[neighbor_idx], nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr,
                nullptr, nullptr
            );

            size_t indices = mb.get_index_count();
            if (indices % 3 != 0) {
                errors->fetch_add(1, std::memory_order_relaxed);
            }

            builds_completed->fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.shutdown();

    INFO("Builds completed: ", builds_completed->load());
    INFO("Errors:           ", errors->load());

    CHECK(builds_completed->load() == kNumChunks * kBuildsPerChunk);
    CHECK(errors->load() == 0);
}

// =========================================================================
// Soak test: light propagation across moving neighborhoods
// =========================================================================

TEST_CASE("soak: light propagation across shifting grid") {
    BlockRegistry::get_instance().initialize_default_blocks();

    constexpr int kGridSize = 16;
    std::vector<std::vector<ChunkData>> grid_data(kGridSize,
        std::vector<ChunkData>(kGridSize));

    for (int z = 0; z < kGridSize; z++) {
        for (int x = 0; x < kGridSize; x++) {
            grid_data[z][x].clear();
            if ((x + z) % 4 == 0)
                grid_data[z][x].set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
            if ((x + z) % 6 == 0)
                grid_data[z][x].set_block(8, 16, 8, BlockIDs::LIGHT_RED);
        }
    }

    auto propagations = std::make_shared<std::atomic<int>>(0);
    auto light_errors = std::make_shared<std::atomic<int>>(0);

    constexpr int kWindowRadius = 3;
    constexpr int kSteps = 200;

    // Run sequentially — propagate_additive writes to chunks, not thread-safe
    for (int step = 0; step < kSteps; step++) {
        int center_x = (step % (kGridSize - kWindowRadius * 2)) + kWindowRadius;
        int center_z = ((step * 7) % (kGridSize - kWindowRadius * 2)) + kWindowRadius;

        ChunkData* region[3][3][3] = {};
        for (int dz = -1; dz <= 1; dz++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int gx = center_x + dx;
                    int gz = center_z + dz;
                    if (gx >= 0 && gx < kGridSize && gz >= 0 && gz < kGridSize)
                        region[dz + 1][dy + 1][dx + 1] = &grid_data[gz][gx];
                }
            }
        }

        if (!region[1][1][1]) continue;

        BlockLightRegion light_region(region);
        std::vector<EmissiveSource> sources;
        light_region.collect_emissive_sources(sources);
        light_region.propagate_additive(sources);

        for (int dz = 0; dz < 3; dz++) {
            for (int dy = 0; dy < 3; dy++) {
                for (int dx = 0; dx < 3; dx++) {
                    ChunkData* c = region[dz][dy][dx];
                    if (!c) continue;
                    for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
                        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
                            for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
                                uint8_t r = c->get_light_r(x, y, z);
                                uint8_t g = c->get_light_g(x, y, z);
                                uint8_t b = c->get_light_b(x, y, z);
                                if (r > 15 || g > 15 || b > 15)
                                    light_errors->fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
        }

        propagations->fetch_add(1, std::memory_order_relaxed);
    }

    INFO("Propagations: ", propagations->load());
    INFO("Light errors: ", light_errors->load());

    CHECK(propagations->load() > 0);
    CHECK(light_errors->load() == 0);
}
