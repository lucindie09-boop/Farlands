#ifndef FARLANDS_FLUID_SIM_HPP
#define FARLANDS_FLUID_SIM_HPP

// -----------------------------------------------------------------------------
// The fluid simulation's scheduling half: when a cell is due, how many a frame
// may touch, and what a change wakes up.
//
// This is deliberately separate from the rules (fluid_rules.hpp). The rules say
// what a cell becomes; this says when it gets asked. Keeping them apart is what
// lets the rules be tested against a naive "recompute everything" driver, and it
// is also the only reason the game can afford them: the expensive part of a tick
// is the drop-seeking search, so the whole design here is about ticking as few
// cells as possible.
//
// How little it asks for:
//   * A cell that recomputes to what it already was is SETTLED. It pushes fluid
//     outward one last time and is never scheduled again until something wakes
//     it. Nothing re-checks a settled ocean.
//   * Nothing is persisted about the schedule. A chunk that loads is seeded from
//     the fluid states in its edit map (those states ARE the record of the flood),
//     which is why a flood survives a save/load without a saved work list.
//   * Writes are batched per TICK, then grouped per chunk, so a 200-write tick
//     over three chunks costs three exclusive locks and three remesh marks
//     instead of six hundred.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "fluids/chunk_fluid.hpp"
#include "fluids/fluid_rules.hpp"
#include "fluids/fluid_state_table.hpp"

// THREADING: the simulation is single-threaded. The pending set, the queue and the
// tick counter all belong to the MAIN thread, and only that thread may call schedule,
// tick, advance or the run_* drivers. Wakes that come from a worker — a generating
// chunk seeding the fluid in its edit map, which is a normal part of streaming terrain
// — go through post_block_changed and are applied on the main thread by the next drain
// (advance / run_* drain automatically). Posting is the whole contract: it queues a
// coordinate and nothing else, so it is safe from any thread.
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace fluids {

class FluidSim {
public:
    struct Config {
        // Game ticks per second, i.e. how fast fluid moves: one cell per
        // FluidTraits::tick_delay ticks (water: 5, so a quarter second a cell).
        double tick_rate_hz = 20.0;
        // Cells one tick may touch. Chunk generation has to win the frame, so a
        // flood is spread over frames rather than done at once.
        int cells_per_tick = 512;
        // Catch-up cap: a stall must not come back as a burst of ticks.
        int max_ticks_per_frame = 2;
        // Anything queued beyond this many seconds of ticks is dropped rather
        // than paid back all at once.
        double max_accumulator = 1.0;
    };

    struct Stats {
        uint64_t ticks = 0;
        uint64_t cells_ticked = 0;
        int last_tick_cells = 0;
        int last_tick_writes = 0;
        size_t pending = 0;
        int frozen = 0;        // cells left alone last tick because a chunk was missing
        int settled = 0;       // cells that recomputed to what they already were
        double last_tick_ms = 0.0;
    };

    void configure(const Config& config) noexcept { config_ = config; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // One-time setup. The state table must be built first; while it is empty (or
    // holds no states) the sim is disabled and every entry point is a no-op, so a
    // world with no fluids costs nothing.
    void set_context(const ChunkMap& map, const BlockRegistry& registry, const FluidStateTable& table,
                     FluidWriteSink* sink) noexcept;

    // Forget the queue and the accumulator (world reload / chunk clear).
    void clear() noexcept;

    // Run whatever ticks are due this frame.
    void advance(double delta);

    // Run until nothing is left to do, ignoring the frame budget and the clock.
    // For tests and for tools that want a settled answer; returns ticks run.
    int run_to_settled(int max_ticks = 100000);

    // Run at most `ticks` ticks, ignoring the clock and the frame budget; returns
    // how many actually ran. `run_to_settled` waits for the queue to drain, which
    // a cell frozen on a missing chunk never lets happen, so a test that expects
    // no quiet needs a bounded driver.
    int run_ticks(int ticks);

    // Something changed this cell outside the simulation (a player edit, a chunk
    // load). Wakes the cell and its neighbours so they re-evaluate — and does
    // nothing at all when none of the seven is a fluid, so editing stone in the
    // middle of nowhere costs seven block reads rather than a window scan.
    //
    // MAIN THREAD ONLY. From anywhere else, post_block_changed.
    void notify_block_changed(int32_t x, int32_t y, int32_t z);

    // The same wake, posted from ANY thread, applied on the next drain.
    //
    // This exists because of a crash, and the crash is worth stating: a chunk being
    // generated applies its edit map on a WORKER thread, and every fluid cell in that
    // map wakes the sim. notify_block_changed mutates the pending set and the queue,
    // so being called from a worker raced the main thread's own wakes and ticks — and
    // the damaged hash map surfaced later as heap corruption (0xC0000374, worker
    // thread, walk `FluidSim::schedule <- schedule_around <- notify_block_changed <-
    // ChunkWorld::apply_edit_map_to_chunk <- generate_chunk <- ThreadPool::worker_loop`).
    // The deferral is invisible: a wake is already scheduled some ticks ahead, so one
    // frame of queueing changes nothing about how a flood flows.
    void post_block_changed(int32_t x, int32_t y, int32_t z);

    // Applies everything posted from other threads and returns how many cells were
    // applied. Called at the top of advance() and of the run_* drivers, so a caller
    // that drives the sim any of the ordinary ways picks posted wakes up for free.
    size_t drain_posted();

    [[nodiscard]] bool enabled() const noexcept { return table_ != nullptr && table_->any(); }
    [[nodiscard]] size_t pending_count() const noexcept { return scheduled_.size(); }
    [[nodiscard]] bool has_due_work() const noexcept;
    [[nodiscard]] uint64_t current_tick() const noexcept { return current_tick_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    // The cross-thread inbox (see post_block_changed). `draining_` is the same buffer
    // after a swap, kept so the per-frame drain does not allocate.
    struct Posted {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };
    std::mutex posted_mutex_;
    std::vector<Posted> posted_;
    std::vector<Posted> draining_;

    struct Entry {
        uint64_t due = 0;
        uint64_t seq = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };
    struct EntryLater {
        bool operator()(const Entry& a, const Entry& b) const noexcept {
            return a.due != b.due ? a.due > b.due : a.seq > b.seq;
        }
    };

    // Wake one cell at a tick. The earliest pending tick wins, so a cell woken
    // twice in the same tick is still ticked once.
    void schedule(int32_t x, int32_t y, int32_t z, uint64_t due_tick);
    // Wake a cell and the six around it — every cell whose own answer can change
    // because this one did.
    void schedule_around(int32_t x, int32_t y, int32_t z, uint64_t due_tick);
    void tick_cell(int32_t x, int32_t y, int32_t z);
    void run_tick();
    [[nodiscard]] uint64_t delay_ticks_for(FluidKind kind) const noexcept;

    [[nodiscard]] static int64_t pack(int32_t x, int32_t y, int32_t z) noexcept;
    static void unpack(int64_t key, int32_t& x, int32_t& y, int32_t& z) noexcept;

    Config config_;
    Stats stats_;

    const ChunkMap* map_ = nullptr;
    const BlockRegistry* registry_ = nullptr;
    const FluidStateTable* table_ = nullptr;
    FluidWriteSink* sink_ = nullptr;

    ChunkFluidWorld world_;
    std::priority_queue<Entry, std::vector<Entry>, EntryLater> queue_;
    std::unordered_map<int64_t, uint64_t> scheduled_;
    std::vector<CellWrite> writes_;

    uint64_t current_tick_ = 0;
    uint64_t next_seq_ = 0;
    double accumulator_ = 0.0;
};

} // namespace fluids
} // namespace VoxelEngine

#endif // FARLANDS_FLUID_SIM_HPP
