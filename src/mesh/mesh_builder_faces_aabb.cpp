// The AABB-face half of face emission: the corner table for a box face, and the
// emitter the partial-remesh path and the shape resolver's owned boxes both come
// through. Whole-block and greedy emission stay in mesh/mesh_builder_faces.cpp.

#include "mesh/mesh_builder.hpp"
#include "mesh/mesh_fluid.hpp"
#include "mesh/smooth_lighting.hpp"

#include <cmath>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// AABB face vertex positioning helper
// -------------------------------------------------------------------------
namespace {
void compute_aabb_face_corners(float corners[4][3], int32_t x, int32_t y, int32_t z,
                               FaceDirection dir, const float aabb_min[3], const float aabb_max[3]) {
    switch (dir) {
        case FaceDirection::Top:
            corners[0][0] = x + aabb_min[0]; corners[0][1] = y + aabb_max[1]; corners[0][2] = z + aabb_min[2];
            corners[1][0] = x + aabb_max[0]; corners[1][1] = y + aabb_max[1]; corners[1][2] = z + aabb_min[2];
            corners[2][0] = x + aabb_max[0]; corners[2][1] = y + aabb_max[1]; corners[2][2] = z + aabb_max[2];
            corners[3][0] = x + aabb_min[0]; corners[3][1] = y + aabb_max[1]; corners[3][2] = z + aabb_max[2];
            break;
        case FaceDirection::Bottom:
            corners[0][0] = x + aabb_min[0]; corners[0][1] = y + aabb_min[1]; corners[0][2] = z + aabb_max[2];
            corners[1][0] = x + aabb_max[0]; corners[1][1] = y + aabb_min[1]; corners[1][2] = z + aabb_max[2];
            corners[2][0] = x + aabb_max[0]; corners[2][1] = y + aabb_min[1]; corners[2][2] = z + aabb_min[2];
            corners[3][0] = x + aabb_min[0]; corners[3][1] = y + aabb_min[1]; corners[3][2] = z + aabb_min[2];
            break;
        case FaceDirection::Right:
            corners[0][0] = x + aabb_max[0]; corners[0][1] = y + aabb_min[1]; corners[0][2] = z + aabb_min[2];
            corners[1][0] = x + aabb_max[0]; corners[1][1] = y + aabb_min[1]; corners[1][2] = z + aabb_max[2];
            corners[2][0] = x + aabb_max[0]; corners[2][1] = y + aabb_max[1]; corners[2][2] = z + aabb_max[2];
            corners[3][0] = x + aabb_max[0]; corners[3][1] = y + aabb_max[1]; corners[3][2] = z + aabb_min[2];
            break;
        case FaceDirection::Left:
            corners[0][0] = x + aabb_min[0]; corners[0][1] = y + aabb_min[1]; corners[0][2] = z + aabb_max[2];
            corners[1][0] = x + aabb_min[0]; corners[1][1] = y + aabb_min[1]; corners[1][2] = z + aabb_min[2];
            corners[2][0] = x + aabb_min[0]; corners[2][1] = y + aabb_max[1]; corners[2][2] = z + aabb_min[2];
            corners[3][0] = x + aabb_min[0]; corners[3][1] = y + aabb_max[1]; corners[3][2] = z + aabb_max[2];
            break;
        case FaceDirection::Front:
            corners[0][0] = x + aabb_max[0]; corners[0][1] = y + aabb_min[1]; corners[0][2] = z + aabb_max[2];
            corners[1][0] = x + aabb_min[0]; corners[1][1] = y + aabb_min[1]; corners[1][2] = z + aabb_max[2];
            corners[2][0] = x + aabb_min[0]; corners[2][1] = y + aabb_max[1]; corners[2][2] = z + aabb_max[2];
            corners[3][0] = x + aabb_max[0]; corners[3][1] = y + aabb_max[1]; corners[3][2] = z + aabb_max[2];
            break;
        case FaceDirection::Back:
            corners[0][0] = x + aabb_min[0]; corners[0][1] = y + aabb_min[1]; corners[0][2] = z + aabb_min[2];
            corners[1][0] = x + aabb_max[0]; corners[1][1] = y + aabb_min[1]; corners[1][2] = z + aabb_min[2];
            corners[2][0] = x + aabb_max[0]; corners[2][1] = y + aabb_max[1]; corners[2][2] = z + aabb_min[2];
            corners[3][0] = x + aabb_min[0]; corners[3][1] = y + aabb_max[1]; corners[3][2] = z + aabb_min[2];
            break;
    }
}
} // namespace

// -------------------------------------------------------------------------
// AABB face emission
// -------------------------------------------------------------------------
void MeshBuilder::add_aabb_face(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                int32_t x, int32_t y, int32_t z,
                                FaceDirection direction, BlockID block_id, const BlockRegistry& registry,
                                const float aabb_min[3], const float aabb_max[3]) {
    // Every liquid family, not just the two natural-water ids: at LOD stride > 1
    // this path is what draws liquids (the fluid pass is full-detail only), so a
    // runoff or falling water cell that was left out of this test ended up in the
    // OPAQUE buffer and rendered with the terrain material — grey blocks where the
    // water is.
    const bool is_water = is_fluid_drawn(block_id, registry);
    auto& dest_vertices = is_water ? water_vertices : vertices;
    auto& dest_indices = is_water ? water_indices : indices;
    uint32_t vertex_count = static_cast<uint32_t>(dest_vertices.size());
    int dir_index = static_cast<int>(direction);

    const BlockType& block_type = registry.get_block(block_id);
    int texture_idx = 0;
    int emissive_idx = 0;

    switch (direction) {
        case FaceDirection::Right:  texture_idx = block_type.texture_indices[0]; emissive_idx = block_type.emissive_texture_indices[0]; break;
        case FaceDirection::Left:   texture_idx = block_type.texture_indices[1]; emissive_idx = block_type.emissive_texture_indices[1]; break;
        case FaceDirection::Top:    texture_idx = block_type.texture_indices[2]; emissive_idx = block_type.emissive_texture_indices[2]; break;
        case FaceDirection::Bottom: texture_idx = block_type.texture_indices[3]; emissive_idx = block_type.emissive_texture_indices[3]; break;
        case FaceDirection::Front:  texture_idx = block_type.texture_indices[4]; emissive_idx = block_type.emissive_texture_indices[4]; break;
        case FaceDirection::Back:   texture_idx = block_type.texture_indices[5]; emissive_idx = block_type.emissive_texture_indices[5]; break;
    }

    float ao[4];
    if (!HasProperty(block_type.properties, BlockProperty::Liquid)) {
        // Pass whether the AABB touches the floor/ceiling to fix AO on stairs
        // Bottom step touches floor but not ceiling, top step touches neither
        bool touches_floor = (aabb_min[1] <= 0.0f);
        bool touches_ceiling = (aabb_max[1] >= 1.0f);
        this->ao.compute_face(accessor, registry, x, y, z, direction, ao, stride_xz_, touches_floor, touches_ceiling);
    } else {
        ao[0] = ao[1] = ao[2] = ao[3] = 1.0f;
    }

    uint16_t light_key;
    if (is_side_face(direction)) {
        const int32_t dir_idx = static_cast<int32_t>(direction);
        const int32_t dx = kDirectionOffsets[dir_idx][0] * stride_xz_;
        const int32_t dy = kDirectionOffsets[dir_idx][1];
        const int32_t dz = kDirectionOffsets[dir_idx][2] * stride_xz_;
        light_key = accessor.get_light_packed(x + dx, y + dy, z + dz);
    } else {
        const int32_t dir_idx = static_cast<int32_t>(direction);
        const int32_t dx = kDirectionOffsets[dir_idx][0];
        const int32_t dy = kDirectionOffsets[dir_idx][1];
        const int32_t dz = kDirectionOffsets[dir_idx][2];
        light_key = accessor.get_light_packed(x + dx, y + dy, z + dz);
    }

    uint16_t light_keys[4];
    if (smooth_lighting_enabled) {
        compute_smooth_light(accessor, registry, x, y, z, direction, light_keys, stride_xz_);
    } else {
        light_keys[0] = light_keys[1] = light_keys[2] = light_keys[3] = light_key;
    }

    bool flip = (ao[0] + ao[2]) < (ao[1] + ao[3]);

    float corners[4][3];
    compute_aabb_face_corners(corners, x, y, z, direction, aabb_min, aabb_max);

    // Compute UV scaling based on AABB dimensions (for partial blocks like slabs/stairs)
    float uv_scale_u, uv_scale_v, uv_offset_u, uv_offset_v;

    // Adjust UV scale/offset based on face direction
    // For side faces, V coordinate should map from top of block (y=1) downward
    bool invert_v = false;
    bool invert_u = false;
    switch (direction) {
        case FaceDirection::Top:
            uv_scale_u = aabb_max[0] - aabb_min[0];
            uv_scale_v = aabb_max[2] - aabb_min[2];
            uv_offset_u = aabb_min[0];
            uv_offset_v = aabb_min[2];
            break;
        case FaceDirection::Bottom:
            uv_scale_u = aabb_max[0] - aabb_min[0];
            uv_scale_v = aabb_max[2] - aabb_min[2];
            uv_offset_u = aabb_min[0];
            uv_offset_v = aabb_min[2];
            invert_v = true; // Invert V for bottom face so texture maps correctly
            break;
        case FaceDirection::Right:
            uv_scale_u = aabb_max[2] - aabb_min[2];
            uv_scale_v = aabb_max[1] - aabb_min[1];
            uv_offset_u = aabb_min[2];
            // For side faces, map V from top of block (y=1) down to the AABB top
            uv_offset_v = 1.0f - aabb_max[1];
            break;
        case FaceDirection::Left:
            uv_scale_u = aabb_max[2] - aabb_min[2];
            uv_scale_v = aabb_max[1] - aabb_min[1];
            uv_offset_u = aabb_min[2];
            // For side faces, map V from top of block (y=1) down to the AABB top
            uv_offset_v = 1.0f - aabb_max[1];
            invert_u = true; // Invert U for left face to fix horizontal mirroring on stair L side
            break;
        case FaceDirection::Front:
        case FaceDirection::Back:
            uv_scale_u = aabb_max[0] - aabb_min[0];
            uv_scale_v = aabb_max[1] - aabb_min[1];
            uv_offset_u = aabb_min[0];
            // For side faces, map V from top of block (y=1) down to the AABB top
            uv_offset_v = 1.0f - aabb_max[1];
            break;
    }

    for (int i = 0; i < 4; i++) {
        Vertex v;
        v.x = static_cast<uint16_t>(std::lroundf(corners[i][0] * 256.0f));
        v.z = static_cast<uint16_t>(std::lroundf(corners[i][2] * 256.0f));
        v.y = static_cast<uint16_t>(std::lroundf(corners[i][1] * 256.0f));
        v.nx = static_cast<int8_t>(kFaceNormals[dir_index][0] * 127.0f);
        v.ny = static_cast<int8_t>(kFaceNormals[dir_index][1] * 127.0f);
        v.nz = static_cast<int8_t>(kFaceNormals[dir_index][2] * 127.0f);
        // Scale UVs based on AABB dimensions so partial blocks sample correct texture region
        float base_u = kFaceUVs[i][0] * uv_scale_u + uv_offset_u;
        v.u = invert_u ? (1.0f - base_u) : base_u;
        float base_v = kFaceUVs[i][1] * uv_scale_v + uv_offset_v;
        v.v = invert_v ? (1.0f - base_v) : base_v;
        v.texture_index = static_cast<uint16_t>(texture_idx);
        // For liquid faces the AO byte is the water shader's texture-flow
        // channel, not occlusion (it can't darken a sloped surface anyway):
        // far LOD boxes read as still water, so flow 0. For solids it stays
        // the packed AO the terrain shader expects.
        v.ao = is_water
            ? mesh_fluid::pack_flow(0, 0.0f)
            : AmbientOcclusion::pack_vertex_ao(ao[i], direction);
        // The water shader reads the emissive channel as the fluid kind
        // (Water=1, Lava=2, Acid=3) to pick per-substance alpha; for solids
        // the water shader never sees it, so this only matters when the LOD
        // box path draws a liquid here.
        v.emissive_index = static_cast<uint8_t>(is_water
            ? static_cast<int>(block_type.fluid_kind) : emissive_idx);
        v.light_r = static_cast<uint8_t>(kBlockBrightness[unpack_r(light_keys[i])] * 255.0f);
        v.light_g = static_cast<uint8_t>(kBlockBrightness[unpack_g(light_keys[i])] * 255.0f);
        v.light_b = static_cast<uint8_t>(kBlockBrightness[unpack_b(light_keys[i])] * 255.0f);
        v.sky_light = static_cast<uint8_t>(kBlockBrightness[unpack_sky(light_keys[i])] * 255.0f);
        dest_vertices.push_back(v);
    }

    if (!flip) {
        dest_indices.push_back(vertex_count + 0);
        dest_indices.push_back(vertex_count + 1);
        dest_indices.push_back(vertex_count + 2);
        dest_indices.push_back(vertex_count + 0);
        dest_indices.push_back(vertex_count + 2);
        dest_indices.push_back(vertex_count + 3);
    } else {
        dest_indices.push_back(vertex_count + 1);
        dest_indices.push_back(vertex_count + 2);
        dest_indices.push_back(vertex_count + 3);
        dest_indices.push_back(vertex_count + 1);
        dest_indices.push_back(vertex_count + 3);
        dest_indices.push_back(vertex_count + 0);
    }

    if (record_quads_) {
        CachedQuad q;
        q.x = x;
        q.y = y;
        q.z = z;
        q.direction = direction;
        q.water = is_water;
        q.verts = { dest_vertices[vertex_count + 0], dest_vertices[vertex_count + 1],
                    dest_vertices[vertex_count + 2], dest_vertices[vertex_count + 3] };
        if (!flip) {
            q.idx = {0, 1, 2, 0, 2, 3};
        } else {
            q.idx = {1, 2, 3, 1, 3, 0};
        }
        quads.push_back(q);
    }
}

} // namespace VoxelEngine
