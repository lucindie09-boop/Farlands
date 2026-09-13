#ifndef FARLANDS_NAV_VIEW_HPP
#define FARLANDS_NAV_VIEW_HPP

// -----------------------------------------------------------------------------
// NavView — a lazy, memoised window onto the world for the planner.
//
// Columns are resolved on demand rather than pre-classified: classifying a
// block-accurate area up front costs milliseconds, while a search only ever
// visits the columns near its frontier. Each resolved column reports the
// topmost standable surface in the window, whether an agent's body fits on it,
// and whether the feet are standing in liquid.
//
// Cells inside chunks that are not resident must be reported as
// CellClass::Unknown by the sampler. Unknown is never traversable, so a route
// can never be plotted through ungenerated space (a missing chunk reads as AIR
// from a raw block query, which would otherwise look like walkable emptiness).
//
// Column results depend on how far above the query the downward scan starts, so
// the memo key includes the hint. That makes a column queried from a low
// vantage and again from a high one resolve twice, which is correct — a scan
// started below a tall wall cannot see its top.
//
// A source may also offer a ranged reader (read_column). When it does, resolving
// a column prefetches exactly the window the scan and its clearance check touch,
// in one call: the number of map locks a search takes then scales with columns
// rather than with cells, and nothing is held between calls. Sampled cells come
// from that buffer, everything else from the per-cell sampler, so the reader is
// a pure optimisation — the sampler alone is always enough.
// -----------------------------------------------------------------------------

#include "pathfinding/nav_types.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace nav {

class NavView {
public:
    // Returns the cell at a world coordinate. Implementations must return
    // CellClass::Unknown for cells whose chunk is not resident.
    using Sampler = std::function<Cell(int32_t x, int32_t y, int32_t z)>;

    // Fills out[y - y_lo] for every y in [y_lo, y_hi] of one column. Must agree
    // with the sampler cell for cell, and must not hold a lock when it returns.
    using Reader = std::function<void(int32_t x, int32_t z, int32_t y_lo, int32_t y_hi, Cell* out)>;

    struct Column {
        bool found = false;        // a standable surface exists in the window
        bool unknown = false;      // an unresident chunk was hit before reaching ground
        int32_t surface_cell = 0;  // cell the surface block occupies
        float surface_top = 0.0f;  // world y the feet rest at
        bool clearance = false;    // the agent's body fits standing here
        bool liquid = false;       // the feet stand inside liquid
    };

    NavView(NavBox box, Sampler sampler, NavCosts costs = NavCosts(), Reader reader = {})
        : box_(box), sampler_(std::move(sampler)), reader_(std::move(reader)), costs_(costs) {}

    // True when a ranged reader is available (it need not be used: columns whose
    // buffer has been overwritten still fall back to the sampler).
    [[nodiscard]] bool has_reader() const noexcept { return static_cast<bool>(reader_); }

    [[nodiscard]] const NavBox& box() const noexcept { return box_; }
    [[nodiscard]] const NavCosts& costs() const noexcept { return costs_; }

    [[nodiscard]] bool in_bounds(int32_t x, int32_t y, int32_t z) const noexcept {
        return x >= box_.min_x && x <= box_.max_x &&
               y >= box_.min_y && y <= box_.max_y &&
               z >= box_.min_z && z <= box_.max_z;
    }

    // Raw cell sample with window clamping: anything above the window is sky
    // (air); anything below or outside it is unknown, so the planner stops
    // rather than inventing terrain.
    [[nodiscard]] Cell sample(int32_t x, int32_t y, int32_t z) const {
        if (x < box_.min_x || x > box_.max_x || z < box_.min_z || z > box_.max_z) {
            return Cell{CellClass::Unknown, 0.0f, 0.0f};
        }
        if (y > box_.max_y) return Cell{CellClass::Air, 0.0f, 0.0f};
        if (y < box_.min_y) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        if (buffered(x, y, z)) return buffer_[static_cast<size_t>(y - buffer_lo_)];
        return sampler_(x, y, z);
    }

    // Resolves (memoised) the column at (x, z). `hint_y` is where the scanning
    // starts looking downward from — normally the querying node's feet cell.
    [[nodiscard]] const Column* column(int32_t x, int32_t z, int32_t hint_y) const;
    [[nodiscard]] const Column* column(const NavNode& n) const { return column(n.x, n.z, n.y); }

    // Does the agent's body fit standing on `stand_top` at (x, z)?
    [[nodiscard]] bool body_fits(int32_t x, int32_t z, float stand_top) const;

    [[nodiscard]] size_t columns_resolved() const noexcept { return columns_resolved_; }

    // Converts a standing height into the feet cell.
    [[nodiscard]] int32_t feet_cell(float surface_top) const noexcept {
        return static_cast<int32_t>(std::floor(surface_top + costs_.epsilon));
    }

private:
    [[nodiscard]] Column compute_column(int32_t x, int32_t z, int32_t hint_y) const;

    // Prefetches [lowest, highest] of one column through the reader, and makes it
    // the buffer the sampler answers from until another column overwrites it.
    void load_buffer(int32_t x, int32_t z, int32_t lowest, int32_t highest) const;

    [[nodiscard]] bool buffered(int32_t x, int32_t y, int32_t z) const noexcept {
        return buffer_lo_ <= buffer_hi_ && x == buffer_x_ && z == buffer_z_ &&
               y >= buffer_lo_ && y <= buffer_hi_;
    }

    NavBox box_;
    Sampler sampler_;
    Reader reader_;
    NavCosts costs_;

    // Cells prefetched through the reader for one column. `buffer_lo_ > buffer_hi_`
    // means empty, which is how the sampler falls back to the per-cell path.
    mutable int32_t buffer_x_ = INT32_MIN;
    mutable int32_t buffer_z_ = INT32_MIN;
    mutable int32_t buffer_lo_ = 0;
    mutable int32_t buffer_hi_ = -1;
    mutable std::vector<Cell> buffer_;

    mutable std::unordered_map<uint64_t, Column> columns_;
    mutable size_t columns_resolved_ = 0;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_VIEW_HPP
