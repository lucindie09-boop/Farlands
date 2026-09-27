#include "mesh/mesh_manager_internal.hpp"

#include <cmath>

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

using namespace godot;

// ---------------------------------------------------------------------------
// World Bend and the frustum
// ---------------------------------------------------------------------------
// A chunk's cull box is set once, when its instance is created, to the chunk's
// own 32-block box (see show_chunk_instance and the upload path) — and that box
// is a lie the moment the bend is on, because the vertices are moved in the
// vertex shader and culling happened first. The consequence is not subtle: with
// the bend at its defaults a chunk two hundred blocks out is drawn 27 blocks
// nearer and 43 blocks higher than its box says, so looking down at a bent world
// culls the very ring of ground the effect exists to show.
//
// The compensation is to grow each instance's box by how far the bend could move
// anything inside it (core/world_bend.hpp does that arithmetic, mirroring the
// include rather than inventing anything). The bend is measured from the
// *camera*, so those boxes are a function of where the camera is, and that is
// what this file refreshes.
//
// Refreshed per camera *block*, not per frame, and with a slack: the margin
// changes by at most about 1.8 blocks for every block the camera moves (see the
// derivatives in core/world_bend.hpp), so a refresh after every whole block of
// movement, with two blocks of slack left in the box, is exact rather than
// merely usually right — and while the player is standing still it costs nothing
// but a length comparison.
//
// Far regions are the same problem on a coarser mesh, and get the same fix: one
// instance per region, one box, grown the same way.
namespace {
constexpr float kBendCameraStep = 1.0f;   // blocks of camera movement per refresh
constexpr float kBendSlack = 2.0f;        // the part that pays for kBendCameraStep
constexpr float kBendEpsilon = 0.25f;     // a margin this much unchanged is not worth a call
}  // namespace

void MeshManager::set_world_bend(bool enabled, float amount, float radius, float rise) {
    const bool changed = world_bend.enabled != enabled
                      || world_bend.amount != amount
                      || world_bend.radius != radius
                      || world_bend.rise != rise;
    world_bend.enabled = enabled;
    world_bend.amount = amount;
    world_bend.radius = radius;
    world_bend.rise = rise;
    if (changed) {
        // A refresh is owed whatever the camera does next: the boxes have to be
        // recomputed with the new knobs even if the player never moves again,
        // and a bend switched off has to have its boxes given back so the world
        // returns to being culled where it is rather than where it was bent to.
        bend_cull_owed = true;
    }
}

AABB MeshManager::bend_cull_for(const AABB& local_aabb, const Vector3& origin) const {
    return world_bend_cull_aabb(local_aabb, origin, bend_cull_camera, world_bend, kBendSlack);
}

// Pushes an instance's cull box and remembers the margin it was grown by, so the
// next refresh can leave this instance alone when nothing has really changed.
// `remembered` is that margin; -1 means never touched, which is not 0 (a box at
// the mesh's own size) because the first box has to be written either way.
void MeshManager::bend_cull_apply(RID instance, float& remembered, const AABB& local_aabb,
                                  const Vector3& origin) {
    const AABB box = bend_cull_for(local_aabb, origin);
    remembered = box.position.x - local_aabb.position.x;
    RenderingServer::get_singleton()->instance_set_custom_aabb(instance, box);
}

void MeshManager::refresh_world_bend_region(FarRegionRenderData& region, uint64_t region_key) {
    if (!region.instance_rid.is_valid()) return;
    int32_t rx, ry, rz;
    ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
    const Vector3 origin(
        rx * region.region_size_xz * CHUNK_WIDTH,
        ry * CHUNK_HEIGHT,
        rz * region.region_size_xz * CHUNK_DEPTH);
    const AABB local(
        Vector3(0, 0, 0),
        Vector3(region.region_size_xz * CHUNK_WIDTH, CHUNK_HEIGHT, region.region_size_xz * CHUNK_DEPTH));
    const AABB grown = world_bend_cull_aabb(local, origin, bend_cull_camera, world_bend, kBendSlack);
    const float margin = grown.position.x - local.position.x;
    if (std::fabs(margin - region.bend_cull_margin) < kBendEpsilon && region.bend_cull_margin >= 0.0f) {
        return;
    }
    RenderingServer::get_singleton()->instance_set_custom_aabb(region.instance_rid, grown);
    region.bend_cull_margin = margin;
}

void MeshManager::update_world_bend_cull(const Vector3& camera_position) {
    if (!chunk_map) return;

    // Nothing has changed since the last refresh but the camera, and it has not
    // moved a whole block: everything below would compute the same boxes.
    if (!bend_cull_owed) {
        const float dx = camera_position.x - bend_cull_camera.x;
        const float dz = camera_position.z - bend_cull_camera.z;
        if (dx * dx + dz * dz < kBendCameraStep * kBendCameraStep) {
            return;
        }
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs == nullptr) return;

    bend_cull_camera = camera_position;
    bend_cull_owed = false;

    const bool active = world_bend.active();
    // Switching the bend off has to undo the boxes once, and after that there is
    // nothing to do at all until it is switched on again.
    if (!active && !bend_cull_on) return;
    bend_cull_on = active;

    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (!render_data || !render_data->instance_rid.is_valid()) return;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        const Vector3 origin(cx * CHUNK_WIDTH, cy * CHUNK_HEIGHT, cz * CHUNK_DEPTH);
        const AABB local(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH));
        const AABB grown = world_bend_cull_aabb(local, origin, camera_position, world_bend, kBendSlack);
        // Remembering the margin and skipping the call when it has not really
        // moved turns a walk over every resident chunk into a walk over every
        // resident chunk's arithmetic, which is what makes this affordable once
        // per block rather than once per frame. A box that has never been grown
        // is written whatever the margin says, because -1 is not 0.
        const float margin = grown.position.x - local.position.x;
        if (render_data->bend_cull_margin >= 0.0f
                && std::fabs(margin - render_data->bend_cull_margin) < kBendEpsilon) {
            return;
        }
        rs->instance_set_custom_aabb(render_data->instance_rid, grown);
        render_data->bend_cull_margin = margin;
    });

    for (auto& [region_key, region] : far_regions) {
        if (region.instance_rid.is_valid()) {
            refresh_world_bend_region(region, region_key);
        }
    }
}

} // namespace VoxelEngine
