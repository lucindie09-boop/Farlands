#include "godot_bindings/chunk_manager.hpp"

#include "debug/crash_dump.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "render/multimesh_instance_layout.hpp"
#include "world/block_editor.hpp"
#include "pathfinding/path_service.hpp"
#include "render/texture_array_generator.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <cmath>
#include <algorithm>

using namespace godot;
using namespace VoxelEngine;

ChunkManager::ChunkManager() {
    controller = std::make_unique<VoxelEngineController>();
    controller->initialize();
}

ChunkManager::~ChunkManager() {
    controller->shutdown();
    controller.reset();
}

PerformanceTimer& ChunkManager::get_perf_timer() {
    return VoxelEngineController::get_perf_timer();
}

void ChunkManager::_ready() {
    // print_line("_ready: called, is_inside_tree=" + String::num(is_inside_tree()));
#ifdef DEBUG_ENABLED
    // Once, at startup, because the thing it checks fails silently: a build preview
    // packed in the wrong float order draws mangled instances off screen and shows
    // only its outline, with nothing in the log to say why. Debug builds only, and it
    // says nothing at all when it agrees — or when there is no renderer to ask.
    {
        std::string layout_report;
        if (!render::verify_multimesh_instance_layout(layout_report)) {
            ERR_PRINT(String(layout_report.c_str()));
        }
    }
#endif
    controller->set_owner(this);
    if (!player_path.is_empty()) {
        Node* player_node = get_node_or_null(player_path);
        cached_player = Object::cast_to<Node3D>(player_node);
        if (cached_player) {
            controller->set_player_position(cached_player->get_global_position());
        }
    }
    update_environment();

    // Load world metadata if it exists, otherwise save initial metadata
    if (controller->world_metadata_exists()) {
        if (!controller->load_world_metadata()) {
            WARN_PRINT("Failed to load world metadata, using current defaults");
        }
    } else {
        controller->save_world_metadata();
    }

    ready_for_auto_update = true;
}

void ChunkManager::_enter_tree() {
    controller->set_owner(this);
    set_process(true);
    // print_line("_enter_tree: called, is_inside_tree=" + String::num(is_inside_tree()));
    RenderingServer* rs = RenderingServer::get_singleton();
    Ref<World3D> world = get_world_3d();
    if (world.is_valid()) {
        RID scenario = world->get_scenario();
        // print_line("_enter_tree: world is valid, scenario is valid=" + String::num(scenario.is_valid()));
        controller->get_chunk_world().get_chunk_map().for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
            if (render_data->instance_rid.is_valid()) {
                rs->instance_set_scenario(render_data->instance_rid, scenario);
            }
        });
        // print_line("_enter_tree: set scenario for " + String::num(controller->get_chunk_world().get_chunk_map().size()) + " chunks");
    } else {
        // print_line("_enter_tree: world is null");
    }
}

void ChunkManager::_process(double delta) {
    if (!controller->get_auto_update()) return;
    Engine* engine = Engine::get_singleton();
    bool is_editor = engine && engine->is_editor_hint();
    if (is_editor && !controller->get_editor_enabled()) return;

    godot::Vector3 player_pos;
    godot::Camera3D* cam = nullptr;
    if (is_editor) {
        // Editor preview: follow the editor's 3D viewport camera so toggling
        // editor_enabled generates the world around wherever the camera is.
        // The root viewport has no active camera in the editor (the 3D editor
        // camera lives in its own SubViewport), so a plain
        // get_viewport()->get_camera_3d() returns null and generation would
        // otherwise anchor at the world origin, deep underground.
        if (EditorInterface* editor = EditorInterface::get_singleton()) {
            if (SubViewport* viewport_3d = editor->get_editor_viewport_3d()) {
                cam = viewport_3d->get_camera_3d();
            }
        }
        if (cam) {
            player_pos = cam->get_global_position();
            cached_camera = cam;
        } else {
            // No editor camera available (e.g. 2D view focused): fall back to
            // the player_position property (set in the scene, e.g. 0/280/0).
            cached_camera = nullptr;
            player_pos = controller->get_player_position();
        }
    } else {
        cam = Object::cast_to<godot::Camera3D>(get_viewport()->get_camera_3d());
        if (cam) {
            player_pos = cam->get_global_position();
            cached_camera = cam;
        } else if (cached_player) {
            player_pos = cached_player->get_global_position();
        } else if (!player_path.is_empty()) {
            Node* player_node = get_node_or_null(player_path);
            Node3D* player = Object::cast_to<Node3D>(player_node);
            if (player) {
                cached_player = player;
                player_pos = player->get_global_position();
            }
        }
    }

    // Extract camera frustum planes for frustum-prioritized chunk loading
    if (cached_camera) {
        godot::TypedArray<godot::Plane> frustum_planes = cached_camera->get_frustum();
        if (frustum_planes.size() >= 6) {
            std::array<godot::Plane, 6> planes;
            for (int i = 0; i < 6; ++i) {
                planes[i] = frustum_planes[i];
            }
            controller->update_frustum(planes);
        }
    } else if (cached_player) {
        cam = Object::cast_to<godot::Camera3D>(cached_player->get_node_or_null(NodePath("Camera3D")));
        if (cam) {
            cached_camera = cam;
            godot::TypedArray<godot::Plane> frustum_planes = cached_camera->get_frustum();
            if (frustum_planes.size() >= 6) {
                std::array<godot::Plane, 6> planes;
                for (int i = 0; i < 6; ++i) {
                    planes[i] = frustum_planes[i];
                }
                controller->update_frustum(planes);
            }
        }
    }

    controller->update(delta, is_editor, player_pos);
    update_environment();
}

void ChunkManager::_exit_tree() {
    ready_for_auto_update = false;
    cached_player = nullptr;
    cached_camera = nullptr;
    // Flush any dirty chunks while the world is still fully alive (all chunks
    // loaded, thread pool running, controller owned by this node). Without this,
    // edits made since the last 5s periodic flush are lost on quit. Blocking:
    // we must not let the process exit while background saves are outstanding.
    if (controller) {
        controller->flush_dirty_chunks(true, 5.0);
    }
}

// -------------------------------------------------------------------------
// Property thin wrappers — every method below just delegates to controller
// -------------------------------------------------------------------------

void ChunkManager::set_seed(int32_t p_seed) { controller->set_seed(p_seed); }
int32_t ChunkManager::get_seed() const { return controller->get_seed(); }

godot::String ChunkManager::engine_build_stamp() const {
    // The compiler's own timestamps for this translation unit, so it cannot be
    // forgotten when the code changes.
    return godot::String("built " __DATE__ " " __TIME__);
}

void ChunkManager::set_render_distance(int32_t distance) { controller->set_render_distance(distance); }
int32_t ChunkManager::get_render_distance() const { return controller->get_render_distance(); }

void ChunkManager::set_player_position(const godot::Vector3& position) {
    controller->set_player_position(position);
    if (ready_for_auto_update && controller->get_auto_update()) {
        update_chunks();
    }
}
godot::Vector3 ChunkManager::get_player_position() const { return controller->get_player_position(); }

void ChunkManager::set_player_path(const godot::NodePath& path) { player_path = path; }
godot::NodePath ChunkManager::get_player_path() const { return player_path; }

void ChunkManager::set_auto_update(bool enabled) { controller->set_auto_update(enabled); }
bool ChunkManager::get_auto_update() const { return controller->get_auto_update(); }

void ChunkManager::update_chunks() { controller->update_chunks(Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()); }

void ChunkManager::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    controller->generate_chunk(chunk_x, chunk_y, chunk_z);
}

void ChunkManager::unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    controller->unload_chunk(chunk_x, chunk_y, chunk_z);
}

void ChunkManager::set_sea_level(float level) { controller->set_sea_level(level); }
float ChunkManager::get_sea_level() const { return controller->get_sea_level(); }

void ChunkManager::set_biome_size(float size) { controller->set_biome_size(size); }
float ChunkManager::get_biome_size() const { return controller->get_biome_size(); }

String ChunkManager::get_performance_report() { return controller->get_performance_report(); }

void ChunkManager::set_chunk_scenario(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    ChunkRenderData* render_data = controller->get_chunk_world().get_chunk_render_data(chunk_x, chunk_y, chunk_z);
    if (!render_data) return;
    if (!render_data->instance_rid.is_valid()) return;

    RenderingServer* rs = RenderingServer::get_singleton();
    Node3D* owner3d = Object::cast_to<Node3D>(this);
    Ref<World3D> world = owner3d ? owner3d->get_world_3d() : Ref<World3D>();
    if (!world.is_valid()) {
        if (is_inside_tree()) {
            call_deferred("set_chunk_scenario", chunk_x, chunk_y, chunk_z);
        }
        return;
    }
    RID scenario = world->get_scenario();
    rs->instance_set_scenario(render_data->instance_rid, scenario);
    rs->instance_set_visible(render_data->instance_rid, true);
}

void ChunkManager::clear_editor_chunks() {
    controller->clear_editor_chunks();
    // print_line("clear_editor_chunks: All chunks cleared");
}

void ChunkManager::set_editor_enabled(bool enabled) {
    controller->set_editor_enabled(enabled);
    if (ready_for_auto_update && enabled && controller->get_auto_update()) {
        update_chunks();
    }
}
bool ChunkManager::get_editor_enabled() const { return controller->get_editor_enabled(); }

void ChunkManager::set_editor_render_distance(int32_t distance) { controller->set_editor_render_distance(distance); }
int32_t ChunkManager::get_editor_render_distance() const { return controller->get_editor_render_distance(); }

Dictionary ChunkManager::raycast_from_camera(double max_distance) {
    Dictionary result;
    result["success"] = false;

    if (player_path.is_empty()) return result;
    Node* player_node = get_node_or_null(player_path);
    if (!player_node) return result;
    Node3D* player = Object::cast_to<Node3D>(player_node);
    if (!player) return result;

    // vanilla targets blocks from the player's EYE along the LOOK direction
    // (eye-origin ray trace along the look direction), never from the camera:
    // the third-person camera sits 4 blocks back, so a camera-based ray
    // diverges from the eye-ray and flips which block is targeted at close
    // range. Ask the PlayerController for the eye ray; fall back to the camera
    // for scenes without one (e.g. the editor).
    Vector3 ray_origin;
    Vector3 ray_dir;
    bool have_eye_ray = false;
    if (player->has_method("get_aim_origin") && player->has_method("get_aim_direction")) {
        const Variant origin_v = player->call("get_aim_origin");
        const Variant dir_v = player->call("get_aim_direction");
        if (origin_v.get_type() == Variant::VECTOR3 && dir_v.get_type() == Variant::VECTOR3) {
            ray_origin = origin_v;
            ray_dir = dir_v;
            have_eye_ray = true;
        }
    }
    if (!have_eye_ray) {
        Camera3D* camera = nullptr;
        if (cached_camera) {
            camera = cached_camera;
        } else {
            camera = Object::cast_to<Camera3D>(player->get_node_or_null(NodePath("Camera3D")));
            if (camera) cached_camera = camera;
        }
        if (!camera) return result;
        ray_origin = camera->get_global_position();
        ray_dir = -camera->get_global_transform().basis.get_column(2).normalized();
    }

    RaycastResult rr = controller->get_block_editor().raycast_from_ray(ray_origin, ray_dir, max_distance);
    if (!rr.success) return result;

    result["success"] = true;
    result["position"] = rr.position;
    result["place_position"] = rr.place_position;
    result["block_id"] = rr.block_id;
    result["hit_normal"] = rr.hit_normal;
    result["hit_point"] = rr.hit_point;
    return result;
}

void ChunkManager::set_block(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    controller->set_block_world(world_x, world_y, world_z, block_id);
}

int ChunkManager::get_block(int32_t world_x, int32_t world_y, int32_t world_z) {
    return controller->get_block_world(world_x, world_y, world_z);
}

String ChunkManager::get_block_name(int block_id) {
    const auto& block = BlockRegistry::get_instance().get_block(static_cast<BlockID>(block_id));
    return String(block.name);
}

#ifdef DEBUG_ENABLED
void ChunkManager::debug_crash_for_test() {
    // Deliberately ignored: false means the harness was not armed, which is not
    // worth a warning in the debug build it only exists in.
    (void)debug::crash_for_test();
}

bool ChunkManager::debug_multimesh_layout_ok() {
    // The startup check again, on demand, so a probe can hold it to the engine without
    // needing a crash or a scene. Answers true in a run with no renderer, where there
    // is nothing to compare.
    std::string report;
    const bool ok = render::verify_multimesh_instance_layout(report);
    if (!ok) ERR_PRINT(String(report.c_str()));
    return ok;
}
#endif

Array ChunkManager::get_selection_boxes(int block_id) {
    Array result;
    const BlockType& bt = BlockRegistry::get_instance().get_block(static_cast<BlockID>(block_id));
    if (bt.selection_boxes.empty()) {
        // Default full cube
        PackedFloat32Array box;
        box.push_back(0.0f); box.push_back(0.0f); box.push_back(0.0f);
        box.push_back(1.0f); box.push_back(1.0f); box.push_back(1.0f);
        result.push_back(box);
    } else {
        for (const auto& b : bt.selection_boxes) {
            PackedFloat32Array box;
            box.push_back(b.min[0]); box.push_back(b.min[1]); box.push_back(b.min[2]);
            box.push_back(b.max[0]); box.push_back(b.max[1]); box.push_back(b.max[2]);
            result.push_back(box);
        }
    }
    return result;
}

Dictionary ChunkManager::find_biome(const String& biome_name, int32_t center_x,
                                    int32_t center_z, int32_t max_radius) {
    return controller->find_biome(biome_name, center_x, center_z, max_radius);
}

Dictionary ChunkManager::inspect_schematic(const PackedByteArray& bytes, const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->inspect_schematic(bytes, options);
}

Dictionary ChunkManager::preview_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                           int32_t origin_y, int32_t origin_z,
                                           const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->preview_schematic(bytes, origin_x, origin_y, origin_z, options);
}

Dictionary ChunkManager::paste_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                         int32_t origin_y, int32_t origin_z,
                                         const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->paste_schematic_bytes(bytes, origin_x, origin_y, origin_z, options);
}

Dictionary ChunkManager::get_pending_paste() {
    if (!controller) return Dictionary();
    return controller->get_pending_paste();
}

Dictionary ChunkManager::take_paste_completion() {
    if (!controller) return Dictionary();
    return controller->take_paste_completion();
}

Dictionary ChunkManager::undo_paste() {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->undo_paste();
}

int64_t ChunkManager::paste_undo_cells() {
    if (!controller) return 0;
    return controller->paste_undo_cells();
}

int64_t ChunkManager::request_path(const Vector3& from, const Vector3& to, int32_t max_expansions,
                                   double max_ms) {
    if (!controller) return 0;
    ThreadPool* pool = controller->get_thread_pool();
    if (pool == nullptr) return 0;
    if (!path_service) {
        path_service = std::make_unique<nav::PathService>(
            controller->get_chunk_world().get_chunk_map());
    }
    // Positions arrive as continuous feet coordinates; the planner works in
    // whole cells and re-anchors both ends onto their column's surface.
    const nav::NavNode start{static_cast<int32_t>(std::floor(from.x)),
                             static_cast<int32_t>(std::floor(from.y)),
                             static_cast<int32_t>(std::floor(from.z))};
    const nav::NavNode goal{static_cast<int32_t>(std::floor(to.x)),
                            static_cast<int32_t>(std::floor(to.y)),
                            static_cast<int32_t>(std::floor(to.z))};
    // The pool is passed in per call: clear_editor_chunks() tears the engine's
    // pool down and builds a new one, so a service holding a reference would
    // queue into freed memory after a runtime reset.
    return static_cast<int64_t>(path_service->submit(*pool, start, goal, max_expansions, max_ms));
}

Dictionary ChunkManager::get_fluid_stats() {
    // A window onto the flow simulation's ticking: how much work it did on its
    // last tick and how much is still queued. A flood that looks stuck is either
    // pending > 0 (still working through its cells, one budget slice a tick) or
    // 0 with no cells ticked (settled, and waiting to be woken by an edit).
    const fluids::FluidSim& sim = controller->get_fluid_sim();
    const fluids::FluidSim::Stats& stats = sim.stats();
    Dictionary out;
    out["enabled"] = sim.enabled();
    out["ticks"] = static_cast<int64_t>(stats.ticks);
    out["cells_ticked"] = static_cast<int64_t>(stats.cells_ticked);
    out["last_tick_cells"] = stats.last_tick_cells;
    out["last_tick_writes"] = stats.last_tick_writes;
    out["last_tick_ms"] = stats.last_tick_ms;
    out["pending"] = static_cast<int64_t>(stats.pending);
    out["frozen"] = stats.frozen;
    out["settled"] = stats.settled;
    return out;
}

Dictionary ChunkManager::get_generation_stats() {
    // The sweep's own accounting. `checks` and the reject counters describe the
    // whole history of the session, which is dominated by the initial load; the
    // rolling window is the part that answers "what does this cost while I am
    // flying right now". Both are here so the two are never confused.
    const VoxelEngine::WorldUpdater::GenerationStats& stats =
        controller->get_world_updater().get_generation_stats();
    Dictionary out;
    out["frames"] = static_cast<int64_t>(stats.frames);
    out["candidate_offsets"] = static_cast<int64_t>(stats.candidate_offsets);
    out["cursor_resets"] = static_cast<int64_t>(stats.cursor_resets);

    out["checks"] = static_cast<int64_t>(stats.checks);
    out["band_pass"] = static_cast<int64_t>(stats.band_pass);
    out["generations"] = static_cast<int64_t>(stats.generations);
    out["generate_refused"] = static_cast<int64_t>(stats.generate_refused);
    out["reject_loaded"] = static_cast<int64_t>(stats.reject_loaded);
    out["reject_above"] = static_cast<int64_t>(stats.reject_above);
    out["reject_below"] = static_cast<int64_t>(stats.reject_below);
    out["reject_oob"] = static_cast<int64_t>(stats.reject_oob);
    out["sweeps_completed"] = static_cast<int64_t>(stats.sweeps_completed);

    out["frustum_checks"] = static_cast<int64_t>(stats.frustum_checks);
    out["frustum_visible"] = static_cast<int64_t>(stats.frustum_visible);
    out["frustum_loaded"] = static_cast<int64_t>(stats.frustum_loaded);
    out["frustum_band_pass"] = static_cast<int64_t>(stats.frustum_band_pass);
    out["frustum_generations"] = static_cast<int64_t>(stats.frustum_generations);

    out["urgent_requested"] = static_cast<int64_t>(stats.urgent_requested);
    out["urgent_generated"] = static_cast<int64_t>(stats.urgent_generated);

    out["total_ms"] = stats.total_ms;
    out["last_ms"] = stats.last_ms;
    out["max_ms"] = stats.max_ms;
    out["avg_ms"] = stats.frames > 0 ? stats.total_ms / static_cast<double>(stats.frames) : 0.0;

    // Window sums, computed here rather than in GDScript so the caller reads the
    // same numbers the counters hold (and so a mid-window reset cannot mix two
    // different frame sets).
    uint64_t window_checks = 0;
    uint64_t window_generations = 0;
    for (size_t i = 0; i < stats.window_frames; ++i) {
        window_checks += stats.window_checks[i];
        window_generations += stats.window_generations[i];
    }
    out["window_frames"] = static_cast<int64_t>(stats.window_frames);
    out["window_checks"] = static_cast<int64_t>(window_checks);
    out["window_generations"] = static_cast<int64_t>(window_generations);

    const int32_t render_distance = get_render_distance();
    out["render_distance"] = render_distance;
    return out;
}

void ChunkManager::reset_generation_stats() {
    controller->get_world_updater().reset_generation_stats();
}

Array ChunkManager::poll_paths() {
    Array out;
    if (!path_service) return out;
    for (const nav::PathResult& result : path_service->poll()) {
        Dictionary entry;
        entry["id"] = static_cast<int64_t>(result.id);
        entry["found"] = result.found;
        entry["truncated"] = result.truncated;
        entry["budget_exhausted"] = result.budget_exhausted;
        entry["time_exhausted"] = result.time_exhausted;
        entry["error"] = String(result.error.c_str());
        entry["ms"] = result.search_ms;
        entry["expansions"] = static_cast<int64_t>(result.stats.expansions);
        entry["columns"] = static_cast<int64_t>(result.stats.columns_resolved);
        // World-view cost: cells classified and the shard locks taken for them.
        entry["cells"] = static_cast<int64_t>(result.cells_read);
        entry["locks"] = static_cast<int64_t>(result.lock_acquisitions);
        // Block cells, not centres — the caller places an overlay cube per cell.
        PackedVector3Array nodes;
        nodes.resize(static_cast<int64_t>(result.nodes.size()));
        for (size_t i = 0; i < result.nodes.size(); ++i) {
            nodes.set(static_cast<int64_t>(i),
                      Vector3(static_cast<real_t>(result.nodes[i].x),
                              static_cast<real_t>(result.nodes[i].y),
                              static_cast<real_t>(result.nodes[i].z)));
        }
        PackedVector3Array waypoints;
        waypoints.resize(static_cast<int64_t>(result.waypoints.size()));
        for (size_t i = 0; i < result.waypoints.size(); ++i) {
            waypoints.set(static_cast<int64_t>(i),
                          Vector3(static_cast<real_t>(result.waypoints[i].x),
                                  static_cast<real_t>(result.waypoints[i].y),
                                  static_cast<real_t>(result.waypoints[i].z)));
        }
        entry["nodes"] = nodes;
        entry["waypoints"] = waypoints;
        out.append(entry);
    }
    return out;
}

int32_t ChunkManager::get_pending_paths() const {
    return path_service ? static_cast<int32_t>(path_service->pending()) : 0;
}

Dictionary ChunkManager::resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size) {
    auto result = controller->resolve_voxel_collision(position, motion, size);
    Dictionary dict;
    dict["position"] = result.position;
    dict["collided_x"] = result.collided_x;
    dict["collided_y"] = result.collided_y;
    dict["collided_z"] = result.collided_z;
    dict["on_floor"] = result.on_floor;
    return dict;
}

VoxelEngine::CollisionResolver* ChunkManager::get_collision_resolver() {
    return &controller->get_collision_resolver();
}

void ChunkManager::set_smooth_lighting(bool enabled) { controller->set_smooth_lighting(enabled); }
bool ChunkManager::get_smooth_lighting() const { return controller->get_smooth_lighting(); }

void ChunkManager::set_lod_distance(int32_t distance) { controller->set_lod_distance(distance); }
int32_t ChunkManager::get_lod_distance() const { return controller->get_lod_distance(); }
void ChunkManager::set_lod_detail_level(float level) { controller->set_lod_detail_level(level); }
float ChunkManager::get_lod_detail_level() const { return controller->get_lod_detail_level(); }
void ChunkManager::set_far_lod_distance(int32_t distance) { controller->set_far_lod_distance(distance); }
int32_t ChunkManager::get_far_lod_distance() const { return controller->get_far_lod_distance(); }
void ChunkManager::set_far_lod_detail_level(float level) { controller->set_far_lod_detail_level(level); }
float ChunkManager::get_far_lod_detail_level() const { return controller->get_far_lod_detail_level(); }

void ChunkManager::set_player_light_enabled(bool enabled) { controller->set_player_light_enabled(enabled); }
bool ChunkManager::get_player_light_enabled() const { return controller->get_player_light_enabled(); }

void ChunkManager::set_player_light_level(int32_t level) { controller->set_player_light_level(level); }
int32_t ChunkManager::get_player_light_level() const { return controller->get_player_light_level(); }

void ChunkManager::set_player_light_color(const Color& color) { controller->set_player_light_color(color); }
Color ChunkManager::get_player_light_color() const { return controller->get_player_light_color(); }

void ChunkManager::set_day_time(double t) { controller->set_day_time(t); }
double ChunkManager::get_day_time() const { return controller->get_day_time(); }
void ChunkManager::set_time(double t) { controller->set_time(t); }
double ChunkManager::get_time() const { return controller->get_time(); }
Vector3 ChunkManager::get_sun_direction() const { return controller->get_sun_direction(); }

void ChunkManager::set_day_night_cycle_enabled(bool enabled) { controller->set_day_night_cycle_enabled(enabled); }
bool ChunkManager::get_day_night_cycle_enabled() const { return controller->get_day_night_cycle_enabled(); }
void ChunkManager::toggle_day_night_cycle() {controller->toggle_day_night_cycle(); }

void ChunkManager::set_day_duration(double duration) { controller->set_day_duration(duration); }
double ChunkManager::get_day_duration() const { return controller->get_day_duration(); }

void ChunkManager::set_day_sky_intensity(double intensity) { controller->set_day_sky_intensity(intensity); }
double ChunkManager::get_day_sky_intensity() const { return controller->get_day_sky_intensity(); }

void ChunkManager::set_night_sky_intensity(double intensity) { controller->set_night_sky_intensity(intensity); }
double ChunkManager::get_night_sky_intensity() const { return controller->get_night_sky_intensity(); }

void ChunkManager::set_day_sky_color(const godot::Color& color) { controller->set_day_sky_color(color); }
godot::Color ChunkManager::get_day_sky_color() const { return controller->get_day_sky_color(); }

void ChunkManager::set_night_sky_color(const godot::Color& color) { controller->set_night_sky_color(color); }
godot::Color ChunkManager::get_night_sky_color() const { return controller->get_night_sky_color(); }

void ChunkManager::set_contrast(double contrast) { controller->set_contrast(contrast); }
double ChunkManager::get_contrast() const { return controller->get_contrast(); }

void ChunkManager::set_saturation(double saturation) { controller->set_saturation(saturation); }
double ChunkManager::get_saturation() const { return controller->get_saturation(); }

void ChunkManager::set_ao_color(const godot::Color& color) { controller->set_ao_color(color); }
godot::Color ChunkManager::get_ao_color() const { return controller->get_ao_color(); }

void ChunkManager::set_ao_strength(double strength) { controller->set_ao_strength(strength); }
double ChunkManager::get_ao_strength() const { return controller->get_ao_strength(); }

void ChunkManager::set_darkness_color(const godot::Color& color) { controller->set_darkness_color(color); }
godot::Color ChunkManager::get_darkness_color() const { return controller->get_darkness_color(); }

void ChunkManager::set_fog_density(double density) { controller->set_fog_density(density); }
double ChunkManager::get_fog_density() const { return controller->get_fog_density(); }
void ChunkManager::set_fog_mode(int32_t mode) { controller->set_fog_mode(mode); }
int32_t ChunkManager::get_fog_mode() const { return controller->get_fog_mode(); }
void ChunkManager::set_mipmaps_enabled(bool enabled) { controller->set_mipmaps_enabled(enabled); }
bool ChunkManager::get_mipmaps_enabled() const { return controller->get_mipmaps_enabled(); }
void ChunkManager::set_mipmap_bias(double bias) { controller->set_mipmap_bias(bias); }
double ChunkManager::get_mipmap_bias() const { return controller->get_mipmap_bias(); }
namespace {
// The lab's generated frames are RGBA8 and whatever resolution the user picked;
// an array layer is fixed at the pack's base resolution with the array's format
// and mipmap state. This copies the frame onto that shape (never mutating the
// caller's image, which the lab keeps for its preview).
//
// `want_mipmaps` cannot come from the array: Godot 4.7's TextureLayered returns
// NULL from get_layer_data() even for a freshly built array, so there is nothing
// to inspect. It comes from the generator's own mipmap flag instead, which is the
// flag that built the array in the first place.
godot::Ref<godot::Image> fit_frame_to_array(const godot::Ref<godot::Image>& frame,
                                            const godot::Ref<godot::Texture2DArray>& array,
                                            bool want_mipmaps) {
    if (frame.is_null() || frame->is_empty() || array.is_null()) return godot::Ref<godot::Image>();
    const int width = array->get_width();
    const int height = array->get_height();
    if (width <= 0 || height <= 0) return godot::Ref<godot::Image>();

    godot::Ref<godot::Image> out = godot::Image::create(width, height, false, godot::Image::FORMAT_RGBA8);
    if (out.is_null()) return godot::Ref<godot::Image>();
    if (frame->get_format() != godot::Image::FORMAT_RGBA8) {
        // copy_from() requires a matching format, so convert the source first.
        godot::Ref<godot::Image> converted = frame->duplicate();
        if (converted.is_null()) return godot::Ref<godot::Image>();
        converted->convert(godot::Image::FORMAT_RGBA8);
        if (converted->get_width() != width || converted->get_height() != height) {
            converted->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
        }
        out->copy_from(converted);
    } else {
        out->copy_from(frame);
        if (out->get_width() != width || out->get_height() != height) {
            out->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
        }
    }

    // A mipmapped array layer rejects a frame without mipmaps.
    if (want_mipmaps && !out->has_mipmaps()) {
        out->generate_mipmaps();
    }
    return out;
}
} // namespace

Dictionary ChunkManager::get_texture_layer_info(const String& texture_name) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    // find_texture_layer reads the table built by the generate call above.
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    Dictionary out;
    out["found"] = layer >= 0;
    out["index"] = layer;
    out["layers"] = array.is_valid() ? array->get_layers() : 0;
    if (array.is_valid()) {
        out["width"] = array->get_width();
        out["height"] = array->get_height();
        out["format"] = static_cast<int>(array->get_format());
        out["mipmaps"] = TextureArrayGenerator::is_mipmaps_enabled();
        // A compressed array cannot take an uncompressed frame; the lab says so
        // up front, and rebuilds uncompressed, instead of pushing frames that
        // Godot silently drops on the format mismatch.
        out["writable"] = array->get_format() == Image::FORMAT_RGBA8;
    } else {
        out["width"] = 0;
        out["height"] = 0;
        out["format"] = -1;
        out["mipmaps"] = false;
        out["writable"] = false;
    }
    return out;
}

Ref<Image> ChunkManager::get_texture_layer_image(const String& texture_name) {
    // NOTE: Godot 4.7's TextureLayered::get_layer_data() returns null here even
    // for an array built from images in this same process, so this is best
    // effort — it answers non-null only on builds that keep the CPU copies.
    // Nothing in the engine depends on it; fit_texture_frame() is the verifiable
    // half of the live-preview path.
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return Ref<Image>();
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return Ref<Image>();
    return array->get_layer_data(layer);
}

Ref<Image> ChunkManager::fit_texture_frame(const String& texture_name, const Ref<Image>& frame) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return Ref<Image>();
    if (TextureArrayGenerator::find_texture_layer(texture_name) < 0) return Ref<Image>();
    if (array->get_format() != Image::FORMAT_RGBA8) return Ref<Image>();
    return fit_frame_to_array(frame, array, TextureArrayGenerator::is_mipmaps_enabled());
}

bool ChunkManager::push_texture_frame(const String& texture_name, const Ref<Image>& frame) {
    TextureArrayGenerator& generator = TextureArrayGenerator::get_instance();
    Ref<Texture2DArray> array = generator.get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return false;
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return false;
    if (array->get_format() != Image::FORMAT_RGBA8) {
        WARN_PRINT("push_texture_frame: texture array is compressed; disable texture compression to preview animated textures.");
        return false;
    }
    const Ref<Image> fitted = fit_frame_to_array(frame, array, TextureArrayGenerator::is_mipmaps_enabled());
    if (fitted.is_null()) return false;
    array->update_layer(fitted, layer);
    return true;
}

bool ChunkManager::restore_texture_layer(const String& texture_name) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return false;
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return false;

    // Resolution order matches the array build: active pack override first, then
    // the built-in set. Pack PNGs live outside the import system.
    const String path = TexturePackManager::get_instance().resolve(texture_name);
    Ref<Image> original;
    if (path.begins_with("user://")) {
        original = Image::load_from_file(path);
    } else if (ResourceLoader* loader = ResourceLoader::get_singleton(); loader != nullptr) {
        const Ref<Texture2D> texture = loader->load(path);
        if (texture.is_valid()) {
            original = texture->get_image();
        }
    }
    if (original.is_null() || original->is_empty()) return false;
    const Ref<Image> fitted = fit_frame_to_array(original, array, TextureArrayGenerator::is_mipmaps_enabled());
    if (fitted.is_null()) return false;
    array->update_layer(fitted, layer);
    return true;
}

void ChunkManager::set_textures_enabled(bool enabled) { controller->set_textures_enabled(enabled); }
bool ChunkManager::get_textures_enabled() const { return controller->get_textures_enabled(); }
void ChunkManager::set_compression_enabled(bool enabled) { controller->set_compression_enabled(enabled); }
bool ChunkManager::get_compression_enabled() const { return controller->get_compression_enabled(); }
void ChunkManager::set_vegetation_enabled(bool enabled) { controller->set_vegetation_enabled(enabled); }

bool ChunkManager::get_vegetation_enabled() const { return controller->is_vegetation_enabled(); }

void ChunkManager::set_move_speed_multiplier(float multiplier) { move_speed_multiplier_ = multiplier; }

float ChunkManager::get_move_speed_multiplier() const { return move_speed_multiplier_; }

void ChunkManager::save_world_metadata() { controller->save_world_metadata(); }
bool ChunkManager::load_world_metadata() { return controller->load_world_metadata(); }
bool ChunkManager::world_metadata_exists() const { return controller->world_metadata_exists(); }
void ChunkManager::flush_dirty_chunks() { controller->flush_dirty_chunks(); }

// -------------------------------------------------------------------------
// Scene tree manipulation — Godot-specific, lives in the binding layer
// -------------------------------------------------------------------------

void ChunkManager::update_environment() {
    Node* parent = get_parent();
    if (!parent) return;
    if (parent != cached_env_parent || !cached_world_env) {
        cached_env_parent = parent;
        cached_world_env = Object::cast_to<WorldEnvironment>(
            parent->get_node_or_null(NodePath("WorldEnvironment"))
        );
        cached_sun_light = Object::cast_to<DirectionalLight3D>(
            parent->get_node_or_null(NodePath("SunLight"))
        );
    }
    if (!cached_world_env) return;
    Ref<Environment> env = cached_world_env->get_environment();
    if (!env.is_valid()) return;

    auto& ec = controller->get_environment_controller();
    const auto& day_night = ec.get_day_night_cycle();
    const float blend = day_night.get_blend();
    const float elevation = day_night.get_sun_elevation();
    const Color horizon_color = day_night.get_horizon_color();
    const Color sun_color = day_night.get_sun_color();
    const Vector3 sun_dir = day_night.get_sun_direction();

    ec.get_sky_controller().update(env.ptr(), blend, static_cast<float>(day_night.get_raw_time()),
                                   sun_color, sun_dir,
                                   day_night.get_moon_phase(), 1.0f, day_night.get_sky_turbidity(), 1.0f,
                                   ec.get_fog_controller().get_fog_scatter(blend, elevation));
    ec.get_fog_controller().update(env.ptr(), blend, horizon_color,
                                   ec.get_fog_controller().get_fog_color(blend, horizon_color, elevation, sun_color, day_night.get_sky_turbidity()),
                                   ec.get_fog_controller().get_fog_scatter(blend, elevation));

    env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
    env->set_ambient_light_color(day_night.get_ambient_color());
    env->set_ambient_light_energy(day_night.get_ambient_intensity());

    if (cached_sun_light) {
        Vector3 light_pos = cached_sun_light->get_global_position();
        cached_sun_light->look_at(light_pos - sun_dir, Vector3(0, 0, 1));

        float sun_visible = std::clamp((elevation + 0.08f) / 0.16f, 0.0f, 1.0f);
        float moon_visible = (1.0f - sun_visible) * (1.0f - blend);

        if (sun_visible > 0.0f) {
            cached_sun_light->set_color(sun_color);
            cached_sun_light->set_param(Light3D::PARAM_ENERGY, 3.0f * sun_visible * day_night.get_day_intensity());
            cached_sun_light->set_shadow(false);
            cached_sun_light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else if (moon_visible > 0.0f) {
            cached_sun_light->set_color(Color(1.0f, 1.0f, 1.0f));
            cached_sun_light->set_param(Light3D::PARAM_ENERGY, 0.25f * moon_visible * day_night.get_night_intensity());
            cached_sun_light->set_shadow(false);
            cached_sun_light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else {
            cached_sun_light->set_param(Light3D::PARAM_ENERGY, 0.0f);
            cached_sun_light->set_shadow(false);
        }
    }
}

// -------------------------------------------------------------------------
// _bind_methods
// -------------------------------------------------------------------------
void ChunkManager::_bind_methods() {
    // Non-property API (manual — each has unique signatures)
    ClassDB::bind_method(D_METHOD("engine_build_stamp"), &ChunkManager::engine_build_stamp);
    ClassDB::bind_method(D_METHOD("update_chunks"), &ChunkManager::update_chunks);
    ClassDB::bind_method(D_METHOD("generate_chunk", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::generate_chunk);
    ClassDB::bind_method(D_METHOD("unload_chunk", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::unload_chunk);
    ClassDB::bind_method(D_METHOD("get_performance_report"), &ChunkManager::get_performance_report);
    ClassDB::bind_method(D_METHOD("set_chunk_scenario", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::set_chunk_scenario);
    ClassDB::bind_method(D_METHOD("clear_editor_chunks"), &ChunkManager::clear_editor_chunks);
    ClassDB::bind_method(D_METHOD("raycast_from_camera", "max_distance"), &ChunkManager::raycast_from_camera);
    ClassDB::bind_method(D_METHOD("set_block", "world_x", "world_y", "world_z", "block_id"), &ChunkManager::set_block);
    ClassDB::bind_method(D_METHOD("get_block", "world_x", "world_y", "world_z"), &ChunkManager::get_block);
    ClassDB::bind_method(D_METHOD("get_block_name", "block_id"), &ChunkManager::get_block_name);
    ClassDB::bind_method(D_METHOD("get_selection_boxes", "block_id"), &ChunkManager::get_selection_boxes);
#ifdef DEBUG_ENABLED
    ClassDB::bind_method(D_METHOD("debug_crash_for_test"), &ChunkManager::debug_crash_for_test);
    ClassDB::bind_method(D_METHOD("debug_multimesh_layout_ok"), &ChunkManager::debug_multimesh_layout_ok);
#endif
    ClassDB::bind_method(D_METHOD("find_biome", "biome_name", "center_x", "center_z", "max_radius"), &ChunkManager::find_biome);
    ClassDB::bind_method(D_METHOD("inspect_schematic", "bytes", "options"),
                         &ChunkManager::inspect_schematic, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("preview_schematic", "bytes", "origin_x", "origin_y", "origin_z",
                                  "options"),
                         &ChunkManager::preview_schematic, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("paste_schematic", "bytes", "origin_x", "origin_y", "origin_z", "options"),
                         &ChunkManager::paste_schematic, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("undo_paste"), &ChunkManager::undo_paste);
    ClassDB::bind_method(D_METHOD("get_pending_paste"), &ChunkManager::get_pending_paste);
    ClassDB::bind_method(D_METHOD("take_paste_completion"), &ChunkManager::take_paste_completion);
    ClassDB::bind_method(D_METHOD("paste_undo_cells"), &ChunkManager::paste_undo_cells);
    ClassDB::bind_method(D_METHOD("resolve_voxel_collision", "position", "motion", "size"), &ChunkManager::resolve_voxel_collision);
    ClassDB::bind_method(D_METHOD("request_path", "from", "to", "max_expansions", "max_ms"),
                         &ChunkManager::request_path, DEFVAL(20000), DEFVAL(32.0));
    ClassDB::bind_method(D_METHOD("poll_paths"), &ChunkManager::poll_paths);
    ClassDB::bind_method(D_METHOD("get_fluid_stats"), &ChunkManager::get_fluid_stats);
    ClassDB::bind_method(D_METHOD("get_generation_stats"), &ChunkManager::get_generation_stats);
    ClassDB::bind_method(D_METHOD("reset_generation_stats"), &ChunkManager::reset_generation_stats);
    ClassDB::bind_method(D_METHOD("get_texture_layer_info", "texture_name"), &ChunkManager::get_texture_layer_info);
    ClassDB::bind_method(D_METHOD("get_texture_layer_image", "texture_name"), &ChunkManager::get_texture_layer_image);
    ClassDB::bind_method(D_METHOD("fit_texture_frame", "texture_name", "frame"), &ChunkManager::fit_texture_frame);
    ClassDB::bind_method(D_METHOD("push_texture_frame", "texture_name", "frame"), &ChunkManager::push_texture_frame);
    ClassDB::bind_method(D_METHOD("restore_texture_layer", "texture_name"), &ChunkManager::restore_texture_layer);
    ClassDB::bind_method(D_METHOD("get_pending_paths"), &ChunkManager::get_pending_paths);

    ClassDB::bind_method(D_METHOD("save_world_metadata"), &ChunkManager::save_world_metadata);
    ClassDB::bind_method(D_METHOD("load_world_metadata"), &ChunkManager::load_world_metadata);
    ClassDB::bind_method(D_METHOD("world_metadata_exists"), &ChunkManager::world_metadata_exists);
    ClassDB::bind_method(D_METHOD("flush_dirty_chunks"), &ChunkManager::flush_dirty_chunks);

ClassDB::bind_method(D_METHOD("set_time", "time"), &ChunkManager::set_time);
ClassDB::bind_method(D_METHOD("get_time"), &ChunkManager::get_time);
ClassDB::bind_method(D_METHOD("toggle_day_night_cycle"), &ChunkManager::toggle_day_night_cycle);
ClassDB::bind_method(D_METHOD("get_sun_direction"), &ChunkManager::get_sun_direction);

    // Block ID constants (exposed to GDScript so block types can be referenced without hardcoding)
#define BIND_BLOCK_CONSTANT(name, id) ClassDB::bind_integer_constant("ChunkManager", "", #name, id)
    BIND_BLOCK_CONSTANT(BLOCK_AIR,           BlockIDs::AIR);
    BIND_BLOCK_CONSTANT(BLOCK_STONE,         BlockIDs::STONE);
    BIND_BLOCK_CONSTANT(BLOCK_DIRT,          BlockIDs::DIRT);
    BIND_BLOCK_CONSTANT(BLOCK_GRASS,         BlockIDs::GRASS);
    BIND_BLOCK_CONSTANT(BLOCK_SAND,          BlockIDs::SAND);
    BIND_BLOCK_CONSTANT(BLOCK_SURFACE_WATER, BlockIDs::SURFACE_WATER);
    BIND_BLOCK_CONSTANT(BLOCK_SURFACE_LAVA,  BlockIDs::SURFACE_LAVA);
    BIND_BLOCK_CONSTANT(BLOCK_SURFACE_ACID,  BlockIDs::SURFACE_ACID);
    BIND_BLOCK_CONSTANT(BLOCK_WATER,         BlockIDs::WATER);
    BIND_BLOCK_CONSTANT(BLOCK_WOOD,          BlockIDs::WOOD);
    BIND_BLOCK_CONSTANT(BLOCK_LEAVES,        BlockIDs::LEAVES);
    BIND_BLOCK_CONSTANT(BLOCK_BEDROCK,       BlockIDs::BEDROCK);
    BIND_BLOCK_CONSTANT(BLOCK_MUD,           BlockIDs::MUD);
    BIND_BLOCK_CONSTANT(BLOCK_WET_SAND,      BlockIDs::WET_SAND);
    BIND_BLOCK_CONSTANT(BLOCK_MUD_FULL,      BlockIDs::MUD_FULL);
    BIND_BLOCK_CONSTANT(BLOCK_WET_SAND_FULL, BlockIDs::WET_SAND_FULL);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_BLOCK,   BlockIDs::LIGHT_BLOCK);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_RED,     BlockIDs::LIGHT_RED);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_GREEN,   BlockIDs::LIGHT_GREEN);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_BLUE,    BlockIDs::LIGHT_BLUE);
#undef BIND_BLOCK_CONSTANT

    // Editor properties: each macro emits the getter, setter, and ADD_PROPERTY line.
    // type   = Godot Variant type
    // base   = property name (also used to build get_*/set_* method names)
    // param  = D_METHOD parameter name for the setter
#define BIND_PROP(type, base, param) \
    ClassDB::bind_method(D_METHOD("set_" #base, param), &ChunkManager::set_##base); \
    ClassDB::bind_method(D_METHOD("get_" #base), &ChunkManager::get_##base); \
    ADD_PROPERTY(PropertyInfo(type, #base), "set_" #base, "get_" #base)

    BIND_PROP(Variant::INT,     seed,                      "seed");
    BIND_PROP(Variant::INT,     render_distance,           "distance");
    BIND_PROP(Variant::NODE_PATH, player_path,             "path");
    BIND_PROP(Variant::VECTOR3, player_position,           "position");
    BIND_PROP(Variant::BOOL,    auto_update,               "enabled");
    BIND_PROP(Variant::FLOAT,   sea_level,                 "level");
    ClassDB::bind_method(D_METHOD("set_biome_size", "size"), &ChunkManager::set_biome_size);
    ClassDB::bind_method(D_METHOD("get_biome_size"), &ChunkManager::get_biome_size);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "biome_size", PROPERTY_HINT_RANGE, "0.25,4.0,0.05"), "set_biome_size", "get_biome_size");
    BIND_PROP(Variant::BOOL,    editor_enabled,            "enabled");
    BIND_PROP(Variant::INT,     editor_render_distance,    "distance");
BIND_PROP(Variant::BOOL, smooth_lighting, "enabled");
    ClassDB::bind_method(D_METHOD("set_lod_distance", "distance"), &ChunkManager::set_lod_distance);
    ClassDB::bind_method(D_METHOD("get_lod_distance"), &ChunkManager::get_lod_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_distance", PROPERTY_HINT_RANGE, "0,64,1"), "set_lod_distance", "get_lod_distance");
    ClassDB::bind_method(D_METHOD("set_lod_detail_level", "level"), &ChunkManager::set_lod_detail_level);
    ClassDB::bind_method(D_METHOD("get_lod_detail_level"), &ChunkManager::get_lod_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_lod_detail_level", "get_lod_detail_level");
    ClassDB::bind_method(D_METHOD("set_far_lod_distance", "distance"), &ChunkManager::set_far_lod_distance);
    ClassDB::bind_method(D_METHOD("get_far_lod_distance"), &ChunkManager::get_far_lod_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "far_lod_distance", PROPERTY_HINT_RANGE, "0,64,1"), "set_far_lod_distance", "get_far_lod_distance");
    ClassDB::bind_method(D_METHOD("set_far_lod_detail_level", "level"), &ChunkManager::set_far_lod_detail_level);
    ClassDB::bind_method(D_METHOD("get_far_lod_detail_level"), &ChunkManager::get_far_lod_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_lod_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_far_lod_detail_level", "get_far_lod_detail_level");
    BIND_PROP(Variant::BOOL,    player_light_enabled,      "enabled");
    BIND_PROP(Variant::INT,     player_light_level,        "level");
    BIND_PROP(Variant::COLOR,   player_light_color,        "color");
BIND_PROP(Variant::FLOAT, day_time, "time");
    BIND_PROP(Variant::BOOL,    day_night_cycle_enabled,   "enabled");
    BIND_PROP(Variant::FLOAT,   day_duration,              "duration");
    BIND_PROP(Variant::FLOAT,   day_sky_intensity,         "intensity");
    BIND_PROP(Variant::FLOAT,   night_sky_intensity,       "intensity");
    BIND_PROP(Variant::COLOR,   day_sky_color,             "color");
    BIND_PROP(Variant::COLOR,   night_sky_color,           "color");
    BIND_PROP(Variant::FLOAT,   contrast,                  "value");
    BIND_PROP(Variant::FLOAT,   saturation,                "value");
    BIND_PROP(Variant::COLOR,   ao_color,                  "color");
    BIND_PROP(Variant::FLOAT,   ao_strength,               "strength");
    BIND_PROP(Variant::COLOR,   darkness_color,            "color");
    BIND_PROP(Variant::FLOAT,   fog_density,               "density");
    ClassDB::bind_method(D_METHOD("set_fog_mode", "mode"), &ChunkManager::set_fog_mode);
    ClassDB::bind_method(D_METHOD("get_fog_mode"), &ChunkManager::get_fog_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "fog_mode", PROPERTY_HINT_ENUM, "Disabled:0,Edge:1,Linear:2,Exponential:3"), "set_fog_mode", "get_fog_mode");
    BIND_PROP(Variant::BOOL,    mipmaps_enabled,            "enabled");
    ClassDB::bind_method(D_METHOD("set_mipmap_bias", "bias"), &ChunkManager::set_mipmap_bias);
    ClassDB::bind_method(D_METHOD("get_mipmap_bias"), &ChunkManager::get_mipmap_bias);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mipmap_bias", PROPERTY_HINT_RANGE, "-4.0,4.0,0.01"), "set_mipmap_bias", "get_mipmap_bias");
    BIND_PROP(Variant::BOOL,    textures_enabled,           "enabled");
    ClassDB::bind_method(D_METHOD("set_compression_enabled", "enabled"), &ChunkManager::set_compression_enabled);
    ClassDB::bind_method(D_METHOD("get_compression_enabled"), &ChunkManager::get_compression_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "compression_enabled"), "set_compression_enabled", "get_compression_enabled");
    BIND_PROP(Variant::BOOL,    vegetation_enabled,         "enabled");
    ClassDB::bind_method(D_METHOD("set_move_speed_multiplier", "multiplier"), &ChunkManager::set_move_speed_multiplier);
    ClassDB::bind_method(D_METHOD("get_move_speed_multiplier"), &ChunkManager::get_move_speed_multiplier);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "move_speed_multiplier", PROPERTY_HINT_RANGE, "0.1,16.0,0.1"), "set_move_speed_multiplier", "get_move_speed_multiplier");
#undef BIND_PROP
}
