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
// -----------------------------------------------------------------------------

#include "core/chunk_map.hpp"
#include "core/thread_pool.hpp"
#include "pathfinding/chunk_nav_source.hpp"
#include "pathfinding/nav_types.hpp"

#include <atomic>
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
    PathService(const ChunkMap& map, ThreadPool& pool);
    ~PathService();

    PathService(const PathService&) = delete;
    PathService& operator=(const PathService&) = delete;

    // Queues a plan between two feet positions. Returns the job id (never 0).
    // `max_ms` bounds the search in wall-clock time (0 disables it); either cap
    // running out yields a truncated but usable partial route.
    uint64_t submit(const NavNode& from, const NavNode& to, int32_t max_expansions = 20000,
                    double max_ms = 0.0);

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

    static void run(const std::shared_ptr<State>& state, uint64_t id,
                    std::shared_ptr<ChunkMapNavSource> source, NavQuery query);

    // Kept alive for the lifetime of the service; the map itself is borrowed.
    const ChunkMap& map_;
    ThreadPool& pool_;
    std::shared_ptr<State> state_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_PATH_SERVICE_HPP
