// The crowd path: the predecessor query and the shared flow field built on it.
//
// The invariant every case here exists for is that a field is the SAME graph the search walks,
// only built backwards. One test walks every edge `generate` emits and proves `predecessors`
// returns it with the same cost; another proves the field's costs match a brute-force Dijkstra;
// and the rest are the cases a crowd actually meets — a one-way drop, a one-cell gap, an
// unloaded column, a goal resolved onto the cell the target really stands in.
//
// This build disables exceptions, so there is no REQUIRE anywhere in the tree (it is a compile
// error); a case that cannot continue returns after its CHECK instead.

#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "nav_test_world.hpp"

#include "core/block_types.hpp"
#include "core/thread_pool.hpp"
#include "pathfinding/nav_field.hpp"
#include "pathfinding/path_service.hpp"
#include "pathfinding/pathfinder.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using namespace VoxelEngine::nav;
namespace nt = navtest;

namespace {

// Deterministic pseudo-random terrain, so a failure names a world that can be rebuilt.
struct Dice {
    uint32_t state = 12345u;

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return (state >> 16) & 0x7fffu;
    }
};

// A plain with ledges, dips and holes in it: enough variety that steps, drops and hops all appear.
nt::World rough_plain() {
    nt::World w;
    w.default_ground = 10;
    Dice dice;
    for (int32_t x = -10; x <= 10; ++x) {
        for (int32_t z = -10; z <= 10; ++z) {
            const uint32_t roll = dice.next() % 100u;
            if (roll < 8u) {
                w.add_hole(x, z);
            } else if (roll < 20u) {
                w.set_ground(x, z, 10 - static_cast<int32_t>(dice.next() % 3u));
            } else if (roll < 30u) {
                w.set_ground(x, z, 10 + static_cast<int32_t>(dice.next() % 2u));
            }
        }
    }
    return w;
}

const NavView::Column* standing_column(const NavView& view, int32_t x, int32_t z) {
    const NavView::Column* col = view.column_at_feet(x, z, 14);
    if (col == nullptr || !col->found || col->unknown || !col->clearance) return nullptr;
    return col;
}

constexpr NavBox kBox{-14, 0, -14, 14, 24, 14};

} // namespace

TEST_CASE("predecessors returns exactly the edges the forward generator emits, at the same cost") {
    nt::World w = rough_plain();
    NavView view = nt::make_view(w, 12);
    MoveGenerator gen(view, NavCosts{});

    size_t edges = 0;
    for (int32_t x = -9; x <= 9; ++x) {
        for (int32_t z = -9; z <= 9; ++z) {
            const NavView::Column* col = standing_column(view, x, z);
            if (col == nullptr) continue;
            const NavNode from{x, view.feet_cell(col->surface_top), z};

            NavMoveList moves;
            gen.generate(from, *col, moves);
            for (int i = 0; i < moves.count; ++i) {
                const NavMove& move = moves.items[i];
                NavPredecessorList preds;
                gen.predecessors(move.to, preds);

                const NavPredecessor* back = nullptr;
                for (int p = 0; p < preds.count; ++p) {
                    if (preds.items[p].from == from) {
                        back = &preds.items[p];
                        break;
                    }
                }
                INFO("edge (" << from.x << "," << from.y << "," << from.z << ") -> ("
                              << move.to.x << "," << move.to.y << "," << move.to.z << ")");
                CHECK(back != nullptr);
                if (back != nullptr) {
                    CHECK(back->cost == doctest::Approx(move.cost).epsilon(0.0001));
                    ++edges;
                }
            }
        }
    }
    // A whole plain's worth: the case is worthless if the fixture produced a handful of edges.
    CHECK(edges > 400);
}

TEST_CASE("every predecessor the query returns is a move the forward generator accepts") {
    nt::World w = rough_plain();
    NavView view = nt::make_view(w, 12);
    MoveGenerator gen(view, NavCosts{});

    size_t checked = 0;
    for (int32_t x = -8; x <= 8; ++x) {
        for (int32_t z = -8; z <= 8; ++z) {
            const NavView::Column* col = standing_column(view, x, z);
            if (col == nullptr) continue;
            const NavNode to{x, view.feet_cell(col->surface_top), z};

            NavPredecessorList preds;
            gen.predecessors(to, preds);
            for (int p = 0; p < preds.count; ++p) {
                const NavNode& from = preds.items[p].from;
                const NavView::Column* from_col = view.column_at_feet(from.x, from.z, from.y);
                INFO("predecessor (" << from.x << "," << from.y << "," << from.z << ")");
                CHECK(from_col != nullptr);
                if (from_col == nullptr) continue;
                CHECK(from_col->found);
                CHECK_FALSE(from_col->unknown);
                CHECK(from_col->clearance);

                NavMoveList moves;
                gen.generate(from, *from_col, moves);
                const NavMove* forward = nullptr;
                for (int m = 0; m < moves.count; ++m) {
                    if (moves.items[m].to == to) {
                        forward = &moves.items[m];
                        break;
                    }
                }
                CHECK(forward != nullptr);
                if (forward != nullptr) {
                    CHECK(forward->cost == doctest::Approx(preds.items[p].cost).epsilon(0.0001));
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 400);
}

TEST_CASE("a field's cost to the goal is the same route Dijkstra finds") {
    nt::World w = rough_plain();
    NavView view = nt::make_view(w, 14);
    MoveGenerator gen(view, NavCosts{});
    const NavCosts costs;

    const NavNode goal{0, 10, 0};
    NavField field;
    const bool built = field.build(view, costs, goal, kBox, NavField::Limits{});
    CHECK(built);
    if (!built) return;
    CHECK(field.size() > 200);

    Dice dice;
    size_t compared = 0;
    for (int i = 0; i < 40; ++i) {
        const int32_t x = static_cast<int32_t>(dice.next() % 21u) - 10;
        const int32_t z = static_cast<int32_t>(dice.next() % 21u) - 10;
        const NavView::Column* col = standing_column(view, x, z);
        if (col == nullptr) continue;
        const NavNode start{x, view.feet_cell(col->surface_top), z};

        const NavFieldCell* node = field.find(start);
        // A cell the field does not hold is one the bounds cut off; Dijkstra is then free to answer
        // where the field cannot, so only the cells the field has are compared.
        if (node == nullptr) continue;
        const nt::DijkstraResult truth = nt::dijkstra_cost(view, gen, start, goal);
        INFO("start (" << x << "," << start.y << "," << z << ") field=" << node->cost
                       << " dijkstra=" << truth.cost);
        CHECK(truth.cost >= 0.0f);
        CHECK(node->cost == doctest::Approx(truth.cost).epsilon(0.0005));
        ++compared;
    }
    CHECK(compared > 10);
}

TEST_CASE("following next from a node reaches the goal by legal moves, on support cells") {
    nt::World w = rough_plain();
    NavView view = nt::make_view(w, 14);
    MoveGenerator gen(view, NavCosts{});
    const NavCosts costs;

    const NavNode goal{2, 10, 2};
    NavField field;
    const bool built = field.build(view, costs, goal, kBox, NavField::Limits{});
    CHECK(built);
    if (!built) return;

    // The field's goal is the anchored cell, and it is the same anchor a search uses because both
    // call resolve_goal.
    const GoalAnchor anchor = resolve_goal(view, goal);
    CHECK(field.goal().x == anchor.x);
    CHECK(field.goal().z == anchor.z);
    CHECK(field.goal().y == view.feet_cell(anchor.column->surface_top));

    Dice dice;
    size_t walked = 0;
    for (int i = 0; i < 30; ++i) {
        const int32_t x = static_cast<int32_t>(dice.next() % 21u) - 10;
        const int32_t z = static_cast<int32_t>(dice.next() % 21u) - 10;
        const NavView::Column* col = standing_column(view, x, z);
        if (col == nullptr) continue;
        NavNode cur{x, view.feet_cell(col->surface_top), z};
        const NavFieldCell* cell = field.find(cur);
        if (cell == nullptr) continue;

        size_t steps = 0;
        while (cell->has_next && steps < 500) {
            // Every hop of the chain is an edge the forward generator emits, which is what makes
            // the route walkable at all.
            const NavView::Column* from_col = view.column_at_feet(cur.x, cur.z, cur.y);
            CHECK(from_col != nullptr);
            if (from_col == nullptr) break;
            NavMoveList moves;
            gen.generate(cur, *from_col, moves);
            bool legal = false;
            for (int m = 0; m < moves.count; ++m) {
                if (node_key(moves.items[m].to.x, moves.items[m].to.y, moves.items[m].to.z) ==
                    cell->next) {
                    legal = true;
                    break;
                }
            }
            CHECK(legal);

            const NavFieldCell* next = field.find(node_from_key(cell->next));
            CHECK(next != nullptr);
            if (next == nullptr) break;
            // The support cell is the block under the feet, the convention a plan's cells use.
            CHECK(next->support.y == next->feet.y - 1);
            cur = next->feet;
            cell = next;
            ++steps;
        }
        CHECK(node_key(cur.x, cur.y, cur.z) ==
              node_key(field.goal().x, field.goal().y, field.goal().z));
        ++walked;
    }
    CHECK(walked > 5);
}

TEST_CASE("a one-way drop is in the field, and its next step is the drop") {
    nt::World w;
    w.default_ground = 10;
    // A plateau three blocks above the plain: its edge cells drop three in one move, which is
    // exactly what max_drop allows and what a field has to be able to walk DOWN.
    for (int32_t x = 2; x <= 6; ++x) {
        for (int32_t z = 2; z <= 6; ++z) w.set_ground(x, z, 13);
    }
    NavView view = nt::make_view(w, 14);
    const NavCosts costs;
    NavField field;
    const bool built = field.build(view, costs, NavNode{2, 10, 2}, kBox, NavField::Limits{});
    CHECK(built);
    if (!built) return;

    const NavFieldCell* top = field.find(NavNode{2, 13, 2});
    CHECK(top != nullptr);
    if (top == nullptr) return;
    CHECK(top->has_next);
    const NavFieldCell* next = top->has_next ? field.find(node_from_key(top->next)) : nullptr;
    CHECK(next != nullptr);
    if (next != nullptr) CHECK(next->feet.y < top->feet.y);
}

TEST_CASE("a one-cell gap is crossed by a hop of two columns") {
    nt::World w;
    w.default_ground = 10;
    // A channel one cell wide: nothing to walk across it on, and no wading in it either.
    for (int32_t z = -2; z <= 2; ++z) w.add_hole(1, z);
    NavView view = nt::make_view(w, 14);
    const NavCosts costs;
    NavField field;
    const bool built = field.build(view, costs, NavNode{3, 10, 0}, kBox, NavField::Limits{});
    CHECK(built);
    if (!built) return;

    const NavFieldCell* near_side = field.find(NavNode{0, 10, 0});
    CHECK(near_side != nullptr);
    if (near_side == nullptr) return;
    CHECK(near_side->has_next);
    const NavFieldCell* next =
        near_side->has_next ? field.find(node_from_key(near_side->next)) : nullptr;
    CHECK(next != nullptr);
    if (next != nullptr) {
        CHECK(next->feet.x == 2);
        CHECK(next->feet.z == 0);
        CHECK(next->feet.y == 10);
    }
}

TEST_CASE("an unloaded column is not in the field, and neither is a cell outside the box") {
    nt::World w;
    w.default_ground = 10;
    w.add_unresident(3, 0);
    NavView view = nt::make_view(w, 16);
    const NavCosts costs;

    NavField narrow;
    const NavBox small_box{-2, 0, -2, 2, 24, 2};
    const bool built = narrow.build(view, costs, NavNode{0, 10, 0}, small_box, NavField::Limits{});
    CHECK(built);
    if (!built) return;
    // Inside the search's reach, but 5 columns out: the box is the bound, and the answer is "ask
    // your own search" rather than a route from here.
    CHECK_FALSE(narrow.contains(NavNode{5, 10, 0}));
    CHECK(narrow.contains(NavNode{0, 10, 0}));

    NavField wide;
    const NavBox wide_box{-16, 0, -16, 16, 24, 16};
    const bool wide_built = wide.build(view, costs, NavNode{0, 10, 0}, wide_box, NavField::Limits{});
    CHECK(wide_built);
    if (!wide_built) return;
    // The chunk that is not resident reads Unknown: nothing routes over it, so the cell itself is
    // never a node of the graph.
    CHECK_FALSE(wide.contains(NavNode{3, 10, 0}));
}

TEST_CASE("a goal at the lip of a shelf anchors on the cell the target stands in") {
    nt::World w;
    w.default_ground = 10;
    // A shelf three up, and a target whose centre is over the plain but whose feet rest on the
    // shelf's lip: the aimed cell has no ground at that level, and its neighbour does.
    for (int32_t z = -3; z <= 3; ++z) w.set_ground(1, z, 13);
    NavView view = nt::make_view(w, 14);
    const NavCosts costs;

    const NavNode aimed{0, 13, 0};
    const GoalAnchor anchor = resolve_goal(view, aimed);
    CHECK(anchor.column != nullptr);
    if (anchor.column == nullptr) return;
    CHECK(anchor.x == 1);
    CHECK(view.feet_cell(anchor.column->surface_top) == 13);

    NavField field;
    const bool built = field.build(view, costs, aimed, kBox, NavField::Limits{});
    CHECK(built);
    if (!built) return;
    CHECK(field.goal().x == anchor.x);
    CHECK(field.goal().z == anchor.z);
    CHECK(field.goal().y == view.feet_cell(anchor.column->surface_top));
}

TEST_CASE("route returns support cells and stops when the caller's cell is not in the field") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 12);
    const NavCosts costs;
    NavField field;
    const NavBox box{-12, 0, -12, 12, 24, 12};
    const bool built = field.build(view, costs, NavNode{0, 10, 0}, box, NavField::Limits{});
    CHECK(built);
    if (!built) return;

    std::vector<NavNode> cells;
    CHECK(field.route(NavNode{0, 10, 0}, 8, cells) == 1);
    CHECK(cells.size() == 1);
    if (cells.size() == 1) CHECK(cells[0].y == 9);  // the block under the goal's feet

    cells.clear();
    CHECK(field.route(NavNode{4, 10, 4}, 2, cells) == 2);
    for (const NavNode& cell : cells) CHECK(cell.y == 9);

    cells.clear();
    CHECK(field.route(NavNode{100, 10, 100}, 8, cells) == 0);
    CHECK(cells.empty());
}

// -----------------------------------------------------------------------------
// Admission control: the cap that keeps a crowd from burying the generation pool.
// -----------------------------------------------------------------------------

TEST_CASE("PathService refuses asks past its cap and accepts again as jobs finish") {
    using VoxelEngine::BlockRegistry;
    using VoxelEngine::ChunkMap;
    using VoxelEngine::ThreadPool;

    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    for (int32_t cx = 0; cx < 4; ++cx) chunktest::insert_floor_chunk(cm, cx, 0, 0);  // x 0..127

    PathService service(cm);
    // One worker: the cap is max(2, workers / 4) = 2, and a sleeper on that worker's only queue
    // holds the two accepted plans in flight while the third is asked for.
    ThreadPool pool(1);
    CHECK(PathService::admission_cap(pool.get_worker_count()) == 2);

    pool.fire_and_forget([]() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); });

    const uint64_t first = service.submit(pool, NavNode{2, 5, 2}, NavNode{120, 5, 2});
    const uint64_t second = service.submit(pool, NavNode{2, 5, 2}, NavNode{110, 5, 2});
    CHECK(first != 0);
    CHECK(second != 0);

    // The cap, not the pool: a third ask is refused outright rather than queued behind them.
    const uint64_t refused = service.submit(pool, NavNode{2, 5, 2}, NavNode{100, 5, 2});
    CHECK(refused == 0);
    const PathService::Stats full = service.get_stats();
    CHECK(full.in_flight == 2);
    CHECK(full.max_in_flight == 2);
    CHECK(full.rejected >= 1);
    CHECK(service.pending() == 2);

    // Every accepted job still delivers, and the seat it held is handed back.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::vector<PathResult> results;
    while (results.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        for (PathResult& r : service.poll()) results.push_back(std::move(r));
        if (results.size() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(results.size() == 2);
    CHECK(service.pending() == 0);
    for (const PathResult& r : results) {
        CHECK(r.id != 0);
        CHECK(r.found);
        CHECK_FALSE(r.truncated);
    }

    // ...and a fresh ask is admitted once there is room again.
    const uint64_t third = service.submit(pool, NavNode{2, 5, 2}, NavNode{90, 5, 2});
    CHECK(third != 0);
    std::vector<PathResult> last;
    while (last.empty() && std::chrono::steady_clock::now() < deadline) {
        for (PathResult& r : service.poll()) last.push_back(std::move(r));
        if (last.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(last.size() == 1);
    CHECK(service.get_stats().finished == 3);
}
