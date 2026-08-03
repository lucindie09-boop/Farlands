#include "mesh/mesh_builder.hpp"
#include "mesh/smooth_lighting.hpp"

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Face emission
// -------------------------------------------------------------------------
void MeshBuilder::add_face(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                           int32_t x, int32_t y, int32_t z,
                           FaceDirection direction, BlockID block_id, const BlockRegistry& registry) {
    const bool is_water = block_id == BlockIDs::SURFACE_WATER || block_id == BlockIDs::WATER;
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
        this->ao.compute_face(accessor, x, y, z, direction, ao, stride_xz_);
    } else {
        ao[0] = ao[1] = ao[2] = ao[3] = 1.0f;
    }

    uint16_t light_key;
    float side_lowered_offset = 0.0f;
    if (is_side_face(direction)) {
        int32_t nx = x + kDirectionOffsets[dir_index][0] * stride_xz_;
        int32_t ny = y + kDirectionOffsets[dir_index][1];
        int32_t nz = z + kDirectionOffsets[dir_index][2] * stride_xz_;
        bool use_self_light = false;
        if (ny >= 0 && ny < CHUNK_HEIGHT) {
            BlockID neighbor_id = BlockIDs::AIR;
            if (nx >= 0 && nx < CHUNK_WIDTH && nz >= 0 && nz < CHUNK_DEPTH) {
                neighbor_id = chunk.get_block_unsafe(nx, ny, nz);
                use_self_light = registry.get_block(neighbor_id).top_face_offset > 0.0f;
            } else {
                neighbor_id = accessor.get_block(nx, ny, nz);
                use_self_light = registry.get_block(neighbor_id).top_face_offset > 0.0f;
            }
            // Check the block below (y-1) for geometry offset, NOT the horizontal neighbor.
            // This handles the case where a solid block sits above water:
            // the water's lowered top face means the solid block's side face
            // should extend downward to fill the gap.
            // The horizontal neighbor check above is for lighting only (use_self_light).
            int32_t by = y - 1;
            if (by >= 0) {
                BlockID below_id = chunk.get_block_unsafe(x, by, z);
                if (registry.get_block(below_id).top_face_offset > 0.0f) {
                    side_lowered_offset = registry.get_block(below_id).top_face_offset;
                }
            }
        }
        if (use_self_light) {
            light_key = 0;
            for (int32_t sy = y + 1; sy < CHUNK_HEIGHT; sy++) {
                uint16_t light = accessor.get_light_packed(x, sy, z);
                if (unpack_sky(light) > 0) {
                    light_key = light;
                    break;
                }
            }
            if (light_key == 0) {
                light_key = accessor.get_light_packed(x, y, z);
            }
        } else {
            const int32_t dir_idx = static_cast<int32_t>(direction);
            const int32_t dx = kDirectionOffsets[dir_idx][0] * stride_xz_;
            const int32_t dy = kDirectionOffsets[dir_idx][1];
            const int32_t dz = kDirectionOffsets[dir_idx][2] * stride_xz_;
            light_key = accessor.get_light_packed(x + dx, y + dy, z + dz);
        }
    } else {
        const int32_t dir_idx = static_cast<int32_t>(direction);
        const int32_t dx = kDirectionOffsets[dir_idx][0];
        const int32_t dy = kDirectionOffsets[dir_idx][1];
        const int32_t dz = kDirectionOffsets[dir_idx][2];
        light_key = accessor.get_light_packed(x + dx, y + dy, z + dz);
    }
uint16_t light_keys[4];
if (smooth_lighting_enabled && side_lowered_offset == 0.0f) {
compute_smooth_light(accessor, x, y, z, direction, light_keys, stride_xz_);
} else {
light_keys[0] = light_keys[1] = light_keys[2] = light_keys[3] = light_key;
}

    bool flip = (ao[0] + ao[2]) < (ao[1] + ao[3]);

    int surface_rotation = get_face_rotation(block_id, x, y, z, direction, dir_index);

    float corners[4][3];
    for (int i = 0; i < 4; i++) {
        corners[i][0] = x + kFaceVertices[dir_index][i][0] * stride_xz_;
        corners[i][1] = y + kFaceVertices[dir_index][i][1];
        corners[i][2] = z + kFaceVertices[dir_index][i][2] * stride_xz_;
    }
    apply_special_block_offsets(corners, block_id, direction);

    if (side_lowered_offset > 0.0f) {
        float top_y = corners[2][1];
        float new_bottom_y = top_y - side_lowered_offset;
        corners[0][1] = new_bottom_y;
        corners[1][1] = new_bottom_y;
    }

    for (int i = 0; i < 4; i++) {
        Vertex v;
        v.x = corners[i][0];
        v.y = corners[i][1];
        v.z = corners[i][2];
        v.nx = static_cast<int8_t>(kFaceNormals[dir_index][0] * 127.0f);
        v.ny = static_cast<int8_t>(kFaceNormals[dir_index][1] * 127.0f);
        v.nz = static_cast<int8_t>(kFaceNormals[dir_index][2] * 127.0f);
        v.u = kFaceUVs[i][0];
        v.v = kFaceUVs[i][1];
        if (side_lowered_offset > 0.0f && i < 2 && surface_rotation == 0) {
            v.v = side_lowered_offset;
        }
        if (surface_rotation != 0) {
            apply_uv_rotation(v.u, v.v, surface_rotation);
        }
        v.texture_index = static_cast<uint16_t>(texture_idx);
        v.ao = AmbientOcclusion::pack_vertex_ao(ao[i], direction);
        v.emissive_index = static_cast<uint8_t>(emissive_idx);
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

void MeshBuilder::add_greedy_face(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                  const Face& face, uint16_t face_light_key, int rotation, const float ao[4], const BlockRegistry& registry) {
    const bool is_water = face.block_id == BlockIDs::SURFACE_WATER || face.block_id == BlockIDs::WATER;
    auto& dest_vertices = is_water ? water_vertices : vertices;
    auto& dest_indices = is_water ? water_indices : indices;
    uint32_t vertex_count = static_cast<uint32_t>(dest_vertices.size());
    int dir_index = static_cast<int>(face.direction);

    int32_t u_size = face.u_max + 1;
    int32_t v_size = face.v_max + 1;

    const BlockType& block_type = registry.get_block(face.block_id);
    int texture_idx = 0;
    int emissive_idx = 0;
    switch (face.direction) {
        case FaceDirection::Right:  texture_idx = block_type.texture_indices[0]; emissive_idx = block_type.emissive_texture_indices[0]; break;
        case FaceDirection::Left:   texture_idx = block_type.texture_indices[1]; emissive_idx = block_type.emissive_texture_indices[1]; break;
        case FaceDirection::Top:    texture_idx = block_type.texture_indices[2]; emissive_idx = block_type.emissive_texture_indices[2]; break;
        case FaceDirection::Bottom: texture_idx = block_type.texture_indices[3]; emissive_idx = block_type.emissive_texture_indices[3]; break;
        case FaceDirection::Front:  texture_idx = block_type.texture_indices[4]; emissive_idx = block_type.emissive_texture_indices[4]; break;
        case FaceDirection::Back:   texture_idx = block_type.texture_indices[5]; emissive_idx = block_type.emissive_texture_indices[5]; break;
    }

bool flip = (ao[0] + ao[2]) < (ao[1] + ao[3]);

    float corners[4][3];
    switch (face.direction) {
        case FaceDirection::Top:
            corners[0][0] = face.x;             corners[0][1] = face.y + 1; corners[0][2] = face.z;
            corners[1][0] = face.x + u_size;    corners[1][1] = face.y + 1; corners[1][2] = face.z;
            corners[2][0] = face.x + u_size;    corners[2][1] = face.y + 1; corners[2][2] = face.z + v_size;
            corners[3][0] = face.x;             corners[3][1] = face.y + 1; corners[3][2] = face.z + v_size;
            break;
        case FaceDirection::Bottom:
            corners[0][0] = face.x;             corners[0][1] = face.y;     corners[0][2] = face.z + v_size;
            corners[1][0] = face.x + u_size;    corners[1][1] = face.y;     corners[1][2] = face.z + v_size;
            corners[2][0] = face.x + u_size;    corners[2][1] = face.y;     corners[2][2] = face.z;
            corners[3][0] = face.x;             corners[3][1] = face.y;     corners[3][2] = face.z;
            break;
        case FaceDirection::Right:
            corners[0][0] = face.x + stride_xz_; corners[0][1] = face.y;             corners[0][2] = face.z;
            corners[1][0] = face.x + stride_xz_; corners[1][1] = face.y;             corners[1][2] = face.z + u_size;
            corners[2][0] = face.x + stride_xz_; corners[2][1] = face.y + v_size;    corners[2][2] = face.z + u_size;
            corners[3][0] = face.x + stride_xz_; corners[3][1] = face.y + v_size;    corners[3][2] = face.z;
            break;
        case FaceDirection::Left:
            corners[0][0] = face.x;             corners[0][1] = face.y;             corners[0][2] = face.z + u_size;
            corners[1][0] = face.x;             corners[1][1] = face.y;             corners[1][2] = face.z;
            corners[2][0] = face.x;             corners[2][1] = face.y + v_size;    corners[2][2] = face.z;
            corners[3][0] = face.x;             corners[3][1] = face.y + v_size;    corners[3][2] = face.z + u_size;
            break;
        case FaceDirection::Front:
            corners[0][0] = face.x + u_size;    corners[0][1] = face.y;             corners[0][2] = face.z + stride_xz_;
            corners[1][0] = face.x;             corners[1][1] = face.y;             corners[1][2] = face.z + stride_xz_;
            corners[2][0] = face.x;             corners[2][1] = face.y + v_size;    corners[2][2] = face.z + stride_xz_;
            corners[3][0] = face.x + u_size;    corners[3][1] = face.y + v_size;    corners[3][2] = face.z + stride_xz_;
            break;
        case FaceDirection::Back:
            corners[0][0] = face.x;             corners[0][1] = face.y;             corners[0][2] = face.z;
            corners[1][0] = face.x + u_size;    corners[1][1] = face.y;             corners[1][2] = face.z;
            corners[2][0] = face.x + u_size;    corners[2][1] = face.y + v_size;    corners[2][2] = face.z;
            corners[3][0] = face.x;             corners[3][1] = face.y + v_size;    corners[3][2] = face.z;
            break;
    }

    // Per-corner light so merged faces keep smooth gradients at light
    // boundaries (shadow/overhang edges) instead of a flat value for the whole
    // merged face. Sampling happens at the true grid corners, matching
    // adjacent per-block faces at shared corners.
    uint16_t corner_light[4];
    if (smooth_lighting_enabled) {
        for (int i = 0; i < 4; i++) {
            corner_light[i] = compute_corner_light(accessor,
                static_cast<int32_t>(corners[i][0]),
                static_cast<int32_t>(corners[i][1]),
                static_cast<int32_t>(corners[i][2]),
                face.direction);
        }
    } else {
        corner_light[0] = corner_light[1] = corner_light[2] = corner_light[3] = face_light_key;
    }

    apply_special_block_offsets(corners, face.block_id, face.direction);

    for (int i = 0; i < 4; i++) {
        Vertex v;
        v.x = corners[i][0];
        v.y = corners[i][1];
        v.z = corners[i][2];
        v.nx = static_cast<int8_t>(kFaceNormals[dir_index][0] * 127.0f);
        v.ny = static_cast<int8_t>(kFaceNormals[dir_index][1] * 127.0f);
        v.nz = static_cast<int8_t>(kFaceNormals[dir_index][2] * 127.0f);
        {
            float u = kFaceUVs[i][0];
            float vt = kFaceUVs[i][1];
            if (rotation != 0) {
                apply_uv_rotation(u, vt, rotation);
            }
            v.u = u * u_size;
            v.v = vt * v_size;
        }
        v.texture_index = static_cast<uint16_t>(texture_idx);
        v.ao = AmbientOcclusion::pack_vertex_ao(ao[i], face.direction);
        v.emissive_index = static_cast<uint8_t>(emissive_idx);
        v.light_r = static_cast<uint8_t>(kBlockBrightness[unpack_r(corner_light[i])] * 255.0f);
        v.light_g = static_cast<uint8_t>(kBlockBrightness[unpack_g(corner_light[i])] * 255.0f);
        v.light_b = static_cast<uint8_t>(kBlockBrightness[unpack_b(corner_light[i])] * 255.0f);
        v.sky_light = static_cast<uint8_t>(kBlockBrightness[unpack_sky(corner_light[i])] * 255.0f);
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
        q.x = face.x;
        q.y = face.y;
        q.z = face.z;
        q.direction = face.direction;
        q.water = is_water;
        q.verts = { dest_vertices[vertex_count + 0], dest_vertices[vertex_count + 1],
                    dest_vertices[vertex_count + 2], dest_vertices[vertex_count + 3] };
        if (!flip) {
            q.idx = {0, 1, 2, 0, 2, 3};
        } else {
            q.idx = {1, 2, 3, 1, 3, 0};
        }
        // Footprint extents (partial rebuilds only run at stride 1).
        const int32_t u_size = face.u_max + 1;
        const int32_t v_size = face.v_max + 1;
        switch (face.direction) {
            case FaceDirection::Top:
            case FaceDirection::Bottom:
                q.ex = u_size;  // x-run
                q.ey = 1;
                q.ez = v_size;  // z-run
                break;
            case FaceDirection::Right:
            case FaceDirection::Left:
                q.ex = 1;
                q.ey = v_size;  // y-run
                q.ez = u_size;  // z-run
                break;
            default:  // Front / Back
                q.ex = u_size;  // x-run
                q.ey = v_size;  // y-run
                q.ez = 1;
                break;
        }
        quads.push_back(q);
    }
}

// -------------------------------------------------------------------------
// Side face emission helper
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// Special block offsets
// -------------------------------------------------------------------------
void MeshBuilder::apply_special_block_offsets(float corners[4][3], BlockID block_id, FaceDirection dir) {
    const BlockType& block_type = BlockRegistry::get_instance().get_block(block_id);
    float offset = block_type.top_face_offset;
    if (offset <= 0.0f || dir == FaceDirection::Bottom) return;

    if (dir == FaceDirection::Top) {
        for (int i = 0; i < 4; i++) {
            corners[i][1] -= offset;
        }
    } else {
        float top_y = corners[0][1];
        for (int i = 1; i < 4; i++) {
            top_y = std::max(top_y, corners[i][1]);
        }
        for (int i = 0; i < 4; i++) {
            if (corners[i][1] > top_y - 0.001f) {
                corners[i][1] -= offset;
            }
        }
    }
}

} // namespace VoxelEngine
