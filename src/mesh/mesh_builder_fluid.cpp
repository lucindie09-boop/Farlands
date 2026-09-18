#include "mesh/mesh_builder.hpp"
#include "mesh/mesh_fluid.hpp"
#include "core/light_packing.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace VoxelEngine {

namespace {

// Vertex positions are Q8.8, so a surface height round-trips to 1/256 of a block.
inline uint16_t to_fixed(float v) noexcept {
    return static_cast<uint16_t>(std::lroundf(v * 256.0f));
}

}  // namespace

// -----------------------------------------------------------------------------
// One fluid face
// -----------------------------------------------------------------------------
// `corners` carries the cell's four surface heights (indexed [cz][cx]); for a
// merged run of flat cells all four are the same height and `z_span` is the run
// length. Every vertex takes its Y from the corner it sits on, so a top face
// slopes and a side face's top edge follows the same two corners the surface
// does.
//
// UVs stay the flat face mapping: the surface bends, the texture does not (a
// texture that followed the corners would smear wherever the rim falls away).
// A merged run tiles the texture per cell, exactly as the greedy path does, and
// a side face samples the strip from its own top edge down — which is what the
// old uniform-height water did, so a pool looks the same except that its rim now
// moves.
//
// One light key per face, not one per corner: the corner heights already make the
// surface non-planar, and a translucent animated surface does not show the
// difference. That is also why a merged run compares light keys exactly (see
// passive_fluid_mesh).
void MeshBuilder::add_fluid_quad(int32_t x, int32_t y, int32_t z, FaceDirection dir,
                                 BlockID block_id, const mesh_fluid::Corners& corners,
                                 int32_t z_span, uint16_t light_key,
                                 const BlockRegistry& registry) {
    const int dir_index = static_cast<int>(dir);
    const BlockType& block_type = registry.get_block(block_id);
    int texture_idx = 0;
    int emissive_idx = 0;
    switch (dir) {
        case FaceDirection::Right:  texture_idx = block_type.texture_indices[0]; emissive_idx = block_type.emissive_texture_indices[0]; break;
        case FaceDirection::Left:   texture_idx = block_type.texture_indices[1]; emissive_idx = block_type.emissive_texture_indices[1]; break;
        case FaceDirection::Top:    texture_idx = block_type.texture_indices[2]; emissive_idx = block_type.emissive_texture_indices[2]; break;
        case FaceDirection::Bottom: texture_idx = block_type.texture_indices[3]; emissive_idx = block_type.emissive_texture_indices[3]; break;
        case FaceDirection::Front:  texture_idx = block_type.texture_indices[4]; emissive_idx = block_type.emissive_texture_indices[4]; break;
        case FaceDirection::Back:   texture_idx = block_type.texture_indices[5]; emissive_idx = block_type.emissive_texture_indices[5]; break;
    }

    const uint32_t vertex_count = static_cast<uint32_t>(water_vertices.size());
    const float span = static_cast<float>(z_span);

    for (int i = 0; i < 4; ++i) {
        const float lx = kFaceVertices[dir_index][i][0];  // 0 or 1 within the cell
        const float ly = kFaceVertices[dir_index][i][1];
        const float lz = kFaceVertices[dir_index][i][2];
        const int cx = (lx > 0.5f) ? 1 : 0;
        const int cz = (lz > 0.5f) ? 1 : 0;
        // A vertex on the top edge of the face sits on the surface; one on the
        // bottom edge sits on the cell floor. (A liquid never draws a bottom face,
        // as everywhere else in the mesher, so a "bottom" vertex is always a side
        // face's lower edge.)
        const float surface = (ly > 0.5f) ? corners.h[cz][cx] : 0.0f;

        Vertex v;
        v.x = to_fixed(static_cast<float>(x) + lx);
        v.y = to_fixed(static_cast<float>(y) + surface);
        v.z = to_fixed(static_cast<float>(z) + lz * span);
        v.nx = static_cast<int8_t>(kFaceNormals[dir_index][0] * 127.0f);
        v.ny = static_cast<int8_t>(kFaceNormals[dir_index][1] * 127.0f);
        v.nz = static_cast<int8_t>(kFaceNormals[dir_index][2] * 127.0f);
        v.u = kFaceUVs[i][0];
        if (dir == FaceDirection::Top) {
            v.v = kFaceUVs[i][1] * span;
        } else {
            v.v = 1.0f - surface;  // from this edge's top corner down to the floor
        }
        v.texture_index = static_cast<uint16_t>(texture_idx);
        // Liquids are not occluded: a surface that slopes cannot carry corner AO,
        // and the old path made the same call for every liquid face.
        v.ao = AmbientOcclusion::pack_vertex_ao(1.0f, dir);
        // The water shader leaves this channel free; it carries the fluid kind
        // (Water=1, Lava=2, Acid=3) so each substance can pick its own alpha.
        v.emissive_index = static_cast<uint8_t>(block_type.fluid_kind);
        // Liquids are never occluded (full-bright AO above), so the AO byte
        // carries the texture-flow direction instead: downhill on top faces
        // (the slope the corners already encode), down the wall on side faces
        // of moving liquid. The shader scrolls the texture along it.
        if (dir == FaceDirection::Top) {
            v.ao = mesh_fluid::top_face_flow(corners);
        } else {
            v.ao = mesh_fluid::side_face_flow(block_type.is_fluid_state(),
                                              block_type.fluid_falling,
                                              static_cast<int>(block_type.fluid_depth));
        }
        v.light_r = static_cast<uint8_t>(kBlockBrightness[unpack_r(light_key)] * 255.0f);
        v.light_g = static_cast<uint8_t>(kBlockBrightness[unpack_g(light_key)] * 255.0f);
        v.light_b = static_cast<uint8_t>(kBlockBrightness[unpack_b(light_key)] * 255.0f);
        v.sky_light = static_cast<uint8_t>(kBlockBrightness[unpack_sky(light_key)] * 255.0f);
        water_vertices.push_back(v);
    }

    water_indices.push_back(vertex_count + 0);
    water_indices.push_back(vertex_count + 1);
    water_indices.push_back(vertex_count + 2);
    water_indices.push_back(vertex_count + 0);
    water_indices.push_back(vertex_count + 2);
    water_indices.push_back(vertex_count + 3);

    if (record_quads_) {
        CachedQuad q;
        q.x = x;
        q.y = y;
        q.z = z;
        q.direction = dir;
        q.water = true;
        q.ex = 1;
        q.ey = 1;
        q.ez = z_span;  // a merged top run is one unit of re-emission
        q.verts = {water_vertices[vertex_count + 0], water_vertices[vertex_count + 1],
                   water_vertices[vertex_count + 2], water_vertices[vertex_count + 3]};
        q.idx = {0, 1, 2, 0, 2, 3};
        quads.push_back(q);
    }
}

// -----------------------------------------------------------------------------
// The fluid surface pass
// -----------------------------------------------------------------------------
void MeshBuilder::passive_fluid_mesh(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                     const BlockRegistry& registry) {
    // A corner height is a sub-block quantity: at a 2-block stride it means
    // nothing, so liquids are only drawn at full detail. At LOD the generic
    // emitters take over and draw each liquid cell as a plain box into the same
    // water buffer (see emit_faces), which is the box approximation the far
    // regions show.
    if (stride_xz_ != 1) return;

    const auto lookup = [&](int32_t lx, int32_t ly, int32_t lz) {
        const BlockID id = accessor.get_block(lx, ly, lz);
        return mesh_fluid::classify(id, registry.get_block_fast(id));
    };
    const auto face_shows = [&](FluidKind family, BlockID neighbor_id, float surface,
                                bool above) {
        const BlockType& nt = registry.get_block_fast(neighbor_id);
        return mesh_fluid::face_visible(family, mesh_fluid::classify(neighbor_id, nt), nt, surface,
                                        above);
    };

    for (int32_t s = 0; s < CHUNK_SECTIONS; ++s) {
        if (chunk.is_section_all_air(s)) continue;
        const int32_t y0 = s * SECTION_HEIGHT;
        const int32_t y1 = y0 + SECTION_HEIGHT;
        for (int32_t y = y0; y < y1; ++y) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                // Partial rebuilds skip rows whose faces cannot intersect the dirty
                // region; the test is on x/y only, because the z scan below walks the
                // WHOLE row. A merged quad is one unit of re-emission, so a run that
                // touches the region is re-emitted in full (the greedy passes work the
                // same way) — a partial build must not split one in half.
                if (partial_mode_ &&
                    !(y >= partial_bounds_.y_min && y < partial_bounds_.y_max &&
                      x < partial_bounds_.x_max && x + 1 > partial_bounds_.x_min)) {
                    continue;
                }

                // One open top run at a time. A run always holds the cell whose top
                // is visible; it only ever grows while every cell in it is FLAT and
                // identical, because a merged quad carries one height and a sloped
                // cell's corners would be lost in it.
                int32_t top_start = -1;  // z where the open run starts
                BlockID top_block = BlockIDs::AIR;
                uint16_t top_light = 0;
                float top_height = 0.0f;
                bool top_flat = false;
                mesh_fluid::Corners top_corners{};

                const auto flush_top = [&](int32_t z_end) {
                    if (top_start < 0) return;
                    const int32_t z_start = top_start;
                    top_start = -1;
                    if (partial_mode_ &&
                        !(z_start < partial_bounds_.z_max && z_end > partial_bounds_.z_min)) {
                        return;  // wholly outside the dirty region: carried forward
                    }
                    const int32_t run = z_end - z_start;
                    if (run == 1) {
                        add_fluid_quad(x, y, z_start, FaceDirection::Top, top_block, top_corners,
                                       1, top_light, registry);
                        return;
                    }
                    // Every cell in the run is flat at the same height, so one quad is
                    // exact. This is what keeps a sea from becoming one quad per cell.
                    mesh_fluid::Corners flat;
                    flat.h[0][0] = flat.h[0][1] = flat.h[1][0] = flat.h[1][1] = top_height;
                    add_fluid_quad(x, y, z_start, FaceDirection::Top, top_block, flat, run,
                                   top_light, registry);
                };

                for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                    const BlockID id = solid_at(y, z + 1, x + 1);
                    const BlockType& bt = registry.get_block_fast(id);
                    const mesh_fluid::CellInfo self = mesh_fluid::classify(id, bt);
                    if (self.family == FluidKind::None) {
                        flush_top(z);
                        continue;
                    }

                    // Which faces could be drawn is decided BEFORE the corner heights,
                    // because the corner maths is the expensive part of this pass (four
                    // heights, each reading a 2x2 block of cells and what sits above
                    // it), while the interior of a pool, of a column, or of the sea
                    // draws nothing at all.
                    //
                    // The height the comparison uses is the cell's declared surface
                    // (1 - top_face_offset) rather than its exact corners. The only case
                    // they disagree on is a sloped rim against a lowered full cube —
                    // mud, one pixel down — and both readings leave it showing.
                    const float nominal = 1.0f - bt.top_face_offset;
                    bool shows[6] = {false, false, false, false, false, false};
                    // Side neighbours of a DIFFERENT substance, indexed by
                    // kSideDirections order; their faces are decided per corner
                    // once this cell's corner heights exist (below).
                    FluidKind side_family[4] = {FluidKind::None, FluidKind::None,
                                                FluidKind::None, FluidKind::None};
                    bool any_face = false;
                    for (int i = 0; i < 6; ++i) {
                        if (i == static_cast<int>(FaceDirection::Bottom)) continue;
                        const BlockID n_id = accessor.get_block(x + kDirectionOffsets[i][0],
                                                                y + kDirectionOffsets[i][1],
                                                                z + kDirectionOffsets[i][2]);
                        const BlockType& n_type = registry.get_block_fast(n_id);
                        const mesh_fluid::CellInfo n_info = mesh_fluid::classify(n_id, n_type);
                        if (i >= 2 && n_info.family != FluidKind::None &&
                            n_info.family != self.family) {
                            // Provisional: the per-corner test needs this cell's
                            // corners, so mark it visible to keep `any_face` honest
                            // and settle it after corners_of below.
                            side_family[i - 2] = n_info.family;
                            shows[i] = true;
                        } else {
                            shows[i] = face_shows(self.family, n_id, nominal,
                                                  i == static_cast<int>(FaceDirection::Top));
                        }
                        any_face = any_face || shows[i];
                    }
                    if (!any_face) {
                        flush_top(z);
                        continue;
                    }

                    const mesh_fluid::Corners corners =
                        mesh_fluid::corners_of(lookup, self.family, x, y, z);

                    // The different-liquid side faces, decided on the shared edge's
                    // actual corner heights: my face is redundant only when its
                    // profile sits at-or-below the neighbour's at BOTH corners.
                    // Opposing flows interleave — each face is the taller one at one
                    // end of the edge — so both draw. Each side's corners are
                    // measured under its own family's corner rule, exactly as that
                    // face is rendered, so the comparison matches what the eye sees.
                    for (int s = 0; s < 4; ++s) {
                        if (side_family[s] == FluidKind::None) continue;
                        const int i = s + 2;
                        // The face's top-edge endpoints in world space, in the
                        // max-cell convention corner_height uses.
                        int32_t ax, az, bx, bz;
                        switch (i) {
                            case 2:  ax = x + 1; az = z;     bx = x + 1; bz = z + 1; break;  // Right (+X)
                            case 3:  ax = x;     az = z;     bx = x;     bz = z + 1; break;  // Left  (-X)
                            case 4:  ax = x;     az = z + 1; bx = x + 1; bz = z + 1; break;  // Front (+Z)
                            default: ax = x;     az = z;     bx = x + 1; bz = z;     break;  // Back  (-Z)
                        }
                        const float my0 = corners.h[az - z][ax - x];
                        const float my1 = corners.h[bz - z][bx - x];
                        const float other0 =
                            mesh_fluid::corner_height(lookup, side_family[s], ax, y, az);
                        const float other1 =
                            mesh_fluid::corner_height(lookup, side_family[s], bx, y, bz);
                        shows[i] = mesh_fluid::different_liquid_side_visible(
                            my0, my1, other0, other1);
                        any_face = any_face || shows[i];
                    }
                    if (!any_face) {
                        flush_top(z);
                        continue;
                    }

                    // Sides: one cell wide, from the floor up to the two corners on
                    // their own top edge, so they never merge.
                    const bool emit_here = !partial_mode_ ||
                        (z < partial_bounds_.z_max && z + 1 > partial_bounds_.z_min);
                    if (emit_here) {
                        // The four sides only. The top is emitted by the run logic
                        // below (and the bottom, as everywhere else in the mesher,
                        // never) — iterating all six here would draw the top twice.
                        for (const FaceDirection dir : kSideDirections) {
                            const int i = static_cast<int>(dir);
                            if (!shows[i]) continue;
                            const uint16_t light =
                                accessor.get_light_packed(x + kDirectionOffsets[i][0],
                                                          y + kDirectionOffsets[i][1],
                                                          z + kDirectionOffsets[i][2]);
                            add_fluid_quad(x, y, z, dir, id, corners, 1, light, registry);
                        }
                    }

                    // Top: a flat cell can absorb its neighbour into one quad, and
                    // anything else closes the open run so its own corners are kept.
                    const bool top_shows = shows[static_cast<int>(FaceDirection::Top)];
                    if (top_shows) {
                        const bool flat = corners.flat();
                        const uint16_t light = accessor.get_light_packed(x, y + 1, z);
                        const bool extends_run = top_start >= 0 && top_flat && flat &&
                                                 id == top_block && light == top_light &&
                                                 corners.h[0][0] == top_height;
                        if (!extends_run) {
                            flush_top(z);
                            top_start = z;
                            top_block = id;
                            top_light = light;
                            top_height = corners.h[0][0];
                            top_flat = flat;
                            top_corners = corners;
                        }
                    } else {
                        flush_top(z);
                    }
                }

                flush_top(CHUNK_DEPTH);
            }
        }
    }
}

}  // namespace VoxelEngine
