#ifndef FARLANDS_NAV_MOVE_GENERATOR_HPP
#define FARLANDS_NAV_MOVE_GENERATOR_HPP

// -----------------------------------------------------------------------------
// Movement primitives for the ground planner.
//
// A move is an edge between two feet cells with a cost. Legality is worked out
// from the target column's surface rather than from raw voxels, so the rules
// read the way an agent walks: level steps are cheap, climbing costs, dropping
// costs more the further it falls, and a one-cell gap can be hopped -- and the
// hop pays the same descent rate, so every fall costs `fall` a block. Diagonals
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
    // A route may not lead into a liquid the body cannot be in. A cost is not a wall -- the old
    // +0.6 surcharge was read as "wade anyway" and a goblin walked into lava and acid -- so the
    // refusal is on the CELL: lava and acid have no body in them and are walls, whatever the
    // numbers say. Water is the other case, and the reason the flag exists at all: a body floats
    // in it and swims, so a wet column is a place a route may cross, at the surcharge (`liquid`)
    // that is what "slow" costs. That is also what the reference's own mobs do -- they swim at
    // what they are chasing and cross the puddle in front of them.
    [[nodiscard]] static bool is_usable(const NavView::Column* c) noexcept {
        return c != nullptr && c->found && !c->unknown && c->clearance &&
               (!c->liquid || c->wade);
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
        // A gap is not a hazard. A liquid column has no body space over it, so it read as "a
        // one-cell gap" and a route was found straight across a channel of lava: the goblin
        // jumped, landed in it, and stood in the channel while the player watched -- and because
        // a route existed it never concluded the player was unreachable, so it never filled the
        // channel either. You cannot hop over lava; that cell is not a gap to be cleared.
        if (mid != nullptr && !mid->unknown && mid->liquid) return false;
        if (mid != nullptr && mid->found && !mid->unknown && mid->clearance &&
            mid->surface_top >= from_col.surface_top - c.max_drop - c.epsilon) {
            return false;  // it is not a gap — the plain step already covers it
        }
        const NavView::Column* far = view_.column(from.x + 2 * dx, from.z + 2 * dz, from.y);
        if (!is_usable(far)) return false;
        // The agent has to pass over the gap, so its body must fit there.
        if (!view_.body_fits(mx, mz, from_col.surface_top)) return false;

        const float dh = far->surface_top - from_col.surface_top;
        // A hop does not climb, at any amount. The jump is pressed off the lip of the gap and
        // has to carry the body across while it is airborne: at the pace the walker moves (a
        // mob's, 0.6 of the player's) that arc covers 0.9 blocks against the 0.7 a one-cell gap
        // needs, and every block of rise spends that clearance on the climb instead -- measured
        // on the sim the walker drives, a hop up one block lands against the far wall and drops
        // into the gap at every launch distance, while the same hop at a sprint clears. A rising
        // gap is reached by a step up beside it, so the search goes round.
        if (dh > c.epsilon) return false;
        if (dh < -c.max_drop - c.epsilon) return false;

        // The jump's own price, plus the descent at the plain drop's rate. A discount for the
        // jump carrying the body was a guess, and it bought nothing: the heuristic has to bound
        // this move too, so the cheaper rate forced the estimate down with it, and that made the
        // estimate so loose that mid-range queries tripled their expansions (65 -> 257 at 64
        // blocks) to stay admissible against a move a route rarely ends on. One rate for every
        // descent keeps the estimate tight and still never above the route.
        float cost = c.jump;
        if (dh < 0.0f) cost += c.fall * (-dh);
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
