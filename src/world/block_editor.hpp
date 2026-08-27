#ifndef FARLANDS_BLOCK_EDITOR_HPP
#define FARLANDS_BLOCK_EDITOR_HPP
#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>
#include <cmath>

#include "core/chunk_types.hpp"

namespace VoxelEngine {

class ChunkWorld;
class MeshManager;
class LightPropagator;

struct RaycastResult {
    bool success = false;
    godot::Vector3 position;
    godot::Vector3 place_position;
    int block_id = 0;
    godot::Vector3 hit_normal;
    godot::Vector3 hit_point;
};

class BlockEditor {
public:
    BlockEditor(ChunkWorld* cw, MeshManager* mm, LightPropagator* lp);

    void place_block(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    int query_block(int32_t world_x, int32_t world_y, int32_t world_z) const;

    RaycastResult raycast_from_ray(const godot::Vector3& origin,
                                    const godot::Vector3& direction,
                                    double max_distance) const;

private:
    ChunkWorld* chunk_world;
    MeshManager* mesh_manager;
    LightPropagator* light_propagator;

    void set_block_variant(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    void update_mud_variants(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block);
    void post_block_change(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block);

    bool is_local_in_bounds(int32_t local_x, int32_t local_y, int32_t local_z) const {
        return local_x >= 0 && local_x < CHUNK_WIDTH &&
               local_y >= 0 && local_y < CHUNK_HEIGHT &&
               local_z >= 0 && local_z < CHUNK_DEPTH;
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_EDITOR_HPP