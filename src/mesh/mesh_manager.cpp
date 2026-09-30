#include "mesh/mesh_manager_internal.hpp"
#include "mesh/lod_shell.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace VoxelEngine {

using namespace godot;
void MeshManager::set_player_chunk(int32_t cx, int32_t cy, int32_t cz) {
    last_player_chunk_x = cx;
    last_player_chunk_y = cy;
    last_player_chunk_z = cz;
    refresh_far_region_visibility();
}

void MeshManager::hide_chunk_instance(ChunkRenderData* render_data) {
    if (!render_data || !render_data->instance_rid.is_valid()) {
        return;
    }
    RenderingServer::get_singleton()->instance_set_visible(render_data->instance_rid, false);
}

void MeshManager::show_chunk_instance(ChunkRenderData* render_data, int32_t cx, int32_t cy, int32_t cz) {
    if (!render_data || !render_data->mesh_rid.is_valid()) {
        return;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!render_data->instance_rid.is_valid()) {
        render_data->instance_rid = rs->instance_create();
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
        rs->instance_geometry_set_cast_shadows_setting(
            render_data->instance_rid,
            RenderingServer::SHADOW_CASTING_SETTING_OFF);
        const Vector3 origin(cx * CHUNK_WIDTH, cy * CHUNK_HEIGHT, cz * CHUNK_DEPTH);
        // The chunk's own box, grown by whichever of the world effects is on: a
        // new instance has to be right the moment it appears, or a chunk
        // uploaded while the player stands still would be culled against where
        // it no longer is until they happen to move a block.
        cull_apply(render_data->instance_rid, render_data->cull_growth,
                   AABB(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH)),
                   origin);
        Transform3D transform;
        transform.origin = origin;
        rs->instance_set_transform(render_data->instance_rid, transform);
        if (owner) {
            Node3D* owner3d = Object::cast_to<Node3D>(owner);
            if (owner3d) {
                Ref<World3D> world = owner3d->get_world_3d();
                if (world.is_valid()) {
                    rs->instance_set_scenario(render_data->instance_rid, world->get_scenario());
                }
            }
        }
    } else {
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
    }
    rs->instance_set_visible(render_data->instance_rid, true);
}

void MeshManager::queue_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    mesh_queue.queue_dirty_chunk(chunk_map->get_chunk_key(cx, cy, cz), dist_sq, false);
}

void MeshManager::queue_dirty_chunk_fast(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data_fast(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    mesh_queue.queue_dirty_chunk(chunk_map->get_chunk_key(cx, cy, cz), dist_sq, false);
}

void MeshManager::queue_immediate_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    uint64_t key = chunk_map->get_chunk_key(cx, cy, cz);
    mesh_queue.queue_immediate_dirty_chunk(key, mesh_queue.is_pending(key));
}

void MeshManager::mark_chunk_urgent(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    mesh_queue.mark_urgent(chunk_map->get_chunk_key(cx, cy, cz));
}

void MeshManager::reprioritize(int32_t player_cx, int32_t player_cy, int32_t player_cz, const Frustum* frustum) {
    last_player_chunk_x = player_cx;
    last_player_chunk_y = player_cy;
    last_player_chunk_z = player_cz;
    mesh_queue.reprioritize(player_cx, player_cy, player_cz, frustum);

    if (lod_distance <= 0 || !chunk_map) {
        refresh_far_region_visibility();
        return;
    }

    // The transition shells are Chebyshev rings around the player, so the
    // scan must reach the outermost tier start vertically as well as
    // horizontally (no vertical render distance cap).
    const int32_t vert_range = std::max(lod_distance, far_lod_distance) + 1;
    int32_t queued = 0;
    constexpr int32_t kMaxLodRemeshPerFrame = 512;

    // Pass A: Active set demotions run FIRST so they are never starved by
    // the much larger shell scans.  Each sweep erases the chunk from its set
    // unconditionally after attempting a requeue — chunks already marked dirty
    // (or unloaded) are simply dropped; they re-insert on completion.

    // Full detail → mid detail (or far) downgrades.
    for (auto it = active_full_detail_chunks_.begin(); it != active_full_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target >= 1.0f) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_full_detail_chunks_.erase(it);
    }

    // Mid detail → full detail upgrades (e.g. after teleport/respawn).
    // Without this, chunks built at mid detail that fall inside the full
    // range are never upgraded because the shell scans only cover ±1 of
    // each transition distance.
    for (auto it = active_mid_detail_chunks_.begin(); it != active_mid_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target < 1.0f) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_mid_detail_chunks_.erase(it);
    }

    // Mid detail → far detail downgrades.
    for (auto it = active_mid_detail_chunks_.begin(); it != active_mid_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target >= lod_detail_level) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_mid_detail_chunks_.erase(it);
    }

    // Far detail → mid (or full) upgrades when the player moves closer.
    // Without this sweep, chunks built at far detail that fall inside the
    // mid/full range are never upgraded because the shell scans only cover
    // ±1 of each transition distance.
    for (auto it = active_far_detail_chunks_.begin(); it != active_far_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target <= far_lod_detail_level) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_far_detail_chunks_.erase(it);
    }

    // Pass B: scan the transition shells of both LOD tiers (mid and far).
    // Chunks newly inside a tier's render start are upgraded in place to the
    // tier's detail level (full res at the lod ring, mid detail at the far ring).
    const int32_t transition_starts[2] = {lod_distance, far_lod_distance};
    for (int32_t tier_start : transition_starts) {
        if (tier_start <= 0) continue;
        const int32_t shell_min = tier_start - 1;
        const int32_t shell_max = tier_start + 1;
        for_each_shell_cell(shell_min, shell_max, vert_range, [&](int32_t dx, int32_t dy, int32_t dz) {
            if (queued >= kMaxLodRemeshPerFrame) return false;
            const int32_t cx = player_cx + dx;
            const int32_t cy = player_cy + dy;
            const int32_t cz = player_cz + dz;

            ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
            if (!render_data) return true;

            float target = compute_chunk_detail_level(cx, cy, cz);
            if (target != render_data->last_built_detail_level && !render_data->is_mesh_dirty) {
                render_data->is_mesh_dirty = true;
                render_data->mesh_version++;
                mark_far_region_dirty_for_chunk(cx, cy, cz);
                queue_dirty_chunk(cx, cy, cz);
                ++queued;
            }
            return true;
        });
    }

    refresh_far_region_visibility();
}

// One chunk's light changed and it needs a remesh. Split out of the region form
// below because a light pass now reports WHICH chunks it wrote (see
// BlockLightRegion::modified_mask): marking a fixed 3x3x3 queued 27 remeshes for
// light that had often not moved at all.
void MeshManager::mark_chunk_dirty_for_light(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        render_data->mesh_version++;
    }
    queue_dirty_chunk(cx, cy, cz);
}

void MeshManager::mark_chunks_dirty_for_light(int32_t center_cx, int32_t center_cy, int32_t center_cz) {
    if (!chunk_map) return;
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dz = -1; dz <= 1; dz++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                mark_chunk_dirty_for_light(center_cx + dx, center_cy + dy, center_cz + dz);
            }
        }
    }
}

void MeshManager::process_queue(int32_t max_immediate, int32_t max_rebuilds, double budget_ms) {
    if (max_rebuilds <= 0) {
        return;
    }
    int32_t far_region_budget = max_rebuilds / kFarRegionBuildDivisor;
    if (max_rebuilds > 1) {
        far_region_budget = std::max(1, far_region_budget);
    }
    far_region_budget = std::min(far_region_budget, max_rebuilds);
    const int32_t chunk_budget = std::max(0, max_rebuilds - far_region_budget);

    int32_t mesh_rd_sq = mesh_render_distance ? mesh_render_distance * mesh_render_distance : INT32_MAX;
    int32_t pcx = last_player_chunk_x;
    int32_t pcz = last_player_chunk_z;
        mesh_queue.process(
            [this, mesh_rd_sq, pcx, pcz](int32_t cx, int32_t cy, int32_t cz) -> bool {
                int32_t dx = cx - pcx;
                int32_t dz = cz - pcz;
                // Horizontal-only: no vertical render distance.
                if (dx*dx + dz*dz > mesh_rd_sq) {
                    return false;
                }
                rebuild_chunk_mesh(cx, cy, cz, async_epoch ? async_epoch->load(std::memory_order_acquire) : 0);
                return true;
            },
        max_immediate,
        chunk_budget,
        budget_ms
    );
    process_far_region_queue(far_region_budget);
}

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
void MeshManager::cull_apply(const RID& instance, Vector2& remembered, const AABB& local_aabb,
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
