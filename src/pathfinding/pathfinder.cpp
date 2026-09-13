#include "pathfinding/pathfinder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace nav {

namespace {

struct NodeRec {
    float g = 0.0f;
    float h = 0.0f;
    uint64_t parent = 0;
    bool closed = false;
    bool has_parent = false;
};

struct Open {
    float f = 0.0f;
    float g = 0.0f;
    uint64_t key = 0;
};

// Min-heap on f, ties broken by node key so runs are reproducible.
struct OpenCmp {
    bool operator()(const Open& a, const Open& b) const noexcept {
        if (a.f != b.f) return a.f > b.f;
        return a.key > b.key;
    }
};

} // namespace

float Pathfinder::heuristic(const NavNode& a, const NavNode& b) const {
    const float dx = static_cast<float>(std::abs(a.x - b.x));
    const float dz = static_cast<float>(std::abs(a.z - b.z));
    const float min_d = std::min(dx, dz);
    const float max_d = std::max(dx, dz);
    float h = max_d + (costs_.diagonal - 1.0f) * min_d;  // octile over columns
    const int32_t dy = b.y - a.y;
    if (dy > 0) h += costs_.step_up * static_cast<float>(dy);
    else h += costs_.fall * static_cast<float>(-dy);
    return h;
}

NavPath Pathfinder::search(const NavQuery& query) const {
    NavPath path;
    last_error_.clear();

    // Started before the endpoints are resolved: their column scans are part of
    // the query's cost, so the budget has to cover them too.
    const auto start_time = std::chrono::steady_clock::now();
    const double budget_ms = query.max_ms;

    const NavView::Column* start_col = view_.column(query.start.x, query.start.z, query.start.y);
    if (start_col == nullptr || !start_col->found || start_col->unknown || !start_col->clearance) {
        last_error_ = "start column has no navigable surface";
        return path;
    }
    const NavView::Column* goal_col = view_.column(query.goal.x, query.goal.z, query.goal.y);
    if (goal_col == nullptr || !goal_col->found || goal_col->unknown || !goal_col->clearance) {
        last_error_ = "goal column has no navigable surface";
        return path;
    }

    // Re-anchor both ends onto their column surfaces: callers may hand us an
    // airborne position (a falling entity, a target standing above ground).
    const NavNode start{query.start.x, view_.feet_cell(start_col->surface_top), query.start.z};
    const NavNode goal{query.goal.x, view_.feet_cell(goal_col->surface_top), query.goal.z};
    const uint64_t start_key = node_key(start.x, start.y, start.z);

    const size_t columns_before = view_.columns_resolved();
    const int32_t budget = std::max(1, query.max_expansions);

    std::unordered_map<uint64_t, NodeRec> nodes;
    nodes.reserve(static_cast<size_t>(std::min(budget, 1 << 16)) * 2);

    std::priority_queue<Open, std::vector<Open>, OpenCmp> open;
    const float start_h = heuristic(start, goal);
    nodes.emplace(start_key, NodeRec{0.0f, start_h * query.weight, 0, false, false});
    open.push(Open{start_h * query.weight, 0.0f, start_key});

    uint64_t best_key = start_key;
    float best_h = start_h;
    uint64_t found_key = 0;
    bool found = false;
    bool time_up = false;
    size_t expansions = 0;

    while (!open.empty()) {
        // The clock is read every 64 expansions — a call per expansion would cost
        // more than it saves, and 64 expansions of overrun is well under a
        // millisecond even on a slow machine.
        if (budget_ms > 0.0 && (expansions & 63u) == 0u) {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - start_time)
                                          .count();
            if (elapsed_ms >= budget_ms) {
                time_up = true;
                break;
            }
        }

        const Open top = open.top();
        open.pop();

        const auto cur_it = nodes.find(top.key);
        if (cur_it == nodes.end() || cur_it->second.closed) continue;
        if (top.g > cur_it->second.g + costs_.epsilon) continue;  // stale heap entry
        cur_it->second.closed = true;
        ++expansions;

        const NavNode cur = node_from_key(top.key);
        if (cur.x == goal.x && cur.z == goal.z && (!query.exact_goal_y || cur.y == goal.y)) {
            found = true;
            found_key = top.key;
            break;
        }

        const float h_cur = heuristic(cur, goal);
        if (h_cur < best_h) {
            best_h = h_cur;
            best_key = top.key;
        }

        const NavView::Column* col = view_.column(cur.x, cur.z, cur.y);
        if (col != nullptr && col->found && !col->unknown && col->clearance) {
            NavMoveList moves;
            generator_.generate(cur, *col, moves);
            for (int i = 0; i < moves.count; ++i) {
                const NavMove& m = moves.items[i];
                const uint64_t key = node_key(m.to.x, m.to.y, m.to.z);
                const float g = cur_it->second.g + m.cost;

                auto [nit, inserted] =
                    nodes.try_emplace(key, NodeRec{g, 0.0f, top.key, false, true});
                if (!inserted) {
                    if (nit->second.closed) continue;
                    if (g >= nit->second.g - costs_.epsilon) continue;
                    nit->second.g = g;
                    nit->second.parent = top.key;
                    nit->second.has_parent = true;
                }
                nit->second.h = heuristic(m.to, goal) * query.weight;
                open.push(Open{g + nit->second.h, g, key});
            }
        }

        if (static_cast<int32_t>(expansions) >= budget) break;
    }

    // Stats first: `truncated` means a budget ran out, which is different from a
    // search that explored the whole reachable graph and genuinely found no
    // route. `found` and `time_up` are mutually exclusive — the goal test breaks
    // out of the loop before the next budget check.
    path.stats.expansions = expansions;
    path.stats.columns_resolved = view_.columns_resolved() - columns_before;
    path.stats.time_exhausted = time_up;
    path.stats.budget_exhausted =
        (!found && !time_up && static_cast<int32_t>(expansions) >= budget);
    path.found = found;
    path.truncated = !found && (path.stats.budget_exhausted || time_up);

    const uint64_t end_key = found ? found_key : best_key;

    std::vector<NavNode> reversed;
    reversed.reserve(64);
    uint64_t key = end_key;
    for (size_t guard = 0; guard <= nodes.size() + 4; ++guard) {
        reversed.push_back(node_from_key(key));
        const auto it = nodes.find(key);
        if (it == nodes.end() || !it->second.has_parent) break;
        key = it->second.parent;
    }
    std::reverse(reversed.begin(), reversed.end());
    if (reversed.empty() || !(reversed.front() == start)) {
        // A best-effort run must still start where the agent is standing.
        reversed.insert(reversed.begin(), start);
    }
    path.nodes = std::move(reversed);

    if (!found) {
        if (time_up) last_error_ = "time budget exhausted";
        else if (path.stats.budget_exhausted) last_error_ = "expansion budget exhausted";
        else last_error_ = "no route to the goal column";
    }
    return path;
}

} // namespace nav
} // namespace VoxelEngine
