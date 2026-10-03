// The world API: block reads and writes, the camera ray, selection boxes, voxel
// collision, path requests and their polling, plus the editor-scenario calls that
// touch the rendering server directly. Kept apart from godot_bindings/chunk_manager.cpp
// (the node's lifecycle and its bindings) so the GDScript-facing world surface is one
// file to read.

#include "godot_bindings/chunk_manager.hpp"

#include "godot_bindings/cached_node.hpp"
#include "debug/crash_dump.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "pathfinding/path_service.hpp"
#include "render/multimesh_instance_layout.hpp"
#include "world/block_editor.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;

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
        Camera3D* camera = resolve_cached<Camera3D>(cached_camera_id);
        if (!camera) {
            camera = Object::cast_to<Camera3D>(player->get_node_or_null(NodePath("Camera3D")));
            if (camera) cache_object(cached_camera_id, camera);
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

namespace {

// A neighbour for a shape claim, read through the locking accessor: the outline
// runs outside any map lock, so this must not use a _fast variant.
struct OutlineShapeContext {
    ChunkManager* manager;
    int32_t x;
    int32_t y;
    int32_t z;
};

BlockID outline_shape_neighbor(void* ctx, ShapeFace face) {
    const OutlineShapeContext& c = *static_cast<OutlineShapeContext*>(ctx);
    switch (face) {
        case ShapeFace::Top:    return static_cast<BlockID>(c.manager->get_block(c.x, c.y + 1, c.z));
        case ShapeFace::Bottom: return static_cast<BlockID>(c.manager->get_block(c.x, c.y - 1, c.z));
        case ShapeFace::Right:  return static_cast<BlockID>(c.manager->get_block(c.x + 1, c.y, c.z));
        case ShapeFace::Left:   return static_cast<BlockID>(c.manager->get_block(c.x - 1, c.y, c.z));
        case ShapeFace::Front:  return static_cast<BlockID>(c.manager->get_block(c.x, c.y, c.z + 1));
        case ShapeFace::Back:   break;
    }
    return static_cast<BlockID>(c.manager->get_block(c.x, c.y, c.z - 1));
}

Array boxes_to_array(const ShapeBoxes& boxes) {
    Array result;
    for (uint8_t i = 0; i < boxes.count(); ++i) {
        const BlockAABB& b = boxes[i];
        PackedFloat32Array box;
        box.push_back(b.min[0]); box.push_back(b.min[1]); box.push_back(b.min[2]);
        box.push_back(b.max[0]); box.push_back(b.max[1]); box.push_back(b.max[2]);
        result.push_back(box);
    }
    return result;
}

} // namespace

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

Array ChunkManager::get_selection_boxes_at(int block_id, int32_t world_x, int32_t world_y,
                                          int32_t world_z) {
    const BlockType& bt = BlockRegistry::get_instance().get_block(static_cast<BlockID>(block_id));
    // No parts: the shape does not depend on the world, so the static list is it.
    if (bt.parts.empty()) return get_selection_boxes(block_id);

    OutlineShapeContext ctx{this, world_x, world_y, world_z};
    ShapeBoxes boxes;
    resolve_shape_boxes(bt, BlockRegistry::get_instance(),
                        ShapeNeighborFn{&outline_shape_neighbor, &ctx}, ShapeBoxKind::Selection,
                        boxes);

    Array result = boxes_to_array(boxes);
    if (result.is_empty()) {
        // Every part was claimed away (a connectable shape with nothing to
        // connect to and no unconditional geometry) — outline the cell itself
        // rather than nothing, so the block is still targetable.
        PackedFloat32Array box;
        box.push_back(0.0f); box.push_back(0.0f); box.push_back(0.0f);
        box.push_back(1.0f); box.push_back(1.0f); box.push_back(1.0f);
        result.push_back(box);
    }
    return result;
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

Array ChunkManager::contacts_for_points(const godot::PackedVector3Array& points, float radius) {
    Array out;
    if (points.is_empty()) return out;
    std::vector<godot::Vector3> flat;
    flat.reserve(static_cast<size_t>(points.size()));
    for (int64_t i = 0; i < points.size(); ++i) flat.push_back(points[i]);
    const auto contacts = controller->get_collision_resolver().contacts_for_points(
        flat.data(), flat.size(), radius);
    for (const auto& c : contacts) {
        Dictionary contact;
        contact["point"] = c.point;
        contact["normal"] = c.normal;
        contact["depth"] = c.depth;
        out.push_back(contact);
    }
    return out;
}

Dictionary ChunkManager::turned_boxes_contact(const godot::Vector3& centre,
                                             const godot::PackedVector3Array& offsets,
                                             const godot::PackedVector3Array& halves,
                                             const godot::Basis& basis) {
    std::vector<godot::Vector3> off;
    std::vector<godot::Vector3> hal;
    const int64_t n = std::min(offsets.size(), halves.size());
    off.reserve(static_cast<size_t>(n));
    hal.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        off.push_back(offsets[i]);
        hal.push_back(halves[i]);
    }
    const auto contact = controller->get_collision_resolver().turned_boxes_contact(
        centre, off, hal, basis);
    Dictionary out;
    out["into"] = contact.into;
    out["normal"] = contact.normal;
    out["depth"] = contact.depth;
    return out;
}

VoxelEngine::CollisionResolver* ChunkManager::get_collision_resolver() {
    return &controller->get_collision_resolver();
}
