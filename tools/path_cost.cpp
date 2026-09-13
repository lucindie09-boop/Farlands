// Planner cost on real generated terrain.
//
// Generates a window of the actual overworld (the game's terrain parameters are
// hand-mirrored here because the JSON loaders need the Godot runtime), then
// times complete planner runs across it at several distances and reports what
// the search actually spends: expansions, columns resolved, raw grid nodes and
// smoothed waypoints. It also counts how much of each route wades through
// liquid, which is the usual reason a route "looks wrong" on inspection.
//
// Build: scons path_cost
// Run:   bin/path_cost [seed] [max_distance]

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/chunk_coords.hpp"
#include "pathfinding/nav_view.hpp"
#include "pathfinding/pathfinder.hpp"
#include "pathfinding/path_smoother.hpp"
#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace VoxelEngine;
namespace nav = VoxelEngine::nav;

namespace {

TerrainParams game_params(int32_t seed) {
    TerrainParams p;
    p.seed = seed;
    p.height_base_y = 312.0f;
    p.climate_blend_radius_nodes = 16;
    return p;
}

BiomeConfig game_biomes() {
    BiomeConfig bc;
    auto set = [&](BiomeType b, float height, float weirdness, float min_weirdness) {
        const size_t i = static_cast<size_t>(b);
        bc.amplification[i].height = height;
        bc.amplification[i].weirdness = weirdness;
        bc.amplification[i].min_weirdness = min_weirdness;
    };
    set(BiomeType::Plains, 0.4f, 0.6f, 1.0f);
    set(BiomeType::Hills, 1.0f, 1.0f, 1.1f);
    set(BiomeType::Ocean, 1.0f, 1.0f, 1.0f);
    return bc;
}

uint64_t pack_chunk(int32_t cx, int32_t cy, int32_t cz) {
    constexpr uint32_t OFF = 1u << 20;
    constexpr uint32_t MASK = 0x1FFFFF;
    const uint64_t ux = (static_cast<uint32_t>(cx) + OFF) & MASK;
    const uint64_t uy = (static_cast<uint32_t>(cy) + OFF) & MASK;
    const uint64_t uz = (static_cast<uint32_t>(cz) + OFF) & MASK;
    return (ux << 42) | (uy << 21) | uz;
}

// Classifies a block into a nav cell.
//
// Note this does NOT consult BlockType::is_full_cube(): the default block set
// registers every block as a full cube (including air), because that flag is
// only recomputed from the real shapes once data/block_definitions.json loads
// under the game runtime. Collision boxes and the Solid property are accurate
// in both paths, so the shape comes from those.
nav::Cell classify(BlockID id) {
    if (id == BlockIDs::AIR) return nav::Cell{nav::CellClass::Air, 0.0f, 0.0f};
    const BlockRegistry& reg = BlockRegistry::get_instance();
    const BlockType& bt = reg.get_block_fast(id);
    if (HasProperty(bt.properties, BlockProperty::Liquid)) {
        return nav::Cell{nav::CellClass::Liquid, 0.0f, 0.0f};
    }
    const auto& boxes = bt.get_collision_boxes();
    if (boxes.empty()) {
        return HasProperty(bt.properties, BlockProperty::Solid)
                   ? nav::Cell{nav::CellClass::Solid, 0.0f, 1.0f}
                   : nav::Cell{nav::CellClass::Air, 0.0f, 0.0f};
    }
    float lo = 1.0f;
    float hi = 0.0f;
    for (const auto& b : boxes) {
        lo = std::min(lo, b.min[1]);
        hi = std::max(hi, b.max[1]);
    }
    return nav::Cell{nav::CellClass::Partial, lo, hi};
}

struct NavWorld {
    std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> chunks;

    [[nodiscard]] const ChunkData* chunk_at(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        const auto it = chunks.find(pack_chunk(cx, cy, cz));
        return it == chunks.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] nav::Cell sample(int32_t wx, int32_t wy, int32_t wz) const {
        const ChunkData* cd = chunk_at(wx, wy, wz);
        if (cd == nullptr) return nav::Cell{nav::CellClass::Unknown, 0.0f, 0.0f};
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        return classify(cd->get_block(lx, ly, lz));
    }
};

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char** argv) {
    const int32_t seed = (argc > 1) ? static_cast<int32_t>(std::atoi(argv[1])) : 1337;
    const int32_t max_distance = (argc > 2) ? static_cast<int32_t>(std::atoi(argv[2])) : 192;

    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainParams params = game_params(seed);
    ChunkGenerator gen(params);
    gen.set_biome_config(game_biomes());

    const int32_t region_x0 = -16;
    const int32_t region_x1 = max_distance + 32;
    const int32_t region_z0 = -48;
    const int32_t region_z1 = 48;

    // Surface scan bounds the vertical slice range the planner could need.
    int32_t min_surf = 1 << 20;
    int32_t max_surf = -(1 << 20);
    for (int32_t x = region_x0; x <= region_x1; x += 16) {
        for (int32_t z = region_z0; z <= region_z1; z += 16) {
            const int32_t s = gen.find_surface_y(x, z);
            min_surf = std::min(min_surf, s);
            max_surf = std::max(max_surf, s);
        }
    }
    const int32_t cy_lo = std::max(0, min_surf / CHUNK_HEIGHT - 1);
    const int32_t cy_hi = std::min(WORLD_HEIGHT_Y / CHUNK_HEIGHT - 1, max_surf / CHUNK_HEIGHT + 1);

    int32_t cx0, cx1, cz0, cz1, dummy;
    world_to_chunk_local(region_x0, 0, region_z0, cx0, dummy, cz0, dummy, dummy, dummy);
    world_to_chunk_local(region_x1, 0, region_z1, cx1, dummy, cz1, dummy, dummy, dummy);

    NavWorld world;
    const double gen_start = now_ms();
    int32_t chunk_count = 0;
    for (int32_t cx = cx0; cx <= cx1; ++cx) {
        for (int32_t cz = cz0; cz <= cz1; ++cz) {
            for (int32_t cy = cy_lo; cy <= cy_hi; ++cy) {
                auto cd = std::make_unique<ChunkData>();
                gen.generate_chunk(*cd, cx, cy, cz, nullptr, true);
                world.chunks.emplace(pack_chunk(cx, cy, cz), std::move(cd));
                ++chunk_count;
            }
        }
    }
    const double gen_ms = now_ms() - gen_start;

    printf("=== path_cost seed=%d ===\n", seed);
    printf("region x=[%d,%d] z=[%d,%d]  surface y=[%d,%d]  slices cy=[%d,%d]\n",
           region_x0, region_x1, region_z0, region_z1, min_surf, max_surf, cy_lo, cy_hi);
    printf("generated %d chunks in %.1f ms\n\n", chunk_count, gen_ms);

    const nav::NavBox box{region_x0, 0, region_z0, region_x1, WORLD_HEIGHT_Y - 1, region_z1};
    nav::NavView view(box, [&world](int32_t x, int32_t y, int32_t z) {
        return world.sample(x, y, z);
    });

    nav::Pathfinder finder(view, nav::NavCosts{});
    nav::PathSmoother smoother(view, nav::NavCosts{});

    // Endpoint probe: the planner's own view of where a route starts and ends.
    // A column that is found but has no clearance is the usual reason a search
    // refuses to start, so the vertical profile is dumped in that case.
    for (const int32_t probe_x : {0, 32}) {
        const int32_t surf = gen.find_surface_y(probe_x, 0);
        const nav::NavView::Column* col = view.column(probe_x, 0, surf);
        const bool usable = col != nullptr && col->found && !col->unknown && col->clearance;
        printf("probe (%d,0): generator surface y=%d -> column found=%d unknown=%d top=%.1f clearance=%d\n",
               probe_x, surf,
               col != nullptr && col->found, col != nullptr && col->unknown,
               col != nullptr ? col->surface_top : 0.0f,
               col != nullptr && col->clearance);
        if (!usable) {
            for (int32_t y = box.max_y; y >= box.min_y; y -= 8) {
                const nav::Cell c = world.sample(probe_x, y, 0);
                const char* name = "air";
                if (c.cls == nav::CellClass::Unknown) name = "unknown";
                else if (c.cls == nav::CellClass::Solid) name = "solid";
                else if (c.cls == nav::CellClass::Partial) name = "partial";
                else if (c.cls == nav::CellClass::Liquid) name = "liquid";
                printf("    y=%d %s\n", y, name);
            }
        }
    }
    printf("\n");

    printf("%-8s %-6s %-6s %-9s %-8s %-8s %-8s %-8s %s\n",
           "dist", "found", "trunc", "expand", "cols", "ms", "grid", "wpts", "water/drops");

    const int32_t distances[] = {32, 64, 128, 192, 256};
    for (const int32_t d : distances) {
        if (d > max_distance || d <= 0) continue;

        const int32_t start_surface = gen.find_surface_y(0, 0);
        const int32_t goal_surface = gen.find_surface_y(d, 0);

        nav::NavQuery q;
        q.start = nav::NavNode{0, start_surface, 0};
        q.goal = nav::NavNode{d, goal_surface, 0};
        q.max_expansions = 200000;

        const double t0 = now_ms();
        const nav::NavPath path = finder.search(q);
        const double search_ms = now_ms() - t0;

        const std::vector<nav::NavNode> waypoints =
            path.nodes.empty() ? std::vector<nav::NavNode>{} : smoother.smooth(path.nodes);

        int32_t water_cells = 0;
        int32_t drops = 0;
        for (const nav::NavNode& n : path.nodes) {
            const nav::NavView::Column* col = view.column(n.x, n.z, n.y);
            if (col != nullptr && col->liquid) ++water_cells;
        }
        for (size_t i = 1; i < path.nodes.size(); ++i) {
            if (path.nodes[i].y < path.nodes[i - 1].y) ++drops;
        }

        printf("%-8d %-6s %-6s %-9zu %-8zu %-8.2f %-8zu %-8zu %d/%d\n",
               d,
               path.found ? "yes" : "no",
               path.truncated ? "yes" : "no",
               path.stats.expansions,
               path.stats.columns_resolved,
               search_ms,
               path.nodes.size(),
               waypoints.size(),
               water_cells,
               drops);
        if (!path.found) printf("    (%s)\n", finder.last_error().c_str());
    }

    // The smoothed legs must all be genuinely walkable — a smoother that
    // shortens a route past an obstacle would show up here.
    nav::NavQuery sanity;
    sanity.start = nav::NavNode{0, gen.find_surface_y(0, 0), 0};
    sanity.goal = nav::NavNode{128, gen.find_surface_y(128, 0), 0};
    sanity.max_expansions = 200000;
    const nav::NavPath full = finder.search(sanity);
    int32_t broken_legs = 0;
    if (full.found) {
        const std::vector<nav::NavNode> wpts = smoother.smooth(full.nodes);
        for (size_t i = 1; i < wpts.size(); ++i) {
            if (!smoother.leg_walkable(wpts[i - 1], wpts[i])) ++broken_legs;
        }
    }
    printf("\nsanity: found=%s legs=%s broken_legs=%d\n",
           full.found ? "yes" : "no",
           full.found ? "ok" : "n/a",
           broken_legs);
    return broken_legs == 0 ? 0 : 1;
}
