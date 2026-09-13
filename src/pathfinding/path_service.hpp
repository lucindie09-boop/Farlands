#ifndef FARLANDS_NAV_PATH_SERVICE_HPP
#define FARLANDS_NAV_PATH_SERVICE_HPP

// -----------------------------------------------------------------------------
// Runs planner jobs on the engine's thread pool and hands the finished ones
// back to the main thread.
//
// A route is a couple of milliseconds of work, so jobs are submitted at normal
// priority (chunk generation must win) and results are polled rather than
// waited on. Every job owns its NavView and its sampler cache, and the shared
// state is held in a shared_ptr captured by the task, so a job can never touch
// a service that has already been destroyed.
//
// The pool is taken per submit() rather than held for the service's life. The
// controller destroys its thread pool and builds a fresh one whenever the
// runtime state is reset (clear_editor_chunks -> reset_runtime_state(true)), so
// a service that remembered the old pool would be queueing a task into freed
// memory on the very next request. The chunk map is safe to hold by reference:
// it is a value member of the controller and outlives every job.
// -----------------------------------------------------------------------------

#include "core/chunk_map.hpp"
#include "core/thread_pool.hpp"
#include "pathfinding/chunk_nav_source.hpp"
#include "pathfinding/nav_types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace VoxelEngine {
namespace nav {

// One finished plan, in world terms.
struct PathResult {
    uint64_t id = 0;
    bool found = false;
    bool truncated = false;      // a budget ran out (partial route)
    bool budget_exhausted = false;  // ...the expansion cap
    bool time_exhausted = false;    // ...the wall-clock cap
    std::string error;           // why it failed, empty on success
    double search_ms = 0.0;
    NavStats stats;

    // How the world view was read: cells classified, and chunk shard locks
    // taken to classify them. Far apart is the point — see ChunkMapNavSource.
    size_t cells_read = 0;
    size_t lock_acquisitions = 0;

    // Each entry is the BLOCK a node stands on (the support block), which is
    // what a debug overlay highlights; `waypoints` is the same route after
    // string-pull smoothing.
    std::vector<NavNode> nodes;
    std::vector<NavNode> waypoints;
};

class PathService {
public:
    explicit PathService(const ChunkMap& map);
    ~PathService();

    PathService(const PathService&) = delete;
    PathService& operator=(const PathService&) = delete;

    // Queues a plan between two feet positions on `pool`, which must be the
    // engine's current pool. Returns the job id (never 0). `max_ms` bounds the
    // search in wall-clock time (0 disables it); either cap running out yields a
    // truncated but usable partial route.
    uint64_t submit(ThreadPool& pool, const NavNode& from, const NavNode& to,
                    int32_t max_expansions = 20000, double max_ms = 0.0);

    // Every plan finished since the previous call.
    std::vector<PathResult> poll();

    [[nodiscard]] size_t pending() const noexcept;
    [[nodiscard]] size_t finished_count() const noexcept;

private:
    struct State {
        std::mutex mutex;
        std::vector<PathResult> done;
        std::atomic<size_t> pending{0};
        std::atomic<size_t> finished{0};
        std::atomic<uint64_t> next_id{1};
    };

    // The source and the query are borrowed for the duration of the call: both
    // only ever get READ here, and the source's shared_ptr is copied onto the
    // view's lambdas that need to outlive it, so neither needs its own copy of
    // the argument (clang-tidy: performance-unnecessary-value-param).
    static void run(const std::shared_ptr<State>& state, uint64_t id,
                    const std::shared_ptr<ChunkMapNavSource>& source,
                    const NavQuery& query);

    // Borrowed; outlives the service. The pool is not stored — see submit().
    const ChunkMap& map_;
    std::shared_ptr<State> state_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_PATH_SERVICE_HPP
