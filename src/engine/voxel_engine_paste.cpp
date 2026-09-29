// Pasting: the plan applied to the world, the one-level undo behind it, and the job
// that streams a paste in when the chunks it needs are not resident yet. Kept apart
// from engine/voxel_engine_schematic.cpp, which only decodes and describes.

#include "engine/voxel_engine_controller.hpp"

#include "schematic/paste_plan.hpp"
#include "schematic/schematic_reader.hpp"
#include "world/block_editor.hpp"

#include <godot_cpp/variant/vector3i.hpp>

#include <algorithm>
#include <unordered_set>

namespace VoxelEngine {
using namespace godot;

Dictionary VoxelEngineController::paste_schematic_bytes(const PackedByteArray& bytes,
                                                        int32_t origin_x, int32_t origin_y,
                                                        int32_t origin_z,
                                                        const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    const schematic::SchematicData* file = nullptr;
    schematic::PastePlan plan;
    std::string error;
    if (!decode_and_plan(bytes, options, origin_x, origin_y, origin_z, paste_options, file, plan,
                         error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    // A paste replaces whatever the previous one was still waiting on: two
    // half-landed buildings and one undo record is worse than a refusal.
    cancel_pending_paste();

    std::vector<schematic::PastePlan::Cell> unwritten;
    unwritten.reserve(plan.cells.size());
    const PasteWriteResult written =
        block_editor.apply_paste(plan, paste_options, false, &unwritten);

    // Cells whose chunk is not resident are not dropped. The chunks are asked
    // for (urgently — the streaming sweep would never generate the sky above a
    // build) and pinned, and the write is retried every frame until nothing is
    // left; `tick_pending_paste` is that loop. This is what turns "14,592 cells
    // skipped: their chunk is not loaded" into a pause.
    size_t pending_cells = 0;
    if (!unwritten.empty()) {
        PendingPaste job;
        job.options = paste_options;
        job.planned = plan.cells.size();
        job.totals = written;
        // The initial write above is this paste's first batch, so the job's own
        // batches add to its undo record rather than replacing it.
        job.undo_started = written.written > 0;
        job.remaining = std::move(unwritten);
        pending_cells = job.remaining.size();
        pending_paste_storage = std::move(job);
        has_pending_paste = true;
        // Start the first batch of requests now rather than next frame, so the
        // chunks a build is waiting on begin generating on this tick.
        tick_pending_paste(0.0);
    }

    // Both halves are reported, because they can disagree in a way the player
    // needs to see: the plan counts what the file implies, the write counts what
    // the world actually took.
    result["ok"] = true;
    result["file_width"] = file->width;
    result["file_height"] = file->height;
    result["file_length"] = file->length;
    result["format"] = String(schematic::block_file_format_name(file->format));
    result["format_version"] = file->format_version;
    // The file's own numbers as well as the world's, so a caller can tell "the
    // table could not map this" from "the world would not take it".
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    result["cells"] = static_cast<int64_t>(written.written);
    result["replaced"] = static_cast<int64_t>(written.written - written.skipped_unchanged);
    result["covered"] = static_cast<int64_t>(written.skipped_covered);
    result["unchanged"] = static_cast<int64_t>(written.skipped_unchanged);
    result["unloaded_chunks"] = static_cast<int64_t>(written.skipped_unloaded);
    result["chunks"] = static_cast<int64_t>(written.chunks_touched);
    result["undo_cells"] = static_cast<int64_t>(block_editor.paste_undo_cells());
    // Cells still waiting for their chunks. The paste is not finished, but it is
    // not lost either: what did land is real, and the rest is queued.
    result["pending_cells"] = static_cast<int64_t>(pending_cells);
    result["pending"] = has_pending_paste;
    if (has_pending_paste) {
        result["queued_chunks"] = static_cast<int64_t>(pending_paste_storage.requested.size());
    }
    if (written.max_x >= written.min_x) {
        result["min"] = Vector3i(written.min_x, written.min_y, written.min_z);
        result["max"] = Vector3i(written.max_x, written.max_y, written.max_z);
    }
    return result;
}

Dictionary VoxelEngineController::undo_paste() {
    Dictionary result;
    // A paste still waiting on chunks is abandoned first: the undo record covers
    // what did land (the wait-batches appended to it), and cells that were never
    // written have nothing to revert.
    cancel_pending_paste();
    PasteWriteResult written;
    const bool undone = block_editor.undo_paste(&written);
    // A revert can come up short when a build is wide enough that its outer
    // cells streamed out since the paste: the record keeps those cells, so this
    // is a partial undo to report (and to continue), not a failure.
    const bool stuck_only = !undone && written.unloaded > 0;
    result["ok"] = undone || stuck_only;
    if (!undone && !stuck_only) {
        result["error"] = String("nothing to undo (no paste this session)");
        return result;
    }
    // The breakdown adds up to the record the paste left: a cell is restored,
    // already what it has to be (`unchanged` — the world can have been
    // regenerated since, which puts the pre-paste block back on its own), out of
    // the world, or still queued because its chunk is not resident. Anything
    // else is a cell the revert dropped on the floor.
    result["cells"] = static_cast<int64_t>(written.restored);
    result["chunks"] = static_cast<int64_t>(written.chunks_touched);
    result["unchanged"] = static_cast<int64_t>(written.skipped_unchanged);
    result["out_of_bounds"] = static_cast<int64_t>(written.skipped_out_of_bounds);
    result["unloaded"] = static_cast<int64_t>(written.unloaded);
    result["remaining"] = static_cast<int64_t>(block_editor.paste_undo_cells());
    return result;
}

int64_t VoxelEngineController::paste_undo_cells() const {
    return static_cast<int64_t>(block_editor.paste_undo_cells());
}

// ---------------------------------------------------------------------------
// A paste that had to wait for chunks
// ---------------------------------------------------------------------------

namespace {

// How many chunks of one paste may be in flight at a time. Small on purpose:
// the point is to stream a build in, not to generate a city in one frame. Each
// one costs a generation on a worker and a slice of the install budget.
constexpr size_t kPasteChunksInFlight = 16;
// How long a paste will wait for its chunks before giving up on the rest. Long
// enough for the far corner of a large build (generation is asynchronous, and
// the pool is shared with the chunks the player is walking into), short enough
// that a request which can never be met does not sit there for the session.
constexpr double kPasteWaitTimeoutMs = 30000.0;

// Folds one batch's counts into the job's running totals, so the numbers a
// caller finally sees describe the whole paste and not just its last batch.
void accumulate_paste_result(PasteWriteResult& into, const PasteWriteResult& batch) {
    into.written += batch.written;
    into.skipped_covered += batch.skipped_covered;
    into.skipped_unchanged += batch.skipped_unchanged;
    into.skipped_unloaded += batch.skipped_unloaded;
    into.skipped_out_of_bounds += batch.skipped_out_of_bounds;
    into.chunks_touched += batch.chunks_touched;
    if (batch.max_x < batch.min_x) return;
    if (into.max_x < into.min_x) {
        into.min_x = batch.min_x;
        into.max_x = batch.max_x;
        into.min_y = batch.min_y;
        into.max_y = batch.max_y;
        into.min_z = batch.min_z;
        into.max_z = batch.max_z;
        return;
    }
    into.min_x = std::min(into.min_x, batch.min_x);
    into.max_x = std::max(into.max_x, batch.max_x);
    into.min_y = std::min(into.min_y, batch.min_y);
    into.max_y = std::max(into.max_y, batch.max_y);
    into.min_z = std::min(into.min_z, batch.min_z);
    into.max_z = std::max(into.max_z, batch.max_z);
}

} // namespace

void VoxelEngineController::cancel_pending_paste() {
    if (!has_pending_paste) return;
    // The chunks were pinned for this paste alone: release them all, then let
    // the unload pass have its normal say again.
    for (const uint64_t key : pending_paste_storage.requested) {
        chunk_world.unpin_chunk(key);
    }
    pending_paste_storage = PendingPaste{};
    has_pending_paste = false;
}

void VoxelEngineController::finish_pending_paste(bool abandoned) {
    if (!has_pending_paste) return;
    const PendingPaste& job = pending_paste_storage;
    // Publish the report before the job is dropped: the caller sees one message
    // per paste whichever way it ended.
    paste_report_ready = true;
    paste_report_abandoned = abandoned;
    paste_report_cells = job.totals.written;
    paste_report_leftover = job.remaining.size();
    paste_report_chunks = job.totals.chunks_touched;
    paste_report_waited_ms = job.waited_ms;
    paste_report_unchanged = job.totals.skipped_unchanged;
    paste_report_covered = job.totals.skipped_covered;
    paste_report_out_of_bounds = job.totals.skipped_out_of_bounds;
    paste_report_planned = job.planned;
    cancel_pending_paste();
}

void VoxelEngineController::tick_pending_paste(double delta) {
    if (!has_pending_paste) return;
    PendingPaste& job = pending_paste_storage;
    ChunkMap& chunk_map = chunk_world.get_chunk_map();
    job.waited_ms += delta * 1000.0;

    // 1. Write whatever can be written now. The writer hands back exactly the
    //    cells whose chunk was not resident — which is precisely the set that
    //    has to stay in `remaining` — so one call per frame is the whole retry
    //    mechanism. Asking the chunk map for that set afterwards instead would
    //    race the arriving chunks and lose the cells in between.
    if (!job.remaining.empty()) {
        schematic::PastePlan batch;
        batch.cells = job.remaining;
        std::vector<schematic::PastePlan::Cell> left;
        left.reserve(job.remaining.size());
        const PasteWriteResult written =
            block_editor.apply_paste(batch, job.options, job.undo_started, &left);
        job.undo_started = true;
        ++job.batches;
        accumulate_paste_result(job.totals, written);

        // What is left is what the writer could not reach: a cell that was
        // covered or already had the block is DONE (its chunk is resident).
        job.remaining.swap(left);
    }

    if (job.remaining.empty()) {
        finish_pending_paste(false);
        return;
    }

    // 2. Chunks still holding unwritten cells, as keys.
    std::unordered_set<uint64_t> needed;
    needed.reserve(job.remaining.size() / 4 + 8);
    for (const schematic::PastePlan::Cell& cell : job.remaining) {
        int32_t cx = 0, cy = 0, cz = 0, lx = 0, ly = 0, lz = 0;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        needed.insert(chunk_map.get_chunk_key(cx, cy, cz));
    }

    // 3. Unpin what is no longer needed. A chunk whose cells all landed is free
    //    to be evicted again — its edits are already persisted in the edit map,
    //    so an unload cannot lose them.
    for (size_t i = 0; i < job.requested.size();) {
        if (needed.count(job.requested[i]) != 0) {
            ++i;
            continue;
        }
        chunk_world.unpin_chunk(job.requested[i]);
        job.requested[i] = job.requested.back();
        job.requested.pop_back();
    }

    // 4. Ask for more, nearest first: a build is pasted where the player is
    //    standing, so the cells closest to them are the ones worth landing first.
    size_t asked = 0;
    if (job.requested.size() < kPasteChunksInFlight) {
        const int32_t pcx = world_updater.get_last_player_chunk_x();
        const int32_t pcy = world_updater.get_last_player_chunk_y();
        const int32_t pcz = world_updater.get_last_player_chunk_z();
        std::vector<std::pair<int64_t, uint64_t>> candidates;
        candidates.reserve(needed.size());
        for (const uint64_t key : needed) {
            bool already = false;
            for (const uint64_t existing : job.requested) {
                if (existing == key) { already = true; break; }
            }
            if (already) continue;
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            const int64_t dx = cx - pcx, dy = cy - pcy, dz = cz - pcz;
            candidates.push_back({dx * dx + dy * dy + dz * dz, key});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const std::pair<int64_t, uint64_t>& a,
                     const std::pair<int64_t, uint64_t>& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;  // deterministic order
                  });
        const size_t room = kPasteChunksInFlight - job.requested.size();
        for (const auto& candidate : candidates) {
            if (asked >= room) break;
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(candidate.second, cx, cy, cz);
            // Pin first: the chunk may already be resident and about to be
            // evicted, and the pin is what makes the write below land in it.
            chunk_world.pin_chunk(candidate.second);
            job.requested.push_back(candidate.second);
            if (!chunk_map.has_loaded_chunk(cx, cy, cz)) {
                chunk_world.request_urgent_chunk(cx, cy, cz);
                ++asked;
            }
        }
    }

    // 5. Either it is still making progress, or it is not. A paste with nothing
    //    requested and cells left cannot advance on its own (every chunk it is
    //    waiting on is outside the world), and one that has waited past the
    //    timeout is not worth holding the pins for.
    if (job.requested.empty()) {
        finish_pending_paste(true);
        return;
    }
    if (job.waited_ms >= kPasteWaitTimeoutMs) {
        finish_pending_paste(true);
    }
}

godot::Dictionary VoxelEngineController::get_pending_paste() {
    godot::Dictionary out;
    out["active"] = has_pending_paste;
    if (!has_pending_paste) return out;
    const PendingPaste& job = pending_paste_storage;
    out["remaining_cells"] = static_cast<int64_t>(job.remaining.size());
    out["requested_chunks"] = static_cast<int64_t>(job.requested.size());
    out["cells_written"] = static_cast<int64_t>(job.totals.written);
    out["planned"] = static_cast<int64_t>(job.planned);
    out["chunks_touched"] = static_cast<int64_t>(job.totals.chunks_touched);
    out["waited_ms"] = job.waited_ms;
    return out;
}

godot::Dictionary VoxelEngineController::take_paste_completion() {
    godot::Dictionary out;
    if (!paste_report_ready) return out;
    out["cells"] = static_cast<int64_t>(paste_report_cells);
    out["chunks"] = static_cast<int64_t>(paste_report_chunks);
    out["waiting_ms"] = paste_report_waited_ms;
    out["abandoned"] = paste_report_abandoned;
    out["leftover_cells"] = static_cast<int64_t>(paste_report_leftover);
    out["unchanged"] = static_cast<int64_t>(paste_report_unchanged);
    out["covered"] = static_cast<int64_t>(paste_report_covered);
    out["out_of_bounds"] = static_cast<int64_t>(paste_report_out_of_bounds);
    out["planned"] = static_cast<int64_t>(paste_report_planned);
    paste_report_ready = false;
    paste_report_abandoned = false;
    paste_report_cells = 0;
    paste_report_leftover = 0;
    paste_report_chunks = 0;
    paste_report_waited_ms = 0.0;
    paste_report_unchanged = 0;
    paste_report_covered = 0;
    paste_report_out_of_bounds = 0;
    paste_report_planned = 0;
    return out;
}

} // namespace VoxelEngine
