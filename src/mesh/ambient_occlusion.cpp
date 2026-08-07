#include "mesh/ambient_occlusion.hpp"
#include "mesh/mesh_builder.hpp"  // for Face struct definition

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Face-direction shading (directional brightness baked into AO)
// -------------------------------------------------------------------------
// NOLINTBEGIN(bugprone-branch-clone) — Left/Right and Front/Back intentionally share shades
float AmbientOcclusion::get_face_shade(FaceDirection direction) {
    switch (direction) {
        case FaceDirection::Top:    return 1.00f;
        case FaceDirection::Bottom: return 0.50f;
        case FaceDirection::Right:  return 0.60f;
        case FaceDirection::Left:   return 0.60f;
        case FaceDirection::Front:  return 0.80f;
        case FaceDirection::Back:   return 0.80f;
    }
    return 1.0f;
}
// NOLINTEND(bugprone-branch-clone)

// -------------------------------------------------------------------------
// Occlusion predicate — a block occludes ambient light if it is solid
// or opaque.  Air and purely transparent blocks (glass, water) do not.
// -------------------------------------------------------------------------
bool AmbientOcclusion::is_occluding(BlockID block_id) {
    if (block_id == BlockIDs::AIR) return false;
    const BlockType& type = BlockRegistry::get_instance().get_block(block_id);
    return HasProperty(type.properties, BlockProperty::Opaque) ||
           HasProperty(type.properties, BlockProperty::Solid);
}

// -------------------------------------------------------------------------
// Minecraft-style vertex AO: count how many of the three adjacent
// blocks are occluding.  If both sides are occluded the corner is
// ignored (light can't squeeze through anyway).
// -------------------------------------------------------------------------
int AmbientOcclusion::compute_level(bool side1, bool side2, bool corner) {
    if (side1 && side2) return 3;
    return static_cast<int>(side1) + static_cast<int>(side2) + static_cast<int>(corner);
}

// -------------------------------------------------------------------------
// Brightness curve for AO levels.
// 0 neighbours occluded -> 1.00  (bright)
// 1 neighbour occluded  -> 0.80
// 2 neighbours occluded -> 0.55
// 3 neighbours occluded -> 0.30  (darkest)
//
// Strengthened from the previous {1.0, 0.85, 0.65, 0.45} table after smooth
// lighting stopped leaking dark light into occluded corners — corner shading
// now comes from AO alone, so the curve needs more contrast to stay visible.
// -------------------------------------------------------------------------
float AmbientOcclusion::level_to_brightness(int level) {
    static constexpr float table[4] = {1.0f, 0.80f, 0.55f, 0.30f};
    return table[level < 4 ? level : 3];
}

int AmbientOcclusion::vertex_level(const ChunkNeighborAccessor& accessor,
                                   int32_t s1x, int32_t s1y, int32_t s1z,
                                   int32_t s2x, int32_t s2y, int32_t s2z,
                                   int32_t cx,  int32_t cy,  int32_t cz) {
    bool side1  = is_occluding(accessor.get_block(s1x, s1y, s1z));
    bool side2  = is_occluding(accessor.get_block(s2x, s2y, s2z));
    bool corner = is_occluding(accessor.get_block(cx,  cy,  cz));
    return compute_level(side1, side2, corner);
}

// -------------------------------------------------------------------------
// Per-block-face AO
// -------------------------------------------------------------------------
void AmbientOcclusion::compute_face(const ChunkNeighborAccessor& accessor,
                                    int32_t x, int32_t y, int32_t z,
                                    FaceDirection direction,
                                    float ao_out[4],
                                    int32_t stride) const {
    BlockID current_block = accessor.center->get_block(x, y, z);
    const BlockType& current_type = BlockRegistry::get_instance().get_block(current_block);
    bool is_lowered = current_type.top_face_offset > 0.0f;

    switch (direction) {
        case FaceDirection::Top: {
            const int dx[4] = {-stride,  stride,  stride, -stride};
            const int dz[4] = {-stride, -stride,  stride,  stride};
            for (int i = 0; i < 4; i++) {
                ao_out[i] = level_to_brightness(vertex_level(
                    accessor,
                    x + dx[i], y + 1, z,
                    x,         y + 1, z + dz[i],
                    x + dx[i], y + 1, z + dz[i]));
            }
            break;
        }
        case FaceDirection::Bottom: {
            const int dx[4] = {-stride,  stride,  stride, -stride};
            const int dz[4] = {-stride, -stride,  stride,  stride};
            for (int i = 0; i < 4; i++) {
                ao_out[i] = level_to_brightness(vertex_level(
                    accessor,
                    x + dx[i], y - 1, z,
                    x,         y - 1, z + dz[i],
                    x + dx[i], y - 1, z + dz[i]));
            }
            break;
        }
        case FaceDirection::Right: {
            const int dy[4] = {-stride, -stride,  stride,  stride};
            const int dz[4] = {-stride,  stride,  stride, -stride};
            for (int i = 0; i < 4; i++) {
                if (is_lowered && dy[i] > 0) {
                    int ao_y   = vertex_level(accessor, x + 1, y + dy[i],     z,   x + 1, y,         z + dz[i], x + 1, y + dy[i],     z + dz[i]);
                    int ao_yp1 = vertex_level(accessor, x + 1, y + dy[i] + 1, z,   x + 1, y + 1,     z + dz[i], x + 1, y + dy[i] + 1, z + dz[i]);
                    ao_out[i] = level_to_brightness(std::max(ao_y, ao_yp1));
                } else {
                    int ao = vertex_level(accessor, x + 1, y + dy[i], z,   x + 1, y, z + dz[i], x + 1, y + dy[i], z + dz[i]);
                    ao_out[i] = level_to_brightness(ao);
                }
            }
            break;
        }
        case FaceDirection::Left: {
            const int dy[4] = {-stride, -stride,  stride,  stride};
            const int dz[4] = { stride, -stride, -stride,  stride};
            for (int i = 0; i < 4; i++) {
                if (is_lowered && dy[i] > 0) {
                    int ao_y   = vertex_level(accessor, x - 1, y + dy[i],     z,   x - 1, y,         z + dz[i], x - 1, y + dy[i],     z + dz[i]);
                    int ao_yp1 = vertex_level(accessor, x - 1, y + dy[i] + 1, z,   x - 1, y + 1,     z + dz[i], x - 1, y + dy[i] + 1, z + dz[i]);
                    ao_out[i] = level_to_brightness(std::max(ao_y, ao_yp1));
                } else {
                    int ao = vertex_level(accessor, x - 1, y + dy[i], z,   x - 1, y, z + dz[i], x - 1, y + dy[i], z + dz[i]);
                    ao_out[i] = level_to_brightness(ao);
                }
            }
            break;
        }
        case FaceDirection::Front: {
            const int dx[4] = { stride, -stride, -stride,  stride};
            const int dy[4] = {-stride, -stride,  stride,  stride};
            for (int i = 0; i < 4; i++) {
                if (is_lowered && dy[i] > 0) {
                    int ao_y   = vertex_level(accessor, x + dx[i], y,         z + 1, x,         y + dy[i],     z + 1, x + dx[i], y + dy[i],     z + 1);
                    int ao_yp1 = vertex_level(accessor, x + dx[i], y + 1,     z + 1, x,         y + dy[i] + 1, z + 1, x + dx[i], y + dy[i] + 1, z + 1);
                    ao_out[i] = level_to_brightness(std::max(ao_y, ao_yp1));
                } else {
                    int ao = vertex_level(accessor, x + dx[i], y, z + 1, x, y + dy[i], z + 1, x + dx[i], y + dy[i], z + 1);
                    ao_out[i] = level_to_brightness(ao);
                }
            }
            break;
        }
        case FaceDirection::Back: {
            const int dx[4] = {-stride,  stride,  stride, -stride};
            const int dy[4] = {-stride, -stride,  stride,  stride};
            for (int i = 0; i < 4; i++) {
                if (is_lowered && dy[i] > 0) {
                    int ao_y   = vertex_level(accessor, x + dx[i], y,         z - 1, x,         y + dy[i],     z - 1, x + dx[i], y + dy[i],     z - 1);
                    int ao_yp1 = vertex_level(accessor, x + dx[i], y + 1,     z - 1, x,         y + dy[i] + 1, z - 1, x + dx[i], y + dy[i] + 1, z - 1);
                    ao_out[i] = level_to_brightness(std::max(ao_y, ao_yp1));
                } else {
                    int ao = vertex_level(accessor, x + dx[i], y, z - 1, x, y + dy[i], z - 1, x + dx[i], y + dy[i], z - 1);
                    ao_out[i] = level_to_brightness(ao);
                }
            }
            break;
        }
    }
}

// -------------------------------------------------------------------------
// Greedy-merged face AO
//
// The greedy meshing code already verifies that every block in the merged
// region has identical AO values.  Therefore the first block's AO is
// valid for all four corners of the merged rectangle.
// -------------------------------------------------------------------------
void AmbientOcclusion::compute_greedy_face(const ChunkNeighborAccessor& accessor,
                                             const Face& face,
                                             float ao_out[4],
                                             int32_t stride) const {
    compute_face(accessor, face.x, face.y, face.z, face.direction, ao_out, stride);
}

// -------------------------------------------------------------------------
// Pack AO into a single byte (0-255) for Vertex::ao
//
// Face shade is no longer baked into AO to avoid double-processing.
// The shader's smooth curve (pow(x, 1.35)) is applied to pure AO values,
// and directional lighting is handled separately via world_normal and sun_dir.
// This prevents darker faces (bottom=0.5) from being incorrectly darkened
// by the AO curve when they have zero actual occlusion.
// -------------------------------------------------------------------------
uint8_t AmbientOcclusion::pack_vertex_ao(float ao, FaceDirection direction) {
    // Pack only the AO value, not multiplied by face shade
    // Face shade is handled by the shader's directional lighting
    (void)direction;  // Direction no longer used for packing, kept for API compatibility
    int iv = static_cast<int>(std::lround(ao * 255.0f));
    if (iv < 0) iv = 0;
    if (iv > 255) iv = 255;
    return static_cast<uint8_t>(iv);
}

} // namespace VoxelEngine
