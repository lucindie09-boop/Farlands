#include "world/block_editor.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "core/block_types.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

namespace {
// Ray-AABB intersection using the slab method. Returns true if the ray hits
// the box, with the parametric distance t and the outward face normal.
bool ray_aabb_intersect(const Vector3& origin, const Vector3& dir,
                        const Vector3& box_min, const Vector3& box_max,
                        double& t_out, Vector3& normal_out) {
    double tmin = -1e30;
    double tmax = 1e30;
    Vector3 normal(0, 0, 0);

    for (int i = 0; i < 3; ++i) {
        double o = (i == 0) ? origin.x : (i == 1) ? origin.y : origin.z;
        double d = (i == 0) ? dir.x : (i == 1) ? dir.y : dir.z;
        double bmin = (i == 0) ? box_min.x : (i == 1) ? box_min.y : box_min.z;
        double bmax = (i == 0) ? box_max.x : (i == 1) ? box_max.y : box_max.z;

        if (std::abs(d) < 1e-12) {
            if (o < bmin || o > bmax) return false;
        } else {
            double inv_d = 1.0 / d;
            double t1 = (bmin - o) * inv_d;
            double t2 = (bmax - o) * inv_d;
            if (t1 > t2) std::swap(t1, t2);

            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;

            if (tmin > tmax) return false;

            // Track which axis/face was hit at tmin
            if (tmin == t1) {
                normal = Vector3(0, 0, 0);
                if (i == 0) normal.x = (dir.x > 0) ? -1.0 : 1.0;
                else if (i == 1) normal.y = (dir.y > 0) ? -1.0 : 1.0;
                else normal.z = (dir.z > 0) ? -1.0 : 1.0;
            }
        }
    }

    if (tmin < 0 || tmin > 1e30) return false;
    t_out = tmin;
    normal_out = normal;
    return true;
}
} // namespace

BlockEditor::BlockEditor(ChunkWorld* cw, MeshManager* mm, LightPropagator* lp)
    : chunk_world(cw), mesh_manager(mm), light_propagator(lp) {}

int BlockEditor::query_block(int32_t world_x, int32_t world_y, int32_t world_z) const {
    return chunk_world->get_block_world(world_x, world_y, world_z);
}

void BlockEditor::place_block(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);

    ChunkMap& cm = chunk_world->get_chunk_map();

    // Quick check: if chunk not loaded, queue pending placement and bail.
    // Uses shared lock only — safe before exclusive scope.
    {
        auto sl = cm.lock_chunk(chunk_x, chunk_y, chunk_z);
        if (!cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z)) {
            chunk_world->queue_pending_placement(world_x, world_y, world_z, static_cast<int>(new_block));
            return;
        }
    }

    // Track mud variant chunk for post-lock edit map write.
    int32_t mud_cx = 0, mud_cy = 0, mud_cz = 0;
    int32_t mud_lx = 0, mud_ly = 0, mud_lz = 0;
    BlockID mud_variant = BlockIDs::AIR;
    bool did_mud = false;

    // Calculate mud variant local coordinates once (world_to_chunk_local needs chunk coords)
    int32_t below_world_y = world_y - 1;
    int32_t bw_cx, bw_cy, bw_cz;
    world_to_chunk_local(world_x, below_world_y, world_z, bw_cx, bw_cy, bw_cz, mud_lx, mud_ly, mud_lz);

    {
        // Block change + light propagation can reach at most 1 chunk in each
        // direction (max light level 15 < chunk size 32). Lock 3×3×3 around
        // the center chunk — covers block write, light BFS, and mud variant.
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = cm.get_chunk_key(chunk_x + dx, chunk_y + dy, chunk_z + dz);
        auto lock = cm.lock_keys_exclusive(keys);
        ChunkData* chunk_data = cm.get_chunk_data_fast(chunk_x, chunk_y, chunk_z);
        if (!chunk_data) return;
        if (!is_local_in_bounds(local_x, local_y, local_z)) return;

        const BlockID old_block = chunk_data->get_block_unsafe(local_x, local_y, local_z);
        const auto& reg = VoxelEngine::BlockRegistry::get_instance();
        const auto* new_slab_fam = reg.get_slab_family(new_block);
        const auto* new_wall_fam = reg.get_wall_family(new_block);
        const auto* old_wall_fam = reg.get_wall_family(old_block);
        const bool is_merge = (new_slab_fam && new_block == new_slab_fam->full
                               && (old_block == new_slab_fam->bottom || old_block == new_slab_fam->top))
                           || (new_wall_fam && old_wall_fam == new_wall_fam
                               && new_block == new_wall_fam->full);
        if (old_block != BlockIDs::AIR && new_block != BlockIDs::AIR && !is_merge) return;
        if (old_block == new_block) return;

        const BlockRegistry& registry = BlockRegistry::get_instance();
        const BlockType& old_type = registry.get_block(old_block);
        const BlockType& new_type = registry.get_block(new_block);
        const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
        const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);

        const uint8_t old_r = chunk_data->get_light_r(local_x, local_y, local_z);
        const uint8_t old_g = chunk_data->get_light_g(local_x, local_y, local_z);
        const uint8_t old_b = chunk_data->get_light_b(local_x, local_y, local_z);

        chunk_data->set_block(local_x, local_y, local_z, new_block);

        if (old_opaque != new_opaque) {
            ChunkData* above = cm.get_chunk_data_fast(chunk_x, chunk_y + 1, chunk_z);
            chunk_data->propagate_sky_light_column(local_x, local_z, above);
        }

        light_propagator->update_block_light_incremental_locked(
            chunk_x, chunk_y, chunk_z, chunk_x, chunk_y, chunk_z,
            local_x, local_y, local_z,
            old_block, new_block, old_r, old_g, old_b);

        ChunkRenderData* render_data = cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z);
        if (render_data) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(local_x, local_y, local_z));
            render_data->mark_block_dirty(local_x, local_y, local_z);
        }

        // Mud variant: inline update_mud_variants logic while lock is held.
        if (world_y > 0) {
            ChunkData* below_chunk = cm.get_chunk_data_fast(bw_cx, bw_cy, bw_cz);
            if (below_chunk && is_local_in_bounds(mud_lx, mud_ly, mud_lz)) {
                const BlockID below = below_chunk->get_block_unsafe(mud_lx, mud_ly, mud_lz);
                BlockID variant = BlockIDs::AIR;
                if (new_block != BlockIDs::AIR) {
                    if (below == BlockIDs::MUD) variant = BlockIDs::MUD_FULL;
                    else if (below == BlockIDs::WET_SAND) variant = BlockIDs::WET_SAND_FULL;
                } else {
                    if (below == BlockIDs::MUD_FULL) variant = BlockIDs::MUD;
                    else if (below == BlockIDs::WET_SAND_FULL) variant = BlockIDs::WET_SAND;
                }
                if (variant != BlockIDs::AIR && below != variant) {
                    below_chunk->set_block(mud_lx, mud_ly, mud_lz, variant);
                    ChunkRenderData* bw_rd = cm.get_chunk_render_data_fast(bw_cx, bw_cy, bw_cz);
                    if (bw_rd) {
                        bw_rd->is_mesh_dirty = true;
                        bw_rd->mesh_version++;
                        bw_rd->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(mud_lx, mud_ly, mud_lz));
                        bw_rd->mark_block_dirty(mud_lx, mud_ly, mud_lz);
                    }
                    mud_cx = bw_cx;
                    mud_cy = bw_cy;
                    mud_cz = bw_cz;
                    mud_variant = variant;
                    did_mud = true;
                }
            }
        }
    }
    // Lock released — auto-locking accessors are safe now.

    // Persist the edit in the edit map instead of marking the whole chunk dirty
    chunk_world->add_block_edit(chunk_x, chunk_y, chunk_z, local_x, local_y, local_z, new_block);
    // A block change (and any light removal) can touch every chunk in the
    // 3×3×3 neighborhood — including diagonals when a light block near a chunk
    // corner spills light across two boundaries at once. Bump mesh_version on
    // the whole neighborhood so rebuild_rendering_server_mesh's version gate
    // lets every affected chunk remesh (the old queue_player_edit_chunk_refresh
    // only set is_mesh_dirty, which the gate silently discarded).
    mesh_manager->mark_chunks_dirty_for_light(chunk_x, chunk_y, chunk_z);
    if (did_mud) {
        // Persist the mud variant edit as well
        chunk_world->add_block_edit(mud_cx, mud_cy, mud_cz, mud_lx, mud_ly, mud_lz, mud_variant);
        mesh_manager->queue_dirty_chunk(mud_cx, mud_cy, mud_cz);
    }
}

RaycastResult BlockEditor::raycast_from_ray(const Vector3& ray_origin,
                                              const Vector3& ray_dir,
                                              double max_distance) const {
    RaycastResult result;

    int32_t current_x = static_cast<int32_t>(std::floor(ray_origin.x));
    int32_t current_y = static_cast<int32_t>(std::floor(ray_origin.y));
    int32_t current_z = static_cast<int32_t>(std::floor(ray_origin.z));

    int32_t step_x = ray_dir.x > 0 ? 1 : (ray_dir.x < 0 ? -1 : 0);
    int32_t step_y = ray_dir.y > 0 ? 1 : (ray_dir.y < 0 ? -1 : 0);
    int32_t step_z = ray_dir.z > 0 ? 1 : (ray_dir.z < 0 ? -1 : 0);

    double t_max_x = step_x != 0 ?
        ((step_x > 0 ? (current_x + 1) : current_x) - ray_origin.x) / ray_dir.x : max_distance;
    double t_max_y = step_y != 0 ?
        ((step_y > 0 ? (current_y + 1) : current_y) - ray_origin.y) / ray_dir.y : max_distance;
    double t_max_z = step_z != 0 ?
        ((step_z > 0 ? (current_z + 1) : current_z) - ray_origin.z) / ray_dir.z : max_distance;

    double t_delta_x = step_x != 0 ? std::abs(1.0 / ray_dir.x) : max_distance;
    double t_delta_y = step_y != 0 ? std::abs(1.0 / ray_dir.y) : max_distance;
    double t_delta_z = step_z != 0 ? std::abs(1.0 / ray_dir.z) : max_distance;

    int32_t prev_x = current_x;
    int32_t prev_y = current_y;
    int32_t prev_z = current_z;

    const int32_t max_steps = static_cast<int32_t>(max_distance) * 3;
    int32_t steps = 0;

    // The DDA below advances up to one block per axis per step, so it stays
    // inside [origin, origin + dir * (3 * max_distance)] blocks. Shared-lock
    // only the shards of the chunks that box can touch (a reach-distance ray
    // spans 1-3 chunks) once, up front — the DDA never leaves the box, so no
    // mid-walk re-acquisition is needed.
    const int32_t end_x = static_cast<int32_t>(std::floor(ray_origin.x + ray_dir.x * (max_distance * 3.0 + 2.0)));
    const int32_t end_y = static_cast<int32_t>(std::floor(ray_origin.y + ray_dir.y * (max_distance * 3.0 + 2.0)));
    const int32_t end_z = static_cast<int32_t>(std::floor(ray_origin.z + ray_dir.z * (max_distance * 3.0 + 2.0)));

    std::vector<uint64_t> keys;
    {
        int32_t min_cx, min_cy, min_cz, max_cx, max_cy, max_cz, dummy;
        world_to_chunk_local(std::min(current_x, end_x), std::min(current_y, end_y), std::min(current_z, end_z),
                             min_cx, min_cy, min_cz, dummy, dummy, dummy);
        world_to_chunk_local(std::max(current_x, end_x), std::max(current_y, end_y), std::max(current_z, end_z),
                             max_cx, max_cy, max_cz, dummy, dummy, dummy);
        for (int32_t cx = min_cx; cx <= max_cx; ++cx)
            for (int32_t cy = min_cy; cy <= max_cy; ++cy)
                for (int32_t cz = min_cz; cz <= max_cz; ++cz)
                    keys.push_back(chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz));
    }
    auto map_lock = chunk_world->get_chunk_map().lock_keys(keys);
    const BlockRegistry& registry = BlockRegistry::get_instance();
    while (steps < max_steps) {
        int block = chunk_world->get_chunk_map().get_block_world_fast(current_x, current_y, current_z);
        if (block != 0) {
            BlockID bid = static_cast<BlockID>(block);
            const BlockType& bt = registry.get_block_fast(bid);
            if (bt.is_full_cube()) {
                Vector3 face_normal(
                    static_cast<double>(prev_x - current_x),
                    static_cast<double>(prev_y - current_y),
                    static_cast<double>(prev_z - current_z));
                double t_face = 0.0;
                if (face_normal.x != 0.0) {
                    double fx = (face_normal.x > 0) ? current_x + 1 : current_x;
                    t_face = (fx - ray_origin.x) / ray_dir.x;
                } else if (face_normal.y != 0.0) {
                    double fy = (face_normal.y > 0) ? current_y + 1 : current_y;
                    t_face = (fy - ray_origin.y) / ray_dir.y;
                } else {
                    double fz = (face_normal.z > 0) ? current_z + 1 : current_z;
                    t_face = (fz - ray_origin.z) / ray_dir.z;
                }
                result.success = true;
                result.position = Vector3(current_x, current_y, current_z);
                result.place_position = Vector3(prev_x, prev_y, prev_z);
                result.block_id = static_cast<int>(bid);
                result.hit_normal = face_normal;
                result.hit_point = ray_origin + ray_dir * t_face;
                return result;
            }
            // Non-full block: test ray against each selection_box
            bool hit_any = false;
            double closest_t = max_distance;
            Vector3 hit_normal;
            for (const auto& box : bt.selection_boxes) {
                Vector3 box_min(current_x + box.min[0], current_y + box.min[1], current_z + box.min[2]);
                Vector3 box_max(current_x + box.max[0], current_y + box.max[1], current_z + box.max[2]);
                double t;
                Vector3 n;
                if (ray_aabb_intersect(ray_origin, ray_dir, box_min, box_max, t, n) && t < closest_t) {
                    closest_t = t;
                    hit_normal = n;
                    hit_any = true;
                }
            }
            if (hit_any) {
                result.success = true;
                result.position = Vector3(current_x, current_y, current_z);
                result.place_position = Vector3(current_x, current_y, current_z) + hit_normal;
                result.block_id = static_cast<int>(bid);
                result.hit_normal = hit_normal;
                result.hit_point = ray_origin + ray_dir * closest_t;
                return result;
            }
            // No AABB hit — continue DDA to find next block
            prev_x = current_x;
            prev_y = current_y;
            prev_z = current_z;
        } else {
            prev_x = current_x;
            prev_y = current_y;
            prev_z = current_z;
        }

        if (t_max_x < t_max_y) {
            if (t_max_x < t_max_z) {
                current_x += step_x;
                t_max_x += t_delta_x;
            } else {
                current_z += step_z;
                t_max_z += t_delta_z;
            }
        } else {
            if (t_max_y < t_max_z) {
                current_y += step_y;
                t_max_y += t_delta_y;
            } else {
                current_z += step_z;
                t_max_z += t_delta_z;
            }
        }
        steps++;
    }
    return result;
}

// -------------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------------

void BlockEditor::update_mud_variants(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    if (world_y <= 0) return;
    const BlockID below = static_cast<BlockID>(chunk_world->get_block_world(world_x, world_y - 1, world_z));
    if (new_block != BlockIDs::AIR) {
        if (below == BlockIDs::MUD) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::MUD_FULL);
        } else if (below == BlockIDs::WET_SAND) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::WET_SAND_FULL);
        }
    } else {
        if (below == BlockIDs::MUD_FULL) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::MUD);
        } else if (below == BlockIDs::WET_SAND_FULL) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::WET_SAND);
        }
    }
}

void BlockEditor::post_block_change(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    update_mud_variants(world_x, world_y, world_z, new_block);
}

void BlockEditor::set_block_variant(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);

    ChunkMap& cm = chunk_world->get_chunk_map();
    {
        uint64_t key = cm.get_chunk_key(chunk_x, chunk_y, chunk_z);
        auto lock = cm.lock_keys_exclusive({key});
        ChunkData* chunk_data = cm.get_chunk_data_fast(chunk_x, chunk_y, chunk_z);
        if (!chunk_data) return;
        if (!is_local_in_bounds(local_x, local_y, local_z)) return;
        const BlockID old_block = chunk_data->get_block_unsafe(local_x, local_y, local_z);
        if (old_block == block_id) return;
        chunk_data->set_block(local_x, local_y, local_z, block_id);
        ChunkRenderData* render_data = cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z);
        if (render_data) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(local_x, local_y, local_z));
            render_data->mark_block_dirty(local_x, local_y, local_z);
        }
    }
    chunk_world->mark_chunk_dirty(chunk_x, chunk_y, chunk_z);
    mesh_manager->queue_dirty_chunk(chunk_x, chunk_y, chunk_z);
}

// -------------------------------------------------------------------------
// Bulk paste
// -------------------------------------------------------------------------

namespace {

// Same rule as BlockEditor::is_local_in_bounds, for a free function that has no
// `this`. Both answer "is this cell inside the chunk it names".
bool cell_in_chunk(int32_t lx, int32_t ly, int32_t lz) {
    return lx >= 0 && lx < CHUNK_WIDTH && ly >= 0 && ly < CHUNK_HEIGHT && lz >= 0 &&
           lz < CHUNK_DEPTH;
}

// Whether a written cell can change what the fluid simulation would do: the cell
// became or stopped being a fluid, or a face-neighbour is one. That is the
// simulation's own rule, answered here because here the neighbours are reads of the
// chunk already in hand.
//
// Only cells that pass this are woken, and the simulation re-tests each one exactly
// as before — so nothing that mattered can be missed: a fluid cell is always woken
// by its own write, and the simulation's scan of that cell covers every neighbour it
// could affect.
bool paste_cell_needs_fluid_wake(const BlockRegistry& registry, const ChunkData& chunk,
                                 const ChunkMap& cm, BlockID old_block, BlockID new_block,
                                 int32_t world_x, int32_t world_y, int32_t world_z, int32_t lx,
                                 int32_t ly, int32_t lz) {
    if (registry.get_block_fast(new_block).is_fluid_state()) return true;
    if (registry.get_block_fast(old_block).is_fluid_state()) return true;
    static constexpr int32_t kOffsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto& offset : kOffsets) {
        const int32_t nx = lx + offset[0];
        const int32_t ny = ly + offset[1];
        const int32_t nz = lz + offset[2];
        // Inside the chunk: an array read. Across the seam: the one case that has
        // to ask the world, and only for the cells that sit on a face.
        const BlockID neighbor = cell_in_chunk(nx, ny, nz)
            ? chunk.get_block_unsafe(nx, ny, nz)
            : static_cast<BlockID>(cm.get_block_world(world_x + offset[0],
                                                      world_y + offset[1],
                                                      world_z + offset[2]));
        if (registry.get_block_fast(neighbor).is_fluid_state()) return true;
    }
    return false;
}

} // namespace

PasteWriteResult BlockEditor::apply_paste(const schematic::PastePlan& plan,
                                          const schematic::PasteOptions& options,
                                          bool append_undo,
                                          std::vector<schematic::PastePlan::Cell>* unwritten) {
    using schematic::PastePlan;
    PasteWriteResult result;
    if (plan.empty()) return result;

    ChunkMap& cm = chunk_world->get_chunk_map();
    const BlockRegistry& registry = BlockRegistry::get_instance();

    // Group the cells by chunk, key-ordered so the lock bands and the write
    // order are the same for the same plan on any machine.
    struct Group {
        int32_t cx = 0, cy = 0, cz = 0;
        std::vector<size_t> cells;
    };
    std::map<uint64_t, Group> groups;
    for (size_t i = 0; i < plan.cells.size(); ++i) {
        const PastePlan::Cell& cell = plan.cells[i];
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        Group& group = groups[cm.get_chunk_key(cx, cy, cz)];
        group.cx = cx;
        group.cy = cy;
        group.cz = cz;
        group.cells.push_back(i);
    }

    // Index-aligned with each other: what each written cell displaced, and what
    // it was set to. The first becomes the undo record, the second is what the
    // edit map and the fluid simulation hear about.
    std::vector<PastePlan::Cell> displaced;
    std::vector<PastePlan::Cell> written;
    displaced.reserve(plan.cells.size());
    written.reserve(plan.cells.size());
    std::vector<std::array<int32_t, 3>> touched;
    // World positions whose write can change a fluid's answer, decided while the
    // chunk was in hand and woken once every band is released.
    std::vector<std::array<int32_t, 3>> wake_world;
    // The subset of `touched` whose BLOCK light can actually have changed. Every
    // chunk a paste writes to needs a remesh, but only these need the 3×3×3 region
    // pass — and that pass is the expensive half of a paste by a wide margin
    // (measured: milliseconds a chunk against fractions of one for the write). A
    // chunk of stone swapped for other stone, or a wall built out of blocks that
    // were already opaque, moves no light at all.
    std::vector<std::array<int32_t, 3>> light_touched;
    // The volume the paste actually changed, accumulated across every chunk it
    // touches — NOT per chunk, which would report only the last chunk's box.
    bool have_bounds = false;

    for (auto& entry : groups) {
        Group& group = entry.second;
        // One exclusive band for the whole chunk, matching place_block's reach
        // (a block write plus a light update never leaves the 3×3×3 around it),
        // rather than a lock per cell.
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = cm.get_chunk_key(group.cx + dx, group.cy + dy, group.cz + dz);
        auto lock = cm.lock_keys_exclusive(keys);

        ChunkData* chunk = cm.get_chunk_data_fast(group.cx, group.cy, group.cz);
        if (chunk == nullptr) {
            // The plan was built against a world that has since moved on here.
            // Nothing is queued by the writer itself: a paste is not a player
            // edit at the loading frontier, so it reports the cells back and the
            // caller decides (the paste job waits for the chunk and retries).
            result.skipped_unloaded += group.cells.size();
            if (unwritten != nullptr) {
                for (const size_t index : group.cells) unwritten->push_back(plan.cells[index]);
            }
            continue;
        }
        ChunkData* above = cm.get_chunk_data_fast(group.cx, group.cy + 1, group.cz);
        ChunkRenderData* render = cm.get_chunk_render_data_fast(group.cx, group.cy, group.cz);

        // Columns whose opacity changed, so sky light is recomputed once per
        // column rather than once per cell.
        std::vector<uint32_t> sky_columns;
        bool wrote_here = false;
        bool light_relevant_here = false;

        for (const size_t index : group.cells) {
            const PastePlan::Cell& cell = plan.cells[index];
            int32_t cx, cy, cz, lx, ly, lz;
            world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
            if (!is_local_in_bounds(lx, ly, lz)) {
                ++result.skipped_out_of_bounds;
                continue;
            }
            const BlockID old_block = chunk->get_block_unsafe(lx, ly, lz);
            if (old_block == cell.block) {
                ++result.skipped_unchanged;
                continue;
            }
            if (!options.replace_solid && old_block != BlockIDs::AIR) {
                ++result.skipped_covered;
                continue;
            }

            chunk->set_block(lx, ly, lz, cell.block);

            const BlockType& old_type = registry.get_block_fast(old_block);
            const BlockType& new_type = registry.get_block_fast(cell.block);
            const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
            const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);
            // Whether BLOCK light can have moved here: whether light passes through
            // the cell, or either block emits.
            if (old_opaque != new_opaque || old_type.light_opacity != new_type.light_opacity ||
                HasProperty(old_type.properties, BlockProperty::Emissive) ||
                HasProperty(new_type.properties, BlockProperty::Emissive)) {
                light_relevant_here = true;
            }
            if (old_opaque != new_opaque) {
                const uint32_t column = (static_cast<uint32_t>(lx) << 16) |
                                        static_cast<uint32_t>(lz & 0xFFFF);
                bool known = false;
                for (const uint32_t seen : sky_columns) {
                    if (seen == column) { known = true; break; }
                }
                if (!known) sky_columns.push_back(column);
            }

            // Whether this write can change what the fluid simulation would do, asked
            // HERE because here the six neighbours are reads of the chunk already in
            // hand. Waking every written cell instead made the main thread pay a
            // locked neighbour scan per CELL to discover that a stone wall has no
            // fluid anywhere near it.
            if (paste_cell_needs_fluid_wake(registry, *chunk, cm, old_block, cell.block,
                                            cell.x, cell.y, cell.z, lx, ly, lz)) {
                wake_world.push_back({cell.x, cell.y, cell.z});
            }

            displaced.push_back(PastePlan::Cell{cell.x, cell.y, cell.z, old_block});
            written.push_back(cell);
            ++result.written;
            // This chunk (not the volume) needs a remesh and a relight, and that
            // is per chunk, so it is a separate flag from the bounds above.
            wrote_here = true;

            if (!have_bounds) {
                result.min_x = result.max_x = cell.x;
                result.min_y = result.max_y = cell.y;
                result.min_z = result.max_z = cell.z;
                have_bounds = true;
            } else {
                if (cell.x < result.min_x) result.min_x = cell.x;
                if (cell.x > result.max_x) result.max_x = cell.x;
                if (cell.y < result.min_y) result.min_y = cell.y;
                if (cell.y > result.max_y) result.max_y = cell.y;
                if (cell.z < result.min_z) result.min_z = cell.z;
                if (cell.z > result.max_z) result.max_z = cell.z;
            }

            if (render) {
                render->is_mesh_dirty = true;
                render->mesh_version++;
                render->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(lx, ly, lz));
                render->mark_block_dirty(lx, ly, lz);
            }
        }

        // Sky light is a column property: recompute the touched columns of this
        // chunk while the band is still held, exactly as a single edit does.
        for (const uint32_t column : sky_columns) {
            chunk->propagate_sky_light_column(static_cast<int32_t>(column >> 16),
                                              static_cast<int32_t>(column & 0xFFFF), above);
        }
        if (wrote_here) {
            touched.push_back({group.cx, group.cy, group.cz});
            if (light_relevant_here) {
                light_touched.push_back({group.cx, group.cy, group.cz});
            }
        }
    }

    // Locks released. Persist the edits one CHUNK at a time, then wake the fluid
    // cells that can matter, then relight and remesh each touched chunk once.
    //
    // The writes are grouped by the same loop that produced them, so the cells of one
    // chunk are already consecutive in `written` — a run can be handed over without
    // copying or regrouping anything.
    {
        std::vector<ChunkWorld::EditCell> run;
        int32_t run_cx = 0, run_cy = 0, run_cz = 0;
        bool have_run = false;
        // A chunk's cells can span several bands' worth of runs, so the run is
        // flushed whenever the chunk changes and the last one after the loop.
        auto flush_run = [&]() {
            if (have_run && !run.empty()) {
                chunk_world->add_block_edits(run_cx, run_cy, run_cz, run);
            }
            run.clear();
        };
        for (const PastePlan::Cell& cell : written) {
            int32_t cx, cy, cz, lx, ly, lz;
            world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
            if (have_run && (cx != run_cx || cy != run_cy || cz != run_cz)) {
                flush_run();
            }
            run_cx = cx;
            run_cy = cy;
            run_cz = cz;
            have_run = true;
            run.push_back(ChunkWorld::EditCell{lx, ly, lz, cell.block});
        }
        flush_run();
    }
    // The fluid wakes, which used to be a side effect of persisting each cell.
    for (const std::array<int32_t, 3>& pos : wake_world) {
        chunk_world->notify_block_change(pos[0], pos[1], pos[2]);
    }
    for (const std::array<int32_t, 3>& pos : touched) {
        chunk_world->mark_chunk_dirty(pos[0], pos[1], pos[2]);
        mesh_manager->queue_dirty_chunk(pos[0], pos[1], pos[2]);
    }
    // Relights the 3×3×3 neighbourhood (and dirties the meshes of the chunks whose
    // light actually changed), which is what a block change can reach. Only for the
    // chunks where a written cell could have moved light: over a build that is
    // mostly stone, this is the difference between a region pass per touched chunk
    // and almost none of them.
    for (const std::array<int32_t, 3>& pos : light_touched) {
        light_propagator->propagate_block_light_region(pos[0], pos[1], pos[2]);
    }

    result.chunks_touched = touched.size();
    result.undo_available = !displaced.empty();
    // A paste that wrote nothing leaves the previous record alone: it did not
    // make the last one unreachable.
    if (!displaced.empty()) {
        if (append_undo && paste_undo_.valid()) {
            // Another batch of the SAME paste: its displaced blocks join the
            // record rather than replacing it, so one undo takes the whole thing
            // back. The batches are disjoint by construction (a cell is written
            // once and then dropped from the plan), so no cell is recorded twice.
            paste_undo_.cells.insert(paste_undo_.cells.end(),
                                     std::make_move_iterator(displaced.begin()),
                                     std::make_move_iterator(displaced.end()));
        } else {
            paste_undo_.cells = std::move(displaced);
        }
    }
    return result;
}

bool BlockEditor::undo_paste(PasteWriteResult* out) {
    if (!paste_undo_.valid()) return false;

    // The record is taken out of the way first, because the write below replaces
    // it with what the revert itself displaced (which is the pasted build, and
    // would make undo a toggle).
    schematic::PasteUndo record = std::move(paste_undo_);
    paste_undo_ = schematic::PasteUndo{};

    schematic::PasteOptions options;
    // A revert restores the volume exactly, holes included, so it replaces what
    // is there and writes air.
    options.replace_solid = true;
    options.write_air = true;

    // Cells whose chunk is gone (evicted since the paste, on a build wide enough
    // to reach past the streaming frontier) come back as "not reverted" rather
    // than being silently dropped: the record keeps exactly those cells, so
    // calling again finishes the job — and reports `restored`/`unloaded` so the
    // caller can tell a finished undo from a partial one.
    std::vector<schematic::PastePlan::Cell> unreached;
    PasteWriteResult result =
        apply_paste(schematic::to_revert_plan(record), options, false, &unreached);
    result.restored = result.written;
    result.unloaded = unreached.size();
    if (unreached.empty()) {
        paste_undo_ = schematic::PasteUndo{};  // fully reverted, and it is spent
    } else {
        // Keep the one-level record, narrowed to what is left to put back.
        record.cells = std::move(unreached);
        paste_undo_ = std::move(record);
    }
    if (out) *out = result;
    return result.written > 0;
}

} // namespace VoxelEngine
