#include "engine/voxel_engine_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "debug/perf_report.hpp"
#include "world/block_editor.hpp"
#include "core/thread_pool.hpp"
#include "render/texture_pack_manager.hpp"
#include "mesh/mesh_builder.hpp"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include "core/item_registry.hpp"
#include "core/chunk_coords.hpp"
#include "schematic/paste_plan.hpp"
#include "schematic/schematic_reader.hpp"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <mutex>

namespace VoxelEngine {
using namespace godot;

PerformanceTimer VoxelEngineController::perf_timer;

PerformanceTimer& VoxelEngineController::get_perf_timer() {
    return perf_timer;
}

VoxelEngineController::VoxelEngineController()
    : block_editor(&chunk_world, &mesh_manager, &light_propagator) {
    static std::once_flag registry_init_flag;
    std::call_once(registry_init_flag, []() {
        auto& registry = BlockRegistry::get_instance();
        if (!registry.load_from_json("res://data/block_definitions.json")) {
            registry.initialize_default_blocks();
        }
    });
    // Fluid states are resolved out of whatever registry just loaded (see
    // fluids/fluid_state_table.hpp), so this has to happen after it and before
    // anything can tick. A registry with no fluid states leaves the simulation
    // disabled rather than broken.
    fluid_state_table.build_from(BlockRegistry::get_instance());

    // Note: reserve(5000) is a placeholder. Real reserve happens in set_render_distance()
    // where the actual render distance value is known.
    chunk_world.get_chunk_map().reserve(5000);
    load_world_configs();
    // Load optional texture packs from user://packs (no-op when absent).
    TexturePackManager::get_instance().load_packs("user://packs");
    create_thread_pool();
    chunk_world.set_thread_pool(thread_pool.get());
    mesh_manager.set_chunk_map(chunk_world.get_chunk_map_ptr());
    mesh_manager.set_chunk_scheduler(&chunk_world.get_scheduler());
    mesh_manager.set_thread_pool(thread_pool.get());
    mesh_manager.set_performance_timer(&perf_timer);
    mesh_manager.set_async_epoch(chunk_world.get_epoch_ptr());
    mesh_manager.set_owner(nullptr);
    light_propagator.set_chunk_map(chunk_world.get_chunk_map_ptr());
    light_propagator.set_mesh_manager(&mesh_manager);
    chunk_world.set_mesh_manager(&mesh_manager);
    chunk_world.set_light_propagator(&light_propagator);
    chunk_world.set_owner(nullptr);
    world_updater.set_chunk_world(&chunk_world);
    world_updater.set_mesh_manager(&mesh_manager);
    world_updater.set_fluid_state_table(&fluid_state_table);
    // Every edit that lands in an edit map wakes the fluid simulation, which is
    // how a player's block change (and a chunk coming back with water in it)
    // gets fluid to re-evaluate. The simulation is the only listener.
    chunk_world.set_edit_listener([this](int32_t x, int32_t y, int32_t z) {
        world_updater.notify_block_edit(x, y, z);
    });
    // The same wakes, but from a generation worker applying a chunk's edit map.
    // Posting is what makes that safe: the simulation's queue belongs to the
    // thread that ticks it, and a worker reaching into it is what produced the
    // intermittent startup heap corruption.
    chunk_world.set_worker_edit_listener([this](int32_t x, int32_t y, int32_t z) {
        world_updater.post_block_edit(x, y, z);
    });
    world_updater.set_thread_pool(thread_pool.get());
    world_updater.set_performance_timer(&perf_timer);
    world_updater.set_material_manager(&environment_controller.get_material_manager());
    world_updater.set_owner(nullptr);
    world_updater.set_seed(seed);
    world_updater.set_sea_level(sea_level);
    world_updater.set_render_distance(render_distance);
    world_updater.set_editor_render_distance(editor_render_distance);
    world_updater.set_lod_distance(lod_distance);
    world_updater.set_lod_detail_level(lod_detail_level);
    world_updater.set_far_lod_distance(far_lod_distance);
    world_updater.set_far_lod_detail_level(far_lod_detail_level);
    mesh_manager.set_mesh_render_distance(render_distance);
    // Buried-chunk mesh culling asks the world updater whether an ungenerated
    // neighbor would be solid (deep underground) so it can skip rendering box
    // walls into the void instead of drawing them.
    mesh_manager.set_chunk_would_be_solid_fn([this](int32_t cx, int32_t cy, int32_t cz) {
        return world_updater.chunk_would_be_solid(cx, cy, cz);
    });
}

VoxelEngineController::~VoxelEngineController() {
    reset_runtime_state(false);
}

void VoxelEngineController::initialize() {
    set_render_distance(render_distance);
}

void VoxelEngineController::shutdown() {
    reset_runtime_state(false);
}

void VoxelEngineController::set_owner(godot::Node* node) {
    mesh_manager.set_owner(node);
    chunk_world.set_owner(node);
    world_updater.set_owner(node);
}

void VoxelEngineController::create_thread_pool() {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    size_t num_threads = hw_threads > 1 ? static_cast<size_t>(hw_threads - 1) : 1;
    thread_pool = std::make_unique<ThreadPool>(num_threads);
    chunk_world.set_thread_pool(thread_pool.get());
    mesh_manager.set_thread_pool(thread_pool.get());
    world_updater.set_thread_pool(thread_pool.get());
}

void VoxelEngineController::shutdown_thread_pool() {
    if (thread_pool) {
        thread_pool->shutdown();
        thread_pool.reset();
    }
    chunk_world.set_thread_pool(nullptr);
    mesh_manager.set_thread_pool(nullptr);
    world_updater.set_thread_pool(nullptr);
}

void VoxelEngineController::clear_async_queues() {
    chunk_world.clear();
    mesh_manager.clear();
    world_updater.clear();
}

void VoxelEngineController::free_loaded_chunks() {
    chunk_world.free_loaded_chunks();
}

void VoxelEngineController::reset_runtime_state(bool restart_thread_pool) {
    // The chunks a waiting paste wanted are about to stop existing, so the job
    // goes with them (and its pins with it).
    cancel_pending_paste();
    chunk_world.increment_epoch();
    shutdown_thread_pool();
    chunk_world.free_loaded_chunks();
    clear_async_queues();
    world_updater.reset();
    runtime_elapsed = 0.0;
    frame_time_accumulator = 0.0;
    frame_count = 0;

    if (restart_thread_pool) {
        create_thread_pool();
    }
}

void VoxelEngineController::update(double delta, bool is_editor, const godot::Vector3& player_pos) {
    if (!auto_update) return;
    if (is_editor && !editor_enabled) return;

    ScopedTimer process_timer(perf_timer, TimerID::ProcessTotal);
    frame_count++;
    runtime_elapsed += delta;
    frame_time_accumulator += delta;
    last_delta = delta;

    player_position = player_pos;

    environment_controller.update(delta, runtime_elapsed, player_position,
                                  chunk_world, light_propagator, mesh_manager,
                                  world_updater.get_initial_loading_duration());

    {
        ScopedTimer t(perf_timer, TimerID::PlayerPosUpdate);
        world_updater.set_player_position(player_position);
    }

    {
        ScopedTimer t(perf_timer, TimerID::WorldUpdate);
        update_chunks(is_editor);
    }

    // After the world update, so a chunk that arrived this frame is written into
    // on this frame rather than the next one.
    tick_pending_paste(delta);

    {
        ScopedTimer t(perf_timer, TimerID::SceneUpdate);
        print_debug_info(delta);
    }
}

void VoxelEngineController::update_frustum(const std::array<godot::Plane, 6>& planes) {
    Frustum f;
    f.update(planes);
    world_updater.set_frustum(f);
}

void VoxelEngineController::update_chunks(bool is_editor) {
    world_updater.update(is_editor, chunk_world.get_epoch(), chunks_processed_total, last_delta);
}

// -------------------------------------------------------------------------
// Block editing (delegated to BlockEditor)
// -------------------------------------------------------------------------

void VoxelEngineController::set_block_world(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    block_editor.place_block(world_x, world_y, world_z, static_cast<BlockID>(block_id));
}

int VoxelEngineController::get_block_world(int32_t world_x, int32_t world_y, int32_t world_z) {
    return block_editor.query_block(world_x, world_y, world_z);
}

// -------------------------------------------------------------------------
// Block files from other tools
// -------------------------------------------------------------------------

bool VoxelEngineController::ensure_minecraft_palette() {
    if (minecraft_palette_loaded_) return true;

    Ref<FileAccess> file = FileAccess::open("res://data/minecraft_blocks.json", FileAccess::READ);
    if (file.is_null()) {
        minecraft_palette_error_ =
            "cannot open res://data/minecraft_blocks.json (no translation table)";
        return false;
    }
    const String text = file->get_as_text();
    std::string table_error;
    if (!minecraft_palette_.load(std::string(text.utf8().get_data()), &table_error)) {
        minecraft_palette_error_ = table_error;
        return false;
    }

    // Resolve every name the table uses to a block id once, by scanning the
    // registry once. Doing it per cell would be a linear scan over the block
    // table for every block in a build.
    std::unordered_map<std::string, BlockID> by_name;
    const BlockRegistry& registry = BlockRegistry::get_instance();
    for (size_t i = 0; i < registry.get_count(); ++i) {
        const BlockType& block = registry.get_block_fast(static_cast<BlockID>(i));
        if (block.name == nullptr) continue;
        by_name.emplace(block.name, static_cast<BlockID>(i));
    }

    std::string missing;
    for (const std::string& name : schematic::target_names(minecraft_palette_)) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
            if (missing.empty()) missing = name;
            continue;
        }
        minecraft_name_ids_.emplace(name, found->second);
    }
    if (!missing.empty()) {
        // A table naming a block this build does not have would paste holes, so
        // the paste is refused with the name in the message rather than
        // silently dropping that state (the report tool catches this offline).
        minecraft_palette_error_ =
            "data/minecraft_blocks.json maps a state to \"" + missing +
            "\", which is not a block in this build";
        return false;
    }

    minecraft_palette_loaded_ = true;
    return true;
}

Dictionary VoxelEngineController::plan_counters(const schematic::PastePlan& plan) {
    Dictionary out;
    out["planned"] = static_cast<int64_t>(plan.cells.size());
    out["placed"] = static_cast<int64_t>(plan.stats.placed);
    out["substituted"] = static_cast<int64_t>(plan.stats.substituted);
    out["stilled"] = static_cast<int64_t>(plan.stats.stilled);
    out["declined_fluid"] = static_cast<int64_t>(plan.stats.declined_fluid);
    out["declined_substitute"] = static_cast<int64_t>(plan.stats.declined_substitute);
    out["skipped"] = static_cast<int64_t>(plan.stats.skipped);
    out["unknown"] = static_cast<int64_t>(plan.stats.unknown);
    out["unresolved"] = static_cast<int64_t>(plan.stats.unresolved);
    out["air_ignored"] = static_cast<int64_t>(plan.stats.air_ignored);
    return out;
}

bool VoxelEngineController::decode_and_plan(const PackedByteArray& bytes,
                                            const Dictionary& options,
                                            int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                            schematic::PasteOptions& out_options,
                                            const schematic::SchematicData*& out_file,
                                            schematic::PastePlan& out_plan, std::string& error) {
    if (!ensure_minecraft_palette()) {
        error = minecraft_palette_error_;
        return false;
    }
    if (bytes.size() == 0) {
        error = "the file is empty";
        return false;
    }

    // The decode cache: the same bytes decode to the same build, and a preview is
    // re-planned at a new origin every time you move the crosshair while the file
    // stays the same. FNV-1a is enough here — this catches "is it still that file"
    // for a few hundred KB of compressed bytes, and a collision would need two
    // different builds to hash alike, not an attacker.
    uint64_t fingerprint = 1469598103934665603ull;
    {
        const uint8_t* raw = bytes.ptr();
        const int32_t n = bytes.size();
        for (int32_t i = 0; i < n; ++i) {
            fingerprint ^= raw[i];
            fingerprint *= 1099511628211ull;
        }
    }
    if (!have_decoded_build_ || decoded_build_.fingerprint != fingerprint) {
        schematic::SchematicData fresh;
        if (!schematic::load_schematic_bytes(reinterpret_cast<const uint8_t*>(bytes.ptr()),
                                             static_cast<size_t>(bytes.size()), fresh, &error)) {
            return false;
        }
        decoded_build_.file = std::move(fresh);
        decoded_build_.fingerprint = fingerprint;
        have_decoded_build_ = true;
    }
    out_file = &decoded_build_.file;

    if (options.has("fluids")) out_options.fluids = static_cast<bool>(options["fluids"]);
    if (options.has("substitutes")) {
        out_options.substitutes = static_cast<bool>(options["substitutes"]);
    }
    if (options.has("replace_solid")) {
        out_options.replace_solid = static_cast<bool>(options["replace_solid"]);
    }
    if (options.has("write_air")) out_options.write_air = static_cast<bool>(options["write_air"]);
    if (options.has("max_cells")) {
        const int64_t cap = static_cast<int64_t>(options["max_cells"]);
        out_options.max_cells = cap > 0 ? static_cast<size_t>(cap) : 0;
    }

    const auto resolve = [this](const std::string& name, BlockID& out) {
        const auto found = minecraft_name_ids_.find(name);
        if (found == minecraft_name_ids_.end()) return false;
        out = found->second;
        return true;
    };
    if (!schematic::plan_paste(*out_file, minecraft_palette_, origin_x, origin_y, origin_z,
                               out_options, resolve, out_plan, &error)) {
        return false;
    }
    // The cell the caller aimed at is where the BUILDING goes, not where the file's
    // box corner goes. A build saved from a region selection carries the selection's
    // empty margin inside its declared box, so planting the corner at the crosshair is
    // what put a big schematic a hundred blocks from where it was right-clicked. Both
    // the ghost and the paste come through here, so this is also what keeps them on
    // the same spot.
    (void)schematic::anchor_plan_on_content(out_plan, origin_x, origin_y, origin_z);
    return true;
}

Dictionary VoxelEngineController::inspect_schematic(const PackedByteArray& bytes,
                                                    const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    const schematic::SchematicData* file = nullptr;
    schematic::PastePlan plan;
    std::string error;
    // The origin does not matter for inspection (only for the coordinates the
    // plan carries), so the plan is built from (0, 0, 0).
    if (!decode_and_plan(bytes, options, 0, 0, 0, paste_options, file, plan, error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    result["ok"] = true;
    result["file_width"] = file->width;
    result["file_height"] = file->height;
    result["file_length"] = file->length;
    result["format"] = String(schematic::block_file_format_name(file->format));
    result["format_version"] = file->format_version;
    result["data_version"] = static_cast<int64_t>(file->data_version);
    result["container"] = String(schematic::container_kind_name(file->container));
    result["data_layout"] = String(schematic::data_layout_name(file->data_layout));
    if (file->has_offset) result["offset"] = Vector3i(file->offset[0], file->offset[1], file->offset[2]);
    result["palette_states"] = static_cast<int64_t>(file->palette.size());
    result["non_air_cells"] = static_cast<int64_t>(file->non_air_cells);
    result["tile_entities"] = static_cast<int64_t>(file->tile_entity_count);
    result["entities"] = static_cast<int64_t>(file->entity_count);
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    if (!plan.empty()) {
        // Local to the file, since inspection has no anchor.
        result["min"] = Vector3i(plan.min_x, plan.min_y, plan.min_z);
        result["max"] = Vector3i(plan.max_x, plan.max_y, plan.max_z);
    }
    return result;
}

Dictionary VoxelEngineController::preview_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                                    int32_t origin_y, int32_t origin_z,
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

    // How many cells the caller wants to draw, and how many it gets. The cap is
    // generous rather than tight: one MultiMesh can hold a few hundred thousand
    // cube instances, and a preview that silently stops at some number would be
    // worse than one that draws a strided ghost.
    int64_t wanted = 20000;
    if (options.has("preview_cells")) {
        wanted = static_cast<int64_t>(options.get("preview_cells", 20000));
    }
    if (wanted < 0) wanted = 0;
    if (wanted > 400000) wanted = 400000;
    const size_t cap = static_cast<size_t>(wanted);

    const size_t total = plan.cells.size();
    const size_t stride = (cap == 0 || total <= cap) ? 1 : (total + cap - 1) / cap;
    size_t returned = 0;
    for (size_t i = 0; i < total; i += stride) ++returned;

    PackedByteArray cells;
    // The same cells again, as the instance buffer a MultiMesh wants: one unit
    // transform per cell with the origin at the cell's CENTRE. Built here rather
    // than in GDScript because a script loop over a hundred thousand cells is a
    // frame hitch at every re-aim, and one `buffer` assignment is a single upload.
    //
    // THE LAYOUT IS NOT WHAT IT LOOKS LIKE, and getting it wrong is invisible rather
    // than loud: an instance packed in the wrong order does not error, it renders as a
    // degenerate transform somewhere off screen, so the ghost simply never appears.
    // Measured from the engine rather than read off the docs (the docs describe the
    // unpacked `transform_array` as "x, y, z, origin", which is NOT this order):
    // `set_instance_transform(0, Transform3D(Basis(), Vector3(11, 22, 33)))` packs to
    //   1,0,0, 11,  0,1,0, 22,  0,0,1, 33
    // i.e. three rows of four — each basis row followed by that row's origin
    // component. So the identity diagonal sits at 0, 5, 10 and the translation at 3, 7,
    // 11. `.freebuff/probe_mm_layout.gd` prints both that packing and what a wrong
    // order decodes to (a basis of (11,0,0), (22,0,0), (33,0,0) at origin (1,1,1)).
    PackedFloat32Array transforms;
    if (returned > 0) {
        cells.resize(static_cast<int32_t>(returned * 4 * sizeof(int32_t)));
        transforms.resize(static_cast<int32_t>(returned * 12));
        uint8_t* out = cells.ptrw();
        float* xform = transforms.ptrw();
        size_t at = 0;
        int32_t slot = 0;
        for (size_t i = 0; i < total; i += stride) {
            const schematic::PastePlan::Cell& cell = plan.cells[i];
            const int32_t values[4] = {cell.x, cell.y, cell.z, static_cast<int32_t>(cell.block)};
            std::memcpy(out + at, values, sizeof(values));
            at += sizeof(values);
            // Row 0, then origin.x; row 1, then origin.y; row 2, then origin.z.
            xform[slot + 0] = 1.0f;
            xform[slot + 1] = 0.0f;
            xform[slot + 2] = 0.0f;
            xform[slot + 3] = static_cast<float>(cell.x) + 0.5f;
            xform[slot + 4] = 0.0f;
            xform[slot + 5] = 1.0f;
            xform[slot + 6] = 0.0f;
            xform[slot + 7] = static_cast<float>(cell.y) + 0.5f;
            xform[slot + 8] = 0.0f;
            xform[slot + 9] = 0.0f;
            xform[slot + 10] = 1.0f;
            xform[slot + 11] = static_cast<float>(cell.z) + 0.5f;
            slot += 12;
        }
    }

    result["ok"] = true;
    result["format"] = String(schematic::block_file_format_name(file->format));
    result["file_width"] = file->width;
    result["file_height"] = file->height;
    result["file_length"] = file->length;
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    result["cells"] = cells;
    result["transforms"] = transforms;
    result["cells_returned"] = static_cast<int64_t>(returned);
    result["cells_sampled"] = stride > 1;
    if (!plan.empty()) {
        // World coordinates of the whole volume, so a preview can box it.
        result["min"] = Vector3i(plan.min_x, plan.min_y, plan.min_z);
        result["max"] = Vector3i(plan.max_x, plan.max_y, plan.max_z);
    }
    return result;
}

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

// -------------------------------------------------------------------------
// Chunk scenario / editor
// -------------------------------------------------------------------------

void VoxelEngineController::clear_editor_chunks() {
    reset_runtime_state(true);
    last_player_block_x = INT32_MIN;
    last_player_block_y = INT32_MIN;
    last_player_block_z = INT32_MIN;
}

void VoxelEngineController::unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    uint64_t key = chunk_world.get_chunk_map().get_chunk_key(chunk_x, chunk_y, chunk_z);
    chunk_world.save_chunk_to_disk(chunk_x, chunk_y, chunk_z);
    world_updater.try_unload(key);
}

void VoxelEngineController::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    world_updater.generate_chunk(chunk_x, chunk_y, chunk_z, chunk_world.get_epoch());
}

// -------------------------------------------------------------------------
// Debug / perf
// -------------------------------------------------------------------------

String VoxelEngineController::get_performance_report() {
    String report = PerfReport::build(
        frame_time_accumulator,
        frame_count,
        2.0, // hardcoded interval
        chunks_processed_total,
        chunks_processed_last_interval,
        perf_timer,
        thread_pool ? thread_pool->get_worker_count() : 0,
        thread_pool ? thread_pool->get_queue_size() : 0,
        chunk_world.get_scheduler().generating_count(),
        chunk_world.get_scheduler().completed_chunk_count(),
        chunk_world.get_chunk_map().size(),
        mesh_manager.gather_render_stats()
    );
    chunks_processed_last_interval = chunks_processed_total;
    frame_count = 0;
    frame_time_accumulator = 0.0;
    perf_timer.reset_all();
    MeshBuilder::get_perf_timer().reset_all();
    ChunkGenerator::get_perf_timer().reset_all();
    MeshBuilder::reset_vertex_tracking();
    MeshBuilder::reset_greedy_vertical_stats();
    return report;
}

void VoxelEngineController::print_debug_info(double delta) {
    // Debug printing removed
}

// -------------------------------------------------------------------------
// Property accessors
// -------------------------------------------------------------------------

void VoxelEngineController::set_seed(int32_t s) { seed = s; world_updater.set_seed(seed); }
int32_t VoxelEngineController::get_seed() const { return seed; }

void VoxelEngineController::set_render_distance(int32_t rd) { 
    render_distance = rd; 
    world_updater.set_render_distance(render_distance); 
    environment_controller.set_render_distance_blocks(static_cast<float>(rd * CHUNK_WIDTH));
    
    // Reserve ChunkMap based on render distance to avoid rehashing during load
    // Total chunks ≈ (2*RD + 1)^3, reserve() divides by 64 internally
    size_t total_chunks = static_cast<size_t>(2 * rd + 1) * static_cast<size_t>(2 * rd + 1) * static_cast<size_t>(2 * rd + 1);
    chunk_world.get_chunk_map().reserve(total_chunks);
}
int32_t VoxelEngineController::get_render_distance() const { return render_distance; }

void VoxelEngineController::set_editor_render_distance(int32_t rd) { editor_render_distance = rd; world_updater.set_editor_render_distance(editor_render_distance); }
int32_t VoxelEngineController::get_editor_render_distance() const { return editor_render_distance; }

void VoxelEngineController::set_player_position(const godot::Vector3& pos) { player_position = pos; world_updater.set_player_position(player_position); }
godot::Vector3 VoxelEngineController::get_player_position() const { return player_position; }

void VoxelEngineController::set_sea_level(float level) { sea_level = level; world_updater.set_sea_level(sea_level); }
float VoxelEngineController::get_sea_level() const { return sea_level; }
void VoxelEngineController::set_biome_size(float size) { biome_size = size; world_updater.set_biome_size(biome_size); }
float VoxelEngineController::get_biome_size() const { return biome_size; }

void VoxelEngineController::set_vegetation_enabled(bool enabled) { vegetation_enabled = enabled; world_updater.set_vegetation_enabled(enabled); }
bool VoxelEngineController::is_vegetation_enabled() const { return vegetation_enabled; }

void VoxelEngineController::load_world_configs() {
    TerrainParams params = world_updater.get_terrain_params();
    params.load_from_json("res://data/terrain_config.json");
    world_updater.set_terrain_params(params);

    BiomeConfig biomes;
    if (!BiomeConfig::load("res://data/biomes.json", biomes)) {
        biomes.reset_defaults();
    }
    world_updater.set_biome_config(biomes);

    VegetationConfig vegetation;
    vegetation.load("res://data/vegetation.json");
    world_updater.set_vegetation_config(vegetation);

    if (!ItemRegistry::get_instance().load_from_json("res://data/items.json")) {
        WARN_PRINT("items.json missing or unparseable; item recipes will not resolve");
    }

    recipe_book.clear();
    if (!recipe_book.load_from_json("res://data/recipes.json")) {
        WARN_PRINT("recipes.json missing or unparseable; crafting disabled");
    }
}

void VoxelEngineController::save_world_metadata() {
    const TerrainParams& params = world_updater.get_terrain_params();
    chunk_world.save_world_metadata(params);
}

bool VoxelEngineController::load_world_metadata() {
    TerrainParams params;
    int32_t chunk_version;
    if (!chunk_world.load_world_metadata(params, chunk_version)) {
        return false;
    }
    // Check for seed mismatch before applying
    if (params.seed != seed) {
        WARN_PRINT("World metadata seed mismatch: saved=" + String::num_int64(params.seed) + ", current=" + String::num_int64(seed) + ". Loading saved world may generate incoherent terrain.");
    }
    // Apply loaded params to controller state
    seed = params.seed;
    sea_level = params.sea_level;
    biome_size = params.biome_size;
    // Update world_updater with loaded params
    world_updater.set_seed(seed);
    world_updater.set_sea_level(sea_level);
    world_updater.set_biome_size(biome_size);
    return true;
}

bool VoxelEngineController::world_metadata_exists() const {
    return chunk_world.world_metadata_exists();
}

void VoxelEngineController::save_inventory(const Inventory& inventory) {
    chunk_world.save_inventory(inventory);
}

bool VoxelEngineController::load_inventory(Inventory& inventory) {
    return chunk_world.load_inventory(inventory);
}

void VoxelEngineController::flush_dirty_chunks(bool wait_for_completion, double timeout_sec) {
    chunk_world.flush_dirty_chunks(wait_for_completion, timeout_sec);
}

Dictionary VoxelEngineController::find_biome(const String& biome_name, int32_t center_x,
                                            int32_t center_z, int32_t max_radius_blocks) {
    Dictionary result;
    BiomeType target;
    if (!biome_from_name(biome_name.utf8().get_data(), target)) {
        result["found"] = false;
        return result;
    }
    int32_t out_x = 0;
    int32_t out_z = 0;
    float out_height = 0.0f;
    const bool found = world_updater.find_nearest_biome(
        target, center_x, center_z, max_radius_blocks, out_x, out_z, out_height);
    result["found"] = found;
    result["x"] = out_x;
    result["y"] = static_cast<int32_t>(std::round(out_height));
    result["z"] = out_z;
    return result;
}

void VoxelEngineController::set_auto_update(bool enabled) { auto_update = enabled; }
bool VoxelEngineController::get_auto_update() const { return auto_update; }

void VoxelEngineController::set_smooth_lighting(bool enabled) {
smooth_lighting = enabled;
mesh_manager.set_smooth_lighting(enabled);
mesh_manager.mark_all_chunks_dirty();
}
bool VoxelEngineController::get_smooth_lighting() const { return smooth_lighting; }

void VoxelEngineController::set_lod_distance(int32_t d) { lod_distance = d; world_updater.set_lod_distance(d); }
int32_t VoxelEngineController::get_lod_distance() const { return lod_distance; }
void VoxelEngineController::set_lod_detail_level(float l) { lod_detail_level = l; world_updater.set_lod_detail_level(l); }
float VoxelEngineController::get_lod_detail_level() const { return lod_detail_level; }
void VoxelEngineController::set_far_lod_distance(int32_t d) { far_lod_distance = d; world_updater.set_far_lod_distance(d); }
int32_t VoxelEngineController::get_far_lod_distance() const { return far_lod_distance; }
void VoxelEngineController::set_far_lod_detail_level(float l) { far_lod_detail_level = l; world_updater.set_far_lod_detail_level(l); }
float VoxelEngineController::get_far_lod_detail_level() const { return far_lod_detail_level; }

void VoxelEngineController::set_editor_enabled(bool enabled) { editor_enabled = enabled; }
bool VoxelEngineController::get_editor_enabled() const { return editor_enabled; }


void VoxelEngineController::set_player_light_enabled(bool enabled) { environment_controller.set_player_light_enabled(enabled); }
bool VoxelEngineController::get_player_light_enabled() const { return environment_controller.get_player_light_enabled(); }

void VoxelEngineController::set_player_light_level(int32_t level) { environment_controller.set_player_light_level(level); }
int32_t VoxelEngineController::get_player_light_level() const { return environment_controller.get_player_light_level(); }

void VoxelEngineController::set_player_light_color(const godot::Color& color) { environment_controller.set_player_light_color(color); }
godot::Color VoxelEngineController::get_player_light_color() const { return environment_controller.get_player_light_color(); }

void VoxelEngineController::set_day_time(double t) { environment_controller.set_day_time(t); }
double  VoxelEngineController::get_day_time() const { return environment_controller.get_day_time(); }

void VoxelEngineController::set_time(double t) { set_day_time(t); }
double VoxelEngineController::get_time() const { return get_day_time(); }
godot::Vector3 VoxelEngineController::get_sun_direction() const { return environment_controller.get_sun_direction(); }

void VoxelEngineController::set_day_night_cycle_enabled(bool enabled) { environment_controller.set_day_night_cycle_enabled(enabled); }
bool VoxelEngineController::get_day_night_cycle_enabled() const { return environment_controller.get_day_night_cycle_enabled(); }
void VoxelEngineController::toggle_day_night_cycle() {set_day_night_cycle_enabled(!get_day_night_cycle_enabled()); }

void VoxelEngineController::set_day_duration(double duration) { environment_controller.set_day_duration(duration); }
double VoxelEngineController::get_day_duration() const { return environment_controller.get_day_duration(); }

void VoxelEngineController::set_day_sky_intensity(double intensity) { environment_controller.set_day_sky_intensity(intensity); }
double VoxelEngineController::get_day_sky_intensity() const { return environment_controller.get_day_sky_intensity(); }

void VoxelEngineController::set_night_sky_intensity(double intensity) { environment_controller.set_night_sky_intensity(intensity); }
double VoxelEngineController::get_night_sky_intensity() const { return environment_controller.get_night_sky_intensity(); }

void VoxelEngineController::set_day_sky_color(const godot::Color& color) { environment_controller.set_day_sky_color(color); }
godot::Color VoxelEngineController::get_day_sky_color() const { return environment_controller.get_day_sky_color(); }

void VoxelEngineController::set_night_sky_color(const godot::Color& color) { environment_controller.set_night_sky_color(color); }
godot::Color VoxelEngineController::get_night_sky_color() const { return environment_controller.get_night_sky_color(); }

void VoxelEngineController::set_contrast(double contrast) { environment_controller.set_contrast(contrast); }
double VoxelEngineController::get_contrast() const { return environment_controller.get_contrast(); }
void VoxelEngineController::set_saturation(double saturation) { environment_controller.set_saturation(saturation); }
double VoxelEngineController::get_saturation() const { return environment_controller.get_saturation(); }
void VoxelEngineController::set_ao_color(const godot::Color& color) { environment_controller.set_ao_color(color); }
godot::Color VoxelEngineController::get_ao_color() const { return environment_controller.get_ao_color(); }
void VoxelEngineController::set_ao_strength(double strength) { environment_controller.set_ao_strength(strength); }
double VoxelEngineController::get_ao_strength() const { return environment_controller.get_ao_strength(); }
void VoxelEngineController::set_darkness_color(const godot::Color& color) { environment_controller.set_darkness_color(color); }
godot::Color VoxelEngineController::get_darkness_color() const { return environment_controller.get_darkness_color(); }

void VoxelEngineController::set_fog_density(double density) { environment_controller.set_fog_density(density); }
double VoxelEngineController::get_fog_density() const { return environment_controller.get_fog_density(); }
void VoxelEngineController::set_fog_mode(int32_t mode) { environment_controller.set_fog_mode(mode); }
int32_t VoxelEngineController::get_fog_mode() const { return environment_controller.get_fog_mode(); }
void VoxelEngineController::set_mipmaps_enabled(bool enabled) { environment_controller.set_mipmaps_enabled(enabled); }
bool VoxelEngineController::get_mipmaps_enabled() const { return environment_controller.get_mipmaps_enabled(); }
void VoxelEngineController::set_mipmap_bias(double bias) { environment_controller.set_mipmap_bias(bias); }
double VoxelEngineController::get_mipmap_bias() const { return environment_controller.get_mipmap_bias(); }
void VoxelEngineController::set_textures_enabled(bool enabled) { environment_controller.set_textures_enabled(enabled); }
bool VoxelEngineController::get_textures_enabled() const { return environment_controller.get_textures_enabled(); }
void VoxelEngineController::set_compression_enabled(bool enabled) { environment_controller.set_compression_enabled(enabled); }
bool VoxelEngineController::get_compression_enabled() const { return environment_controller.get_compression_enabled(); }
void VoxelEngineController::set_render_distance_blocks(float blocks) { environment_controller.set_render_distance_blocks(blocks); }
float VoxelEngineController::get_render_distance_blocks() const { return environment_controller.get_render_distance_blocks(); }

} // namespace VoxelEngine
