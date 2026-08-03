#include "mesh/smooth_lighting.hpp"
#include "core/light_packing.hpp"
#include "mesh/ambient_occlusion.hpp"

namespace VoxelEngine {

// Average the packed light of the non-occluding samples around a face corner.
//
// Occluding (solid/opaque) blocks do not store meaningful propagated light, so
// their stored values must not leak into the interpolated corner light.  When
// the ±1 sampling window overlaps neighboring geometry (a second pillar, raised
// terrain, a cliff), a blind average pulls the corner down to a different level
// than the rest of the face and produces visible seams.  Excluding occluding
// samples (same predicate AmbientOcclusion uses) keeps the corner light uniform
// while AO independently shades the occluded corner.
static uint16_t average_light_filtered(
    const ChunkNeighborAccessor& accessor,
    int32_t cx, int32_t cy, int32_t cz,
    int32_t ox1, int32_t oy1, int32_t oz1,
    int32_t ox2, int32_t oy2, int32_t oz2,
    uint16_t center_light
) {
    const int32_t sx[4] = {0, ox1, ox2, ox1 + ox2};
    const int32_t sy[4] = {0, oy1, oy2, oy1 + oy2};
    const int32_t sz[4] = {0, oz1, oz2, oz1 + oz2};

    int sky = 0, r = 0, g = 0, b = 0, count = 0;
    for (int i = 0; i < 4; i++) {
        const int32_t px = cx + sx[i];
        const int32_t py = cy + sy[i];
        const int32_t pz = cz + sz[i];
        if (AmbientOcclusion::is_occluding(accessor.get_block(px, py, pz)))
            continue;
        const uint16_t light = (i == 0) ? center_light
                                        : accessor.get_light_packed(px, py, pz);
        sky += unpack_sky(light);
        r   += unpack_r(light);
        g   += unpack_g(light);
        b   += unpack_b(light);
        count++;
    }
    if (count == 0) {
        return center_light;  // unreachable for an emitted face; fall back safely
    }
    return pack_light(
        static_cast<uint8_t>((sky + count / 2) / count),
        static_cast<uint8_t>((r   + count / 2) / count),
        static_cast<uint8_t>((g   + count / 2) / count),
        static_cast<uint8_t>((b   + count / 2) / count)
    );
}

uint16_t compute_corner_light(
    const ChunkNeighborAccessor& accessor,
    int32_t gx, int32_t gy, int32_t gz,
    FaceDirection direction
) {
    // The 4 cells meeting at the grid corner on the air side of the face.
    // "center" is the cell in the air half-space directly at the corner; the
    // offsets add the two in-plane axis neighbors and the diagonal cell.
    int32_t cx, cy, cz;
    int32_t o1x, o1y, o1z, o2x, o2y, o2z;
    switch (direction) {
        case FaceDirection::Top:    cx = gx;     cy = gy;     cz = gz;     o1x = -1; o1y = 0; o1z = 0; o2x = 0; o2y = 0; o2z = -1; break;
        case FaceDirection::Bottom: cx = gx;     cy = gy - 1; cz = gz;     o1x = -1; o1y = 0; o1z = 0; o2x = 0; o2y = 0; o2z = -1; break;
        case FaceDirection::Right:  cx = gx;     cy = gy;     cz = gz;     o1x = 0; o1y = -1; o1z = 0; o2x = 0; o2y = 0; o2z = -1; break;
        case FaceDirection::Left:   cx = gx - 1; cy = gy;     cz = gz;     o1x = 0; o1y = -1; o1z = 0; o2x = 0; o2y = 0; o2z = -1; break;
        case FaceDirection::Front:  cx = gx;     cy = gy;     cz = gz;     o1x = 0; o1y = -1; o1z = 0; o2x = -1; o2y = 0; o2z = 0; break;
        case FaceDirection::Back:   cx = gx;     cy = gy;     cz = gz - 1; o1x = 0; o1y = -1; o1z = 0; o2x = -1; o2y = 0; o2z = 0; break;
    }
    return average_light_filtered(accessor, cx, cy, cz,
                                  o1x, o1y, o1z, o2x, o2y, o2z,
                                  accessor.get_light_packed(cx, cy, cz));
}

void compute_smooth_light(
    const ChunkNeighborAccessor& accessor,
    int32_t x, int32_t y, int32_t z,
    FaceDirection direction,
    uint16_t light_keys_out[4],
    int32_t stride
) {
    // Corner grid positions relative to the block anchor, matching
    // MeshBuilder::kFaceVertices so merged and per-block faces agree at shared
    // grid corners. The in-plane axes scale with stride; y never does.
    static constexpr int32_t kCornerOffsets[6][4][3] = {
        // Top (+Y)
        {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
        // Bottom (-Y)
        {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
        // Right (+X)
        {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
        // Left (-X)
        {{0, 0, 1}, {0, 0, 0}, {0, 1, 0}, {0, 1, 1}},
        // Front (+Z)
        {{1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {1, 1, 1}},
        // Back (-Z)
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}
    };
    const int di = static_cast<int>(direction);
    for (int i = 0; i < 4; i++) {
        const int32_t gx = x + kCornerOffsets[di][i][0] * stride;
        const int32_t gy = y + kCornerOffsets[di][i][1];
        const int32_t gz = z + kCornerOffsets[di][i][2] * stride;
        light_keys_out[i] = compute_corner_light(accessor, gx, gy, gz, direction);
    }
}

} // namespace VoxelEngine
