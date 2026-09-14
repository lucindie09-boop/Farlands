#ifndef FARLANDS_FLUID_TEST_WORLD_HPP
#define FARLANDS_FLUID_TEST_WORLD_HPP

// A world in a std::vector for the fluid rules to run against, plus the two
// drivers the rules are checked with.
//
// The drivers are the point of this file. They are deliberately different in
// kind, not just in detail:
//
//   * settle_sweep() is the SLOW REFERENCE. Every round it recomputes every
//     cell in the box, in one fixed order, and repeats until a whole round
//     changes nothing. It does no bookkeeping at all — no idea which cell could
//     possibly have changed, no wake-ups — so it is the yardstick the real
//     driver has to reproduce. It is also, by construction, the thing that
//     cannot be subtly wrong about scheduling, which makes it the right oracle.
//
//   * settle_queue() is the driver the game will actually use: a work list of
//     cells that might change, seeded with the fluid that exists, re-seeded by
//     whatever each step touched. It has to know every way one cell's change can
//     affect another, and if it misses one, the two drivers disagree — which is
//     exactly the bug class this file exists to catch, before the scheduler
//     exists to have it in.
//
// Everything is in-bounds-indexed, so the world edge behaves as a wall: out of
// bounds reads as blocked and fluid-free. That keeps a test's result from
// depending on how big the box happens to be.

#include "fluids/fluid_rules.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fluid_test {

using VoxelEngine::fluids::CellWrite;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::fluids::FluidStep;

class FluidTestWorld final : public VoxelEngine::fluids::FluidWorld {
public:
    struct CellState {
        bool solid = false;
        FluidCell fluid{};
    };

    FluidTestWorld(int sx, int sy, int sz)
        : sx_(sx), sy_(sy), sz_(sz),
          cells_(static_cast<size_t>(sx) * static_cast<size_t>(sy) * static_cast<size_t>(sz)),
          queued_(cells_.size(), 0) {}

    // ---------------------------------------------------------------- setup

    void set_solid(int x, int y, int z) {
        if (in_bounds(x, y, z)) cell(x, y, z).solid = true;
    }

    // A solid layer filling the whole x/z footprint at that height.
    void set_floor(int y) {
        for (int z = 0; z < sz_; ++z) {
            for (int x = 0; x < sx_; ++x) set_solid(x, y, z);
        }
    }

    // Solid columns from `y0` up to `y1` inclusive, at one x/z position.
    void set_pillar(int x, int z, int y0, int y1) {
        for (int y = y0; y <= y1; ++y) set_solid(x, y, z);
    }

    // Remove whatever is here, solid or fluid.
    void set_open(int x, int y, int z) {
        if (!in_bounds(x, y, z)) return;
        CellState& c = cell(x, y, z);
        c.solid = false;
        c.fluid = FluidCell{};
    }

    void set_cell_fluid(int x, int y, int z, FluidCell f) {
        if (!in_bounds(x, y, z)) return;
        cell(x, y, z).fluid = f;
    }

    // depth 0 = a source (a spring); 1..7 = runoff.
    void set_water(int x, int y, int z, int depth = 0) {
        set_cell_fluid(x, y, z, FluidCell{ FluidKind::Water, static_cast<uint8_t>(depth), false });
    }

    // Part of a column pouring downward: full strength, depth not meaningful.
    void set_water_falling(int x, int y, int z) {
        set_cell_fluid(x, y, z, FluidCell{ FluidKind::Water, 0, true });
    }

    void clear_fluid(int x, int y, int z) {
        if (in_bounds(x, y, z)) cell(x, y, z).fluid = FluidCell{};
    }

    // --------------------------------------------------------------- queries

    [[nodiscard]] bool in_bounds(int x, int y, int z) const noexcept {
        return x >= 0 && y >= 0 && z >= 0 && x < sx_ && y < sy_ && z < sz_;
    }

    // Reads that count, so a test can assert the rules stayed inside their bound.
    [[nodiscard]] FluidCell fluid_at(int x, int y, int z) const override {
        ++queries;
        return at(x, y, z);
    }

    [[nodiscard]] bool blocked(int x, int y, int z) const override {
        ++queries;
        if (!in_bounds(x, y, z)) return true;  // the world edge is a wall
        return cell(x, y, z).solid;
    }

    // The same two reads without counting.
    [[nodiscard]] FluidCell at(int x, int y, int z) const {
        if (!in_bounds(x, y, z)) return FluidCell{};
        return cell(x, y, z).fluid;
    }

    [[nodiscard]] bool is_solid(int x, int y, int z) const {
        if (!in_bounds(x, y, z)) return false;
        return cell(x, y, z).solid;
    }

    [[nodiscard]] int count_fluid() const {
        int n = 0;
        for (const CellState& c : cells_) {
            if (c.fluid.present()) ++n;
        }
        return n;
    }

    [[nodiscard]] long long query_count() const noexcept { return queries; }
    void reset_query_count() const noexcept { queries = 0; }

    [[nodiscard]] int size_x() const noexcept { return sx_; }
    [[nodiscard]] int size_y() const noexcept { return sy_; }
    [[nodiscard]] int size_z() const noexcept { return sz_; }

    // The first thing two worlds disagree about, or an empty string when they
    // agree everywhere. For failure messages.
    [[nodiscard]] std::string first_difference(const FluidTestWorld& other) const {
        for (int y = 0; y < sy_; ++y) {
            for (int z = 0; z < sz_; ++z) {
                for (int x = 0; x < sx_; ++x) {
                    if (at(x, y, z) != other.at(x, y, z)) {
                        return "cell (" + std::to_string(x) + "," + std::to_string(y) + "," +
                               std::to_string(z) + "): " + cell_text(cell(x, y, z)) + " vs " +
                               cell_text(other.cell(x, y, z));
                    }
                    if (is_solid(x, y, z) != other.is_solid(x, y, z)) {
                        return "cell (" + std::to_string(x) + "," + std::to_string(y) + "," +
                               std::to_string(z) + ") solidity differs";
                    }
                }
            }
        }
        return std::string();
    }

    // --------------------------------------------------------------- drivers

    // Apply one step to the world. Returns true when anything actually changed.
    // `landed`/`landed_count` receive the indices of the writes that landed, so
    // a queue driver knows which cells to wake.
    bool apply_step(const FluidStep& step, int x, int y, int z,
                    int* landed = nullptr, int* landed_count = nullptr) {
        bool changed = false;
        if (step.remove) {
            if (at(x, y, z).present()) {
                clear_fluid(x, y, z);
                changed = true;
            }
        } else if (step.changed) {
            set_cell_fluid(x, y, z, step.next);
            changed = true;
        }
        for (int i = 0; i < step.write_count; ++i) {
            const CellWrite& w = step.writes[i];
            if (at(w.x, w.y, w.z) == w.state) continue;
            set_cell_fluid(w.x, w.y, w.z, w.state);
            changed = true;
            if (landed != nullptr && landed_count != nullptr) landed[(*landed_count)++] = i;
        }
        return changed;
    }

    // The slow reference: recompute everything, repeatedly, until a whole round
    // is quiet. Returns the number of rounds, or -1 if it never settled within
    // `max_rounds` (which would mean the rules oscillate).
    int settle_sweep(int max_rounds = 400) {
        for (int round = 0; round < max_rounds; ++round) {
            bool changed = false;
            for (int y = 0; y < sy_; ++y) {
                for (int z = 0; z < sz_; ++z) {
                    for (int x = 0; x < sx_; ++x) {
                        if (!at(x, y, z).present()) continue;
                        const FluidStep step = VoxelEngine::fluids::tick(*this, x, y, z);
                        if (apply_step(step, x, y, z)) changed = true;
                    }
                }
            }
            if (!changed) return round + 1;
        }
        return -1;
    }

    // The driver phase 3 will use. Seeds from the fluid that exists, then ticks
    // until the work list drains. Returns the number of ticks, or -1 if it did
    // not drain within `max_ticks`.
    int settle_queue(int max_ticks = 200000) {
        queue_.clear();
        std::fill(queued_.begin(), queued_.end(), 0);
        for (int y = 0; y < sy_; ++y) {
            for (int z = 0; z < sz_; ++z) {
                for (int x = 0; x < sx_; ++x) {
                    if (at(x, y, z).present()) push_with_neighbours(x, y, z);
                }
            }
        }

        int ticks = 0;
        while (!queue_.empty()) {
            if (ticks >= max_ticks) return -1;
            const int x = queue_.front() % sx_;
            const int z = (queue_.front() / sx_) % sz_;
            const int y = queue_.front() / (sx_ * sz_);
            queue_.pop_front();
            queued_[index(x, y, z)] = 0;
            ++ticks;

            int landed[4] = { 0, 0, 0, 0 };
            int landed_count = 0;
            const FluidStep step = VoxelEngine::fluids::tick(*this, x, y, z);
            if (apply_step(step, x, y, z, landed, &landed_count)) {
                // The cell and everything around it now needs re-evaluating:
                // a neighbour's supply changed, and a cell above or below cares
                // whether this one can still take fluid.
                push_with_neighbours(x, y, z);
            }
            for (int i = 0; i < landed_count; ++i) {
                const CellWrite& w = step.writes[landed[i]];
                push_with_neighbours(w.x, w.y, w.z);
            }
        }
        return ticks;
    }

    // ------------------------------------------------------------ diagnostics

    // "S" source, "F" falling, 1..7 runoff depth, "." nothing, "#" solid.
    [[nodiscard]] static std::string cell_text(const CellState& c) {
        if (c.fluid.present()) {
            if (c.fluid.is_source()) return "S";
            if (c.fluid.falling) return "F";
            return std::string(1, static_cast<char>('0' + c.fluid.depth));
        }
        return c.solid ? "#" : ".";
    }

    // One line per z, x across, at one height. For failure messages.
    [[nodiscard]] std::string layer(int y) const {
        std::string out = "\n";
        for (int z = 0; z < sz_; ++z) {
            for (int x = 0; x < sx_; ++x) out += cell_text(cell(x, y, z));
            out += "\n";
        }
        return out;
    }

private:
    size_t index(int x, int y, int z) const noexcept {
        return static_cast<size_t>(y) * static_cast<size_t>(sx_) * static_cast<size_t>(sz_) +
               static_cast<size_t>(z) * static_cast<size_t>(sx_) + static_cast<size_t>(x);
    }

    CellState& cell(int x, int y, int z) { return cells_[index(x, y, z)]; }
    [[nodiscard]] const CellState& cell(int x, int y, int z) const { return cells_[index(x, y, z)]; }

    void push(int x, int y, int z) {
        if (!in_bounds(x, y, z)) return;
        const size_t i = index(x, y, z);
        if (queued_[i] != 0) return;
        queued_[i] = 1;
        queue_.push_back(static_cast<int>(i));
    }

    void push_with_neighbours(int x, int y, int z) {
        push(x, y, z);
        push(x + 1, y, z);
        push(x - 1, y, z);
        push(x, y, z + 1);
        push(x, y, z - 1);
        // Vertically too, and for two different reasons: the cell BELOW a new
        // fluid cell has to notice what is now above it (it becomes falling),
        // and the cell ABOVE it has to notice that it can no longer pour down
        // (it spreads sideways instead). Dropping either one leaves fluid that
        // never re-evaluates — which the cross-check catches, but only for
        // scenes that actually create fluid on top of or under fluid.
        push(x, y + 1, z);
        push(x, y - 1, z);
    }

    int sx_;
    int sy_;
    int sz_;
    std::vector<CellState> cells_;
    std::deque<int> queue_;
    std::vector<uint8_t> queued_;
    mutable long long queries = 0;
};

} // namespace fluid_test

#endif // FARLANDS_FLUID_TEST_WORLD_HPP
