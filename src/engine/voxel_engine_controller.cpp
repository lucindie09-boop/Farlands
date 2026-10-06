// The engine controller's own half: the process-wide perf timer, construction and
// teardown of the world/worker plumbing, the per-frame update and its cull, the
// single-block edit entry points, the editor-scenario calls, the property surface
// GDScript drives, and the perf report. Block files from other tools live in
// voxel_engine_schematic.cpp and voxel_engine_paste.cpp; the world configuration
// and persistence in voxel_engine_config.cpp.

#include "engine/voxel_engine_controller.hpp"

#include "core/thread_pool.hpp"
#include "debug/perf_report.hpp"
#include "mesh/mesh_builder.hpp"
#include "render/texture_pack_manager.hpp"
#include "world/block_editor.hpp"

#include <algorithm>
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
    // The far mode's instances live in the same scenario as the chunks, and its
    // material is loaded here (on the main thread, once) rather than at first
    // enable, so switching the mode on never loads a resource mid-frame.
    lod_grid.set_owner(node);
    lod_grid.set_material(environment_controller.get_material_manager().get_lod_grid_material());
}

void VoxelEngineController::create_thread_pool() {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    size_t num_threads = hw_threads > 1 ? static_cast<size_t>(hw_threads - 1) : 1;
    thread_pool = std::make_unique<ThreadPool>(num_threads);
    chunk_world.set_thread_pool(thread_pool.get());
    mesh_manager.set_thread_pool(thread_pool.get());
    world_updater.set_thread_pool(thread_pool.get());
    lod_grid.set_thread_pool(thread_pool.get());
}

void VoxelEngineController::shutdown_thread_pool() {
    if (thread_pool) {
        thread_pool->shutdown();
        thread_pool.reset();
    }
    chunk_world.set_thread_pool(nullptr);
    mesh_manager.set_thread_pool(nullptr);
    world_updater.set_thread_pool(nullptr);
    lod_grid.set_thread_pool(nullptr);
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
    // The far tiles belong to the world that just went away: drop them before the
    // pool does, so their workers see the new epoch and refuse their own results.
    lod_grid.set_epoch(chunk_world.get_epoch());
    lod_grid.reset();
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

    // The far mode, after the world: it owns only the ring beyond the loaded
    // world, so nothing it does can depend on this frame's chunks, and the fog
    // range it asks for is derived from the radius the world is using.
    if (lod_grid.is_enabled()) {
        // Timed as a phase of its own: the far field's main-thread work is the merge
        // of the levels whose tiles arrived, and on a frame that merged a level it is
        // the largest thing in this function (see LodGrid::Stats::merge_ms_per_frame).
        ScopedTimer far_grid_timer(perf_timer, TimerID::FarGridUpdate);
        const int32_t inner_blocks = render_distance * CHUNK_WIDTH;
        lod_grid.set_player_position(player_position);
        lod_grid.set_inner_radius_blocks(inner_blocks);
        lod_grid.update(delta);
        environment_controller.set_lod_grid_fog_range(
            inner_blocks, lod_grid.get_outer_radius_blocks());
    }

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

uint16_t VoxelEngineController::get_light_world(int32_t world_x, int32_t world_y, int32_t world_z) {
    return chunk_world.get_light_world(world_x, world_y, world_z);
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
// Property accessors
// -------------------------------------------------------------------------
// Every setter/getter that writes a member and forwards it to the world updater or
// the environment controller: the GDScript-facing knobs.

void VoxelEngineController::set_seed(int32_t s) {
    seed = s;
    world_updater.set_seed(seed);
    // The far field is sampled from the seed, so a new seed is a different world
    // for it as much as for generation.
    lod_grid.set_config(seed, world_updater.get_terrain_params(), lod_grid_biomes);
}
int32_t VoxelEngineController::get_seed() const { return seed; }

void VoxelEngineController::set_render_distance(int32_t rd) { 
    render_distance = rd; 
    world_updater.set_render_distance(render_distance); 
    // Where the far mode starts: the edge of the loaded world, so the two never
    // overlap and the grid never has to hide or replace a chunk.
    lod_grid.set_inner_radius_blocks(rd * CHUNK_WIDTH);
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

// --- The squish test toggle ------------------------------------------------
// Routed through the terrain params like every other generation knob, which is
// what makes the change reach all three consumers at once: the scheduler's
// height estimator, the prefetch workers' published config, and the generation
// workers' per-call params. Existing chunks keep the terrain they were built
// with; callers set the toggle and then regenerate (clear_editor_chunks()).

void VoxelEngineController::set_squish_enabled(bool enabled) {
    TerrainParams params = world_updater.get_terrain_params();
    params.squish_enabled = enabled;
    world_updater.set_terrain_params(params);
}

bool VoxelEngineController::get_squish_enabled() const {
    return world_updater.get_terrain_params().squish_enabled;
}

void VoxelEngineController::set_squish_slice(int32_t slice) {
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    TerrainParams params = world_updater.get_terrain_params();
    params.squish_slice = std::max(0, std::min(slice, kWorldChunkSlices - 1));
    world_updater.set_terrain_params(params);
}

int32_t VoxelEngineController::get_squish_slice() const {
    return world_updater.get_terrain_params().squish_slice;
}

void VoxelEngineController::set_squish_span(int32_t span) {
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    TerrainParams params = world_updater.get_terrain_params();
    params.squish_span = std::max(1, std::min(span, kWorldChunkSlices));
    world_updater.set_terrain_params(params);
}

int32_t VoxelEngineController::get_squish_span() const {
    return world_updater.get_terrain_params().squish_span;
}

void VoxelEngineController::set_vegetation_enabled(bool enabled) { vegetation_enabled = enabled; world_updater.set_vegetation_enabled(enabled); }
bool VoxelEngineController::is_vegetation_enabled() const { return vegetation_enabled; }


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
