// Sky light: the scalar channel's own walk, and the two places it is run from.
//
// Sky light's value came from a per-column scan alone. That scan is exact inside
// a column and says nothing about the cell beside it: a cell under a ceiling came
// out 0 however bright the air one step away was, so a 3x3 roof over open ground
// was a hole rather than a dimple and a cave mouth stopped dead. This file runs
// the same six-neighbour relaxation the block-light engine uses -- a step costs 1
// plus the destination's light_opacity, and an opaque block takes nothing -- over
// the sky channel, so shade becomes a gradient that grows with distance to the
// nearest open column: the ring under a 3x3 roof reads 14 and its centre 13, a
// 4x4 reads 14 on the outer ring and 13 inside, and 0 is reserved for volumes no
// open column can reach.
//
// Two properties keep it affordable, and both are load-bearing:
//
//   1. A walk only ever RAISES a level. A column is a lower bound on the cell it
//      wrote, and relaxation from lower bounds reaches the same fixed point
//      whatever order it runs in -- so a partial pass is safe, a re-run is a
//      no-op, and nothing here ever needs a snapshot to compare against.
//   2. Bright cells are never enumerated on chunk install. The arriving chunk's
//      scan already wrote its open columns, so the pass walks only the cells that
//      can beat a neighbour (its shade) plus its six border planes. A plain, an
//      ocean and a solid chunk walk nothing at all, which is the common case
//      while streaming and the whole reason this is not a second region pass.
//
// Liquid cells are shade too, but they are deliberately NOT walked: an ocean's
// column already dims by its own opacity three levels a block, the sideways
// contribution there is a rounding difference, and walking every water cell of
// every arriving ocean chunk would put the pass on the streaming hot path for a
// result nobody can see. The same exclusion applies to the targets of the
// install pass, noted at each site.

#include "lighting/light_propagator.hpp"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
// The public install entry marks the meshes of what it changed, the same way
// propagate_block_light_region does, so MeshManager is complete here (the test
// build links tests/mesh_manager_stub.cpp for it).
#include "mesh/mesh_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VoxelEngine {

namespace {

// See the note on the block-light walk's trim: the buffer is reused between calls
// so the steady case never allocates, but a pathological pass can grow it and the
// memory would then sit pinned in the worker thread forever.
constexpr size_t kQueueBufferTrimCapacity = 4096;

void trim_queue_buffer(std::vector<SkyNode>& buffer) {
    if (buffer.capacity() > kQueueBufferTrimCapacity) {
        std::vector<SkyNode> trimmed;
        trimmed.reserve(64);
        buffer.swap(trimmed);
    }
}

// The 27-bit modified mask's layout is BlockLightRegion::slot_bit's -- bit
// (dx+1) + (dy+1)*3 + (dz+1)*9 -- so a caller can dirty the meshes of exactly the
// chunks a pass wrote without knowing which pass produced the mask.
[[nodiscard]] inline uint32_t sky_slot_bit(int32_t dcx, int32_t dcy, int32_t dcz) noexcept {
    return 1u << static_cast<uint32_t>((dcx + 1) + (dcy + 1) * 3 + (dcz + 1) * 9);
}

[[nodiscard]] inline bool sky_in_band(int32_t dcx, int32_t dcy, int32_t dcz) noexcept {
    return dcx >= -1 && dcx <= 1 && dcy >= -1 && dcy <= 1 && dcz >= -1 && dcz <= 1;
}

// Whether a cell can hold sky light that arrives sideways: an opaque block cannot,
// and a liquid deliberately does not (see the file header).
[[nodiscard]] inline bool sky_is_shade_type(const BlockType& type) noexcept {
    if (HasProperty(type.properties, BlockProperty::Opaque)) return false;
    return !type.is_liquid();
}

}  // namespace

thread_local std::vector<SkyNode> LightPropagator::sky_add_buffer;
thread_local std::vector<SkyNode> LightPropagator::sky_remove_buffer;

void LightPropagator::sky_propagate_add_locked(std::vector<SkyNode>& queue,
                                               int32_t region_cx, int32_t region_cy, int32_t region_cz,
                                               uint32_t* modified_out) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    queue.reserve(1024);

    size_t idx = 0;
    while (idx < queue.size()) {
        const SkyNode node = queue[idx++];
        // A level-1 cell has nothing left to give: one step costs at least one
        // level, so everything it touches would land on 0.
        if (node.level <= 1) continue;
        const uint8_t next = static_cast<uint8_t>(node.level - 1);

        for (int i = 0; i < 6; ++i) {
            int32_t ncx = node.cx;
            int32_t ncy = node.cy;
            int32_t ncz = node.cz;
            int16_t nx = static_cast<int16_t>(node.x + kDiamondOffsets[i][0]);
            int16_t ny = static_cast<int16_t>(node.y + kDiamondOffsets[i][1]);
            int16_t nz = static_cast<int16_t>(node.z + kDiamondOffsets[i][2]);

            wrap_local_to_world(nx, ny, nz, ncx, ncy, ncz);

            // The caller holds an exclusive band around ONE chunk, and a chain from
            // a seed inside it can never leave the band (max level 15 < chunk size
            // 32), so a step that would land outside is a step onto a shard nobody
            // locked. Refusing it loses nothing: no reachable level can get there.
            const int32_t dcx = ncx - region_cx;
            const int32_t dcy = ncy - region_cy;
            const int32_t dcz = ncz - region_cz;
            if (!sky_in_band(dcx, dcy, dcz)) continue;

            ChunkData* dst = chunk_map->get_chunk_data_fast(ncx, ncy, ncz);
            if (!dst) {
                // Light spilling toward a chunk that is not resident is not tracked
                // here the way block light tracks it. A chunk's own column scan is
                // the source of truth for its cells, and its install pass seeds from
                // this side of the boundary, so the value is recovered by the
                // arriving chunk rather than by a pending-removal set.
                continue;
            }

            const BlockID neighbor_block = dst->get_block_unsafe(nx, ny, nz);
            const BlockType& neighbor_type = registry.get_block(neighbor_block);
            if (HasProperty(neighbor_type.properties, BlockProperty::Opaque)) continue;

            const int extra = neighbor_type.light_opacity;
            const uint8_t acquired = extra > 0
                                         ? static_cast<uint8_t>(std::max(0, static_cast<int>(next) - extra))
                                         : next;
            const uint8_t cur = dst->get_sky_light_unsafe(nx, ny, nz);
            if (acquired > cur) {
                dst->set_sky_light_unsafe(nx, ny, nz, acquired);
                if (modified_out != nullptr) {
                    *modified_out |= sky_slot_bit(dcx, dcy, dcz);
                }
                queue.push_back({ncx, ncy, ncz, nx, ny, nz, acquired});
            }
        }
    }

    trim_queue_buffer(queue);
}

void LightPropagator::sky_propagate_remove_locked(std::vector<SkyNode>& remove_queue,
                                                  std::vector<SkyNode>& add_queue,
                                                  int32_t region_cx, int32_t region_cy, int32_t region_cz) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    remove_queue.reserve(64);
    add_queue.reserve(64);

    size_t idx = 0;
    while (idx < remove_queue.size()) {
        const SkyNode node = remove_queue[idx++];
        if (node.level <= 1) continue;
        const uint8_t next = static_cast<uint8_t>(node.level - 1);

        for (int i = 0; i < 6; ++i) {
            int32_t ncx = node.cx;
            int32_t ncy = node.cy;
            int32_t ncz = node.cz;
            int16_t nx = static_cast<int16_t>(node.x + kDiamondOffsets[i][0]);
            int16_t ny = static_cast<int16_t>(node.y + kDiamondOffsets[i][1]);
            int16_t nz = static_cast<int16_t>(node.z + kDiamondOffsets[i][2]);

            wrap_local_to_world(nx, ny, nz, ncx, ncy, ncz);

            const int32_t dcx = ncx - region_cx;
            const int32_t dcy = ncy - region_cy;
            const int32_t dcz = ncz - region_cz;
            if (!sky_in_band(dcx, dcy, dcz)) continue;

            ChunkData* dst = chunk_map->get_chunk_data_fast(ncx, ncy, ncz);
            if (!dst) continue;

            const uint8_t cur = dst->get_sky_light_unsafe(nx, ny, nz);
            if (cur == 0) continue;

            const BlockID neighbor_block = dst->get_block_unsafe(nx, ny, nz);
            const BlockType& neighbor_type = registry.get_block(neighbor_block);
            if (HasProperty(neighbor_type.properties, BlockProperty::Opaque)) continue;

            // Was this cell's level something a step from `node` would have made?
            // The chain's prediction uses the DESTINATION's opacity, because that is
            // what a step into this cell costs. A cell at or below the prediction is
            // cleared and carries the walk on; anything brighter came from a source
            // of its own and is kept -- and handed to the add queue, because the
            // volume this walk is about to clear has to be refillable by it.
            const int extra = neighbor_type.light_opacity;
            const uint8_t predicted = extra > 0
                                          ? static_cast<uint8_t>(std::max(0, static_cast<int>(next) - extra))
                                          : next;
            if (cur > predicted) {
                add_queue.push_back({ncx, ncy, ncz, nx, ny, nz, cur});
                continue;
            }

            dst->set_sky_light_unsafe(nx, ny, nz, 0);
            // Carry the cell's old level: it is what tells the next step what this
            // chain could have produced further out. Each cell is cleared at most
            // once (it is zero afterwards and zero is skipped), so a region with
            // many surviving sources still terminates.
            remove_queue.push_back({ncx, ncy, ncz, nx, ny, nz, cur});
        }
    }

    // Only the removal queue is trimmed. `add_queue` is an OUTPUT: the caller runs
    // the add walk over it afterwards, and swapping its storage away would empty
    // the survivors' refill.
    trim_queue_buffer(remove_queue);
}

void LightPropagator::sky_light_recompute_column_locked(int32_t cx, int32_t cy, int32_t cz,
                                                        int32_t x, int32_t z) {
    // One column is the degenerate batch, and the two paths must not drift: a
    // single edit and a paste have to leave the same field behind.
    std::vector<uint32_t> one;
    one.push_back((static_cast<uint32_t>(x) << 16) | (static_cast<uint32_t>(z) & 0xFFFFu));
    // The caller is the block-edit path, which dirties the whole 3x3x3 afterwards,
    // so the mask is not needed here.
    (void)sky_light_recompute_columns_locked(cx, cy, cz, one);
}

uint32_t LightPropagator::sky_light_recompute_columns_locked(int32_t cx, int32_t cy, int32_t cz,
                                                            const std::vector<uint32_t>& columns) {
    ChunkData* chunk = chunk_map->get_chunk_data_fast(cx, cy, cz);
    if (!chunk) return 0;
    ChunkData* above = chunk_map->get_chunk_data_fast(cx, cy + 1, cz);

    std::vector<SkyColumnChange> changes;
    sky_remove_buffer.clear();
    sky_add_buffer.clear();

    for (const uint32_t column : columns) {
        const int32_t x = static_cast<int32_t>(column >> 16);
        const int32_t z = static_cast<int32_t>(column & 0xFFFFu);
        changes.clear();
        chunk->propagate_sky_light_column(x, z, above, &changes);
        for (const SkyColumnChange& change : changes) {
            const int16_t ly = change.y;
            if (change.old_level > change.new_level) {
                // The cell used to pass this much light on and no longer does. It is
                // already carrying its new (lower) value from the scan; the walk's job
                // is to un-propagate what left this column through its sides.
                sky_remove_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), ly,
                                             static_cast<int16_t>(z), change.old_level});
            } else if (change.new_level > change.old_level) {
                // The column gained light (a block above was broken): relax it out.
                sky_add_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), ly,
                                          static_cast<int16_t>(z), change.new_level});
            }
        }
    }

    if (sky_remove_buffer.empty() && sky_add_buffer.empty()) return 0;

    uint32_t modified = 0;
    if (!sky_remove_buffer.empty()) {
        sky_propagate_remove_locked(sky_remove_buffer, sky_add_buffer, cx, cy, cz);
    }
    if (!sky_add_buffer.empty()) {
        sky_propagate_add_locked(sky_add_buffer, cx, cy, cz, &modified);
    }
    return modified;
}

// ---------------------------------------------------------------------------
// The install pass.
// ---------------------------------------------------------------------------

namespace {

// The 3x3x3 the caller has locked, addressable by cell coordinates that may step
// one chunk out of the centre.
struct SkyRegionGrid {
    ChunkData* chunks[3][3][3] = {};
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;

    [[nodiscard]] ChunkData* chunk_at(int32_t dcx, int32_t dcy, int32_t dcz) const noexcept {
        if (!sky_in_band(dcx, dcy, dcz)) return nullptr;
        return chunks[dcx + 1][dcy + 1][dcz + 1];
    }
};

// Wraps a cell coordinate that may sit one chunk out, the same arithmetic the
// walks perform on every step, and reports which chunk the cell ended up in.
inline void sky_unwrap(int32_t& x, int32_t& y, int32_t& z,
                       int32_t& dcx, int32_t& dcy, int32_t& dcz) noexcept {
    if (x < 0) { x += CHUNK_WIDTH; --dcx; }
    else if (x >= CHUNK_WIDTH) { x -= CHUNK_WIDTH; ++dcx; }
    if (y < 0) { y += CHUNK_HEIGHT; --dcy; }
    else if (y >= CHUNK_HEIGHT) { y -= CHUNK_HEIGHT; ++dcy; }
    if (z < 0) { z += CHUNK_DEPTH; --dcz; }
    else if (z >= CHUNK_DEPTH) { z -= CHUNK_DEPTH; ++dcz; }
}

// Reads one cell by centre-chunk-local coordinates, following the coordinate out
// into the neighbouring chunk when it falls outside. The coordinate is rewritten
// in place so the caller can use it for a node. False means "not resident", which
// is not the same answer as "dark".
[[nodiscard]] bool sky_read_level(const SkyRegionGrid& grid, int32_t& x, int32_t& y, int32_t& z,
                                  int32_t& dcx, int32_t& dcy, int32_t& dcz, uint8_t& level) {
    dcx = 0; dcy = 0; dcz = 0;
    sky_unwrap(x, y, z, dcx, dcy, dcz);
    ChunkData* c = grid.chunk_at(dcx, dcy, dcz);
    if (!c) return false;
    level = c->get_sky_light_unsafe(x, y, z);
    return true;
}

[[nodiscard]] bool sky_read_block(const SkyRegionGrid& grid, int32_t x, int32_t y, int32_t z,
                                  BlockID& id) {
    int32_t dcx = 0, dcy = 0, dcz = 0;
    sky_unwrap(x, y, z, dcx, dcy, dcz);
    ChunkData* c = grid.chunk_at(dcx, dcy, dcz);
    if (!c) return false;
    id = c->get_block_unsafe(x, y, z);
    return true;
}

}  // namespace

uint32_t LightPropagator::scatter_sky_light_region(int32_t cx, int32_t cy, int32_t cz) {
    uint32_t modified = 0;
    {
        uint64_t keys[27];
        int idx = 0;
        for (int32_t dz = -1; dz <= 1; ++dz)
            for (int32_t dy = -1; dy <= 1; ++dy)
                for (int32_t dx = -1; dx <= 1; ++dx)
                    keys[idx++] = chunk_map->get_chunk_key(cx + dx, cy + dy, cz + dz);
        auto lock = chunk_map->lock_keys_exclusive(keys);
        modified = scatter_sky_light_region_locked(cx, cy, cz);
    }
    if (mesh_manager != nullptr && modified != 0) {
        for (int32_t dz = -1; dz <= 1; ++dz)
            for (int32_t dy = -1; dy <= 1; ++dy)
                for (int32_t dx = -1; dx <= 1; ++dx)
                    if ((modified & sky_slot_bit(dx, dy, dz)) != 0)
                        mesh_manager->mark_chunk_dirty_for_light(cx + dx, cy + dy, cz + dz);
    }
    return modified;
}

uint32_t LightPropagator::scatter_sky_light_region_locked(int32_t cx, int32_t cy, int32_t cz) {
    ChunkData* centre = chunk_map->get_chunk_data_fast(cx, cy, cz);
    if (!centre) return 0;

    SkyRegionGrid grid;
    grid.cx = cx;
    grid.cy = cy;
    grid.cz = cz;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                grid.chunks[dx + 1][dy + 1][dz + 1] = chunk_map->get_chunk_data_fast(cx + dx, cy + dy, cz + dz);
            }
        }
    }

    const BlockRegistry& registry = BlockRegistry::get_instance();
    sky_add_buffer.clear();

    // (1) The arriving chunk's own shade. Only a source that can BEAT a cell is
    // pushed, and only cells the scan recorded shade in are looked at -- so an open
    // chunk, an ocean and a solid chunk all cost one flag test.
    if (centre->has_sky_shade()) {
        for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    const uint8_t level = centre->get_sky_light_unsafe(x, y, z);
                    if (level == 15) continue;
                    const BlockID id = centre->get_block_unsafe(x, y, z);
                    const BlockType& type = registry.get_block(id);
                    if (!sky_is_shade_type(type)) continue;

                    const int needed = static_cast<int>(level) + 1 + type.light_opacity;
                    for (int i = 0; i < 6; ++i) {
                        int32_t nx = x + kDiamondOffsets[i][0];
                        int32_t ny = y + kDiamondOffsets[i][1];
                        int32_t nz = z + kDiamondOffsets[i][2];
                        int32_t dcx = 0, dcy = 0, dcz = 0;
                        uint8_t neighbour_level = 0;
                        if (!sky_read_level(grid, nx, ny, nz, dcx, dcy, dcz, neighbour_level)) continue;
                        if (neighbour_level <= needed) continue;
                        sky_add_buffer.push_back({cx + dcx, cy + dcy, cz + dcz,
                                                  static_cast<int16_t>(nx), static_cast<int16_t>(ny),
                                                  static_cast<int16_t>(nz), neighbour_level});
                    }
                }
            }
        }
    }

    // (2) The arriving chunk's own border cells that can improve a cell across the
    // border. (1) already covers the other direction -- a shaded border cell of the
    // arriving chunk reads its neighbour as a source -- but a bright cell of this
    // chunk lighting a neighbour's shade is a source nothing else would push, and
    // it is what a cliff face or a wall standing on a chunk line needs.
    for (int dir = 0; dir < 6; ++dir) {
        const int32_t axis = kDiamondOffsets[dir][0] != 0 ? 0 : (kDiamondOffsets[dir][1] != 0 ? 1 : 2);
        const int32_t step = kDiamondOffsets[dir][axis];  // +1 or -1
        const int32_t extent = axis == 0 ? CHUNK_WIDTH : (axis == 1 ? CHUNK_HEIGHT : CHUNK_DEPTH);
        const int32_t fixed = step > 0 ? extent - 1 : 0;
        const int32_t a_max = axis == 0 ? CHUNK_HEIGHT : CHUNK_WIDTH;
        const int32_t b_max = axis == 2 ? CHUNK_HEIGHT : CHUNK_DEPTH;

        for (int32_t a = 0; a < a_max; ++a) {
            for (int32_t b = 0; b < b_max; ++b) {
                int32_t x = 0, y = 0, z = 0;
                if (axis == 0) { x = fixed; y = a; z = b; }
                else if (axis == 1) { x = a; y = fixed; z = b; }
                else { x = a; y = b; z = fixed; }

                const uint8_t level = centre->get_sky_light_unsafe(x, y, z);
                if (level <= 1) continue;

                int32_t tx = x + kDiamondOffsets[dir][0];
                int32_t ty = y + kDiamondOffsets[dir][1];
                int32_t tz = z + kDiamondOffsets[dir][2];
                int32_t dcx = 0, dcy = 0, dcz = 0;
                uint8_t target_level = 0;
                if (!sky_read_level(grid, tx, ty, tz, dcx, dcy, dcz, target_level)) continue;
                // Cheap necessary condition before the block lookup: a step costs at
                // least one level, so a source only a level above the target cannot
                // improve it whatever the target's opacity is.
                if (level <= static_cast<uint8_t>(target_level + 1)) continue;

                BlockID target_id = BlockIDs::AIR;
                if (!sky_read_block(grid, tx, ty, tz, target_id)) continue;
                const BlockType& target_type = registry.get_block(target_id);
                if (!sky_is_shade_type(target_type)) continue;
                const int needed = static_cast<int>(target_level) + 1 + target_type.light_opacity;
                if (static_cast<int>(level) <= needed) continue;

                // Push this chunk's own cell as the source. The walk recomputes the
                // exact amount that crosses into the target.
                sky_add_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y),
                                          static_cast<int16_t>(z), level});
            }
        }
    }

    if (sky_add_buffer.empty()) return 0;

    uint32_t modified = 0;
    sky_propagate_add_locked(sky_add_buffer, cx, cy, cz, &modified);
    return modified;
}

}  // namespace VoxelEngine
