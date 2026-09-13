#ifndef FARLANDS_NAV_MOVE_GENERATOR_HPP
#define FARLANDS_NAV_MOVE_GENERATOR_HPP

// -----------------------------------------------------------------------------
// Movement primitives for the ground planner.
//
// A move is an edge between two feet cells with a cost. Legality is worked out
// from the target column's surface rather than from raw voxels, so the rules
// read the way an agent walks: level steps are cheap, climbing costs, dropping
// costs more the further it falls, and a one-cell gap can be hopped. Diagonals
// additionally require both columns the body squeezes between to be clear, so a
// route never clips a wall corner.
// -----------------------------------------------------------------------------

#include "pathfinding/nav_view.hpp"

#include <cmath>
#include <cstdint>

namespace VoxelEngine {
namespace nav {

struct NavMove {
    NavNode to{};
    float cost = 0.0f;
    MoveKind kind = MoveKind::Walk;
};

// Fixed-capacity output: the generator always emits at most 12 moves (8 steps
// plus 4 hops), so the search never allocates per expansion.
struct NavMoveList {
    static constexpr int kMax = 16;
    NavMove items[kMax];
    int count = 0;

    void add(const NavNode& to, float cost, MoveKind kind) noexcept {
        if (count < kMax) items[count++] = NavMove{to, cost, kind};
    }
};

class MoveGenerator {
public:
    // Costs are held by value so a generator can be built from a temporary.
    MoveGenerator(const NavView& view, NavCosts costs) : view_(view), costs_(costs) {}

    // One 8-connected step. Returns false when the step is illegal.
    [[nodiscard]] bool step(const NavNode& from, const NavView::Column& from_col,
                            int32_t dx, int32_t dz, NavMove& out) const {
        const NavCosts& c = costs_;
        const bool diagonal = (dx != 0 && dz != 0);
        if (diagonal) {
            // The body passes between these two columns; both must be
            // traversable at this height or the move clips a corner.
            if (!orthogonal_ok(from, from_col, dx, 0)) return false;
            if (!orthogonal_ok(from, from_col, 0, dz)) return false;
        }
        const NavView::Column* to_col = view_.column(from.x + dx, from.z + dz, from.y);
        if (!is_usable(to_col)) return false;

        const float dh = to_col->surface_top - from_col.surface_top;
        if (dh > c.max_rise + c.epsilon) return false;
        if (dh < -c.max_drop - c.epsilon) return false;

        float cost = diagonal ? c.diagonal : c.walk;
        if (dh > 0.0f) cost += c.step_up * dh;
        else if (dh < 0.0f) cost += c.fall * (-dh);
        if (to_col->liquid || from_col.liquid) cost += c.liquid;

        out.to = NavNode{from.x + dx, view_.feet_cell(to_col->surface_top), from.z + dz};
        out.cost = cost;
        out.kind = dh > c.epsilon ? MoveKind::StepUp
                 : (dh < -c.epsilon ? MoveKind::Drop : MoveKind::Walk);
        return true;
    }

    // Every legal move out of a node: the 8 connected steps, then a hop across
    // a one-cell gap in each cardinal direction.
    void generate(const NavNode& from, const NavView::Column& from_col, NavMoveList& out) const {
        static constexpr int32_t kOffsets[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };
        for (const auto& off : kOffsets) {
            NavMove m;
            if (step(from, from_col, off[0], off[1], m)) out.add(m.to, m.cost, m.kind);
        }
        static constexpr int32_t kCardinals[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& off : kCardinals) {
            NavMove m;
            if (hop(from, from_col, off[0], off[1], m)) out.add(m.to, m.cost, m.kind);
        }
    }

private:
    [[nodiscard]] static bool is_usable(const NavView::Column* c) noexcept {
        return c != nullptr && c->found && !c->unknown && c->clearance;
    }

    [[nodiscard]] bool orthogonal_ok(const NavNode& from, const NavView::Column& from_col,
                                     int32_t dx, int32_t dz) const {
        const NavView::Column* c = view_.column(from.x + dx, from.z + dz, from.y);
        if (!is_usable(c)) return false;
        const float dh = c->surface_top - from_col.surface_top;
        return dh <= costs_.max_rise + costs_.epsilon &&
               dh >= -costs_.max_drop - costs_.epsilon;
    }

    // Hop over a one-cell gap: the middle column has no usable surface, the
    // landing column beyond it does, and the launch space is unobstructed.
    [[nodiscard]] bool hop(const NavNode& from, const NavView::Column& from_col,
                           int32_t dx, int32_t dz, NavMove& out) const {
        const NavCosts& c = costs_;
        const int32_t mx = from.x + dx;
        const int32_t mz = from.z + dz;

        const NavView::Column* mid = view_.column(mx, mz, from.y);
        if (mid != nullptr && mid->found && !mid->unknown && mid->clearance &&
            mid->surface_top >= from_col.surface_top - c.max_drop - c.epsilon) {
            return false;  // it is not a gap — the plain step already covers it
        }
        const NavView::Column* far = view_.column(from.x + 2 * dx, from.z + 2 * dz, from.y);
        if (!is_usable(far)) return false;
        // The agent has to pass over the gap, so its body must fit there.
        if (!view_.body_fits(mx, mz, from_col.surface_top)) return false;

        const float dh = far->surface_top - from_col.surface_top;
        if (dh > c.max_rise + c.epsilon) return false;
        if (dh < -c.max_drop - c.epsilon) return false;

        float cost = c.jump;
        if (dh > 0.0f) cost += c.step_up * dh;
        else if (dh < 0.0f) cost += c.fall * (-dh) * 0.5f;
        if (far->liquid || from_col.liquid) cost += c.liquid;

        out.to = NavNode{from.x + 2 * dx, view_.feet_cell(far->surface_top), from.z + 2 * dz};
        out.cost = cost;
        out.kind = MoveKind::Hop;
        return true;
    }

    const NavView& view_;
    const NavCosts costs_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_MOVE_GENERATOR_HPP
