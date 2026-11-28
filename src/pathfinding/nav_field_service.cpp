#include "pathfinding/nav_field_service.hpp"

#include "pathfinding/chunk_nav_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace VoxelEngine {
namespace nav {

namespace {

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

int32_t clamp_y(int32_t y) {
    return std::clamp(y, 0, WORLD_HEIGHT_Y - 1);
}

} // namespace

NavFieldService::NavFieldService(const ChunkMap& map) : map_(map), state_(std::make_shared<State>()) {
    state_->window_start_ms = now_ms();
}

NavFieldService::~NavFieldService() = default;

std::vector<NavNode> NavFieldService::route(ThreadPool& pool, const NavNode& from,
                                            const NavNode& goal, int32_t count) {
    std::shared_ptr<const NavField> field;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const double now = now_ms();
        if (now - state_->window_start_ms >= 1000.0) {
            state_->window_start_ms = now;
            state_->wants = 0;
        }
        ++state_->wants;
        state_->max_wants = std::max(state_->max_wants, state_->wants);

        const bool moved =
            !state_->has_goal ||
            std::abs(goal.x - state_->goal.x) > kGoalMoveBlocks ||
            std::abs(goal.y - state_->goal.y) > kGoalMoveBlocks ||
            std::abs(goal.z - state_->goal.z) > kGoalMoveBlocks;
        const bool stale = (now - state_->built_ms) > kRebuildMs;
        // No wants, no builds. The threshold is what keeps a lone walker on its own A*: a field
        // costs a few milliseconds and one worker slot, and one chaser is not worth either.
        if (!state_->building && state_->wants >= static_cast<size_t>(kMinWants) &&
            (state_->field == nullptr || moved || stale)) {
            state_->building = true;
            state_->goal = goal;
            state_->has_goal = true;
            start = true;
        }
        field = state_->field;
    }

    if (start) start_build(pool, goal);

    std::vector<NavNode> out;
    if (field != nullptr) {
        field->route(from, static_cast<size_t>(std::max(count, 1)), out);
    }
    return out;
}

void NavFieldService::start_build(ThreadPool& pool, const NavNode& goal) {
    const NavBox box{goal.x - kHalfColumns, clamp_y(goal.y - kBelow), goal.z - kHalfColumns,
                     goal.x + kHalfColumns, clamp_y(goal.y + kAbove), goal.z + kHalfColumns};
    const NavCosts costs;
    auto source = std::make_shared<ChunkMapNavSource>(map_);
    std::shared_ptr<State> state = state_;

    pool.fire_and_forget(
        [state, goal, box, costs, source]() {
            // The same view a search builds: the ranged reader is what keeps a column at one lock
            // instead of one per cell, and it is released before every call returns.
            NavView view(box,
                         [source](int32_t x, int32_t y, int32_t z) { return source->sample(x, y, z); },
                         costs,
                         [source](int32_t x, int32_t z, int32_t y_lo, int32_t y_hi, Cell* out) {
                             source->read_column(x, z, y_lo, y_hi, out);
                         });
            auto field = std::make_shared<NavField>();
            const bool ok = field->build(view, costs, goal, box, NavField::Limits{});
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->building = false;
                state->builds += 1;
                state->built_ms = now_ms();
                // A field for a goal that cannot be anchored is no field: keeping the previous
                // one would walk this crowd to where the target used to stand.
                state->field = ok ? std::shared_ptr<const NavField>(field) : nullptr;
            }
        },
        PoolTaskClass::Field);
}

NavFieldService::Stats NavFieldService::get_stats() const {
    Stats out;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        out.builds = state_->builds;
        out.wants = state_->wants;
        out.max_wants = state_->max_wants;
        out.ready = state_->field != nullptr;
        out.building = state_->building;
        out.nodes = state_->field != nullptr ? state_->field->size() : 0;
        out.age_ms = state_->field != nullptr ? now_ms() - state_->built_ms : 0.0;
    }
    return out;
}

} // namespace nav
} // namespace VoxelEngine
