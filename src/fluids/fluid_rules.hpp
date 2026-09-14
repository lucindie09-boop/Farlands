#ifndef FARLANDS_FLUID_RULES_HPP
#define FARLANDS_FLUID_RULES_HPP

// -----------------------------------------------------------------------------
// Fluid flow — the rules, and nothing else.
//
// This module answers exactly two questions about one cell:
//
//   1. On its next tick, what does this cell become? (or: does it go away?)
//   2. Which neighbouring cells does it push fluid into, and at what strength?
//
// It never reads or writes a world. Everything it needs to know comes through
// `FluidWorld`, which the caller implements — the game backs it with the chunk
// map, the tests back it with a grid in a vector. Nothing here touches Godot,
// so it links into the standalone test binary. Scheduling (when a cell is due,
// how many cells a frame may touch) is deliberately NOT here: that is the tick
// driver's job, and keeping the two apart is what lets these rules be tested
// against a naive "recompute everything" driver.
//
// -----------------------------------------------------------------------------
// The rules
// -----------------------------------------------------------------------------
//
// A fluid cell is either a SOURCE (a spring: permanent, never dries, never
// thins) or it is RUNOFF, whose depth says how many steps it is from the source
// that fed it, or it is FALLING (part of a column pouring downward, full
// strength the whole way, depth not meaningful).
//
// On each tick a cell recomputes itself, in this order:
//
//   1. A source stays a source. Nothing else about it changes, ever.
//   2. Otherwise, look at the four horizontal neighbours that hold the same
//      fluid: a neighbour's FEED COST is 0 if it is a source or falling and its
//      depth otherwise. Take the smallest. The new depth is that plus the
//      fluid's decay-per-cell.
//   3. No such neighbour, or the new depth passes the fluid's maximum, and the
//      cell DRIES UP — it is removed. A cell never thickens back in place.
//   4. If the cell directly ABOVE holds the same fluid, the cell is FALLING
//      instead: full strength, and its depth is discarded. That is how a
//      column stays full all the way down a cliff face, and note both halves of
//      it: falling comes only from ABOVE (fluid sitting on top of fluid does
//      not make the upper cell falling), and this verdict overrides the dry-up
//      in 3, because a cell in a column has no side supply at all.
//   5. If two or more horizontal neighbours are SOURCES and the cell has
//      something solid directly beneath it (or a source beneath it), the cell
//      becomes a source itself. That is the rule that makes a poured pool keep
//      feeding itself, and it is the reason two springs a block apart fill the
//      gap between them.
//
// Then it pushes fluid outward:
//
//   6. Down always wins. If the cell below can receive fluid, the cell writes a
//      FALLING cell there and stops — it does not also spread sideways.
//   7. Otherwise sideways may happen, but only from a source, or when the cell
//      below is blocked (fluid with fluid under it just sits there). The depth
//      written is the cell's own depth plus the decay; a FALLING cell writes
//      depth 1, which is what makes water fan out at full strength where a
//      waterfall lands. A cell already at the maximum depth writes nothing.
//   8. Which sideways directions get that write is the other half of the
//      behaviour: a bounded search (4 steps, all four ways, never doubling back)
//      measures the distance from each candidate neighbour to the nearest place
//      fluid could drop from, and the cell spreads ONLY toward the best-scoring
//      directions — every direction tied for best, none of the worse ones. That
//      is what makes a stream run for a hole instead of blobbing outward as a
//      square, and it is why a cell beside a hole does not fill the flat ground
//      around it.
//
// Two deliberate departures from the reference this was modelled on, both of
// which make the result simpler rather than different in any visible way:
//
//   * A cell that is drying up pushes nothing. The reference still wrote a
//     full-ish cell downward in that case, which is an artefact of how it
//     ordered its two decisions, not a behaviour worth reproducing.
//   * Two different fluids never displace each other. The reference lets one
//     flow into the other; mixing is its own feature (and needs a table of what
//     each pair makes), so until that exists a cell holding any fluid can't be
//     written into.
// -----------------------------------------------------------------------------

#include <array>
#include <cstdint>
#include <optional>

namespace VoxelEngine {
namespace fluids {

// Which fluid a cell holds. How far it spreads and how fast it ticks differ per
// kind (see traits_for), so the rules never hardcode water's numbers.
enum class FluidKind : uint8_t {
    None = 0,   // not a fluid
    Water = 1,
    Lava = 2,   // no traits yet — traits_for returns null, so it does not flow
    Acid = 3    // no traits yet
};

// One cell's whole fluid state. It is small and trivially copyable because the
// driver stores and compares these: the state is the entire interface between
// the rules and whatever holds the cells (in phase 3, a block id per state).
struct FluidCell {
    FluidKind kind = FluidKind::None;
    // 1..max_depth = runoff, that many steps from the source that fed it.
    // 0 = full strength: either a source or a falling cell.
    uint8_t depth = 0;
    // Arrived by falling: full strength, `depth` is not meaningful. Falling is
    // one state, not eight — nothing ever reads the depth of a falling cell,
    // because everything that looks at it treats it as a source for supply.
    bool falling = false;

    [[nodiscard]] bool present() const noexcept { return kind != FluidKind::None; }

    [[nodiscard]] bool is_source() const noexcept {
        return present() && depth == 0 && !falling;
    }

    // What this cell supplies a horizontal neighbour: full strength (0) if it is
    // a source or falling, its distance from a source otherwise.
    [[nodiscard]] int feed_cost() const noexcept { return falling ? 0 : depth; }

    friend bool operator==(const FluidCell& a, const FluidCell& b) noexcept {
        return a.kind == b.kind && a.depth == b.depth && a.falling == b.falling;
    }
    friend bool operator!=(const FluidCell& a, const FluidCell& b) noexcept {
        return !(a == b);
    }
};

// Per-kind numbers. The defaults are water's; water's spread is the one the
// whole system was designed around (7 steps of runoff, 0.25 s per tick).
struct FluidTraits {
    // Added to the best neighbour's feed cost to get this cell's depth.
    int decay_per_step = 1;
    // Depths above this never exist: a cell that would reach it dries up
    // instead, and a cell already at it spreads no further sideways.
    int max_depth = 7;
    // Game ticks (20 per second) between one cell's updates.
    int tick_delay = 5;
    // How far the drop-seeking search may look, in steps.
    int search_distance = 4;
};

// Traits for a kind, or null when that fluid does not flow yet.
[[nodiscard]] const FluidTraits* traits_for(FluidKind kind) noexcept;

// What the rules need to ask the world. Coordinates are block cells; y grows
// upward, so "below" is y - 1.
//
// Callers must be explicit about cells they cannot answer for: an unloaded
// chunk is not air, and reporting it as a free cell lets fluid pour into space
// that will be regenerated later, discarding the writes. That is a property of
// the implementation, not of the rules, which is why it is called out here.
class FluidWorld {
public:
    virtual ~FluidWorld() = default;

    // The fluid at a cell, or a default-constructed (kind None) cell when there
    // is none.
    [[nodiscard]] virtual FluidCell fluid_at(int x, int y, int z) const = 0;

    // True when something is there that fluid cannot occupy and cannot pass:
    // a solid block, or a non-full one that still fills the path. Air and
    // replaceable decoration are not blocked.
    [[nodiscard]] virtual bool blocked(int x, int y, int z) const = 0;
};

// One cell the rules want written, and what to write there.
struct CellWrite {
    int x = 0;
    int y = 0;
    int z = 0;
    FluidCell state{};
};

// Everything one cell's tick decided. Applying it is the driver's job: it is
// exactly two things, "the cell itself changed" and "here is a list of writes".
struct FluidStep {
    // The cell stops being fluid — nothing feeds it any more, or it is too far
    // from its source.
    bool remove = false;
    // What the cell becomes. Not meaningful when `remove` is true.
    FluidCell next{};
    // True when `next` differs from what the cell already was. The cell needs
    // rewriting, and its neighbours need re-evaluating — that is the wake-up
    // edge. A cell that reports unchanged is settled: it can still push fluid
    // outward, but nothing about it will change anyone else's mind again.
    bool changed = false;
    // Cells this cell pushes fluid into. Down is always preferred and is the
    // only write when the cell below can take fluid; otherwise these are the
    // sideways directions that scored best, in a fixed order so the result is
    // reproducible.
    std::array<CellWrite, 4> writes{};
    int write_count = 0;
};

// Tick one cell. A cell holding no fluid does nothing, as does a kind with no
// traits, so a driver may call this on any position without checking first.
[[nodiscard]] FluidStep tick(const FluidWorld& world, int x, int y, int z) noexcept;

} // namespace fluids
} // namespace VoxelEngine

#endif // FARLANDS_FLUID_RULES_HPP
