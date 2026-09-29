// The engine controller's own half: the process-wide perf timer, construction and
// teardown of the world/worker plumbing, the per-frame update and its cull, the
// single-block edit entry points, the editor-scenario calls, and the perf report.
// Block files from other tools live in voxel_engine_schematic.cpp and
// voxel_engine_paste.cpp; the property surface in voxel_engine_properties.cpp; the
// world configuration and persistence in voxel_engine_config.cpp.

#include "engine/voxel_engine_controller.hpp"

#include "core/thread_pool.hpp"
#include "debug/perf_report.hpp"
#include "mesh/mesh_builder.hpp"
#include "render/texture_pack_manager.hpp"
#include "world/block_editor.hpp"

#include <cmath>
#include <cstring>

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

    environment_controller.update(delta, player_position);

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

void VoxelEngineController::set_world_bend(bool enabled, double amount, double radius, double rise) {
    mesh_manager.set_world_bend(enabled, static_cast<float>(amount),
                                static_cast<float>(radius), static_cast<float>(rise));
}

void VoxelEngineController::set_world_horizon(bool enabled, double radius) {
    mesh_manager.set_world_horizon(enabled, static_cast<float>(radius));
}

void VoxelEngineController::update_world_cull(const godot::Vector3& camera_position) {
    mesh_manager.update_world_cull(camera_position);
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
        mesh_manager.gather_render_stats(),
        &chunk_world.get_chunk_map()
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

} // namespace VoxelEngine
