// Bulk paste: the volume writer (one lock band, one light pass and one remesh
// bookkeeping per chunk instead of per block), the fluid wake rule it needs to
// answer from inside the locked chunk, and the one-level undo that calls it back.
// Kept apart from world/block_editor.cpp, whose write path is the single-block
// shape of the same idea.

#include "world/block_editor.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "core/block_types.hpp"
#include <array>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace VoxelEngine {
using namespace godot;

// -------------------------------------------------------------------------
// Bulk paste
// -------------------------------------------------------------------------

namespace {

// Same rule as BlockEditor::is_local_in_bounds, for a free function that has no
// `this`. Both answer "is this cell inside the chunk it names".
bool cell_in_chunk(int32_t lx, int32_t ly, int32_t lz) {
    return lx >= 0 && lx < CHUNK_WIDTH && ly >= 0 && ly < CHUNK_HEIGHT && lz >= 0 &&
           lz < CHUNK_DEPTH;
}

// Whether a written cell can change what the fluid simulation would do: the cell
// became or stopped being a fluid, or a face-neighbour is one. That is the
// simulation's own rule, answered here because here the neighbours are reads of the
// chunk already in hand.
//
// Only cells that pass this are woken, and the simulation re-tests each one exactly
// as before — so nothing that mattered can be missed: a fluid cell is always woken
// by its own write, and the simulation's scan of that cell covers every neighbour it
// could affect.
bool paste_cell_needs_fluid_wake(const BlockRegistry& registry, const ChunkData& chunk,
                                 const ChunkMap& cm, BlockID old_block, BlockID new_block,
                                 int32_t world_x, int32_t world_y, int32_t world_z, int32_t lx,
                                 int32_t ly, int32_t lz) {
    if (registry.get_block_fast(new_block).is_fluid_state()) return true;
    if (registry.get_block_fast(old_block).is_fluid_state()) return true;
    static constexpr int32_t kOffsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto& offset : kOffsets) {
        const int32_t nx = lx + offset[0];
        const int32_t ny = ly + offset[1];
        const int32_t nz = lz + offset[2];
        // Inside the chunk: an array read. Across the seam: the one case that has
        // to ask the world, and only for the cells that sit on a face.
        //
        // The _fast accessor is required, not an optimisation: the caller holds the
        // 3x3x3 exclusive band for the chunk being written, and a face neighbour is at
        // most one chunk away, so its shard IS one of the ones held. The locking
        // accessor would take a shared lock on a shard this thread owns exclusively,
        // which std::shared_mutex answers with a deadlock — a hang, not a wrong
        // answer, and one every big paste hits because a build spanning chunks always
        // has cells on a face. Asking under the lock we already hold is the whole
        // reason the neighbour is read here instead of through the world.
        const BlockID neighbor = cell_in_chunk(nx, ny, nz)
            ? chunk.get_block_unsafe(nx, ny, nz)
            : static_cast<BlockID>(cm.get_block_world_fast(world_x + offset[0],
                                                           world_y + offset[1],
                                                           world_z + offset[2]));
        if (registry.get_block_fast(neighbor).is_fluid_state()) return true;
    }
    return false;
}

// -------------------------------------------------------------------------
// The phases of a paste
// -------------------------------------------------------------------------

// One apply_paste call's state and accumulators, so the phases below can be
// functions of their own instead of one function threading ten locals.
struct PasteWrite {
    const schematic::PastePlan& plan;
    const schematic::PasteOptions& options;
    ChunkMap& cm;
    const BlockRegistry& registry;

    PasteWriteResult result;
    // Index-aligned with each other: what each written cell displaced, and what
    // it was set to. The first becomes the undo record, the second is what the
    // edit map and the fluid simulation hear about.
    std::vector<schematic::PastePlan::Cell> displaced;
    std::vector<schematic::PastePlan::Cell> written;
    std::vector<std::array<int32_t, 3>> touched;
    // World positions whose write can change a fluid's answer, decided while the
    // chunk was in hand and woken once every band is released.
    std::vector<std::array<int32_t, 3>> wake_world;
    // The subset of `touched` whose BLOCK light can actually have changed. Every
    // chunk a paste writes to needs a remesh, but only these need the 3×3×3 region
    // pass — and that pass is the expensive half of a paste by a wide margin
    // (measured: milliseconds a chunk against fractions of one for the write). A
    // chunk of stone swapped for other stone, or a wall built out of blocks that
    // were already opaque, moves no light at all.
    std::vector<std::array<int32_t, 3>> light_touched;
    // The volume the paste actually changed, accumulated across every chunk it
    // touches — NOT per chunk, which would report only the last chunk's box.
    bool have_bounds = false;
};

// The cells of one chunk, key-ordered by the caller.
struct PasteGroup {
    int32_t cx = 0, cy = 0, cz = 0;
    std::vector<size_t> cells;
};

// What one chunk's cell writes accumulate for it.
struct PasteChunkState {
    // Columns whose opacity changed, so sky light is recomputed once per
    // column rather than once per cell.
    std::vector<uint32_t> sky_columns;
    bool wrote_here = false;
    bool light_relevant_here = false;
};

// Groups a plan's cells by the chunk they land in, key-ordered so the lock bands
// and the write order are the same for the same plan on any machine.
void group_paste_cells(PasteWrite& write, std::map<uint64_t, PasteGroup>& groups) {
    for (size_t i = 0; i < write.plan.cells.size(); ++i) {
        const schematic::PastePlan::Cell& cell = write.plan.cells[i];
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        PasteGroup& group = groups[write.cm.get_chunk_key(cx, cy, cz)];
        group.cx = cx;
        group.cy = cy;
        group.cz = cz;
        group.cells.push_back(i);
    }
}

// Writes one cell into a chunk that is already locked and resident, and records
// what the write means for the volume: the undo record, the edit map, the light
// work and the fluid wakes.
void write_paste_cell(PasteWrite& write, PasteChunkState& state, ChunkData& chunk,
                      ChunkRenderData* render, size_t index) {
    const schematic::PastePlan::Cell& cell = write.plan.cells[index];
    int32_t cx, cy, cz, lx, ly, lz;
    world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
    // The same rule as BlockEditor::is_local_in_bounds, for a free function that
    // has no `this`.
    if (!cell_in_chunk(lx, ly, lz)) {
        ++write.result.skipped_out_of_bounds;
        return;
    }
    const BlockID old_block = chunk.get_block_unsafe(lx, ly, lz);
    if (old_block == cell.block) {
        ++write.result.skipped_unchanged;
        return;
    }
    if (!write.options.replace_solid && old_block != BlockIDs::AIR) {
        ++write.result.skipped_covered;
        return;
    }

    chunk.set_block(lx, ly, lz, cell.block);

    const BlockType& old_type = write.registry.get_block_fast(old_block);
    const BlockType& new_type = write.registry.get_block_fast(cell.block);
    const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
    const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);
    // Whether BLOCK light can have moved here: whether light passes through
    // the cell, or either block emits.
    if (old_opaque != new_opaque || old_type.light_opacity != new_type.light_opacity ||
        HasProperty(old_type.properties, BlockProperty::Emissive) ||
        HasProperty(new_type.properties, BlockProperty::Emissive)) {
        state.light_relevant_here = true;
    }
    if (old_opaque != new_opaque) {
        const uint32_t column = (static_cast<uint32_t>(lx) << 16) |
                                static_cast<uint32_t>(lz & 0xFFFF);
        bool known = false;
        for (const uint32_t seen : state.sky_columns) {
            if (seen == column) { known = true; break; }
        }
        if (!known) state.sky_columns.push_back(column);
    }

    // Whether this write can change what the fluid simulation would do, asked
    // HERE because here the six neighbours are reads of the chunk already in
    // hand. Waking every written cell instead made the main thread pay a
    // locked neighbour scan per CELL to discover that a stone wall has no
    // fluid anywhere near it.
    if (paste_cell_needs_fluid_wake(write.registry, chunk, write.cm, old_block, cell.block,
                                    cell.x, cell.y, cell.z, lx, ly, lz)) {
        write.wake_world.push_back({cell.x, cell.y, cell.z});
    }

    write.displaced.push_back(schematic::PastePlan::Cell{cell.x, cell.y, cell.z, old_block});
    write.written.push_back(cell);
    ++write.result.written;
    // This chunk (not the volume) needs a remesh and a relight, and that
    // is per chunk, so it is a separate flag from the bounds above.
    state.wrote_here = true;

    if (!write.have_bounds) {
        write.result.min_x = write.result.max_x = cell.x;
        write.result.min_y = write.result.max_y = cell.y;
        write.result.min_z = write.result.max_z = cell.z;
        write.have_bounds = true;
    } else {
        if (cell.x < write.result.min_x) write.result.min_x = cell.x;
        if (cell.x > write.result.max_x) write.result.max_x = cell.x;
        if (cell.y < write.result.min_y) write.result.min_y = cell.y;
        if (cell.y > write.result.max_y) write.result.max_y = cell.y;
        if (cell.z < write.result.min_z) write.result.min_z = cell.z;
        if (cell.z > write.result.max_z) write.result.max_z = cell.z;
    }

    if (render) {
        render->is_mesh_dirty = true;
        render->mesh_version++;
        render->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(lx, ly, lz));
        render->mark_block_dirty(lx, ly, lz);
    }
}

// Writes every cell of one chunk under a single exclusive band, then leaves the
// chunk's sky light and the two per-chunk flags behind.
void write_paste_group(PasteWrite& write, PasteGroup& group,
                       std::vector<schematic::PastePlan::Cell>* unwritten) {
    // One exclusive band for the whole chunk, matching place_block's reach
    // (a block write plus a light update never leaves the 3×3×3 around it),
    // rather than a lock per cell.
    uint64_t keys[27];
    int idx = 0;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                keys[idx++] = write.cm.get_chunk_key(group.cx + dx, group.cy + dy, group.cz + dz);
    auto lock = write.cm.lock_keys_exclusive(keys);

    ChunkData* chunk = write.cm.get_chunk_data_fast(group.cx, group.cy, group.cz);
    if (chunk == nullptr) {
        // The plan was built against a world that has since moved on here.
        // Nothing is queued by the writer itself: a paste is not a player
        // edit at the loading frontier, so it reports the cells back and the
        // caller decides (the paste job waits for the chunk and retries).
        write.result.skipped_unloaded += group.cells.size();
        if (unwritten != nullptr) {
            for (const size_t index : group.cells) unwritten->push_back(write.plan.cells[index]);
        }
        return;
    }
    ChunkData* above = write.cm.get_chunk_data_fast(group.cx, group.cy + 1, group.cz);
    ChunkRenderData* render = write.cm.get_chunk_render_data_fast(group.cx, group.cy, group.cz);

    PasteChunkState state;
    for (const size_t index : group.cells) {
        write_paste_cell(write, state, *chunk, render, index);
    }

    // Sky light is a column property: recompute the touched columns of this
    // chunk while the band is still held, exactly as a single edit does.
    for (const uint32_t column : state.sky_columns) {
        chunk->propagate_sky_light_column(static_cast<int32_t>(column >> 16),
                                          static_cast<int32_t>(column & 0xFFFF), above);
    }
    if (state.wrote_here) {
        write.touched.push_back({group.cx, group.cy, group.cz});
        if (state.light_relevant_here) {
            write.light_touched.push_back({group.cx, group.cy, group.cz});
        }
    }
}

// Persists the edits one CHUNK at a time. The writes are grouped by the same loop
// that produced them, so the cells of one chunk are already consecutive in
// `written` — a run can be handed over without copying or regrouping anything.
void persist_paste_edits(ChunkWorld& world, const PasteWrite& write) {
    std::vector<ChunkWorld::EditCell> run;
    int32_t run_cx = 0, run_cy = 0, run_cz = 0;
    bool have_run = false;
    // A chunk's cells can span several bands' worth of runs, so the run is
    // flushed whenever the chunk changes and the last one after the loop.
    auto flush_run = [&]() {
        if (have_run && !run.empty()) {
            world.add_block_edits(run_cx, run_cy, run_cz, run);
        }
        run.clear();
    };
    for (const schematic::PastePlan::Cell& cell : write.written) {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        if (have_run && (cx != run_cx || cy != run_cy || cz != run_cz)) {
            flush_run();
        }
        run_cx = cx;
        run_cy = cy;
        run_cz = cz;
        have_run = true;
        run.push_back(ChunkWorld::EditCell{lx, ly, lz, cell.block});
    }
    flush_run();
}

// Once every band is released: wake the fluid cells that can matter, then dirty
// and remesh each touched chunk once, then relight the 3×3×3 neighbourhood of the
// chunks where a written cell could actually have moved light (and dirty the
// meshes of the chunks whose light did change).
void publish_paste_writes(ChunkWorld& world, MeshManager& meshes, LightPropagator& light,
                          const PasteWrite& write) {
    // The fluid wakes, which used to be a side effect of persisting each cell.
    for (const std::array<int32_t, 3>& pos : write.wake_world) {
        world.notify_block_change(pos[0], pos[1], pos[2]);
    }
    for (const std::array<int32_t, 3>& pos : write.touched) {
        world.mark_chunk_dirty(pos[0], pos[1], pos[2]);
        meshes.queue_dirty_chunk(pos[0], pos[1], pos[2]);
    }
    for (const std::array<int32_t, 3>& pos : write.light_touched) {
        light.propagate_block_light_region(pos[0], pos[1], pos[2]);
    }
}
} // namespace

PasteWriteResult BlockEditor::apply_paste(const schematic::PastePlan& plan,
                                          const schematic::PasteOptions& options,
                                          bool append_undo,
                                          std::vector<schematic::PastePlan::Cell>* unwritten) {
    if (plan.empty()) return PasteWriteResult{};

    PasteWrite write{plan, options, chunk_world->get_chunk_map(), BlockRegistry::get_instance()};
    // One entry per planned cell at worst, so a big paste's accumulators never
    // reallocate while it runs.
    write.displaced.reserve(plan.cells.size());
    write.written.reserve(plan.cells.size());

    std::map<uint64_t, PasteGroup> groups;
    group_paste_cells(write, groups);
    for (auto& entry : groups) {
        write_paste_group(write, entry.second, unwritten);
    }

    // Locks released. Persist the edits a chunk at a time, wake the fluid cells
    // that can matter, then relight and remesh each touched chunk once.
    persist_paste_edits(*chunk_world, write);
    publish_paste_writes(*chunk_world, *mesh_manager, *light_propagator, write);

    PasteWriteResult& result = write.result;
    result.chunks_touched = write.touched.size();
    result.undo_available = !write.displaced.empty();
    // A paste that wrote nothing leaves the previous record alone: it did not
    // make the last one unreachable.
    if (!write.displaced.empty()) {
        if (append_undo && paste_undo_.valid()) {
            // Another batch of the SAME paste: its displaced blocks join the
            // record rather than replacing it, so one undo takes the whole thing
            // back. The batches are disjoint by construction (a cell is written
            // once and then dropped from the plan), so no cell is recorded twice.
            paste_undo_.cells.insert(paste_undo_.cells.end(),
                                     std::make_move_iterator(write.displaced.begin()),
                                     std::make_move_iterator(write.displaced.end()));
        } else {
            paste_undo_.cells = std::move(write.displaced);
        }
    }
    return result;
}

bool BlockEditor::undo_paste(PasteWriteResult* out) {
    if (!paste_undo_.valid()) return false;

    // The record is taken out of the way first, because the write below replaces
    // it with what the revert itself displaced (which is the pasted build, and
    // would make undo a toggle).
    schematic::PasteUndo record = std::move(paste_undo_);
    paste_undo_ = schematic::PasteUndo{};

    schematic::PasteOptions options;
    // A revert restores the volume exactly, holes included, so it replaces what
    // is there and writes air.
    options.replace_solid = true;
    options.write_air = true;

    // Cells whose chunk is gone (evicted since the paste, on a build wide enough
    // to reach past the streaming frontier) come back as "not reverted" rather
    // than being silently dropped: the record keeps exactly those cells, so
    // calling again finishes the job — and reports `restored`/`unloaded` so the
    // caller can tell a finished undo from a partial one.
    std::vector<schematic::PastePlan::Cell> unreached;
    PasteWriteResult result =
        apply_paste(schematic::to_revert_plan(record), options, false, &unreached);
    result.restored = result.written;
    result.unloaded = unreached.size();
    if (unreached.empty()) {
        paste_undo_ = schematic::PasteUndo{};  // fully reverted, and it is spent
    } else {
        // Keep the one-level record, narrowed to what is left to put back.
        record.cells = std::move(unreached);
        paste_undo_ = std::move(record);
    }
    if (out) *out = result;
    return result.written > 0;
}

} // namespace VoxelEngine
