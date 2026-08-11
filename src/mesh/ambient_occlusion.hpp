#ifndef FUK_MINECRAFT_AMBIENT_OCCLUSION_HPP
#define FUK_MINECRAFT_AMBIENT_OCCLUSION_HPP

#include "core/block_types.hpp"
#include "mesh/chunk_neighbor_accessor.hpp"
#include "mesh/mesh_types.hpp"

namespace VoxelEngine {

// Forward declaration — defined in mesh_builder.hpp
struct Face;

/**
 * Dedicated ambient occlusion calculator for voxel chunk meshes.
 *
 * Computes per-vertex AO for block faces using the standard Minecraft-style
 * 3-sample occlusion test (two side blocks + one diagonal corner).  The
 * occlusion curve and face-direction shading are configurable and live in one
 * place instead of being scattered through MeshBuilder.
 */
class AmbientOcclusion {
public:
    // -----------------------------------------------------------------
    // Face-direction shade baked into the final AO value
    // -----------------------------------------------------------------
    static float get_face_shade(FaceDirection direction);

    // -----------------------------------------------------------------
    // Does this block type occlude ambient light?
    // -----------------------------------------------------------------
    static bool is_occluding(BlockID block_id, const BlockRegistry& registry);

    // -----------------------------------------------------------------
    // Per-block-face AO (4 vertices)
    // -----------------------------------------------------------------
    void compute_face(const ChunkNeighborAccessor& accessor,
                      const BlockRegistry& registry,
                      int32_t x, int32_t y, int32_t z,
                      FaceDirection direction,
                      float ao_out[4],
                      int32_t stride = 1) const;

    // -----------------------------------------------------------------
    // Greedy-merged face AO
    //
    // Because the greedy meshing code already verifies that every block
    // in the merged region has identical AO values, we simply sample the
    // first block and use its AO for all four corners of the merged face.
    // -----------------------------------------------------------------
    void compute_greedy_face(const ChunkNeighborAccessor& accessor,
                             const BlockRegistry& registry,
                             const Face& face,
                             float ao_out[4],
                             int32_t stride = 1) const;

    // -----------------------------------------------------------------
    // Combined helper: AO * face_shade * 255  (ready for Vertex::ao)
    // -----------------------------------------------------------------
    static uint8_t pack_vertex_ao(float ao, FaceDirection direction);

private:
    // Standard Minecraft-style AO: returns 0..3
    static int compute_level(bool side1, bool side2, bool corner);

    // Convert occlusion level to brightness multiplier
    static float level_to_brightness(int level);

    // Per-corner AO level from the 3 neighbouring blocks
    static int vertex_level(const ChunkNeighborAccessor& accessor,
                            const BlockRegistry& registry,
                            int32_t s1x, int32_t s1y, int32_t s1z,
                            int32_t s2x, int32_t s2y, int32_t s2z,
                            int32_t cx,  int32_t cy,  int32_t cz);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_AMBIENT_OCCLUSION_HPP
