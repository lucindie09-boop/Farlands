#include "pathfinding/path_service.hpp"

#include "pathfinding/pathfinder.hpp"
#include "pathfinding/path_smoother.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace VoxelEngine {
namespace nav {

namespace {

// Slack around the two endpoints for the planner's window. A route that has to
// detour around an obstacle stays inside this, and the window costs nothing
// unless the search actually visits a column inside it.
constexpr int32_t kBoxPad = 32;

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

} // namespace

PathService::PathService(const ChunkMap& map, ThreadPool& pool)
    : map_(map), pool_(pool), state_(std::make_shared<State>()) {}

PathService::~PathService() = default;

uint64_t PathService::submit(const NavNode& from, const NavNode& to, int32_t max_expansions) {
    const uint64_t id = state_->next_id.fetch_add(1, std::memory_order_relaxed);

    NavQuery query;
    query.start = from;
    query.goal = to;
    query.max_expansions = std::max(1, max_expansions);
    query.weight = 1.0f;

    auto source = std::make_shared<ChunkMapNavSource>(map_);
    std::shared_ptr<State> state = state_;
    state->pending.fetch_add(1, std::memory_order_relaxed);
    pool_.fire_and_forget([state, id, source, query]() {
        run(state, id, source, query);
    });
    return id;
}

void PathService::run(const std::shared_ptr<State>& state, uint64_t id,
                      std::shared_ptr<ChunkMapNavSource> source, NavQuery query) {
    const double start_ms = now_ms();

    const int32_t min_x = std::min(query.start.x, query.goal.x) - kBoxPad;
    const int32_t max_x = std::max(query.start.x, query.goal.x) + kBoxPad;
    const int32_t min_z = std::min(query.start.z, query.goal.z) - kBoxPad;
    const int32_t max_z = std::max(query.start.z, query.goal.z) + kBoxPad;
    const NavBox box{min_x, 0, min_z, max_x, WORLD_HEIGHT_Y - 1, max_z};

    const NavCosts costs;
    NavView view(box, [source](int32_t x, int32_t y, int32_t z) {
        return source->sample(x, y, z);
    }, costs);

    Pathfinder finder(view, costs);
    NavPath path = finder.search(query);
    const double search_ms = now_ms() - start_ms;

    PathResult result;
    result.id = id;
    result.found = path.found;
    result.truncated = path.truncated;
    result.budget_exhausted = path.stats.budget_exhausted;
    result.error = finder.last_error();
    result.search_ms = search_ms;
    result.stats = path.stats;

    // Emit support blocks: the block under each node's feet is what an overlay
    // wants to highlight, and what an agent needs to stand on.
    auto to_support = [&view](const std::vector<NavNode>& nodes, std::vector<NavNode>& out) {
        out.reserve(nodes.size());
        for (const NavNode& node : nodes) {
            const NavView::Column* column = view.column(node.x, node.z, node.y);
            const int32_t support_y = (column != nullptr && column->found)
                                          ? column->surface_cell
                                          : node.y - 1;
            out.push_back(NavNode{node.x, support_y, node.z});
        }
    };
    to_support(path.nodes, result.nodes);
    if (!path.nodes.empty()) {
        PathSmoother smoother(view, costs);
        const std::vector<NavNode> waypoints = smoother.smooth(path.nodes);
        to_support(waypoints, result.waypoints);
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done.push_back(std::move(result));
    }
    state->finished.fetch_add(1, std::memory_order_relaxed);
    state->pending.fetch_sub(1, std::memory_order_relaxed);
}

std::vector<PathResult> PathService::poll() {
    std::vector<PathResult> out;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        out.swap(state_->done);
    }
    return out;
}

size_t PathService::pending() const noexcept {
    return state_->pending.load(std::memory_order_relaxed);
}

size_t PathService::finished_count() const noexcept {
    return state_->finished.load(std::memory_order_relaxed);
}

} // namespace nav
} // namespace VoxelEngine
