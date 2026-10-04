#ifndef FARLANDS_COLLISION_RESOLVER_INTERNAL_HPP
#define FARLANDS_COLLISION_RESOLVER_INTERNAL_HPP

// Internal header shared by the CollisionResolver translation units
// (collision_resolver.cpp, collision_resolver_turned.cpp). Not for public use.
//
// These three were file-local to collision_resolver.cpp before the split, in an
// anonymous namespace. Two translation units need them now, so they are `inline`
// in a named namespace rather than duplicated.

#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/chunk_map.hpp"
#include "core/shape_resolver.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace VoxelEngine {

// A neighbour for a shape claim, read through the map the caller has ALREADY
// locked for this query. is_aabb_solid pads its key set by one block in every
// direction, so a +-1 neighbour of any cell the loop visits is inside the locked
// shards: the resolver never reaches outside the caller's lock.
struct CollisionShapeContext {
    const ChunkMap* map;
    int32_t x;
    int32_t y;
    int32_t z;
};

inline BlockID collision_shape_neighbor(void* ctx, ShapeFace face) {
    const CollisionShapeContext& c = *static_cast<CollisionShapeContext*>(ctx);
    switch (face) {
        case ShapeFace::Top:    return static_cast<BlockID>(c.map->get_block_world_fast(c.x, c.y + 1, c.z));
        case ShapeFace::Bottom: return static_cast<BlockID>(c.map->get_block_world_fast(c.x, c.y - 1, c.z));
        case ShapeFace::Right:  return static_cast<BlockID>(c.map->get_block_world_fast(c.x + 1, c.y, c.z));
        case ShapeFace::Left:   return static_cast<BlockID>(c.map->get_block_world_fast(c.x - 1, c.y, c.z));
        case ShapeFace::Front:  return static_cast<BlockID>(c.map->get_block_world_fast(c.x, c.y, c.z + 1));
        case ShapeFace::Back:   break;
    }
    return static_cast<BlockID>(c.map->get_block_world_fast(c.x, c.y, c.z - 1));
}

// Shared-lock only the shards of the chunks intersecting a block-space box,
// instead of the whole map. Collision probes only ever touch the swept volume of
// the body asking, so the key set is tiny (usually 1-8 chunks).
inline std::vector<uint64_t> chunk_keys_for_box(const ChunkMap* chunk_map,
                                                const godot::Vector3& box_lo,
                                                const godot::Vector3& box_hi) {
    int32_t min_cx, min_cy, min_cz, max_cx, max_cy, max_cz;
    int32_t dummy;
    world_to_chunk_local(static_cast<int32_t>(std::floor(box_lo.x)),
                         static_cast<int32_t>(std::floor(box_lo.y)),
                         static_cast<int32_t>(std::floor(box_lo.z)),
                         min_cx, min_cy, min_cz, dummy, dummy, dummy);
    world_to_chunk_local(static_cast<int32_t>(std::floor(box_hi.x)),
                         static_cast<int32_t>(std::floor(box_hi.y)),
                         static_cast<int32_t>(std::floor(box_hi.z)),
                         max_cx, max_cy, max_cz, dummy, dummy, dummy);
    std::vector<uint64_t> keys;
    keys.reserve(static_cast<size_t>(max_cx - min_cx + 1) *
                 static_cast<size_t>(max_cy - min_cy + 1) *
                 static_cast<size_t>(max_cz - min_cz + 1));
    for (int32_t cx = min_cx; cx <= max_cx; ++cx)
        for (int32_t cy = min_cy; cy <= max_cy; ++cy)
            for (int32_t cz = min_cz; cz <= max_cz; ++cz)
                keys.push_back(chunk_map->get_chunk_key(cx, cy, cz));
    return keys;
}

} // namespace VoxelEngine

#endif // FARLANDS_COLLISION_RESOLVER_INTERNAL_HPP