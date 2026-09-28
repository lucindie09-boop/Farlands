#include "mesh/mesh_manager_internal.hpp"

#include <cmath>

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

using namespace godot;

// ---------------------------------------------------------------------------
// The world's geometry effects and the frustum
// ---------------------------------------------------------------------------
// A chunk's cull box is set once, when its instance is created, to the chunk's
// own 32-block box (see show_chunk_instance and the upload path) — and that box
// is a lie the moment one of the Shaders menu's two geometry effects is on,
// because the vertices are moved in the vertex shader and culling happened first.
// The consequence is not subtle. With World Bend at its defaults a chunk two
// hundred blocks out is drawn 27 blocks nearer and 43 blocks higher than its box
// says, so looking down at a bent world culls the very ring of ground the effect
// exists to show. The Horizon Curve's failure is the milder one and the same
// kind: a far chunk is drawn sixty-odd blocks below where its box says it is, and
// a box that does not reach down there is a box the frustum is free to throw
// away along with the world inside it.
//
// The compensation is to grow each instance's box by how far the effects could
// move anything inside it (core/world_cull.hpp does that arithmetic, mirroring
// the two includes rather than inventing anything). Both effects are measured
// from the *camera*, so those boxes are a function of where the camera is, and
// that is what this file refreshes.
//
// Refreshed per camera *block*, not per frame, and with a slack: the bend's
// margin changes by at most about 1.8 blocks for every block the camera moves
// (see the derivatives in core/world_cull.hpp) and the curve's by at most 0.4, so
// a refresh after every whole block of movement, with two blocks of slack left in
// the box, is exact rather than merely usually right — and while the player is
// standing still it costs nothing but a length comparison.
//
// Far regions are the same problem on a coarser mesh, and get the same fix: one
// instance per region, one box, grown the same way.
namespace {
constexpr float kCullCameraStep = 1.0f;   // blocks of camera movement per refresh
constexpr float kCullSlack = 2.0f;        // the part that pays for kCullCameraStep
constexpr float kCullEpsilon = 0.25f;     // a margin this much unchanged is not worth a call
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
        cull_owed = true;
    }
}

void MeshManager::set_world_horizon(bool enabled, float radius) {
    const bool changed = world_horizon.enabled != enabled || world_horizon.radius != radius;
    world_horizon.enabled = enabled;
    world_horizon.radius = radius;
    if (changed) {
        // As above, and for the same reason: the curve's own boxes are a
        // function of its radius, so a radius that moved while the camera stood
        // still still needs a refresh, and a curve switched off has to give its
        // boxes back.
        cull_owed = true;
    }
}

AABB MeshManager::cull_for(const AABB& local_aabb, const Vector3& origin) const {
    return world_cull_aabb(local_aabb, origin, cull_camera, world_bend, world_horizon, kCullSlack);
}

// Pushes an instance's cull box and remembers how far it was grown, so the next
// refresh can leave this instance alone when nothing has really changed. The
// remembered value is the growth rather than the box: two numbers, and negative
// on an instance that has never been touched, which is not 0 (a box at the mesh's
// own size) because the first box has to be written either way.
void MeshManager::cull_apply(RID instance, Vector2& remembered, const AABB& local_aabb,
                             const Vector3& origin) {
    const AABB box = cull_for(local_aabb, origin);
    remembered = world_cull_growth(local_aabb, box);
    RenderingServer::get_singleton()->instance_set_custom_aabb(instance, box);
}

void MeshManager::refresh_world_cull_region(FarRegionRenderData& region, uint64_t region_key) {
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
    const AABB grown = world_cull_aabb(local, origin, cull_camera, world_bend, world_horizon, kCullSlack);
    const Vector2 growth = world_cull_growth(local, grown);
    if (world_cull_growth_unchanged(region.cull_growth, growth, kCullEpsilon)) {
        return;
    }
    RenderingServer::get_singleton()->instance_set_custom_aabb(region.instance_rid, grown);
    region.cull_growth = growth;
}

void MeshManager::update_world_cull(const Vector3& camera_position) {
    if (!chunk_map) return;

    // Nothing has changed since the last refresh but the camera, and it has not
    // moved a whole block: everything below would compute the same boxes.
    if (!cull_owed) {
        const float dx = camera_position.x - cull_camera.x;
        const float dz = camera_position.z - cull_camera.z;
        if (dx * dx + dz * dz < kCullCameraStep * kCullCameraStep) {
            return;
        }
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs == nullptr) return;

    cull_camera = camera_position;
    cull_owed = false;

    const bool active = world_bend.active() || world_horizon.active();
    // Switching both effects off has to undo the boxes once, and after that
    // there is nothing to do at all until one of them is switched on again.
    if (!active && !cull_on) return;
    cull_on = active;

    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (!render_data || !render_data->instance_rid.is_valid()) return;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        const Vector3 origin(cx * CHUNK_WIDTH, cy * CHUNK_HEIGHT, cz * CHUNK_DEPTH);
        const AABB local(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH));
        const AABB grown = cull_for(local, origin);
        // Remembering the growth and skipping the call when it has not really
        // moved turns a walk over every resident chunk into a walk over every
        // resident chunk's arithmetic, which is what makes this affordable once
        // per block rather than once per frame. The growth is a magnitude rather
        // than the box's own position change (see `world_cull_growth`): written
        // the other way round it is negative whenever the box is grown, so the
        // skip below would never once have fired while an effect was on. A box
        // that has never been grown is written whatever the growth says, because
        // a negative remembered value is not any growth at all.
        const Vector2 growth = world_cull_growth(local, grown);
        if (world_cull_growth_unchanged(render_data->cull_growth, growth, kCullEpsilon)) {
            return;
        }
        rs->instance_set_custom_aabb(render_data->instance_rid, grown);
        render_data->cull_growth = growth;
    });

    for (auto& [region_key, region] : far_regions) {
        if (region.instance_rid.is_valid()) {
            refresh_world_cull_region(region, region_key);
        }
    }
}

} // namespace VoxelEngine
