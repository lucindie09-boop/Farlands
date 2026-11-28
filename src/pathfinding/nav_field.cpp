#include "pathfinding/nav_field.hpp"

#include "pathfinding/pathfinder.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <utility>

namespace VoxelEngine {
namespace nav {

namespace {

// A node's support block is the canvas the world's own column answers, so it is asked once per
// node rather than stored by `predecessors` (which answers edges, not cells). The column is
// already in the view's memo — `predecessors` resolved it to build the edge — so this is a lookup.
NavNode support_of(const NavView& view, const NavNode& feet) {
    const NavView::Column* col = view.column_at_feet(feet.x, feet.z, feet.y);
    if (col != nullptr) return NavNode{feet.x, col->surface_cell, feet.z};
    return NavNode{feet.x, feet.y - 1, feet.z};
}

} // namespace

bool NavField::build(const NavView& view, const NavCosts& costs, const NavNode& goal,
                     const NavBox& box, const Limits& limits) {
    cells_.clear();
    goal_ = NavNode{};

    const GoalAnchor anchor = resolve_goal(view, goal);
    if (anchor.column == nullptr || !anchor.column->found || anchor.column->unknown ||
        !anchor.column->clearance) {
        return false;
    }
    goal_ = NavNode{anchor.x, view.feet_cell(anchor.column->surface_top), anchor.z};
    const uint64_t goal_key = node_key(goal_.x, goal_.y, goal_.z);

    cells_.reserve(static_cast<size_t>(std::min(std::max(limits.max_nodes, 1), 16384)));
    cells_.emplace(goal_key, NavFieldCell{goal_, support_of(view, goal_), 0.0f, 0, false});

    MoveGenerator gen(view, costs);
    using Entry = std::pair<float, uint64_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    open.push(Entry{0.0f, goal_key});

    while (!open.empty()) {
        const auto [cost, key] = open.top();
        open.pop();
        const auto it = cells_.find(key);
        if (it == cells_.end()) continue;
        if (cost > it->second.cost + costs.epsilon) continue;  // a stale heap entry
        const NavNode cur = it->second.feet;

        if (limits.max_nodes > 0 && static_cast<int32_t>(cells_.size()) >= limits.max_nodes) break;

        NavPredecessorList preds;
        gen.predecessors(cur, preds);
        for (int i = 0; i < preds.count; ++i) {
            const NavNode& from = preds.items[i].from;
            if (from.x < box.min_x || from.x > box.max_x || from.z < box.min_z ||
                from.z > box.max_z || from.y < box.min_y || from.y > box.max_y) {
                continue;
            }
            const float next_cost = cost + preds.items[i].cost;
            if (limits.max_cost > 0.0f && next_cost > limits.max_cost) continue;

            const uint64_t from_key = node_key(from.x, from.y, from.z);
            const auto found = cells_.find(from_key);
            if (found != cells_.end()) {
                if (next_cost >= found->second.cost - costs.epsilon) continue;
                found->second.cost = next_cost;
                found->second.next = key;
                found->second.has_next = true;
            } else {
                if (limits.max_nodes > 0 &&
                    static_cast<int32_t>(cells_.size()) >= limits.max_nodes) {
                    break;
                }
                cells_.emplace(from_key,
                               NavFieldCell{from, support_of(view, from), next_cost, key, true});
            }
            open.push(Entry{next_cost, from_key});
        }
    }
    return !cells_.empty();
}

bool NavField::contains(const NavNode& from) const {
    return cells_.find(node_key(from.x, from.y, from.z)) != cells_.end();
}

const NavFieldCell* NavField::find(const NavNode& from) const {
    const auto it = cells_.find(node_key(from.x, from.y, from.z));
    return it == cells_.end() ? nullptr : &it->second;
}

size_t NavField::route(const NavNode& from, size_t max_cells, std::vector<NavNode>& out) const {
    out.clear();
    if (max_cells == 0) return 0;
    const auto start = cells_.find(node_key(from.x, from.y, from.z));
    if (start == cells_.end()) return 0;

    // Every node has one `next` and the field was built away from the goal, so this cannot loop.
    // The guard is the table's own size: a follow longer than that would be a bug here, and a
    // caller must never be handed an unbounded walk.
    const size_t limit = std::min(max_cells, cells_.size() + 1);
    const NavFieldCell* cell = &start->second;
    for (size_t i = 0; i < limit; ++i) {
        out.push_back(cell->support);
        if (!cell->has_next) break;
        const auto next = cells_.find(cell->next);
        if (next == cells_.end()) break;
        cell = &next->second;
    }
    return out.size();
}

} // namespace nav
} // namespace VoxelEngine
