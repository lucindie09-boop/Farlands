#include "fluids/fluid_sim.hpp"

#include <chrono>

namespace VoxelEngine {
namespace fluids {

namespace {

// World cells are packed into one integer for the pending set. 21 bits per axis
// is +-1,048,575 blocks (and 2 million tall), comfortably beyond the world.
constexpr int32_t kCoordBits = 21;
constexpr int64_t kCoordMask = (static_cast<int64_t>(1) << kCoordBits) - 1;

// How long a cell left alone for a missing chunk waits before looking again.
// A second at water's rate: slow enough to be free, quick enough that water at
// the edge of loaded space picks up on its own once the terrain arrives, even if
// nothing else wakes it.
constexpr uint64_t kFrozenRetryTicks = 20;

[[nodiscard]] int32_t sign_extend(int64_t value) noexcept {
    value &= kCoordMask;
    if ((value & (static_cast<int64_t>(1) << (kCoordBits - 1))) != 0) {
        value -= static_cast<int64_t>(1) << kCoordBits;
    }
    return static_cast<int32_t>(value);
}

} // namespace

void FluidSim::set_context(const ChunkMap& map, const BlockRegistry& registry, const FluidStateTable& table,
                           FluidWriteSink* sink) noexcept {
    map_ = &map;
    registry_ = &registry;
    table_ = &table;
    sink_ = sink;
    world_.set_context(registry, table);
}

int64_t FluidSim::pack(int32_t x, int32_t y, int32_t z) noexcept {
    return ((static_cast<int64_t>(x) & kCoordMask) << (kCoordBits * 2)) |
           ((static_cast<int64_t>(z) & kCoordMask) << kCoordBits) |
           (static_cast<int64_t>(y) & kCoordMask);
}

void FluidSim::unpack(int64_t key, int32_t& x, int32_t& y, int32_t& z) noexcept {
    x = sign_extend(key >> (kCoordBits * 2));
    z = sign_extend(key >> kCoordBits);
    y = sign_extend(key);
}

void FluidSim::clear() noexcept {
    queue_ = decltype(queue_)();
    scheduled_.clear();
    writes_.clear();
    accumulator_ = 0.0;
    current_tick_ = 0;
    next_seq_ = 0;
    stats_ = Stats{};
}

uint64_t FluidSim::delay_ticks_for(FluidKind kind) const noexcept {
    const FluidTraits* traits = traits_for(kind);
    return static_cast<uint64_t>(traits != nullptr ? traits->tick_delay : 5);
}

bool FluidSim::has_due_work() const noexcept {
    return !queue_.empty() && queue_.top().due <= current_tick_;
}

void FluidSim::schedule(int32_t x, int32_t y, int32_t z, uint64_t due_tick) {
    if (y < 0 || y >= WORLD_HEIGHT_Y) return;
    // A tick's writes are applied at the END of it, so no caller may ask for the
    // tick currently in progress: the cell would be read against the world as it
    // was before the very write that prompted the wake-up. Every caller already
    // passes current_tick_ + delay; this is the belt to that pair of braces.
    if (due_tick <= current_tick_) due_tick = current_tick_ + 1;

    const int64_t key = pack(x, y, z);
    const auto it = scheduled_.find(key);
    if (it != scheduled_.end()) {
        // Already going to be asked, and no later than this. Keep the earlier
        // tick: waking the same cell ten times in a tick is still one tick, and
        // that is what keeps a busy flood from thrashing.
        //
        // Unless that earlier tick has ALREADY BEEN REACHED, in which case it is
        // spent. This is the difference between a flood that spreads and one
        // that stops after a single ring: cells are woken at current_tick_ +
        // delay, and a cell already queued for the tick in progress (or already
        // ticked by it) answered "no fluid here" against the world before the
        // write landed. Keeping that spent tick silently drops the wake-up, so a
        // reached tick is replaced by the new one instead.
        if (it->second > current_tick_ && it->second <= due_tick) return;
        it->second = due_tick;
    } else {
        scheduled_.emplace(key, due_tick);
    }
    queue_.push(Entry{ due_tick, next_seq_++, x, y, z });
}

void FluidSim::schedule_around(int32_t x, int32_t y, int32_t z, uint64_t due_tick) {
    schedule(x, y, z, due_tick);
    schedule(x + 1, y, z, due_tick);
    schedule(x - 1, y, z, due_tick);
    schedule(x, y, z + 1, due_tick);
    schedule(x, y, z - 1, due_tick);
    // Vertically too, and for two different reasons: the cell below a new fluid
    // cell has to notice what is now above it (it becomes full strength), and
    // the cell above has to notice that it can no longer pour down.
    schedule(x, y + 1, z, due_tick);
    schedule(x, y - 1, z, due_tick);
}

void FluidSim::notify_block_changed(int32_t x, int32_t y, int32_t z) {
    if (!enabled() || map_ == nullptr) return;

    // Filter before waking anything: a block edit with no fluid within one cell
    // cannot change a fluid's answer, and that is most edits a player makes.
    // Seven block reads is nothing next to a window scan.
    FluidKind kind = FluidKind::None;
    const int32_t offsets[7][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 },
                                    { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 } };
    for (const auto& offset : offsets) {
        const int block = map_->get_block_world(x + offset[0], y + offset[1], z + offset[2]);
        if (table_->is_fluid(static_cast<BlockID>(block))) {
            kind = table_->state_of(static_cast<BlockID>(block)).kind;
            break;
        }
    }
    if (kind == FluidKind::None) return;

    schedule_around(x, y, z, current_tick_ + delay_ticks_for(kind));
}

void FluidSim::advance(double delta) {
    if (!enabled() || map_ == nullptr) return;
    if (queue_.empty()) {
        // Idle: no backlog accumulates while nothing is flowing.
        accumulator_ = 0.0;
        stats_.pending = 0;
        return;
    }

    const double tick_seconds = 1.0 / config_.tick_rate_hz;
    accumulator_ += delta;
    if (accumulator_ > config_.max_accumulator) accumulator_ = config_.max_accumulator;

    int ticks = 0;
    while (accumulator_ >= tick_seconds && ticks < config_.max_ticks_per_frame && !queue_.empty()) {
        accumulator_ -= tick_seconds;
        run_tick();
        ++ticks;
    }
    stats_.pending = scheduled_.size();
}

int FluidSim::run_to_settled(int max_ticks) { return run_ticks(max_ticks); }

int FluidSim::run_ticks(int max_ticks) {
    if (!enabled() || map_ == nullptr) return 0;
    int ticks = 0;
    while (!queue_.empty() && ticks < max_ticks) {
        run_tick();
        ++ticks;
    }
    stats_.pending = scheduled_.size();
    return ticks;
}

void FluidSim::run_tick() {
    const auto start = std::chrono::steady_clock::now();
    ++current_tick_;
    ++stats_.ticks;
    stats_.last_tick_cells = 0;
    stats_.last_tick_writes = 0;
    stats_.frozen = 0;
    stats_.settled = 0;

    // Writes are collected for the whole tick and applied at the end, so a tick
    // costs one exclusive lock and one remesh mark per chunk instead of one per
    // cell. It is safe because every write wakes the cell it landed in and that
    // cell's neighbours for a later tick, so nothing misses the change.
    writes_.clear();

    while (!queue_.empty() && stats_.last_tick_cells < config_.cells_per_tick) {
        const Entry entry = queue_.top();
        if (entry.due > current_tick_) break;
        queue_.pop();

        const int64_t key = pack(entry.x, entry.y, entry.z);
        const auto it = scheduled_.find(key);
        if (it == scheduled_.end()) continue;        // handled already
        if (it->second != entry.due) continue;       // superseded by an earlier wake-up
        scheduled_.erase(it);

        ++stats_.last_tick_cells;
        ++stats_.cells_ticked;
        tick_cell(entry.x, entry.y, entry.z);
    }

    if (!writes_.empty()) {
        stats_.last_tick_writes = world_.apply_writes(*map_, sink_, writes_);
    }

    stats_.last_tick_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stats_.pending = scheduled_.size();
}

void FluidSim::tick_cell(int32_t x, int32_t y, int32_t z) {
    if (!world_.read_window(*map_, x, y, z)) {
        // A chunk this cell's window needs is not resident, so there is no way
        // to know what it becomes. Never decide with missing data: a missing
        // chunk reads as air, which would let the flood pour into space that is
        // regenerated over it, and for the cell itself "nothing beside me feeds
        // me" would read as "dry up" — draining the water at the edge of loaded
        // space. It is left exactly as it is, and it is woken again when the
        // chunk it was waiting on loads.
        ++stats_.frozen;
        schedule(x, y, z, current_tick_ + kFrozenRetryTicks);
        return;
    }

    const FluidCell current = world_.fluid_at(x, y, z);
    if (!current.present()) return;  // removed or rewritten since it was queued

    const FluidStep step = tick(world_, x, y, z);
    const uint64_t wake = current_tick_ + delay_ticks_for(current.kind);

    if (step.remove) {
        // An empty state maps to AIR: the cell stops being fluid.
        writes_.push_back(CellWrite{ x, y, z, FluidCell{} });
    } else if (step.changed) {
        writes_.push_back(CellWrite{ x, y, z, step.next });
    }
    for (int i = 0; i < step.write_count; ++i) {
        writes_.push_back(step.writes[i]);
    }

    if (step.remove || step.changed) {
        schedule_around(x, y, z, wake);
    } else {
        // Settled. It may still have pushed fluid outward this tick, but nothing
        // about it will change anyone's mind again, so it is not scheduled and
        // will not be looked at until something wakes it.
        ++stats_.settled;
    }
    for (int i = 0; i < step.write_count; ++i) {
        schedule_around(step.writes[i].x, step.writes[i].y, step.writes[i].z, wake);
    }
}

} // namespace fluids
} // namespace VoxelEngine
