#include "pathfinding/path_smoother.hpp"

#include <cstdlib>

namespace VoxelEngine {
namespace nav {

bool PathSmoother::leg_walkable(const NavNode& a, const NavNode& b) const {
    if (a == b) return true;

    const NavView::Column* cur_col = view_.column(a.x, a.z, a.y);
    if (cur_col == nullptr || !cur_col->found || cur_col->unknown || !cur_col->clearance) {
        return false;
    }

    // Integer Bresenham over columns: consecutive cells of the line are always
    // 8-neighbours, so each pair is tested with the same rule the search uses.
    NavNode cur = a;
    int32_t x = a.x;
    int32_t z = a.z;
    const int32_t dx = std::abs(b.x - a.x);
    const int32_t dz = std::abs(b.z - a.z);
    const int32_t sx = (b.x > a.x) - (b.x < a.x);
    const int32_t sz = (b.z > a.z) - (b.z < a.z);
    int32_t err = dx - dz;

    while (x != b.x || z != b.z) {
        const int32_t e2 = 2 * err;
        int32_t nx = x;
        int32_t nz = z;
        if (e2 > -dz) { err -= dz; nx += sx; }
        if (e2 < dx) { err += dx; nz += sz; }

        NavMove m;
        if (!generator_.step(cur, *cur_col, nx - x, nz - z, m)) return false;
        if (m.to.x != nx || m.to.z != nz) return false;

        cur = m.to;
        x = nx;
        z = nz;
        cur_col = view_.column(cur.x, cur.z, cur.y);
        if (cur_col == nullptr || !cur_col->found || cur_col->unknown || !cur_col->clearance) {
            return false;
        }
    }
    // The leg has to actually land on the node the path claims, not merely on
    // its column.
    return cur == b;
}

std::vector<NavNode> PathSmoother::smooth(const std::vector<NavNode>& path) const {
    if (path.size() < 3) return path;

    std::vector<NavNode> out;
    out.reserve(path.size());
    out.push_back(path.front());

    size_t anchor = 0;
    for (size_t i = 1; i + 1 < path.size(); ++i) {
        if (!leg_walkable(path[anchor], path[i + 1])) {
            // path[i] is the furthest node still reachable in a straight leg.
            if (!(path[i] == path[anchor])) out.push_back(path[i]);
            anchor = i;
        }
    }
    if (!(path.back() == out.back())) out.push_back(path.back());
    return out;
}

} // namespace nav
} // namespace VoxelEngine
