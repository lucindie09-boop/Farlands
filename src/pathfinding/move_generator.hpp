#ifndef FARLANDS_NAV_MOVE_GENERATOR_HPP
#define FARLANDS_NAV_MOVE_GENERATOR_HPP

// -----------------------------------------------------------------------------
// Movement primitives for the ground planner.
//
// A move is an edge between two feet cells with a cost. Legality is worked out
// from the target column's surface rather than from raw voxels, so the rules
// read the way an agent walks: level steps are cheap, climbing costs, dropping
// costs more the further it falls, and a one-cell gap can be hopped -- and the
// hop pays the same descent rate, so every fall costs `fall` a block. A diagonal
// is a corner the body passes, and it needs room on ONE side of it: two columns
// that both stop the body at its own height are a squeeze it cannot fit through,
// while a wall beside a hole -- or beside open ground -- leaves it the room to
// slide past, which is what a player does.
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

// One edge INTO a node, reversed: the cell it comes from and what walking it costs there. It is
// the same edge `generate` emits, read backwards, which is what lets a flow field be built by
// the same rules the search uses instead of a second copy of them.
struct NavPredecessor {
    NavNode from{};
    float cost = 0.0f;
};

// Fixed-capacity for the same reason as NavMoveList: 8 step directions and 4 hop directions, each
// tried from two scan hints. `add` deduplicates, because the two hints often name one column.
struct NavPredecessorList {
    static constexpr int kMax = 24;
    NavPredecessor items[kMax];
    int count = 0;

    void add(const NavNode& from, float cost) noexcept {
        for (int i = 0; i < count; ++i) {
            if (items[i].from == from) return;
        }
        if (count < kMax) items[count++] = NavPredecessor{from, cost};
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
            // ONE side of the corner has to be room the body can slant into. BOTH of them stop a
            // 0.6-wide body -- two blocks meeting at a corner are a squeeze with nowhere for it to
            // be -- and that is the only thing refused here. What a side needs is SPACE at the
            // move's height, not a floor: a corner a player crosses keeps him up by the cells his
            // box still overlaps, and the two beside-columns are exactly the ones he slants past.
            // Asking them for a standable surface instead (a step into it) looked right and was
            // wrong in the field: the player's own crossing has a hole beside the wall, so it was
            // refused, and from that cell the goal became unreachable -- `no route to the goal
            // column` with 4,338 nodes expanded, from a body ONE CELL from the player, over and
            // over, while the player watched it stand there (`nav_trace` 2026-11-25 23:43).
            // Footing is the walker's question, not the planner's: it slides along the open side
            // and refuses a foot it cannot stand on (`_slide_around`, `docs/ui.md`).
            if (!passes_beside(from, from_col, dx, 0) && !passes_beside(from, from_col, 0, dz)) {
                return false;
            }
        }
        const NavView::Column* to_col = view_.column(from.x + dx, from.z + dz, from.y);
        if (!is_usable(to_col)) return false;

        const float dh = to_col->surface_top - from_col.surface_top;
        if (dh > c.max_rise + c.epsilon) return false;
        if (dh < -c.max_drop - c.epsilon) return false;
        // ...and a swimmer does not climb OUT of the water. Its surface is the liquid's own top,
        // where the body rides, and the lift that carries it over a rim (the swim-into-a-wall
        // boost in the sim) exists only while it is still IN the liquid: a shore one block above
        // the water line is a wall it presses against for as long as its anger lasts. That was a
        // live stuck goblin, plan after identical plan (nav_trace, 2026-11-25 22:01: `next=
        // (-2,247,2)` bobbing at a staircase foot for ten seconds under a player on the stairs).
        // Rising INTO water is fine -- a body strokes up and there is nothing to jump -- so the
        // refusal is one direction only: wet to dry, uphill. A plan then says what the body can
        // do: the bank at the water line, a way round, or no route at all.
        if (from_col.wade && !to_col->wade && dh > c.epsilon) return false;

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

    // Every edge the forward generator can emit INTO `to`, reversed: for each of the 8 step
    // directions and 4 hop directions the candidate `from` column is resolved and then put through
    // the EXISTING `step`/`hop`, and the edge is kept only when the move it produces lands exactly
    // on `to`. Nothing about legality or cost is re-derived here -- that is the invariant a flow
    // field rests on, and the test walks every move `generate` emits to prove it.
    //
    // Two scan hints per direction, because the downward scan starts where it is told to: the
    // goal's own feet find a level or step-up source, and `to.y + ceil(max_drop)` finds a source
    // that DROPS into it (a one-way ledge, where the same column asked from below resolves to the
    // lower surface and the edge would be missed).
    void predecessors(const NavNode& to, NavPredecessorList& out) const {
        out.count = 0;
        static constexpr int32_t kOffsets[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };
        static constexpr int32_t kCardinals[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        const int32_t hints[2] = {
            to.y,
            to.y + static_cast<int32_t>(std::ceil(costs_.max_drop)),
        };
        for (const int32_t hint : hints) {
            for (const auto& off : kOffsets) {
                const int32_t cx = to.x - off[0];
                const int32_t cz = to.z - off[1];
                const NavView::Column* col = view_.column_at_feet(cx, cz, hint);
                if (!is_usable(col)) continue;
                const NavNode from{cx, view_.feet_cell(col->surface_top), cz};
                NavMove m;
                if (step(from, *col, off[0], off[1], m) && m.to == to) out.add(from, m.cost);
            }
            for (const auto& off : kCardinals) {
                const int32_t cx = to.x - 2 * off[0];
                const int32_t cz = to.z - 2 * off[1];
                const NavView::Column* col = view_.column_at_feet(cx, cz, hint);
                if (!is_usable(col)) continue;
                const NavNode from{cx, view_.feet_cell(col->surface_top), cz};
                NavMove m;
                if (hop(from, *col, off[0], off[1], m) && m.to == to) out.add(from, m.cost);
            }
        }
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

    // Can the body slant into this column as it passes the corner? The question is the body's own
    // SPACE at the move's height (`body_fits`), not whether the column is somewhere it could stand:
    // a column whose ground is four blocks up still gives a body room if its blocks start above it,
    // and a hole -- no floor, plenty of air -- is exactly the side a crossing slants past while the
    // body's own footprint stays on the cells it is crossing.
    //
    // A liquid no body may be in is never a way past: cutting a lava corner would be a shortcut
    // through a hazard that the plain steps refuse (see `is_usable`). The column is resolved once
    // through `view_.column`, which prefetches it -- sampling a few cells of a column the search has
    // not visited buys a ranged read per CELL, and the number of lock acquisitions a search takes is
    // a pinned budget (`test_chunk_nav_source.cpp`).
    [[nodiscard]] bool passes_beside(const NavNode& from, const NavView::Column& from_col,
                                     int32_t dx, int32_t dz) const {
        const int32_t x = from.x + dx;
        const int32_t z = from.z + dz;
        const NavView::Column* beside = view_.column(x, z, from.y);
        if (beside == nullptr) return false;
        if (beside->liquid && !beside->wade) return false;
        return view_.body_fits(x, z, from_col.surface_top);
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
