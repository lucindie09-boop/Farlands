#include "godot_bindings/chunk_manager.hpp"

#include "godot_bindings/cached_node.hpp"
#include "engine/item_body_solver.hpp"
#include "engine/item_pair_solver.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "pathfinding/path_service.hpp"
#include "render/multimesh_instance_layout.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

// resolve_cached<T>() / cache_object<T>() come from godot_bindings/cached_node.hpp:
// every scene node this class caches is held as an instance ID and resolved on
// use, so a freed or replaced node degrades to "not found" and re-resolves from
// the scene instead of becoming a use-after-free.
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
        Node3D* player = Object::cast_to<Node3D>(player_node);
        cache_object(cached_player_id, player);
        if (player) {
            controller->set_player_position(player->get_global_position());
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
    RenderingServer* rs = RenderingServer::get_singleton();
    Ref<World3D> world = get_world_3d();
    if (world.is_valid()) {
        RID scenario = world->get_scenario();
        controller->get_chunk_world().get_chunk_map().for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
            if (render_data->instance_rid.is_valid()) {
                rs->instance_set_scenario(render_data->instance_rid, scenario);
            }
        });
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
            cache_object(cached_camera_id, cam);
        } else {
            // No editor camera available (e.g. 2D view focused): fall back to
            // the player_position property (set in the scene, e.g. 0/280/0).
            cached_camera_id = 0;
            player_pos = controller->get_player_position();
        }
    } else {
        cam = Object::cast_to<godot::Camera3D>(get_viewport()->get_camera_3d());
        if (cam) {
            player_pos = cam->get_global_position();
            cache_object(cached_camera_id, cam);
        } else {
            Node3D* player = resolve_cached<Node3D>(cached_player_id);
            if (player) {
                player_pos = player->get_global_position();
            } else if (!player_path.is_empty()) {
                Node* player_node = get_node_or_null(player_path);
                Node3D* found = Object::cast_to<Node3D>(player_node);
                cache_object(cached_player_id, found);
                if (found) {
                    player_pos = found->get_global_position();
                }
            }
        }
    }

    // Extract camera frustum planes for frustum-prioritized chunk loading
    godot::Camera3D* cached_camera = resolve_cached<godot::Camera3D>(cached_camera_id);
    if (cached_camera) {
        godot::TypedArray<godot::Plane> frustum_planes = cached_camera->get_frustum();
        if (frustum_planes.size() >= 6) {
            std::array<godot::Plane, 6> planes;
            for (int i = 0; i < 6; ++i) {
                planes[i] = frustum_planes[i];
            }
            controller->update_frustum(planes);
        }
    } else {
        Node3D* player = resolve_cached<Node3D>(cached_player_id);
        if (player) {
            cam = Object::cast_to<godot::Camera3D>(player->get_node_or_null(NodePath("Camera3D")));
            if (cam) {
                cache_object(cached_camera_id, cam);
                godot::TypedArray<godot::Plane> frustum_planes = cam->get_frustum();
                if (frustum_planes.size() >= 6) {
                    std::array<godot::Plane, 6> planes;
                    for (int i = 0; i < 6; ++i) {
                        planes[i] = frustum_planes[i];
                    }
                    controller->update_frustum(planes);
                }
            }
        }
    }

    // Both world effects are measured from the camera, so the boxes the chunks
    // are culled against depend on where it is. This is a no-op unless one of
    // them is on or the camera has moved a whole block since the last refresh.
    controller->update_world_cull(player_pos);

    controller->update(delta, is_editor, player_pos);
    update_environment();
}

void ChunkManager::_exit_tree() {
    ready_for_auto_update = false;
    cached_player_id = 0;
    cached_camera_id = 0;
    // Flush any dirty chunks while the world is still fully alive (all chunks
    // loaded, thread pool running, controller owned by this node). Without this,
    // edits made since the last 5s periodic flush are lost on quit. Blocking:
    // we must not let the process exit while background saves are outstanding.
    if (controller) {
        controller->flush_dirty_chunks(true, 5.0);
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
    ClassDB::bind_method(D_METHOD("get_light_at", "world_x", "world_y", "world_z"), &ChunkManager::get_light_at);
    ClassDB::bind_method(D_METHOD("apply_item_lighting", "material"), &ChunkManager::apply_item_lighting);
    ClassDB::bind_method(D_METHOD("get_body_sky_warmth"), &ChunkManager::get_body_sky_warmth);
    ClassDB::bind_method(D_METHOD("get_selection_boxes", "block_id"), &ChunkManager::get_selection_boxes);
    ClassDB::bind_method(D_METHOD("get_selection_boxes_at", "block_id", "world_x", "world_y", "world_z"),
                         &ChunkManager::get_selection_boxes_at);
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
    ClassDB::bind_method(D_METHOD("contacts_for_points", "points", "radius"), &ChunkManager::contacts_for_points);
    ClassDB::bind_method(D_METHOD("turned_boxes_contact", "centre", "offsets", "halves", "basis"), &ChunkManager::turned_boxes_contact);
    ClassDB::bind_method(D_METHOD("solve_item_pairs", "bodies", "delta"), &ChunkManager::solve_item_pairs);
    ClassDB::bind_method(D_METHOD("solve_item_bodies", "bodies", "delta"), &ChunkManager::solve_item_bodies);
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

// Same, but registered without PROPERTY_USAGE_STORAGE: the property is live in
// the inspector and from GDScript, and the scene saver will not write it. For
// STATE THAT IS NOT A WORLD SETTING — a scene save with such a property set
// bakes it into Main.tscn, and every later boot (and every measurement run)
// then starts in that state.
#define BIND_PROP_VOLATILE(type, base, param) \
    ClassDB::bind_method(D_METHOD("set_" #base, param), &ChunkManager::set_##base); \
    ClassDB::bind_method(D_METHOD("get_" #base), &ChunkManager::get_##base); \
    ADD_PROPERTY(PropertyInfo(type, #base, PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR), "set_" #base, "get_" #base)

    BIND_PROP(Variant::INT,     seed,                      "seed");
    BIND_PROP(Variant::INT,     render_distance,           "distance");
    BIND_PROP(Variant::NODE_PATH, player_path,             "path");
    BIND_PROP(Variant::VECTOR3, player_position,           "position");
    BIND_PROP(Variant::BOOL,    auto_update,               "enabled");
    BIND_PROP(Variant::FLOAT,   sea_level,                 "level");
    ClassDB::bind_method(D_METHOD("set_biome_size", "size"), &ChunkManager::set_biome_size);
    ClassDB::bind_method(D_METHOD("get_biome_size"), &ChunkManager::get_biome_size);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "biome_size", PROPERTY_HINT_RANGE, "0.25,4.0,0.05"), "set_biome_size", "get_biome_size");
    // The squish test toggle: does not persist, and changing it only affects
    // chunks generated afterwards (callers regenerate through
    // clear_editor_chunks() when they want the change applied to the world).
    // VOLATILE as well as unstored in world.meta: the default binding let a
    // scene save with the toggle on write `squish_enabled = true` into
    // Main.tscn, and every boot after that came up squished — which is how a
    // probe run labelled "off" was actually the squished world.
    BIND_PROP_VOLATILE(Variant::BOOL, squish_enabled,       "enabled");
    BIND_PROP_VOLATILE(Variant::INT,  squish_slice,         "slice");
    BIND_PROP_VOLATILE(Variant::INT,  squish_span,          "slices");
    BIND_PROP(Variant::BOOL,    editor_enabled,            "enabled");
    BIND_PROP(Variant::INT,     editor_render_distance,    "distance");
BIND_PROP(Variant::BOOL, smooth_lighting, "enabled");
    ClassDB::bind_method(D_METHOD("set_world_bend", "enabled", "amount", "radius", "rise"),
                         &ChunkManager::set_world_bend);
    ClassDB::bind_method(D_METHOD("set_world_horizon", "enabled", "radius"),
                         &ChunkManager::set_world_horizon);
    ClassDB::bind_method(D_METHOD("has_pending_mesh_work"), &ChunkManager::has_pending_mesh_work);
    ClassDB::bind_method(D_METHOD("set_lod_distance", "distance"), &ChunkManager::set_lod_distance);
    ClassDB::bind_method(D_METHOD("get_lod_distance"), &ChunkManager::get_lod_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_distance", PROPERTY_HINT_RANGE, "0,64,1"), "set_lod_distance", "get_lod_distance");
    ClassDB::bind_method(D_METHOD("set_lod_detail_level", "level"), &ChunkManager::set_lod_detail_level);
    ClassDB::bind_method(D_METHOD("get_lod_detail_level"), &ChunkManager::get_lod_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_lod_detail_level", "get_lod_detail_level");
    ClassDB::bind_method(D_METHOD("set_far_lod_distance", "distance"), &ChunkManager::set_far_lod_distance);
    ClassDB::bind_method(D_METHOD("get_far_lod_distance"), &ChunkManager::get_far_lod_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "far_lod_distance", PROPERTY_HINT_RANGE, "0,64,1"), "set_far_lod_distance", "get_far_lod_distance");
    // The seed-grid far mode (docs/lod-modes.md). Off by default: when it is off
    // no tile exists, so a settings reset leaves the world as it was.
    BIND_PROP(Variant::BOOL,    lod_grid_enabled,          "enabled");
    ClassDB::bind_method(D_METHOD("set_lod_grid_spacing", "blocks"), &ChunkManager::set_lod_grid_spacing);
    ClassDB::bind_method(D_METHOD("get_lod_grid_spacing"), &ChunkManager::get_lod_grid_spacing);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_grid_spacing", PROPERTY_HINT_RANGE, "4,256,4"), "set_lod_grid_spacing", "get_lod_grid_spacing");
    ClassDB::bind_method(D_METHOD("set_lod_grid_rings", "rings"), &ChunkManager::set_lod_grid_rings);
    ClassDB::bind_method(D_METHOD("get_lod_grid_rings"), &ChunkManager::get_lod_grid_rings);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_grid_rings", PROPERTY_HINT_RANGE, "1,8,1"), "set_lod_grid_rings", "get_lod_grid_rings");
    ClassDB::bind_method(D_METHOD("set_lod_grid_outer_rings", "rings"),
                         &ChunkManager::set_lod_grid_outer_rings);
    ClassDB::bind_method(D_METHOD("get_lod_grid_outer_rings"),
                         &ChunkManager::get_lod_grid_outer_rings);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_grid_outer_rings", PROPERTY_HINT_RANGE, "0,100,1"),
                 "set_lod_grid_outer_rings", "get_lod_grid_outer_rings");
    ClassDB::bind_method(D_METHOD("get_lod_grid_stats"), &ChunkManager::get_lod_grid_stats);
    ClassDB::bind_method(D_METHOD("debug_lod_grid_column", "x", "z"),
                         &ChunkManager::debug_lod_grid_column);
    ClassDB::bind_method(D_METHOD("debug_lod_grid_water"), &ChunkManager::debug_lod_grid_water);
    ClassDB::bind_method(D_METHOD("set_far_lod_detail_level", "level"), &ChunkManager::set_far_lod_detail_level);
    ClassDB::bind_method(D_METHOD("get_far_lod_detail_level"), &ChunkManager::get_far_lod_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_lod_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_far_lod_detail_level", "get_far_lod_detail_level");
    BIND_PROP(Variant::BOOL,    player_light_enabled,      "enabled");
    BIND_PROP(Variant::INT,     player_light_level,        "level");
    BIND_PROP(Variant::COLOR,   player_light_color,        "color");
BIND_PROP(Variant::FLOAT, day_time, "time");
    BIND_PROP(Variant::BOOL,    day_night_cycle_enabled,   "enabled");
    BIND_PROP(Variant::BOOL,    sky_tint_enabled,          "enabled");
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
