#ifndef FARLANDS_COLUMN_PREFETCH_HPP
#define FARLANDS_COLUMN_PREFETCH_HPP

#include "core/terrain_params.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace VoxelEngine {

// One column's content bounds, in blocks: everything below `land_h` is solid
// rock and everything above `top_h` is air. The generation sweep's band filter
// needs both (see sweep_band.hpp).
struct ColumnBounds {
    float land_h = 0.0f;
    float top_h  = 0.0f;
};

// Off-thread producer of column content bounds.
//
// Why this exists: the sweep's band filter answers "which chunk slices of this
// column can possibly hold terrain" from those bounds, and deriving them is a
// rigorous chunk height range over the column's lattice — measured at ~181 us
// cold, because it evaluates the climate blend on 81 lattice nodes. The sweep
// reads one bound per column in the disc (3,209 at render distance 32), so a
// fresh disc is ~580 ms of work and a single entered ring (~130 columns) is
// ~23 ms, all of it on the main thread: /genstats showed 70% of the whole sweep
// inside `bands read`, against a 2 ms per-frame budget that a worst frame blew
// out to 10.83 ms.
//
// Moving the work does not shrink it, but it stops being a frame cost: the main
// thread requests columns it has not seen, the pool computes them while it
// renders, and `try_take` hands back a ready answer instead of paying a cold
// range. The main thread is always allowed to compute a bound itself, so a
// prefetch that lags behind is slower, never wrong, and can never stall the
// frontier it feeds.
//
// Correctness rests on the epoch. A column's bounds depend on the terrain
// configuration alone, so `set_config` republishes that configuration and bumps
// the epoch, and an answer computed under the previous epoch is refused rather
// than cached — it would describe terrain that no longer exists. Nothing else is
// shared, and nothing here touches world state, so a worker cannot observe a
// half-written world.
//
// Lifetime: the state is shared-ownership, and a queued task captures the state
// rather than the updater, so a task still running when the updater dies writes
// into memory it owns itself.
//
// Header-only by design: the library and the test binary compile this same
// definition, so a test cannot pass against a build of the class the game does
// not use.
class ColumnPrefetch {
public:
    using Key = uint64_t;

    // The whole of the generator state a bound depends on. Workers keep their own
    // generator configured from a copy of this, so this is never read while the
    // main thread owns the live generator and its setters mutate it.
    struct Config {
        TerrainParams terrain;
        BiomeConfig biomes;
        VegetationConfig vegetation;
    };

    struct Stats {
        uint64_t requested = 0;    // columns handed to a worker
        uint64_t published = 0;    // answers a worker produced
        uint64_t taken     = 0;    // answers the main thread actually consumed
        uint64_t stale     = 0;    // answers refused: the terrain changed under them
        uint64_t dropped   = 0;    // answers discarded: no room left for them
        uint64_t outstanding = 0;  // requested and not yet answered (a gauge)
    };

    // Columns are addressed by chunk coordinates, packed the way the chunk map
    // packs them: one convention, and a negative coordinate cannot collide with a
    // positive one.
    [[nodiscard]] static Key key_of(int32_t cx, int32_t cz) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(cz));
    }

    ColumnPrefetch() : shared_(std::make_shared<Shared>()) {}

    // ------------------------------------------------------------------
    // Main thread
    // ------------------------------------------------------------------

    // Republishes the configuration the workers compute with, drops everything in
    // flight and waiting, and bumps the epoch. Called wherever the column cache is
    // invalidated, because those bounds come from the same configuration.
    inline void set_config(const Config& config) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->config = config;
        ++shared_->epoch;
        shared_->ready.clear();
        shared_->pending.clear();
    }

    [[nodiscard]] inline uint32_t epoch() const {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->epoch;
    }

    // Registers a request for `key` against `epoch`. False when it is already
    // requested or already has an answer waiting, so a caller never enqueues the
    // same column twice — the only way a duplicate could cost anything, since a
    // worker's answer for a column the main thread computed itself is simply never
    // claimed. False as well when `epoch` has already been retired: the caller's
    // scan was reading a list built for terrain that no longer exists, and
    // accepting the request would leave it recorded with no task to answer it.
    inline bool request(Key key, uint32_t epoch) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->epoch != epoch) return false;
        if (shared_->pending.count(key) != 0) return false;
        if (shared_->ready.count(key) != 0) return false;
        shared_->pending.insert(key);
        ++shared_->stats.requested;
        return true;
    }

    // Consumes the answer for `key` if a worker has produced one. False when it has
    // not landed yet, including "the terrain changed under the request", in which
    // case the caller falls back to computing the bound itself.
    inline bool try_take(Key key, ColumnBounds& out) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        auto it = shared_->ready.find(key);
        if (it == shared_->ready.end()) return false;
        out = it->second;
        shared_->ready.erase(it);
        ++shared_->stats.taken;
        return true;
    }

    [[nodiscard]] inline size_t outstanding() const {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->pending.size();
    }

    [[nodiscard]] inline Stats stats() const {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        Stats s = shared_->stats;
        s.outstanding = shared_->pending.size();
        return s;
    }

    // ------------------------------------------------------------------
    // Worker
    // ------------------------------------------------------------------

    // Copies the configuration for `epoch` into `config` when this thread has not
    // seen that epoch yet; `copied` reports whether it had to. False when `epoch`
    // is no longer current, in which case the worker drops the task: the answer
    // would describe terrain the epoch has retired.
    inline bool worker_config(uint32_t epoch, uint32_t& seen_epoch,
                              Config& config, bool& copied) const {
        copied = false;
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->epoch != epoch) return false;
        if (seen_epoch != epoch) {
            config = shared_->config;
            seen_epoch = epoch;
            copied = true;
        }
        return true;
    }

    // Hands an answer back. Refused when the epoch moved on; discarded when the
    // waiting queue is already full, which bounds memory if the consumer stops
    // asking. The request is always retired, in every path, so a refused answer
    // cannot leave a column looking permanently in flight.
    inline void publish(Key key, uint32_t epoch, const ColumnBounds& bounds) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->pending.erase(key);
        if (shared_->epoch != epoch) {
            ++shared_->stats.stale;
            return;
        }
        if (shared_->ready.size() >= kReadyCapacity) {
            ++shared_->stats.dropped;
            return;
        }
        shared_->ready[key] = bounds;
        ++shared_->stats.published;
    }

private:
    // Answers waiting to be claimed, and requests still out. A handful of hash
    // operations per column under one mutex, which is nothing next to the ~181 us
    // of work they hand off.
    struct Shared {
        std::mutex mutex;
        std::unordered_map<Key, ColumnBounds> ready;
        std::unordered_set<Key> pending;
        uint32_t epoch = 1;
        Config config;
        Stats stats;
    };

    // Enough for every column of several discs. In steady state the frontier
    // claims answers on the pass that follows the request, so this is a safety net
    // for a consumer that has stopped asking, not a working set.
    static constexpr size_t kReadyCapacity = 8192;

    std::shared_ptr<Shared> shared_;
};

} // namespace VoxelEngine

#endif // FARLANDS_COLUMN_PREFETCH_HPP
