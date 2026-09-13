#ifndef FARLANDS_NAV_TEST_WORLD_HPP
#define FARLANDS_NAV_TEST_WORLD_HPP

// Synthetic voxel world for the planner tests.
//
// Terrain is a per-column ground height (everything below it is solid) with
// optional per-cell overrides, so a test can express "flat plain", "terrace",
// "pit", "slab" or "wall" in one line. Columns can also be marked unresident
// (chunks the planner must refuse to route through) or punched into holes.

#include "pathfinding/move_generator.hpp"
#include "pathfinding/nav_view.hpp"

#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace navtest {

using VoxelEngine::nav::Cell;
using VoxelEngine::nav::CellClass;
using VoxelEngine::nav::MoveGenerator;
using VoxelEngine::nav::NavBox;
using VoxelEngine::nav::NavMoveList;
using VoxelEngine::nav::NavNode;
using VoxelEngine::nav::NavView;
using VoxelEngine::nav::column_key;
using VoxelEngine::nav::node_key;

struct World {
    int32_t default_ground = 10;
    int32_t min_y = 0;
    int32_t max_y = 48;

    std::unordered_map<uint64_t, int32_t> ground;    // column -> ground top
    std::unordered_map<uint64_t, Cell> overrides;    // cell -> exact sample
    std::unordered_set<uint64_t> holes;              // column -> ground removed
    std::unordered_set<uint64_t> unresident;         // column -> chunk not loaded

    void set_ground(int32_t x, int32_t z, int32_t top) { ground[column_key(x, z)] = top; }

    void fill(int32_t x0, int32_t y0, int32_t z0, int32_t w, int32_t h, int32_t d) {
        for (int32_t x = x0; x < x0 + w; ++x)
            for (int32_t y = y0; y < y0 + h; ++y)
                for (int32_t z = z0; z < z0 + d; ++z)
                    overrides[node_key(x, y, z)] = Cell{CellClass::Solid, 0.0f, 1.0f};
    }

    void set_cell(int32_t x, int32_t y, int32_t z, CellClass cls, float lo = 0.0f, float hi = 1.0f) {
        overrides[node_key(x, y, z)] = Cell{cls, lo, hi};
    }

    void add_hole(int32_t x, int32_t z) { holes.insert(column_key(x, z)); }
    void add_unresident(int32_t x, int32_t z) { unresident.insert(column_key(x, z)); }

    [[nodiscard]] int32_t ground_top_at(int32_t x, int32_t z) const {
        const auto it = ground.find(column_key(x, z));
        return it != ground.end() ? it->second : default_ground;
    }

    [[nodiscard]] Cell sample(int32_t x, int32_t y, int32_t z) const {
        const uint64_t col = column_key(x, z);
        if (unresident.count(col) != 0) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        const auto ov = overrides.find(node_key(x, y, z));
        if (ov != overrides.end()) return ov->second;
        if (holes.count(col) != 0) return Cell{CellClass::Air, 0.0f, 0.0f};
        if (y < ground_top_at(x, z)) return Cell{CellClass::Solid, 0.0f, 1.0f};
        return Cell{CellClass::Air, 0.0f, 0.0f};
    }
};

inline NavView make_view(const World& world, int32_t half_xz) {
    NavBox box{-half_xz, world.min_y, -half_xz, half_xz, world.max_y, half_xz};
    return NavView(box, [&world](int32_t x, int32_t y, int32_t z) {
        return world.sample(x, y, z);
    });
}

// Total cost of a path, re-derived from the move generator — which also proves
// every consecutive pair is a legal move. Returns -1 when it is not.
inline float path_cost(const NavView& view, const MoveGenerator& gen,
                       const std::vector<NavNode>& path) {
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) {
        const NavNode& from = path[i - 1];
        const NavView::Column* col = view.column(from.x, from.z, from.y);
        if (col == nullptr || !col->found || col->unknown || !col->clearance) return -1.0f;
        NavMoveList moves;
        gen.generate(from, *col, moves);
        bool matched = false;
        for (int m = 0; m < moves.count; ++m) {
            if (moves.items[m].to == path[i]) {
                total += moves.items[m].cost;
                matched = true;
                break;
            }
        }
        if (!matched) return -1.0f;
    }
    return total;
}

struct DijkstraResult {
    float cost = -1.0f;
    size_t expansions = 0;
};

// Brute-force Dijkstra over the same graph, used to check A*'s optimality. It
// uses the planner's own goal test (reach the goal column) and re-anchors both
// ends the same way.
inline DijkstraResult dijkstra_cost(const NavView& view, const MoveGenerator& gen,
                                    const NavNode& start, const NavNode& goal,
                                    int32_t budget = 200000) {
    DijkstraResult out;
    const NavView::Column* sc = view.column(start.x, start.z, start.y);
    const NavView::Column* gc = view.column(goal.x, goal.z, goal.y);
    if (sc == nullptr || !sc->found || sc->unknown || !sc->clearance) return out;
    if (gc == nullptr || !gc->found || gc->unknown || !gc->clearance) return out;

    const NavNode s{start.x, view.feet_cell(sc->surface_top), start.z};
    const NavNode g{goal.x, view.feet_cell(gc->surface_top), goal.z};

    std::unordered_map<uint64_t, float> dist;
    using Entry = std::pair<float, uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;
    dist[node_key(s.x, s.y, s.z)] = 0.0f;
    pq.push({0.0f, node_key(s.x, s.y, s.z)});

    while (!pq.empty()) {
        const auto [d, key] = pq.top();
        pq.pop();
        const auto it = dist.find(key);
        if (it == dist.end() || d > it->second + 1e-5f) continue;
        ++out.expansions;

        const NavNode cur = VoxelEngine::nav::node_from_key(key);
        if (cur.x == g.x && cur.z == g.z) {
            out.cost = d;
            return out;
        }
        const NavView::Column* col = view.column(cur.x, cur.z, cur.y);
        if (col == nullptr || !col->found || col->unknown || !col->clearance) continue;

        NavMoveList moves;
        gen.generate(cur, *col, moves);
        for (int m = 0; m < moves.count; ++m) {
            const NavNode& to = moves.items[m].to;
            const float nd = d + moves.items[m].cost;
            const uint64_t nk = node_key(to.x, to.y, to.z);
            const auto [nit, inserted] = dist.try_emplace(nk, nd);
            if (!inserted && nd >= nit->second - 1e-5f) continue;
            nit->second = nd;
            pq.push({nd, nk});
        }
        if (static_cast<int32_t>(out.expansions) >= budget) return out;
    }
    return out;
}

} // namespace navtest

#endif // FARLANDS_NAV_TEST_WORLD_HPP
