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
#include <map>
#include <vector>
#include <iterator>

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

} // namespace

PasteWriteResult BlockEditor::apply_paste(const schematic::PastePlan& plan,
                                          const schematic::PasteOptions& options,
                                          bool append_undo,
                                          std::vector<schematic::PastePlan::Cell>* unwritten) {
    using schematic::PastePlan;
    PasteWriteResult result;
    if (plan.empty()) return result;

    ChunkMap& cm = chunk_world->get_chunk_map();
    const BlockRegistry& registry = BlockRegistry::get_instance();

    // Group the cells by chunk, key-ordered so the lock bands and the write
    // order are the same for the same plan on any machine.
    struct Group {
        int32_t cx = 0, cy = 0, cz = 0;
        std::vector<size_t> cells;
    };
    std::map<uint64_t, Group> groups;
    for (size_t i = 0; i < plan.cells.size(); ++i) {
        const PastePlan::Cell& cell = plan.cells[i];
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        Group& group = groups[cm.get_chunk_key(cx, cy, cz)];
        group.cx = cx;
        group.cy = cy;
        group.cz = cz;
        group.cells.push_back(i);
    }

    // Index-aligned with each other: what each written cell displaced, and what
    // it was set to. The first becomes the undo record, the second is what the
    // edit map and the fluid simulation hear about.
    std::vector<PastePlan::Cell> displaced;
    std::vector<PastePlan::Cell> written;
    displaced.reserve(plan.cells.size());
    written.reserve(plan.cells.size());
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

    for (auto& entry : groups) {
        Group& group = entry.second;
        // One exclusive band for the whole chunk, matching place_block's reach
        // (a block write plus a light update never leaves the 3×3×3 around it),
        // rather than a lock per cell.
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = cm.get_chunk_key(group.cx + dx, group.cy + dy, group.cz + dz);
        auto lock = cm.lock_keys_exclusive(keys);

        ChunkData* chunk = cm.get_chunk_data_fast(group.cx, group.cy, group.cz);
        if (chunk == nullptr) {
            // The plan was built against a world that has since moved on here.
            // Nothing is queued by the writer itself: a paste is not a player
            // edit at the loading frontier, so it reports the cells back and the
            // caller decides (the paste job waits for the chunk and retries).
            result.skipped_unloaded += group.cells.size();
            if (unwritten != nullptr) {
                for (const size_t index : group.cells) unwritten->push_back(plan.cells[index]);
            }
            continue;
        }
        ChunkData* above = cm.get_chunk_data_fast(group.cx, group.cy + 1, group.cz);
        ChunkRenderData* render = cm.get_chunk_render_data_fast(group.cx, group.cy, group.cz);

        // Columns whose opacity changed, so sky light is recomputed once per
        // column rather than once per cell.
        std::vector<uint32_t> sky_columns;
        bool wrote_here = false;
        bool light_relevant_here = false;

        for (const size_t index : group.cells) {
            const PastePlan::Cell& cell = plan.cells[index];
            int32_t cx, cy, cz, lx, ly, lz;
            world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
            if (!is_local_in_bounds(lx, ly, lz)) {
                ++result.skipped_out_of_bounds;
                continue;
            }
            const BlockID old_block = chunk->get_block_unsafe(lx, ly, lz);
            if (old_block == cell.block) {
                ++result.skipped_unchanged;
                continue;
            }
            if (!options.replace_solid && old_block != BlockIDs::AIR) {
                ++result.skipped_covered;
                continue;
            }

            chunk->set_block(lx, ly, lz, cell.block);

            const BlockType& old_type = registry.get_block_fast(old_block);
            const BlockType& new_type = registry.get_block_fast(cell.block);
            const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
            const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);
            // Whether BLOCK light can have moved here: whether light passes through
            // the cell, or either block emits.
            if (old_opaque != new_opaque || old_type.light_opacity != new_type.light_opacity ||
                HasProperty(old_type.properties, BlockProperty::Emissive) ||
                HasProperty(new_type.properties, BlockProperty::Emissive)) {
                light_relevant_here = true;
            }
            if (old_opaque != new_opaque) {
                const uint32_t column = (static_cast<uint32_t>(lx) << 16) |
                                        static_cast<uint32_t>(lz & 0xFFFF);
                bool known = false;
                for (const uint32_t seen : sky_columns) {
                    if (seen == column) { known = true; break; }
                }
                if (!known) sky_columns.push_back(column);
            }

            // Whether this write can change what the fluid simulation would do, asked
            // HERE because here the six neighbours are reads of the chunk already in
            // hand. Waking every written cell instead made the main thread pay a
            // locked neighbour scan per CELL to discover that a stone wall has no
            // fluid anywhere near it.
            if (paste_cell_needs_fluid_wake(registry, *chunk, cm, old_block, cell.block,
                                            cell.x, cell.y, cell.z, lx, ly, lz)) {
                wake_world.push_back({cell.x, cell.y, cell.z});
            }

            displaced.push_back(PastePlan::Cell{cell.x, cell.y, cell.z, old_block});
            written.push_back(cell);
            ++result.written;
            // This chunk (not the volume) needs a remesh and a relight, and that
            // is per chunk, so it is a separate flag from the bounds above.
            wrote_here = true;

            if (!have_bounds) {
                result.min_x = result.max_x = cell.x;
                result.min_y = result.max_y = cell.y;
                result.min_z = result.max_z = cell.z;
                have_bounds = true;
            } else {
                if (cell.x < result.min_x) result.min_x = cell.x;
                if (cell.x > result.max_x) result.max_x = cell.x;
                if (cell.y < result.min_y) result.min_y = cell.y;
                if (cell.y > result.max_y) result.max_y = cell.y;
                if (cell.z < result.min_z) result.min_z = cell.z;
                if (cell.z > result.max_z) result.max_z = cell.z;
            }

            if (render) {
                render->is_mesh_dirty = true;
                render->mesh_version++;
                render->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(lx, ly, lz));
                render->mark_block_dirty(lx, ly, lz);
            }
        }

        // Sky light is a column property: recompute the touched columns of this
        // chunk while the band is still held, exactly as a single edit does.
        for (const uint32_t column : sky_columns) {
            chunk->propagate_sky_light_column(static_cast<int32_t>(column >> 16),
                                              static_cast<int32_t>(column & 0xFFFF), above);
        }
        if (wrote_here) {
            touched.push_back({group.cx, group.cy, group.cz});
            if (light_relevant_here) {
                light_touched.push_back({group.cx, group.cy, group.cz});
            }
        }
    }

    // Locks released. Persist the edits one CHUNK at a time, then wake the fluid
    // cells that can matter, then relight and remesh each touched chunk once.
    //
    // The writes are grouped by the same loop that produced them, so the cells of one
    // chunk are already consecutive in `written` — a run can be handed over without
    // copying or regrouping anything.
    {
        std::vector<ChunkWorld::EditCell> run;
        int32_t run_cx = 0, run_cy = 0, run_cz = 0;
        bool have_run = false;
        // A chunk's cells can span several bands' worth of runs, so the run is
        // flushed whenever the chunk changes and the last one after the loop.
        auto flush_run = [&]() {
            if (have_run && !run.empty()) {
                chunk_world->add_block_edits(run_cx, run_cy, run_cz, run);
            }
            run.clear();
        };
        for (const PastePlan::Cell& cell : written) {
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
    // The fluid wakes, which used to be a side effect of persisting each cell.
    for (const std::array<int32_t, 3>& pos : wake_world) {
        chunk_world->notify_block_change(pos[0], pos[1], pos[2]);
    }
    for (const std::array<int32_t, 3>& pos : touched) {
        chunk_world->mark_chunk_dirty(pos[0], pos[1], pos[2]);
        mesh_manager->queue_dirty_chunk(pos[0], pos[1], pos[2]);
    }
    // Relights the 3×3×3 neighbourhood (and dirties the meshes of the chunks whose
    // light actually changed), which is what a block change can reach. Only for the
    // chunks where a written cell could have moved light: over a build that is
    // mostly stone, this is the difference between a region pass per touched chunk
    // and almost none of them.
    for (const std::array<int32_t, 3>& pos : light_touched) {
        light_propagator->propagate_block_light_region(pos[0], pos[1], pos[2]);
    }

    result.chunks_touched = touched.size();
    result.undo_available = !displaced.empty();
    // A paste that wrote nothing leaves the previous record alone: it did not
    // make the last one unreachable.
    if (!displaced.empty()) {
        if (append_undo && paste_undo_.valid()) {
            // Another batch of the SAME paste: its displaced blocks join the
            // record rather than replacing it, so one undo takes the whole thing
            // back. The batches are disjoint by construction (a cell is written
            // once and then dropped from the plan), so no cell is recorded twice.
            paste_undo_.cells.insert(paste_undo_.cells.end(),
                                     std::make_move_iterator(displaced.begin()),
                                     std::make_move_iterator(displaced.end()));
        } else {
            paste_undo_.cells = std::move(displaced);
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
