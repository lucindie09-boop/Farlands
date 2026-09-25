#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace VoxelEngine;

TEST_CASE("water faces route to water vertex/index buffers") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() > 0);
    CHECK(mb.get_water_indices().size() > 0);
}

TEST_CASE("opaque blocks do not appear in water buffers") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() == 0);
    CHECK(mb.get_water_indices().size() == 0);
}

TEST_CASE("surface water vs deep water both produce water mesh") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(10, 10, 10, BlockIDs::SURFACE_WATER);
    chunk.set_block(10, 9, 10, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() > 0);
}

TEST_CASE("solid block adjacent to water produces non-water mesh") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(16, 15, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    CHECK(mb.get_water_vertices().size() > 0);
}

TEST_CASE("side face above water is NOT shortened (side_lowered_offset removed)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(15, 14, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    bool found_normal = false;
    for (const auto& v : mb.get_vertices()) {
        float fy = static_cast<float>(v.y) / 256.0f;
        if (std::abs(fy - 16.0f) < 0.02f) {
            found_normal = true;
        }
    }
    CHECK(found_normal);
}

namespace {

BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// Vertices are Q8.8 fixed point, so a position is exact when it is a multiple
// of 1/256. `x`/`y`/`z` are CHUNK-local, like the crucible test's checks.
bool has_vertex_at(const MeshBuilder& mb, float x, float y, float z) {
    for (const Vertex& v : mb.get_vertices()) {
        if (std::fabs(static_cast<float>(v.x) / 256.0f - x) < 0.002f &&
            std::fabs(static_cast<float>(v.y) / 256.0f - y) < 0.002f &&
            std::fabs(static_cast<float>(v.z) / 256.0f - z) < 0.002f) {
            return true;
        }
    }
    return false;
}

// data/block_shapes.json "crucible": four 1x1px corner legs 2px tall, a 1px
// floor plate across the whole footprint, and four 1px walls open at the top.
std::vector<BlockAABB> crucible_model() {
    return {
        box(0.0f, 0.0f, 0.0f, 0.0625f, 0.125f, 0.0625f),
        box(0.9375f, 0.0f, 0.0f, 1.0f, 0.125f, 0.0625f),
        box(0.0f, 0.0f, 0.9375f, 0.0625f, 0.125f, 1.0f),
        box(0.9375f, 0.0f, 0.9375f, 1.0f, 0.125f, 1.0f),
        box(0.0f, 0.125f, 0.0f, 1.0f, 0.1875f, 1.0f),
        box(0.0f, 0.1875f, 0.0f, 0.0625f, 1.0f, 1.0f),
        box(0.9375f, 0.1875f, 0.0f, 1.0f, 1.0f, 1.0f),
        box(0.0625f, 0.1875f, 0.0f, 0.9375f, 1.0f, 0.0625f),
        box(0.0625f, 0.1875f, 0.9375f, 0.9375f, 1.0f, 1.0f),
    };
}

} // namespace

TEST_CASE("a hollow multi-box block emits its cavity, not a solid cube") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    BlockType crucible{};
    crucible.name = "test_crucible";
    crucible.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    crucible.visible_faces = {true, true, true, true, true, true};
    crucible.selection_boxes = crucible_model();
    crucible.full_cube_ = false;
    crucible.greedy_mergeable = false;
    const BlockID id = reg.register_block(crucible);
    if (id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, id);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);

    // Vertices are Q8.8 fixed point, so 0.0625 lands exactly on 16/256.
    std::vector<float> xs, ys, zs;
    for (const Vertex& v : mb.get_vertices()) {
        xs.push_back(static_cast<float>(v.x) / 256.0f);
        ys.push_back(static_cast<float>(v.y) / 256.0f);
        zs.push_back(static_cast<float>(v.z) / 256.0f);
    }
    if (xs.empty()) {
        CHECK(false);
        return;
    }
    const auto has_point = [&](float x, float y, float z) {
        for (size_t i = 0; i < xs.size(); ++i) {
            if (std::fabs(xs[i] - x) < 0.002f && std::fabs(ys[i] - y) < 0.002f &&
                std::fabs(zs[i] - z) < 0.002f) {
                return true;
            }
        }
        return false;
    };

    // The walls reach the top of the block and the floor plate sits on the legs
    // (2/16), so the vessel is open-mouthed rather than a stubby lip.
    CHECK(has_point(15.0625f, 16.0f, 15.0f));       // -X wall, inner edge, full height
    CHECK(has_point(16.0f, 16.0f, 16.0f));          // +X/+Z corner at full height
    CHECK(has_point(15.0f, 15.1875f, 15.0f));       // floor plate top (3/16)
    CHECK(has_point(15.0f, 15.125f, 15.0f));        // leg top (2/16)
    CHECK(has_point(15.0f, 15.0f, 15.0f));          // leg bottom, outer corner

    // The cavity's inner wall planes exist: a single full cube would have none
    // of these, so this is the check that the shape was expanded, not ignored.
    CHECK(has_point(15.0625f, 15.1875f, 15.0f));    // -X wall inner face
    CHECK(has_point(15.9375f, 15.1875f, 16.0f));    // +X wall inner face
    // The +/-Z walls sit between the +/-X walls, so their inner faces span x
    // from 1/16 to 15/16 rather than the whole footprint.
    CHECK(has_point(15.0625f, 15.1875f, 15.0625f)); // -Z wall inner face
    CHECK(has_point(15.9375f, 15.1875f, 15.9375f)); // +Z wall inner face

    // 9 boxes, each emitting its visible faces (bottoms included where the box
    // floats), is far more geometry than the 24 vertices of a full cube.
    CHECK(mb.get_vertex_count() > 100);
}

// A shape box raised off the cell floor has an underside you can see from below
// (a wall torch, a fence rail, a lantern). A box resting on the floor does not,
// and emitting it would cost a sixth of the per-AABB pass for nothing. The rule
// is what makes a floating model read as a model instead of a hole.
TEST_CASE("a shape box raised off the floor emits its underside; a floor box does not") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto bottom_vertices = [](const MeshBuilder& mb) {
        int count = 0;
        for (const Vertex& v : mb.get_vertices()) {
            if (v.ny == -127) ++count;  // packed face normal for -Y
        }
        return count;
    };

    // Exactly the wall-torch box: a 2/16 stick hugging one face, raised to 3.5/16.
    BlockType raised{};
    raised.name = "test_raised_stick";
    raised.properties = BlockProperty::NoOcclusion;
    raised.visible_faces = {true, true, true, true, true, true};
    raised.selection_boxes = {box(0.4375f, 0.21875f, 0.0f, 0.5625f, 0.84375f, 0.125f)};
    raised.full_cube_ = false;
    raised.greedy_mergeable = false;

    // The same box dropped onto the floor, which is the floor-torch case.
    BlockType grounded = raised;
    grounded.name = "test_floor_stick";
    grounded.selection_boxes = {box(0.4375f, 0.0f, 0.0f, 0.5625f, 0.625f, 0.125f)};

    const BlockID raised_id = reg.register_block(raised);
    const BlockID grounded_id = reg.register_block(grounded);
    if (raised_id == BlockIDs::AIR || grounded_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, raised_id);
    chunk.compute_section_flags();
    MeshBuilder raised_mb;
    raised_mb.set_greedy_enabled(false);
    raised_mb.build_mesh(chunk);

    // One quad, four vertices, all of them on the box's own bottom plane.
    CHECK(bottom_vertices(raised_mb) == 4);
    // Vertices are chunk-local, so the cell at (15,15,15) puts that plane at
    // 15.21875 and the box's corners at its own edges.
    CHECK(has_vertex_at(raised_mb, 15.4375f, 15.21875f, 15.0f));
    CHECK(has_vertex_at(raised_mb, 15.5625f, 15.21875f, 15.125f));

    ChunkData floor_chunk;
    floor_chunk.fill_blocks(BlockIDs::AIR);
    floor_chunk.set_block(15, 15, 15, grounded_id);
    floor_chunk.compute_section_flags();
    MeshBuilder floor_mb;
    floor_mb.set_greedy_enabled(false);
    floor_mb.build_mesh(floor_chunk);

    CHECK(bottom_vertices(floor_mb) == 0);
    // ...and the rest of the box really was drawn, so the check above is about
    // the bottom face rather than about a block that emitted nothing at all.
    CHECK(floor_mb.get_vertex_count() > 4);
}

// The single exception to that rule: when a sibling box of the same shape sits
// directly under the box, its underside is inside the shape and must not be
// emitted. A stair is exactly this case — its upper step over its own lower
// step — and an emitted bottom face there is a hidden quad that ships to the
// GPU on every stair block in the world.
TEST_CASE("a shape box whose underside is buried in a sibling box emits no bottom face") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto bottom_vertices = [](const MeshBuilder& mb) {
        int count = 0;
        for (const Vertex& v : mb.get_vertices()) {
            if (v.ny == -127) ++count;  // packed face normal for -Y
        }
        return count;
    };

    const auto shaped = [&](const char* name, std::vector<BlockAABB> boxes) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::NoOcclusion;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.selection_boxes = std::move(boxes);
        bt.full_cube_ = false;
        bt.greedy_mergeable = false;
        return reg.register_block(bt);
    };

    // A stair: full-footprint lower step, half-footprint upper step on top of it.
    const BlockID stair = shaped("test_buried_step", {
        box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f),
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f),
    });
    // The upper step alone, to show the sibling is what suppresses the face.
    const BlockID step_only = shaped("test_step_alone", {
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f),
    });
    // The upper step over a narrow post: the lower box does not span the upper
    // box's footprint, so the underside is real and stays.
    const BlockID step_on_post = shaped("test_step_on_post", {
        box(0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f),
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f),
    });
    if (stair == BlockIDs::AIR || step_only == BlockIDs::AIR || step_on_post == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const auto mesh_of = [&](BlockID id) {
        ChunkData chunk;
        chunk.fill_blocks(BlockIDs::AIR);
        chunk.set_block(15, 15, 15, id);
        chunk.compute_section_flags();
        MeshBuilder mb;
        mb.set_greedy_enabled(false);
        mb.build_mesh(chunk);
        return mb;
    };

    // Lower step's bottom is floor-anchored (always skipped) and the upper
    // step's is buried, so a stair contributes no downward faces at all.
    CHECK(bottom_vertices(mesh_of(stair)) == 0);
    // Same box with nothing under it: four vertices on its own bottom plane.
    CHECK(bottom_vertices(mesh_of(step_only)) == 4);
    // A partial sibling does not cover the footprint, so the face survives.
    CHECK(bottom_vertices(mesh_of(step_on_post)) == 4);
}

namespace {

// A cell-local copy of one emitted quad: the four vertices of a +Y face span a
// rectangle in x/z at one height.
struct TopQuad {
    float x0, x1, z0, z1, y;
};

std::vector<TopQuad> top_quads(const MeshBuilder& mb, float cell_x, float cell_z) {
    std::vector<TopQuad> out;
    const auto& verts = mb.get_vertices();
    size_t i = 0;
    while (i + 3 < verts.size()) {
        if (verts[i].ny != 127) {  // packed face normal for +Y
            ++i;
            continue;
        }
        TopQuad q{};
        q.y = static_cast<float>(verts[i].y) / 256.0f;
        q.x0 = q.x1 = static_cast<float>(verts[i].x) / 256.0f - cell_x;
        q.z0 = q.z1 = static_cast<float>(verts[i].z) / 256.0f - cell_z;
        for (int k = 1; k < 4; ++k) {
            const float vx = static_cast<float>(verts[i + k].x) / 256.0f - cell_x;
            const float vz = static_cast<float>(verts[i + k].z) / 256.0f - cell_z;
            q.x0 = std::min(q.x0, vx);
            q.x1 = std::max(q.x1, vx);
            q.z0 = std::min(q.z0, vz);
            q.z1 = std::max(q.z1, vz);
        }
        out.push_back(q);
        i += 4;
    }
    return out;
}

// How many quads of one cell lie on a given horizontal plane facing a given way.
// The +Y helper above answers "what is on top"; this one is for the other side of
// the same plane, which is where a hidden underside would be.
int quads_facing(const MeshBuilder& mb, float cell_x, float cell_z, float plane_y,
                 int8_t packed_normal) {
    const auto& verts = mb.get_vertices();
    int count = 0;
    for (size_t i = 0; i + 3 < verts.size(); i += 4) {
        if (verts[i].ny != packed_normal) continue;
        if (std::fabs(static_cast<float>(verts[i].y) / 256.0f - plane_y) > 1e-4f) continue;
        if (std::fabs(static_cast<float>(verts[i].x) / 256.0f - cell_x) > 1.0f) continue;
        if (std::fabs(static_cast<float>(verts[i].z) / 256.0f - cell_z) > 1.0f) continue;
        ++count;
    }
    return count;
}

// Sample centres offset off every 16th, so a point never lands exactly on the
// seam between two pieces and counts for both.
bool rect_covers(float x0, float x1, float z0, float z1, float px, float pz) {
    constexpr float kE = 1e-4f;
    return px >= x0 - kE && px <= x1 + kE && pz >= z0 - kE && pz <= z1 + kE;
}

}  // namespace

// Four reaches drawn at once — a wall standing with every side connected — is
// the case that forced this: all four of their tops sit at the same height and
// overlap in the middle of the cell, so the same coplanar area used to be drawn
// twice and the two fought for the depth buffer.
TEST_CASE("a shape's overlapping boxes draw each coplanar face exactly once") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto shaped = [&](const char* name, std::vector<BlockAABB> boxes) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::NoOcclusion;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.selection_boxes = std::move(boxes);
        bt.full_cube_ = false;
        bt.greedy_mergeable = false;
        return reg.register_block(bt);
    };

    const auto mesh_of = [&](BlockID id) {
        ChunkData chunk;
        chunk.fill_blocks(BlockIDs::AIR);
        chunk.set_block(15, 15, 15, id);
        chunk.compute_section_flags();
        MeshBuilder mb;
        mb.set_greedy_enabled(false);
        mb.build_mesh(chunk);
        return mb;
    };

    // The wall's four reaches, exactly as data/block_shapes.json draws them.
    const std::vector<BlockAABB> reaches = {
        box(0.3125f, 0.0f, 0.0f, 0.6875f, 0.875f, 0.5f),
        box(0.3125f, 0.0f, 0.5f, 0.6875f, 0.875f, 1.0f),
        box(0.5f, 0.0f, 0.3125f, 1.0f, 0.875f, 0.6875f),
        box(0.0f, 0.0f, 0.3125f, 0.5f, 0.875f, 0.6875f),
    };
    const BlockID cross = shaped("test_coplanar_cross", reaches);
    if (cross == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const MeshBuilder mb = mesh_of(cross);
    const std::vector<TopQuad> tops = top_quads(mb, 15.0f, 15.0f);
    CHECK(tops.size() >= reaches.size());  // the four tops are still all there

    // No two quads on the same plane may share any area at all: that is the
    // z-fight, stated as geometry rather than as an observation.
    for (size_t a = 0; a < tops.size(); ++a) {
        for (size_t b = a + 1; b < tops.size(); ++b) {
            if (std::fabs(tops[a].y - tops[b].y) > 1e-4f) continue;
            const float ox = std::min(tops[a].x1, tops[b].x1) - std::max(tops[a].x0, tops[b].x0);
            const float oz = std::min(tops[a].z1, tops[b].z1) - std::max(tops[a].z0, tops[b].z0);
            CHECK(std::min(ox, oz) <= 1e-4f);
        }
    }

    // ...and the union is untouched: every point the four reaches cover on their
    // top plane is drawn, so the culling removed duplicates rather than geometry.
    constexpr int kSteps = 64;
    int covered_by_boxes = 0;
    int covered_by_quads = 0;
    int drawn_twice = 0;
    for (int iz = 0; iz < kSteps; ++iz) {
        for (int ix = 0; ix < kSteps; ++ix) {
            const float px = (static_cast<float>(ix) + 0.37f) / kSteps;
            const float pz = (static_cast<float>(iz) + 0.37f) / kSteps;
            bool in_boxes = false;
            for (const BlockAABB& b : reaches) {
                if (rect_covers(b.min[0], b.max[0], b.min[2], b.max[2], px, pz)) in_boxes = true;
            }
            int hits = 0;
            for (const TopQuad& q : tops) {
                if (rect_covers(q.x0, q.x1, q.z0, q.z1, px, pz)) ++hits;
            }
            if (in_boxes) ++covered_by_boxes;
            if (hits > 0) ++covered_by_quads;
            if (hits > 1) ++drawn_twice;
        }
    }
    CHECK(covered_by_boxes > 0);
    CHECK(covered_by_quads == covered_by_boxes);
    CHECK(drawn_twice == 0);
}

TEST_CASE("a box's face is trimmed where an earlier sibling covers it") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto shaped = [&](const char* name, std::vector<BlockAABB> boxes) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::NoOcclusion;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.selection_boxes = std::move(boxes);
        bt.full_cube_ = false;
        bt.greedy_mergeable = false;
        return reg.register_block(bt);
    };

    const auto mesh_of = [&](BlockID id) {
        ChunkData chunk;
        chunk.fill_blocks(BlockIDs::AIR);
        chunk.set_block(15, 15, 15, id);
        chunk.compute_section_flags();
        MeshBuilder mb;
        mb.set_greedy_enabled(false);
        mb.build_mesh(chunk);
        return mb;
    };

    // A stair: slab across the cell, step on top of half of it. The step stands ON
    // the slab's top, so from that plane the step's body is the outside: the part
    // of the slab's top under the step is inside the model and goes, and so does
    // the step's underside, which the slab's own body covers in turn. The plane is
    // then drawn exactly once, and the step's top, which is what you see, is
    // untouched.
    const BlockID stair = shaped("test_coplanar_stair", {
        box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f),
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f),
    });
    // And a post with a rail set into it: the rail's end cap is the post's own
    // side, so it is not drawn twice.
    const BlockID railed = shaped("test_coplanar_rail", {
        box(0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f),
        box(0.25f, 0.375f, 0.375f, 1.0f, 0.75f, 0.625f),
    });
    if (stair == BlockIDs::AIR || railed == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const MeshBuilder stair_mb = mesh_of(stair);
    const std::vector<TopQuad> slab_tops = top_quads(stair_mb, 15.0f, 15.0f);
    int slab_plane = 0;
    int step_plane = 0;
    for (const TopQuad& q : slab_tops) {
        if (std::fabs(q.y - 15.5f) < 1e-4f) {
            ++slab_plane;
            // The plane the two boxes share, drawn by the EARLIER box over its own
            // whole footprint. The step reaches that plane from above and does not
            // pass through it, so it is flush with the slab's top: exactly one of
            // the two may draw there, and the resolved order decides.
            CHECK(q.x0 == doctest::Approx(0.0f));
            CHECK(q.x1 == doctest::Approx(1.0f));
            CHECK(q.z0 == doctest::Approx(0.0f));
            CHECK(q.z1 == doctest::Approx(1.0f));
        } else if (std::fabs(q.y - 16.0f) < 1e-4f) {
            ++step_plane;
        }
    }
    CHECK(slab_plane == 1);  // drawn once, not once per box that touches it
    CHECK(step_plane == 1);  // ...and the step, as it was
    // The other half of that trade: the step's own underside is the face that goes,
    // which is what stops the shared plane being drawn twice by two coincident
    // quads facing opposite ways.
    CHECK(quads_facing(stair_mb, 15.0f, 15.0f, 15.5f, -127) == 0);

    // The rail's top: the part of it that is inside the post's footprint is the
    // post's own surface, so nothing at the rail's height may be drawn there.
    const MeshBuilder rail_mb = mesh_of(railed);
    int cap_quads = 0;
    for (const TopQuad& q : top_quads(rail_mb, 15.0f, 15.0f)) {
        if (std::fabs(q.y - 15.75f) > 1e-4f) continue;
        const float ox = std::min(q.x1, 0.75f) - std::max(q.x0, 0.25f);
        const float oz = std::min(q.z1, 0.75f) - std::max(q.z0, 0.25f);
        if (std::min(ox, oz) > 1e-4f) ++cap_quads;
    }
    CHECK(cap_quads == 0);
}

TEST_CASE("AO: hole in solid chunk produces more faces than solid") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData solid;
    solid.fill_blocks(BlockIDs::STONE);
    solid.compute_section_flags();
    MeshBuilder mb_solid;
    mb_solid.set_greedy_enabled(false);
    mb_solid.build_mesh(solid);

    ChunkData with_hole;
    with_hole.fill_blocks(BlockIDs::STONE);
    with_hole.set_block(15, 15, 15, BlockIDs::AIR);
    with_hole.compute_section_flags();
    MeshBuilder mb_hole;
    mb_hole.set_greedy_enabled(false);
    mb_hole.build_mesh(with_hole);

    CHECK(mb_solid.get_vertex_count() > 0);
    CHECK(mb_hole.get_vertex_count() > mb_solid.get_vertex_count());
}
