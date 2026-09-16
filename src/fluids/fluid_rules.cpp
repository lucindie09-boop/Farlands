#include "fluids/fluid_rules.hpp"

#include <algorithm>

namespace VoxelEngine {
namespace fluids {

const FluidTraits* traits_for(FluidKind kind) noexcept {
    // Water is the fluid the whole system was designed around, so the struct's
    // defaults are its numbers.
    static const FluidTraits kWater{};

    // Lava: the slow, shallow cousin of water — three cells deep instead of
    // seven, and a second between one cell and the next, so a poured pool
    // creeps outward and then sits there. It also looks less far for a drop. It
    // keeps the source-pair rule, which is what stops a wide pool draining away
    // again.
    //                       decay, depth, tick delay, search, pools
    static const FluidTraits kLava{ 1, 3, 20, 3, true };

    // Acid: water's depth, but quicker cell to cell, and it looks one step
    // further for a drop than water does — it would rather run for the drain
    // than pool where it landed. It is also the one substance that does NOT
    // pool from source pairs: two buckets of acid do not conjure a third
    // source, so a splash spends itself and drains.
    //                       decay, depth, tick delay, search, pools
    static const FluidTraits kAcid{ 1, 7, 3, 5, false };

    switch (kind) {
        case FluidKind::Water: return &kWater;
        case FluidKind::Lava: return &kLava;
        case FluidKind::Acid: return &kAcid;
        case FluidKind::None: break;
    }
    return nullptr;
}

namespace {

// The four horizontal directions. Index i and (i ^ 1) are opposites — that one
// bit is what lets the recursive search step back the way it came.
constexpr int kDx[4] = { 1, -1, 0, 0 };
constexpr int kDz[4] = { 0, 0, 1, -1 };

// The "no drop reachable" score. Every real distance is smaller than this, so a
// cell that cannot see a drop keeps all of its walkable directions rather than
// none of them.
constexpr int kNoDrop = 1000;

// Fluid may move into a cell nothing is stopping it in that holds no fluid yet.
// Displacing another fluid is mixing (see the header), so for now a cell with
// any fluid in it is simply not writable.
[[nodiscard]] bool can_receive(const FluidWorld& world, int x, int y, int z) noexcept {
    if (world.blocked(x, y, z)) return false;
    return !world.fluid_at(x, y, z).present();
}

// Cells the drop-seeking search may walk through.
[[nodiscard]] bool walkable(const FluidWorld& world, int x, int y, int z, FluidKind kind) noexcept {
    if (world.blocked(x, y, z)) return false;
    const FluidCell c = world.fluid_at(x, y, z);
    // A source is where this fluid already is at full strength, so it is not a
    // route toward a drop. Any other cell — air, another fluid, runoff of this
    // kind — can be walked through.
    return c.kind != kind || !c.is_source();
}

// Steps from (x, y, z) to the nearest cell this fluid could pour out of, looking
// at most `search_distance` steps and never doubling straight back on itself.
// kNoDrop when there is none in range.
//
// This is the whole of the directional preference, and it is the expensive part
// of a tick: the recursion is bounded to floor(3^search_distance) cells per
// call, and a cell makes at most four calls. `tools/`-style measurement in the
// tests pins that bound, because an unbounded version of this search is what
// makes an ocean tick cost minutes.
[[nodiscard]] int drop_distance(const FluidWorld& world, FluidKind kind, int x, int y, int z,
                                int distance, int avoid, int search_distance) noexcept {
    int best = kNoDrop;
    for (int i = 0; i < 4; ++i) {
        if (i == avoid) continue;
        const int nx = x + kDx[i];
        const int nz = z + kDz[i];
        if (!walkable(world, nx, y, nz, kind)) continue;
        // Open air under this neighbour: it is a drop, and nothing closer can
        // beat a step we are already standing next to.
        if (!world.blocked(nx, y - 1, nz)) return distance;
        if (distance < search_distance) {
            best = std::min(best,
                            drop_distance(world, kind, nx, y, nz, distance + 1, i ^ 1, search_distance));
        }
    }
    return best;
}

// Which sideways directions this cell should spread into: the ones whose
// distance to the nearest drop is smallest, ties included. Returns the count and
// fills `out`.
//
// The two passes are not a micro-optimisation, they are the difference between a
// flood being affordable and not. A score of 0 — "this side drops straight
// off" — is the minimum a direction can have, and only the cheap check can
// produce it, so as soon as one direction scores 0 the answer is exactly the
// directions that score 0 and no search is needed at all. Fluid at a cliff edge
// or beside a hole is the case that actually churns during a flood, and it costs
// four reads a direction instead of the full search. The search runs only when
// no neighbour can drop at all, which is the case measured in the tests at
// ~1400 reads for one cell (constant, never world-sized — see the bound test).
[[nodiscard]] int best_directions(const FluidWorld& world, FluidKind kind, int x, int y, int z,
                                  const FluidTraits& traits, bool out[4]) noexcept {
    for (int i = 0; i < 4; ++i) out[i] = false;

    int count = 0;
    for (int i = 0; i < 4; ++i) {
        const int nx = x + kDx[i];
        const int nz = z + kDz[i];
        if (!walkable(world, nx, y, nz, kind)) continue;
        if (!world.blocked(nx, y - 1, nz)) {
            out[i] = true;
            ++count;
        }
    }
    if (count > 0) return count;  // 0 is unbeatable: nothing to search for

    int best = kNoDrop;
    for (int i = 0; i < 4; ++i) {
        const int nx = x + kDx[i];
        const int nz = z + kDz[i];
        if (!walkable(world, nx, y, nz, kind)) continue;
        const int score = drop_distance(world, kind, nx, y, nz, 1, i ^ 1, traits.search_distance);
        if (score < best) {
            // A strictly better direction discards every earlier winner.
            best = score;
            count = 0;
            for (int j = 0; j < 4; ++j) out[j] = false;
        }
        if (score <= best) {
            out[i] = true;
            ++count;
        }
    }
    return count;
}

// A cell counts as a source for the spring-formation rule only when it really
// is one (not merely a cell with depth 0 that is falling).
[[nodiscard]] bool source_at(const FluidWorld& world, int x, int y, int z, FluidKind kind) noexcept {
    const FluidCell c = world.fluid_at(x, y, z);
    return c.kind == kind && c.is_source();
}

} // namespace

FluidStep tick(const FluidWorld& world, int x, int y, int z) noexcept {
    FluidStep step;

    const FluidCell current = world.fluid_at(x, y, z);
    if (!current.present()) return step;  // nothing here to tick
    const FluidTraits* traits = traits_for(current.kind);
    if (traits == nullptr) return step;   // this kind does not flow yet

    // ---- 1. what this cell becomes on its own ------------------------------
    if (current.is_source()) {
        step.next = current;  // a source is permanent: never thins, never dries
    } else {
        int min_feed = -1;  // -1 = no neighbour of this fluid feeds it
        int sources = 0;
        for (int i = 0; i < 4; ++i) {
            const FluidCell n = world.fluid_at(x + kDx[i], y, z + kDz[i]);
            if (n.kind != current.kind) continue;
            if (n.is_source()) ++sources;
            const int feed = n.feed_cost();
            min_feed = min_feed < 0 ? feed : std::min(min_feed, feed);
        }

        // Nothing beside this cell feeds it, or the supply is thinner than one
        // more step reaches. That is not the end of the decision: the two rules
        // below can each save it, and that is not an accident of ordering. A
        // cell under a waterfall has NO side supply at all and must still
        // become falling rather than dry up, or a column would delete itself on
        // the way down.
        bool dry = min_feed < 0 || min_feed + traits->decay_per_step > traits->max_depth;
        if (!dry) {
            step.next = FluidCell{ current.kind,
                                   static_cast<uint8_t>(min_feed + traits->decay_per_step),
                                   false };
        }

        // Fluid directly above makes this a falling cell: full strength, depth
        // discarded, fed from above rather than from the side.
        if (world.fluid_at(x, y + 1, z).kind == current.kind) {
            step.next = FluidCell{ current.kind, 0, true };
            dry = false;
        }

        // Two side sources over something to sit on turn this cell into a source
        // itself. This runs last, so it beats both verdicts above — which is how
        // a poured pool becomes self-sustaining.
        if (sources >= 2 && traits->sources_pair_into_source &&
            (world.blocked(x, y - 1, z) || source_at(world, x, y - 1, z, current.kind))) {
            step.next = FluidCell{ current.kind, 0, false };
            dry = false;
        }

        if (dry) {
            step.remove = true;
            // A removal is the loudest wake-up there is, so it reports changed
            // as well: every neighbour is about to lose a supplier. A cell on
            // its way out pushes nothing (see the header: this is one of the
            // two deliberate departures from the reference).
            step.changed = true;
            return step;
        }
    }

    step.changed = step.next != current;

    // ---- 2. where it pushes fluid ------------------------------------------
    if (can_receive(world, x, y - 1, z)) {
        // Down always wins, and a cell that pours downward does not also fan out
        // sideways: the fluid goes where gravity takes it first.
        step.writes[step.write_count++] = CellWrite{ x, y - 1, z, FluidCell{ current.kind, 0, true } };
        return step;
    }

    // Sideways needs a reason: either this is a source (which always spreads), or
    // the cell below is blocked, so the fluid has nowhere to go but across.
    // Fluid with fluid under it just sits there.
    if (!step.next.is_source() && !world.blocked(x, y - 1, z)) return step;

    // A falling cell spreads at full strength (depth 1), which is what fans
    // water out where a waterfall lands; anything else spreads one step thinner
    // than itself.
    const int spread_depth = step.next.falling ? 1 : step.next.depth + traits->decay_per_step;
    if (spread_depth > traits->max_depth) return step;  // already as thin as it gets

    bool directions[4] = { false, false, false, false };
    if (best_directions(world, current.kind, x, y, z, *traits, directions) == 0) return step;
    for (int i = 0; i < 4; ++i) {
        if (!directions[i]) continue;
        const int nx = x + kDx[i];
        const int nz = z + kDz[i];
        if (!can_receive(world, nx, y, nz)) continue;
        step.writes[step.write_count++] =
            CellWrite{ nx, y, nz, FluidCell{ current.kind, static_cast<uint8_t>(spread_depth), false } };
    }
    return step;
}

} // namespace fluids
} // namespace VoxelEngine
