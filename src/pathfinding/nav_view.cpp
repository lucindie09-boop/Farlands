#include "pathfinding/nav_view.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {
namespace nav {

namespace {
// How far below the scan start a column is allowed to look before it is
// reported as a hole. Deep enough for a cliff face inside the window, shallow
// enough that a void column never costs a full-column walk.
constexpr int32_t kMaxColumnScan = 64;
} // namespace

const NavView::Column* NavView::column(int32_t x, int32_t z, int32_t hint_y) const {
    const int32_t hint = std::clamp(hint_y, box_.min_y, box_.max_y);
    const uint64_t key = node_key(x, hint, z);
    const auto it = columns_.find(key);
    if (it != columns_.end()) return &it->second;
    Column col = compute_column(x, z, hint);
    const auto [inserted, _] = columns_.emplace(key, col);
    (void)_;
    return &inserted->second;
}

NavView::Column NavView::compute_column(int32_t x, int32_t z, int32_t hint_y) const {
    Column col;
    ++columns_resolved_;

    // An agent can only have arrived from a surface within max_rise of its own,
    // so start the downward scan just above that and walk down to the ground.
    const int32_t rise = static_cast<int32_t>(std::ceil(costs_.max_rise));
    const int32_t start = std::min(box_.max_y, hint_y + rise + 1);
    const int32_t lowest = std::max(box_.min_y, start - kMaxColumnScan);

    bool liquid_passed = false;
    for (int32_t y = start; y >= lowest; --y) {
        const Cell c = sampler_(x, y, z);
        switch (c.cls) {
            case CellClass::Unknown:
                col.unknown = true;
                return col;
            case CellClass::Air:
                break;
            case CellClass::Liquid:
                // Liquid does not support the body: keep descending to the
                // floor, but remember that the feet will be submerged.
                liquid_passed = true;
                break;
            case CellClass::Partial:
            case CellClass::Solid:
                if (c.hi >= costs_.min_stand_top) {
                    col.found = true;
                    col.surface_cell = y;
                    col.surface_top = static_cast<float>(y) + c.hi;
                    col.liquid = liquid_passed;
                    col.clearance = body_fits(x, z, col.surface_top);
                    return col;
                }
                // Collision too low to stand on (snow layer, fence top): keep
                // looking down for real ground.
                break;
        }
    }
    return col;
}

bool NavView::body_fits(int32_t x, int32_t z, float stand_top) const {
    const float body_lo = stand_top;
    const float body_hi = stand_top + costs_.body_height;
    const int32_t y0 = static_cast<int32_t>(std::floor(body_lo));
    const int32_t y1 = static_cast<int32_t>(std::floor(body_hi - costs_.epsilon));
    for (int32_t y = y0; y <= y1; ++y) {
        if (y > box_.max_y) break;  // sky above the window
        const Cell c = sample(x, y, z);
        if (c.cls == CellClass::Unknown) return false;
        if (!c.blocks_body()) continue;
        const float lo = static_cast<float>(y) + c.lo;
        const float hi = static_cast<float>(y) + c.hi;
        if (lo < body_hi - costs_.epsilon && hi > body_lo + costs_.epsilon) return false;
    }
    return true;
}

} // namespace nav
} // namespace VoxelEngine
