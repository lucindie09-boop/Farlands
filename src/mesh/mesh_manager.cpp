#include "mesh/mesh_manager.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <algorithm>
#include <chrono>
#include "core/hash_utils.hpp"
#include <cstring>

namespace VoxelEngine {

using namespace godot;

namespace {
constexpr int32_t kGreedyDisableBlockRadius = 16;
constexpr int32_t kFarRegionUploadDivisor = 4;
constexpr int32_t kFarRegionBuildDivisor = 4;
// Coalescing window for far-region rebuilds: cache arrivals within this window
// are batched into a single rebuild instead of one rebuild per arrival.
constexpr double kFarRegionDebounceMs = 250.0;

static int32_t floor_div(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r != 0 && ((r > 0) != (divisor > 0))) {
        --q;
    }
    return q;
}

static uint8_t encode_normal_dir(int8_t nx, int8_t ny, int8_t nz) {
    if (ny > 0) return 0;
    if (ny < 0) return 1;
    if (nx > 0) return 2;
    if (nx < 0) return 3;
    if (nz > 0) return 4;
    return 5;
}

PackedBuiltMeshData pack_vertex_array(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    PackedBuiltMeshData packed;
    if (vertices.empty() || indices.empty()) {
        packed.empty = true;
        return packed;
    }
    packed.empty = false;
    const size_t n = vertices.size();
    packed.vertices.resize(n);
    packed.custom0.resize(n * 4);
    packed.custom1.resize(n * 4);
    packed.custom2.resize(n * 4);
    packed.indices.resize(indices.size());

    Vector3* v_ptr = packed.vertices.ptrw();
    uint8_t* c0_ptr = packed.custom0.ptrw();
    uint8_t* c1_ptr = packed.custom1.ptrw();
    int32_t* idx_ptr = packed.indices.ptrw();

    for (size_t i = 0; i < n; i++) {
        const Vertex& v = vertices[i];
        v_ptr[i] = Vector3(v.x, v.y, v.z);
        c0_ptr[i * 4 + 0] = v.light_r;
        c0_ptr[i * 4 + 1] = v.light_g;
        c0_ptr[i * 4 + 2] = v.light_b;
        c0_ptr[i * 4 + 3] = v.sky_light;
        c1_ptr[i * 4 + 0] = static_cast<uint8_t>(v.texture_index);
        c1_ptr[i * 4 + 1] = v.ao;
        c1_ptr[i * 4 + 2] = encode_normal_dir(v.nx, v.ny, v.nz);
        c1_ptr[i * 4 + 3] = v.emissive_index;
        packed.custom2.encode_half(static_cast<int64_t>(i * 4), static_cast<double>(v.u));
        packed.custom2.encode_half(static_cast<int64_t>(i * 4 + 2), static_cast<double>(v.v));
    }
    std::memcpy(idx_ptr, indices.data(), indices.size() * sizeof(int32_t));
    return packed;
}

void append_packed_mesh_data(PackedBuiltMeshData& dst, const PackedBuiltMeshData& src,
                             int32_t offset_x, int32_t offset_y, int32_t offset_z) {
    if (src.empty) {
        return;
    }

    const int32_t dst_vertex_offset = dst.vertices.size();
    const int32_t src_vertex_count = src.vertices.size();
    const int32_t src_index_count = src.indices.size();
    const int32_t old_index_count = dst.indices.size();

    dst.vertices.resize(dst_vertex_offset + src_vertex_count);
    dst.custom0.resize(static_cast<size_t>(dst_vertex_offset + src_vertex_count) * 4);
    dst.custom1.resize(static_cast<size_t>(dst_vertex_offset + src_vertex_count) * 4);
    dst.custom2.resize(static_cast<size_t>(dst_vertex_offset + src_vertex_count) * 4);
    dst.indices.resize(old_index_count + src_index_count);

    const Vector3* src_vertices = src.vertices.ptr();
    Vector3* dst_vertices = dst.vertices.ptrw();
    for (int32_t i = 0; i < src_vertex_count; ++i) {
        const Vector3& v = src_vertices[i];
        dst_vertices[dst_vertex_offset + i] = Vector3(
            v.x + static_cast<float>(offset_x),
            v.y + static_cast<float>(offset_y),
            v.z + static_cast<float>(offset_z));
    }

    if (src_vertex_count > 0) {
        std::memcpy(dst.custom0.ptrw() + static_cast<size_t>(dst_vertex_offset) * 4, src.custom0.ptr(), static_cast<size_t>(src_vertex_count) * 4);
        std::memcpy(dst.custom1.ptrw() + static_cast<size_t>(dst_vertex_offset) * 4, src.custom1.ptr(), static_cast<size_t>(src_vertex_count) * 4);
        std::memcpy(dst.custom2.ptrw() + static_cast<size_t>(dst_vertex_offset) * 4, src.custom2.ptr(), static_cast<size_t>(src_vertex_count) * 4);
    }

    const int32_t* src_indices = src.indices.ptr();
    int32_t* dst_indices = dst.indices.ptrw();
    for (int32_t i = 0; i < src_index_count; ++i) {
        dst_indices[old_index_count + i] = src_indices[i] + dst_vertex_offset;
    }

    dst.empty = false;
}

}

struct MeshBuildTask : Task {
    ChunkMap* chunk_map;
    ChunkScheduler* chunk_scheduler;
    std::atomic<uint64_t>* async_epoch;
    ChunkRenderData* render_data;
    int32_t chunk_x, chunk_y, chunk_z;
    uint64_t epoch;
    uint64_t mesh_job_serial;
    int32_t player_bx, player_by, player_bz;
    bool smooth_lighting;
    float detail_level = 1.0f;
    bool far_mode = false;
    uint8_t dirty_subchunks = 0xFF;
    // Partial remeshing: only re-emit the dirty region, carry everything else.
    bool partial = false;
    bool record_quads = false;
    bool greedy_enabled = true;
    // Raw block-level dirty bbox snapshot (inclusive block coords) taken at
    // enqueue. build_mesh_incremental expands it by the AO/light ring itself.
    MeshBuilder::SubChunkBounds dirty_bounds{0, 0, 0, 0, 0, 0};
    bool have_dirty_bounds = false;
    std::vector<CachedQuad> prev_quads;
    MeshLightChecksums prev_light_checksums;

    void execute() override {
        thread_local MeshBuilder builder;
        builder.set_smooth_lighting(smooth_lighting);
        builder.set_detail_level(detail_level);
        builder.set_far_mode(far_mode);
        builder.set_greedy_enabled(greedy_enabled);
        builder.set_quad_recording(record_quads);

        // Narrow work to only dirty sub-chunks
        MeshBuilder::SubChunkBounds bounds;
        bool have_bounds = false;
        if (partial && have_dirty_bounds) {
            // Block-level bbox: a single-block edit remeshes only a ~3³ region.
            bounds = dirty_bounds;
            have_bounds = true;
        } else if (dirty_subchunks != 0xFF) {
            int32_t x_min = CHUNK_WIDTH, x_max = 0;
            int32_t y_min = CHUNK_HEIGHT, y_max = 0;
            int32_t z_min = CHUNK_DEPTH, z_max = 0;
            for (int32_t sx = 0; sx < SUBCHUNK_DIM; ++sx) {
                for (int32_t sy = 0; sy < SUBCHUNK_DIM; ++sy) {
                    for (int32_t sz = 0; sz < SUBCHUNK_DIM; ++sz) {
                        const int32_t idx = sx + sy * SUBCHUNK_DIM + sz * SUBCHUNK_DIM * SUBCHUNK_DIM;
                        if (dirty_subchunks & (1 << idx)) {
                            if (sx * SUBCHUNK_SIZE < x_min) x_min = sx * SUBCHUNK_SIZE;
                            if ((sx + 1) * SUBCHUNK_SIZE > x_max) x_max = (sx + 1) * SUBCHUNK_SIZE;
                            if (sy * SUBCHUNK_SIZE < y_min) y_min = sy * SUBCHUNK_SIZE;
                            if ((sy + 1) * SUBCHUNK_SIZE > y_max) y_max = (sy + 1) * SUBCHUNK_SIZE;
                            if (sz * SUBCHUNK_SIZE < z_min) z_min = sz * SUBCHUNK_SIZE;
                            if ((sz + 1) * SUBCHUNK_SIZE > z_max) z_max = (sz + 1) * SUBCHUNK_SIZE;
                        }
                    }
                }
            }
            if (x_min < x_max && y_min < y_max && z_min < z_max) {
                bounds.x_min = x_min; bounds.x_max = x_max;
                bounds.y_min = y_min; bounds.y_max = y_max;
                bounds.z_min = z_min; bounds.z_max = z_max;
                have_bounds = true;
            }
        } else {
            bounds = {0, CHUNK_WIDTH, 0, CHUNK_HEIGHT, 0, CHUNK_DEPTH};
        }
        builder.set_subchunk_bounds(bounds);

        ChunkRenderData* all_neighbors[26] = {};
        static constexpr int32_t kNeighborOffsets[26][3] = {
            {-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},
            {-1,0,-1},{-1,0,1},{1,0,-1},{1,0,1},
            {-1,-1,0},{1,-1,0},{-1,1,0},{1,1,0},
            {0,-1,-1},{0,-1,1},{0,1,-1},{0,1,1},
            {-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
            {-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}
        };
        auto unpin_neighbors = [&]() {
            if (!chunk_map) return;
            for (int i = 0; i < 26; i++) {
                if (all_neighbors[i]) {
                    chunk_map->unpin_chunk(chunk_map->get_chunk_key(
                        chunk_x + kNeighborOffsets[i][0],
                        chunk_y + kNeighborOffsets[i][1],
                        chunk_z + kNeighborOffsets[i][2]));
                }
            }
        };

        if (chunk_map) {
            chunk_map->get_all_neighbors(chunk_x, chunk_y, chunk_z, all_neighbors);

            // Pin neighbor chunks to prevent unload during build.
            // get_all_neighbors returned raw pointers after releasing shard locks;
            // without pinning, try_unload_chunk could destroy a neighbor while we
            // read its ChunkData. Pinning increments pending_mesh_builds (under shard
            // lock), which try_unload_chunk checks before erasing.
            for (int i = 0; i < 26; i++) {
                if (all_neighbors[i]) {
                    chunk_map->pin_chunk(chunk_map->get_chunk_key(
                        chunk_x + kNeighborOffsets[i][0],
                        chunk_y + kNeighborOffsets[i][1],
                        chunk_z + kNeighborOffsets[i][2]));
                }
            }
        }
        auto data_or_null = [](ChunkRenderData* rd) -> const ChunkData* {
            return (rd && rd->data) ? rd->data.get() : nullptr;
        };

        if (partial && have_bounds && !prev_quads.empty()) {
            MeshBuilder::NeighborPtrs n;
            n.neg_x = data_or_null(all_neighbors[0]);
            n.pos_x = data_or_null(all_neighbors[1]);
            n.neg_y = data_or_null(all_neighbors[2]);
            n.pos_y = data_or_null(all_neighbors[3]);
            n.neg_z = data_or_null(all_neighbors[4]);
            n.pos_z = data_or_null(all_neighbors[5]);
            n.neg_x_neg_z = data_or_null(all_neighbors[6]);
            n.neg_x_pos_z = data_or_null(all_neighbors[7]);
            n.pos_x_neg_z = data_or_null(all_neighbors[8]);
            n.pos_x_pos_z = data_or_null(all_neighbors[9]);
            n.neg_x_neg_y = data_or_null(all_neighbors[10]);
            n.pos_x_neg_y = data_or_null(all_neighbors[11]);
            n.neg_x_pos_y = data_or_null(all_neighbors[12]);
            n.pos_x_pos_y = data_or_null(all_neighbors[13]);
            n.neg_y_neg_z = data_or_null(all_neighbors[14]);
            n.neg_y_pos_z = data_or_null(all_neighbors[15]);
            n.pos_y_neg_z = data_or_null(all_neighbors[16]);
            n.pos_y_pos_z = data_or_null(all_neighbors[17]);
            n.neg_x_neg_y_neg_z = data_or_null(all_neighbors[18]);
            n.pos_x_neg_y_neg_z = data_or_null(all_neighbors[19]);
            n.neg_x_pos_y_neg_z = data_or_null(all_neighbors[20]);
            n.pos_x_pos_y_neg_z = data_or_null(all_neighbors[21]);
            n.neg_x_neg_y_pos_z = data_or_null(all_neighbors[22]);
            n.pos_x_neg_y_pos_z = data_or_null(all_neighbors[23]);
            n.neg_x_pos_y_pos_z = data_or_null(all_neighbors[24]);
            n.pos_x_pos_y_pos_z = data_or_null(all_neighbors[25]);
            builder.build_mesh_incremental(
                *render_data->data, prev_quads, prev_light_checksums, bounds, n);
        } else {
            builder.build_mesh(
                *render_data->data,
                data_or_null(all_neighbors[0]),  // neg_x
                data_or_null(all_neighbors[1]),  // pos_x
                data_or_null(all_neighbors[2]),  // neg_y
                data_or_null(all_neighbors[3]),  // pos_y
                data_or_null(all_neighbors[4]),  // neg_z
                data_or_null(all_neighbors[5]),  // pos_z
                data_or_null(all_neighbors[6]),  // neg_x_neg_z
                data_or_null(all_neighbors[7]),  // neg_x_pos_z
                data_or_null(all_neighbors[8]),  // pos_x_neg_z
                data_or_null(all_neighbors[9]),  // pos_x_pos_z
                data_or_null(all_neighbors[10]), // neg_x_neg_y
                data_or_null(all_neighbors[11]), // pos_x_neg_y
                data_or_null(all_neighbors[12]), // neg_x_pos_y
                data_or_null(all_neighbors[13]), // pos_x_pos_y
                data_or_null(all_neighbors[14]), // neg_y_neg_z
                data_or_null(all_neighbors[15]), // neg_y_pos_z
                data_or_null(all_neighbors[16]), // pos_y_neg_z
                data_or_null(all_neighbors[17]), // pos_y_pos_z
                data_or_null(all_neighbors[18]), // neg_x_neg_y_neg_z
                data_or_null(all_neighbors[19]), // pos_x_neg_y_neg_z
                data_or_null(all_neighbors[20]), // neg_x_pos_y_neg_z
                data_or_null(all_neighbors[21]), // pos_x_pos_y_neg_z
                data_or_null(all_neighbors[22]), // neg_x_neg_y_pos_z
                data_or_null(all_neighbors[23]), // pos_x_neg_y_pos_z
                data_or_null(all_neighbors[24]), // neg_x_pos_y_pos_z
                data_or_null(all_neighbors[25])  // pos_x_pos_y_pos_z
            );
        }

        PackedBuiltMeshData packed_mesh = pack_vertex_array(builder.get_vertices(), builder.get_indices());
        PackedBuiltMeshData water_mesh = pack_vertex_array(builder.get_water_vertices(), builder.get_water_indices());

        // Content hash for upload deduplication (opaque only; water always uploaded)
        uint64_t content_hash;
        if (builder.get_vertices().empty() || builder.get_indices().empty()) {
            content_hash = 0;
        } else {
            content_hash = fnv1a_hash_bytes(builder.get_vertices().data(), builder.get_vertices().size() * sizeof(Vertex));
            content_hash = fnv1a_hash_bytes(builder.get_indices().data(), builder.get_indices().size() * sizeof(uint32_t), content_hash);
        }

        if (async_epoch && epoch != async_epoch->load(std::memory_order_acquire)) {
            render_data->pending_mesh_builds.fetch_sub(1, std::memory_order_relaxed);
            render_data->pending_mesh_uploads.fetch_sub(1, std::memory_order_relaxed);
            unpin_neighbors();
            return;
        }

        CompletedMesh completed;
        completed.chunk_x = chunk_x;
        completed.chunk_y = chunk_y;
        completed.chunk_z = chunk_z;
        completed.epoch = epoch;
        completed.mesh_job_serial = mesh_job_serial;
        completed.source_chunk = render_data;
        completed.mesh_data = std::move(packed_mesh);
        completed.water_mesh_data = std::move(water_mesh);
        completed.mesh_content_hash = content_hash;
        completed.detail_level = detail_level;
        completed.quads = builder.get_quads();
        completed.light_checksums = builder.get_light_checksums();
        completed.greedy_mode = greedy_enabled;
        completed.dirty_subchunks = dirty_subchunks;

        chunk_scheduler->push_completed_mesh(std::move(completed), high_priority);

        render_data->pending_mesh_builds.fetch_sub(1, std::memory_order_relaxed);
        unpin_neighbors();
    }

    bool high_priority;
};

void MeshManager::set_player_chunk(int32_t cx, int32_t cy, int32_t cz) {
    last_player_chunk_x = cx;
    last_player_chunk_y = cy;
    last_player_chunk_z = cz;
    refresh_far_region_visibility();
}

void MeshManager::hide_chunk_instance(ChunkRenderData* render_data) {
    if (!render_data || !render_data->instance_rid.is_valid()) {
        return;
    }
    RenderingServer::get_singleton()->instance_set_visible(render_data->instance_rid, false);
}

void MeshManager::show_chunk_instance(ChunkRenderData* render_data, int32_t cx, int32_t cy, int32_t cz) {
    if (!render_data || !render_data->mesh_rid.is_valid()) {
        return;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!render_data->instance_rid.is_valid()) {
        render_data->instance_rid = rs->instance_create();
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
        rs->instance_geometry_set_cast_shadows_setting(
            render_data->instance_rid,
            RenderingServer::SHADOW_CASTING_SETTING_OFF);
        rs->instance_set_custom_aabb(
            render_data->instance_rid,
            AABB(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH)));
        Transform3D transform;
        transform.origin = Vector3(cx * CHUNK_WIDTH, cy * CHUNK_HEIGHT, cz * CHUNK_DEPTH);
        rs->instance_set_transform(render_data->instance_rid, transform);
        if (owner) {
            Node3D* owner3d = Object::cast_to<Node3D>(owner);
            if (owner3d) {
                Ref<World3D> world = owner3d->get_world_3d();
                if (world.is_valid()) {
                    rs->instance_set_scenario(render_data->instance_rid, world->get_scenario());
                }
            }
        }
    } else {
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
    }
    rs->instance_set_visible(render_data->instance_rid, true);
}

bool MeshManager::is_chunk_within_render_distance(int32_t cx, int32_t cy, int32_t cz) const {
    if (mesh_render_distance <= 0 || last_player_chunk_x == INT32_MIN) {
        return true;
    }
    const int32_t dx = cx - last_player_chunk_x;
    const int32_t dz = cz - last_player_chunk_z;
    const int32_t dy = std::abs(cy - last_player_chunk_y);
    return (dx * dx + dz * dz) <= (mesh_render_distance * mesh_render_distance) && dy <= 10;
}

bool MeshManager::should_use_far_region_for_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    return compute_chunk_detail_level(cx, cy, cz) < 1.0f && is_chunk_within_render_distance(cx, cy, cz);
}

uint64_t MeshManager::get_far_region_key(int32_t cx, int32_t cy, int32_t cz) const {
    if (!chunk_map) {
        return 0;
    }
    const int32_t rx = floor_div(cx, kFarRegionSizeXZ);
    const int32_t rz = floor_div(cz, kFarRegionSizeXZ);
    return chunk_map->get_chunk_key(rx, cy, rz);
}

bool MeshManager::is_far_region_active_for_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    if (!should_use_far_region_for_chunk(cx, cy, cz)) {
        return false;
    }
    const uint64_t chunk_key = chunk_map ? chunk_map->get_chunk_key(cx, cy, cz) : 0;
    const auto it = far_regions.find(get_far_region_key(cx, cy, cz));
    if (it == far_regions.end() || !it->second.active) {
        return false;
    }
    const auto& members = it->second.active_chunk_keys;
    return std::find(members.begin(), members.end(), chunk_key) != members.end();
}

void MeshManager::free_far_region_resources(FarRegionRenderData& region) {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (region.instance_rid.is_valid()) {
        rs->free_rid(region.instance_rid);
        region.instance_rid = RID();
    }
    if (region.mesh_rid.is_valid()) {
        rs->free_rid(region.mesh_rid);
        region.mesh_rid = RID();
    }
    region.active = false;
}

void MeshManager::ensure_far_region_instance(FarRegionRenderData& region, uint64_t region_key, bool visible) {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!visible || !region.mesh_rid.is_valid()) {
        if (region.instance_rid.is_valid()) {
            rs->instance_set_visible(region.instance_rid, false);
        }
        region.active = false;
        return;
    }

    if (!region.instance_rid.is_valid()) {
        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);

        region.instance_rid = rs->instance_create();
        rs->instance_set_base(region.instance_rid, region.mesh_rid);
        rs->instance_geometry_set_cast_shadows_setting(
            region.instance_rid,
            RenderingServer::SHADOW_CASTING_SETTING_OFF);
        AABB region_aabb(
            Vector3(0, 0, 0),
            Vector3(kFarRegionSizeXZ * CHUNK_WIDTH, CHUNK_HEIGHT, kFarRegionSizeXZ * CHUNK_DEPTH));
        rs->instance_set_custom_aabb(region.instance_rid, region_aabb);

        Transform3D transform;
        transform.origin = Vector3(
            rx * kFarRegionSizeXZ * CHUNK_WIDTH,
            ry * CHUNK_HEIGHT,
            rz * kFarRegionSizeXZ * CHUNK_DEPTH);
        rs->instance_set_transform(region.instance_rid, transform);

        if (owner) {
            Node3D* owner3d = Object::cast_to<Node3D>(owner);
            if (owner3d) {
                Ref<World3D> world = owner3d->get_world_3d();
                if (world.is_valid()) {
                    rs->instance_set_scenario(region.instance_rid, world->get_scenario());
                }
            }
        }
    }

    rs->instance_set_visible(region.instance_rid, true);
    region.active = true;
}

void MeshManager::sync_far_region_members_visibility(FarRegionRenderData& region) {
    if (!chunk_map) {
        return;
    }
    for (uint64_t chunk_key : region.active_chunk_keys) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (!render_data || !render_data->instance_rid.is_valid()) {
            continue;
        }
        const bool show_chunk = is_chunk_within_render_distance(cx, cy, cz) &&
                                !(region.active && should_use_far_region_for_chunk(cx, cy, cz));
        if (show_chunk) {
            show_chunk_instance(render_data, cx, cy, cz);
        } else {
            hide_chunk_instance(render_data);
        }
    }
}

void MeshManager::refresh_far_region_visibility() {
    for (auto& [region_key, region] : far_regions) {
        bool any_visible_members = false;
        for (uint64_t chunk_key : region.active_chunk_keys) {
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
            if (should_use_far_region_for_chunk(cx, cy, cz)) {
                any_visible_members = true;
                break;
            }
        }
        ensure_far_region_instance(region, region_key, any_visible_members);
        sync_far_region_members_visibility(region);
    }
}

void MeshManager::mark_far_region_dirty_for_chunk(int32_t cx, int32_t cy, int32_t cz) {
    // Debounce: record when the region became dirty and bump the revision.
    // process_far_region_queue skips regions marked recently so a burst of
    // child-cache arrivals (initial streaming) coalesces into one rebuild
    // instead of one rebuild per child.
    FarRegionRenderData& region = far_regions[get_far_region_key(cx, cy, cz)];
    region.last_dirty_at = std::chrono::steady_clock::now();
    region.dirty = true;
    ++region.revision;
}

void MeshManager::process_completed_meshes(uint64_t epoch, double budget_ms, int32_t max_uploads,
                                           const Ref<ShaderMaterial>& material,
                                           const Ref<ShaderMaterial>& water_material) {
    if (!chunk_scheduler || !chunk_map) return;

    int32_t dynamic_max_uploads = max_uploads;
    int32_t uploads_this_frame = 0;
    int32_t region_upload_budget = dynamic_max_uploads / kFarRegionUploadDivisor;
    if (dynamic_max_uploads > 1) {
        region_upload_budget = std::max(1, region_upload_budget);
    }
    region_upload_budget = std::min(region_upload_budget, dynamic_max_uploads);
    const int32_t chunk_upload_budget = std::max(0, dynamic_max_uploads - region_upload_budget);

    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);

    if (chunk_scheduler->completed_mesh_count() <= 0) {
        process_completed_region_meshes(epoch, region_upload_budget, material, water_material);
        return;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    const auto start_time = std::chrono::high_resolution_clock::now();

    while (uploads_this_frame < chunk_upload_budget) {
        // Strict frame budget: stop once the allowed wall time is exceeded so a
        // large completion backlog cannot stall the main thread.
        const auto current_time = std::chrono::high_resolution_clock::now();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(current_time - start_time).count();
        if (elapsed_ms >= budget_ms) break;

        bool high_priority = false;
        CompletedMesh completed;
        bool got_completion;
        if (last_player_chunk_x == INT32_MIN) {
            got_completion = chunk_scheduler->poll_completed_mesh(completed, high_priority);
        } else {
            // Nearest-first: spend the frame's few uploads on the most visible
            // chunks. Stale entries (wrong epoch) are dropped during the scan.
            got_completion = chunk_scheduler->poll_completed_mesh_nearest(
                last_player_chunk_x, last_player_chunk_y, last_player_chunk_z, epoch, completed, high_priority);
        }
        if (!got_completion) break;

        if (completed.epoch != epoch || !completed.source_chunk) {
            uploads_this_frame++;
            continue;
        }

        ChunkRenderData* render_data = completed.source_chunk;
        render_data->pending_mesh_uploads.fetch_sub(1, std::memory_order_relaxed);
        if (render_data->mesh_job_serial.load(std::memory_order_acquire) != completed.mesh_job_serial) {
            continue;
        }

        // Partial-remesh bookkeeping: adopt the new quad cache + light checksums,
        // and clear only the dirty sub-chunks covered by this build's snapshot.
        // Edits that landed while the build was in flight remain marked.
        render_data->cached_quads = std::move(completed.quads);
        render_data->light_checksums = completed.light_checksums;
        render_data->last_built_greedy_mode = completed.greedy_mode;
        render_data->dirty_subchunks &= ~completed.dirty_subchunks;

        if (completed.mesh_data.empty && completed.water_mesh_data.empty) {
            if (render_data->mesh_rid.is_valid()) {
                rs->free_rid(render_data->mesh_rid);
                render_data->mesh_rid = RID();
            }
            if (render_data->instance_rid.is_valid()) {
                rs->free_rid(render_data->instance_rid);
                render_data->instance_rid = RID();
            }
            const bool had_far_cache = static_cast<bool>(render_data->far_mesh_cache);
            render_data->far_mesh_cache.reset();
            if (had_far_cache) {
                mark_far_region_dirty_for_chunk(completed.chunk_x, completed.chunk_y, completed.chunk_z);
            }
            uploads_this_frame++;
            continue;
        }

        // 3.1 Upload deduplication: skip GPU upload if content hash unchanged (first upload always goes through)
        const bool content_unchanged = render_data->mesh_content_hash != 0 &&
                                       render_data->mesh_content_hash == completed.mesh_content_hash;

        if (!content_unchanged) {
            // Reuse mesh RID instead of creating a new one every frame
            if (!render_data->mesh_rid.is_valid()) {
                render_data->mesh_rid = rs->mesh_create();
                render_data->material_set = false;
                AABB chunk_aabb(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH));
                rs->mesh_set_custom_aabb(render_data->mesh_rid, chunk_aabb);
            } else {
                rs->mesh_clear(render_data->mesh_rid);
                render_data->material_set = false;
            }

            int64_t fmt = 0;
            fmt |= RenderingServer::ARRAY_FORMAT_VERTEX;
            fmt |= RenderingServer::ARRAY_FORMAT_INDEX;
            fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM0;
            fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM) << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT;
            fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM1;
            fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM) << RenderingServer::ARRAY_FORMAT_CUSTOM1_SHIFT;
            fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM2;
            fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RG_HALF) << RenderingServer::ARRAY_FORMAT_CUSTOM2_SHIFT;
            fmt |= RenderingServer::ARRAY_FLAG_COMPRESS_ATTRIBUTES;

            // Surface 0: opaque
            int surface_index = 0;
            if (!completed.mesh_data.empty) {
                arrays[Mesh::ARRAY_VERTEX] = completed.mesh_data.vertices;
                arrays[Mesh::ARRAY_INDEX] = completed.mesh_data.indices;
                arrays[Mesh::ARRAY_CUSTOM0] = completed.mesh_data.custom0;
                arrays[Mesh::ARRAY_CUSTOM1] = completed.mesh_data.custom1;
                arrays[Mesh::ARRAY_CUSTOM2] = completed.mesh_data.custom2;

                if (perf_timer) {
                    ScopedTimer t(*perf_timer, TimerID::MeshUploadGpu);
                    rs->mesh_add_surface_from_arrays(render_data->mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
                } else {
                    rs->mesh_add_surface_from_arrays(render_data->mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
                }

                if (material.is_valid()) {
                    rs->mesh_surface_set_material(render_data->mesh_rid, surface_index, material->get_rid());
                }
                surface_index++;
            }

            // Surface 1: water (if present)
            if (!completed.water_mesh_data.empty) {
                arrays[Mesh::ARRAY_VERTEX] = completed.water_mesh_data.vertices;
                arrays[Mesh::ARRAY_INDEX] = completed.water_mesh_data.indices;
                arrays[Mesh::ARRAY_CUSTOM0] = completed.water_mesh_data.custom0;
                arrays[Mesh::ARRAY_CUSTOM1] = completed.water_mesh_data.custom1;
                arrays[Mesh::ARRAY_CUSTOM2] = completed.water_mesh_data.custom2;

                if (perf_timer) {
                    ScopedTimer t(*perf_timer, TimerID::MeshUploadGpu);
                    rs->mesh_add_surface_from_arrays(render_data->mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
                } else {
                    rs->mesh_add_surface_from_arrays(render_data->mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
                }

                if (water_material.is_valid()) {
                    rs->mesh_surface_set_material(render_data->mesh_rid, surface_index, water_material->get_rid());
                }
            }

            render_data->material_set = true;
            render_data->mesh_content_hash = completed.mesh_content_hash;
        }

        // 4.4 Instance budget cap: don't create instances for chunks beyond render distance
        // Uses same 2D horizontal distance + vertical buffer logic as the LOD classifier
        const bool cache_for_far_region = completed.detail_level < 1.0f;
        if (cache_for_far_region) {
            auto far_cache = std::make_shared<CachedFarChunkMesh>();
            far_cache->mesh_data = completed.mesh_data;
            far_cache->water_mesh_data = completed.water_mesh_data;
            render_data->far_mesh_cache = std::move(far_cache);
            mark_far_region_dirty_for_chunk(completed.chunk_x, completed.chunk_y, completed.chunk_z);
        } else if (render_data->far_mesh_cache) {
            render_data->far_mesh_cache.reset();
            mark_far_region_dirty_for_chunk(completed.chunk_x, completed.chunk_y, completed.chunk_z);
        }

        const bool show_instance =
            is_chunk_within_render_distance(completed.chunk_x, completed.chunk_y, completed.chunk_z) &&
            !is_far_region_active_for_chunk(completed.chunk_x, completed.chunk_y, completed.chunk_z);
        if (render_data->instance_rid.is_valid()) {
            rs->instance_geometry_set_cast_shadows_setting(
                render_data->instance_rid,
                RenderingServer::SHADOW_CASTING_SETTING_OFF);
            rs->instance_set_visible(render_data->instance_rid, show_instance);
        } else if (show_instance) {
            render_data->instance_rid = rs->instance_create();
            rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
            rs->instance_geometry_set_cast_shadows_setting(
                render_data->instance_rid,
                RenderingServer::SHADOW_CASTING_SETTING_OFF);
            AABB chunk_aabb(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH));
            rs->instance_set_custom_aabb(render_data->instance_rid, chunk_aabb);
            Transform3D transform;
            transform.origin = Vector3(completed.chunk_x * CHUNK_WIDTH, completed.chunk_y * CHUNK_HEIGHT, completed.chunk_z * CHUNK_DEPTH);
            rs->instance_set_transform(render_data->instance_rid, transform);
            if (owner) {
                Node3D* owner3d = Object::cast_to<Node3D>(owner);
                if (owner3d) {
                    Ref<World3D> world = owner3d->get_world_3d();
                    if (world.is_valid()) {
                        rs->instance_set_scenario(render_data->instance_rid, world->get_scenario());
                    }
                }
            }
            rs->instance_set_visible(render_data->instance_rid, true);
        }

        if (completed.chunk_y > 0) {
            queue_dirty_chunk(completed.chunk_x, completed.chunk_y - 1, completed.chunk_z);
        }

        uploads_this_frame++;
    }

    process_completed_region_meshes(epoch, region_upload_budget, material, water_material);
}

void MeshManager::process_completed_region_meshes(uint64_t epoch, int32_t max_uploads,
                                                  const Ref<ShaderMaterial>& material,
                                                  const Ref<ShaderMaterial>& water_material) {
    if (max_uploads <= 0) {
        return;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    int32_t uploads = 0;

    while (uploads < max_uploads) {
        CompletedRegionMesh completed;
        {
            std::lock_guard<std::mutex> lock(completed_far_region_meshes_mutex);
            if (completed_far_region_meshes.empty()) {
                break;
            }
            completed = std::move(completed_far_region_meshes.front());
            completed_far_region_meshes.pop();
            completed_far_region_mesh_count.fetch_sub(1, std::memory_order_relaxed);
        }

        auto it = far_regions.find(completed.region_key);
        if (it == far_regions.end()) {
            continue;
        }

        FarRegionRenderData& region = it->second;
        region.pending_builds.fetch_sub(1, std::memory_order_relaxed);
        if (completed.epoch != epoch || completed.revision != region.revision) {
            continue;
        }

        std::vector<uint64_t> previous_members = region.active_chunk_keys;

        if (completed.member_chunk_keys.size() < 2 ||
            (completed.mesh_data.empty && completed.water_mesh_data.empty)) {
            ensure_far_region_instance(region, completed.region_key, false);
            region.active_chunk_keys.clear();
            for (uint64_t old_key : previous_members) {
                int32_t cx, cy, cz;
                ChunkMap::decode_chunk_key(old_key, cx, cy, cz);
                ChunkRenderData* render_data = chunk_map ? chunk_map->get_chunk_render_data(cx, cy, cz) : nullptr;
                if (render_data && render_data->mesh_rid.is_valid() && is_chunk_within_render_distance(cx, cy, cz)) {
                    show_chunk_instance(render_data, cx, cy, cz);
                }
            }
            free_far_region_resources(region);
            continue;
        }

        if (!region.mesh_rid.is_valid()) {
            region.mesh_rid = rs->mesh_create();
        } else {
            rs->mesh_clear(region.mesh_rid);
        }
        rs->mesh_set_custom_aabb(
            region.mesh_rid,
            AABB(Vector3(0, 0, 0), Vector3(kFarRegionSizeXZ * CHUNK_WIDTH, CHUNK_HEIGHT, kFarRegionSizeXZ * CHUNK_DEPTH)));

        int64_t fmt = 0;
        fmt |= RenderingServer::ARRAY_FORMAT_VERTEX;
        fmt |= RenderingServer::ARRAY_FORMAT_INDEX;
        fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM0;
        fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM) << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT;
        fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM1;
        fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM) << RenderingServer::ARRAY_FORMAT_CUSTOM1_SHIFT;
        fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM2;
        fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RG_HALF) << RenderingServer::ARRAY_FORMAT_CUSTOM2_SHIFT;
        fmt |= RenderingServer::ARRAY_FLAG_COMPRESS_ATTRIBUTES;

        int surface_index = 0;
        if (!completed.mesh_data.empty) {
            arrays[Mesh::ARRAY_VERTEX] = completed.mesh_data.vertices;
            arrays[Mesh::ARRAY_INDEX] = completed.mesh_data.indices;
            arrays[Mesh::ARRAY_CUSTOM0] = completed.mesh_data.custom0;
            arrays[Mesh::ARRAY_CUSTOM1] = completed.mesh_data.custom1;
            arrays[Mesh::ARRAY_CUSTOM2] = completed.mesh_data.custom2;
            if (perf_timer) {
                ScopedTimer t(*perf_timer, TimerID::MeshUploadGpu);
                rs->mesh_add_surface_from_arrays(region.mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
            } else {
                rs->mesh_add_surface_from_arrays(region.mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
            }
            if (material.is_valid()) {
                rs->mesh_surface_set_material(region.mesh_rid, surface_index, material->get_rid());
            }
            ++surface_index;
        }
        if (!completed.water_mesh_data.empty) {
            arrays[Mesh::ARRAY_VERTEX] = completed.water_mesh_data.vertices;
            arrays[Mesh::ARRAY_INDEX] = completed.water_mesh_data.indices;
            arrays[Mesh::ARRAY_CUSTOM0] = completed.water_mesh_data.custom0;
            arrays[Mesh::ARRAY_CUSTOM1] = completed.water_mesh_data.custom1;
            arrays[Mesh::ARRAY_CUSTOM2] = completed.water_mesh_data.custom2;
            if (perf_timer) {
                ScopedTimer t(*perf_timer, TimerID::MeshUploadGpu);
                rs->mesh_add_surface_from_arrays(region.mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
            } else {
                rs->mesh_add_surface_from_arrays(region.mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), BitField<RenderingServer::ArrayFormat>(fmt));
            }
            if (water_material.is_valid()) {
                rs->mesh_surface_set_material(region.mesh_rid, surface_index, water_material->get_rid());
            }
        }

        region.active_chunk_keys = completed.member_chunk_keys;
        bool any_visible_members = false;
        for (uint64_t chunk_key : region.active_chunk_keys) {
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
            if (should_use_far_region_for_chunk(cx, cy, cz)) {
                any_visible_members = true;
                break;
            }
        }
        ensure_far_region_instance(region, completed.region_key, any_visible_members);

        for (uint64_t old_key : previous_members) {
            if (std::find(region.active_chunk_keys.begin(), region.active_chunk_keys.end(), old_key) != region.active_chunk_keys.end()) {
                continue;
            }
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(old_key, cx, cy, cz);
            ChunkRenderData* render_data = chunk_map ? chunk_map->get_chunk_render_data(cx, cy, cz) : nullptr;
            if (render_data && render_data->mesh_rid.is_valid() && is_chunk_within_render_distance(cx, cy, cz)) {
                show_chunk_instance(render_data, cx, cy, cz);
            }
        }
        sync_far_region_members_visibility(region);
        ++uploads;
    }
}

static inline bool should_cull_neighbor(BlockID current, BlockID neighbor, FaceDirection direction, const BlockRegistry& registry) {
    if (neighbor == BlockIDs::AIR) {
         return false;
    }
    const BlockType& neighbor_type = registry.get_block(neighbor);
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) {
        if (current != neighbor) return false;
    }
    const BlockType& current_type = registry.get_block(current);
    if (current == neighbor && current_type.cull_against_same) return true;
    if (direction == FaceDirection::Right || direction == FaceDirection::Left ||
        direction == FaceDirection::Front || direction == FaceDirection::Back) {
        float current_height = 1.0f - current_type.top_face_offset;
        float neighbor_height = 1.0f - neighbor_type.top_face_offset;
        if (neighbor_height < current_height) return false;
        if (neighbor_height > current_height) return true;
    }
    return true;
}

void MeshManager::rebuild_rendering_server_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                                   ChunkRenderData* render_data,
                                                   ChunkRenderData* d_x_neg,
                                                   ChunkRenderData* d_x_pos,
                                                   ChunkRenderData* d_y_neg,
                                                   ChunkRenderData* d_y_pos,
                                                   ChunkRenderData* d_z_neg,
                                                   ChunkRenderData* d_z_pos) {
    if (!render_data || !render_data->is_mesh_dirty) return;
    if (!thread_pool || !chunk_scheduler || !chunk_map) return;

    // Skip mesh builds for chunks beyond render distance + 2 (allows unload to proceed)
    if (mesh_render_distance > 0 && last_player_chunk_x != INT32_MIN) {
        int32_t dx = chunk_x - last_player_chunk_x;
        int32_t dz = chunk_z - last_player_chunk_z;
        int32_t dy = std::abs(chunk_y - last_player_chunk_y);
        if (dx*dx + dz*dz > (mesh_render_distance + 2) * (mesh_render_distance + 2) || dy > 10) {
            render_data->is_mesh_dirty = false;
            render_data->dirty_subchunks = 0;
            render_data->reset_dirty_bbox();
            return;
        }
    }

    // 1.5 Version check: skip enqueuing if versions match
    bool needs_enqueue = false;
    bool own_version_changed = false;
    bool neighbor_version_changed = false;
    if (render_data->mesh_version != render_data->last_built_version) {
        needs_enqueue = true;
        own_version_changed = true;
    } else {
        ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
        for (int i = 0; i < 6; ++i) {
            uint32_t n_ver = neighbors[i] ? neighbors[i]->mesh_version : 0;
            if (n_ver != render_data->last_built_neighbor_versions[i]) {
                needs_enqueue = true;
                neighbor_version_changed = true;
                break;
            }
        }
    }

    if (!needs_enqueue) {
        render_data->is_mesh_dirty = false;
        render_data->dirty_subchunks = 0;
        render_data->reset_dirty_bbox();
        return;
    }

    // Occlusion culling: skip mesh generation for completely invisible chunks
    if (!render_data->data || render_data->data->is_all_air()) {
        if (render_data->mesh_rid.is_valid()) {
            RenderingServer* rs = RenderingServer::get_singleton();
            RID old_mesh = render_data->mesh_rid;
            rs->free_rid(old_mesh);
            render_data->mesh_rid = RID();
        }
        if (render_data->instance_rid.is_valid()) {
            RenderingServer* rs = RenderingServer::get_singleton();
            rs->free_rid(render_data->instance_rid);
            render_data->instance_rid = RID();
        }
        render_data->is_mesh_dirty = false;
        render_data->dirty_subchunks = 0;
        render_data->reset_dirty_bbox();
        render_data->cached_quads.clear();
        render_data->light_checksums = {};

        render_data->last_built_version = render_data->mesh_version;
        ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
        for (int i = 0; i < 6; ++i) {
            render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
        }
        return;
    }

    if (render_data->data && render_data->data->fully_solid()) {
        const auto neighbor_fully_solid = [](ChunkRenderData* n) {
            return n && n->data && n->data->fully_solid();
        };
        const bool buried =
            neighbor_fully_solid(d_x_neg) &&
            neighbor_fully_solid(d_x_pos) &&
            neighbor_fully_solid(d_y_pos) &&
            neighbor_fully_solid(d_z_neg) &&
            neighbor_fully_solid(d_z_pos) &&
            (neighbor_fully_solid(d_y_neg) || chunk_y == 0);
        if (buried) {
            if (render_data->mesh_rid.is_valid()) {
                RenderingServer* rs = RenderingServer::get_singleton();
                rs->free_rid(render_data->mesh_rid);
                render_data->mesh_rid = RID();
            }
            if (render_data->instance_rid.is_valid()) {
                RenderingServer* rs = RenderingServer::get_singleton();
                rs->free_rid(render_data->instance_rid);
                render_data->instance_rid = RID();
            }
            render_data->is_mesh_dirty = false;
            render_data->dirty_subchunks = 0;
            render_data->reset_dirty_bbox();
            render_data->cached_quads.clear();
            render_data->light_checksums = {};

            render_data->last_built_version = render_data->mesh_version;
            ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
            for (int i = 0; i < 6; ++i) {
                render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
            }
            return;
        }
    }

    if (render_data->pending_mesh_builds.load(std::memory_order_acquire) > 0) {
        queue_immediate_dirty_chunk(chunk_x, chunk_y, chunk_z);
        return;
    }
    render_data->is_mesh_dirty = false;
    render_data->pending_mesh_builds.fetch_add(1, std::memory_order_relaxed);
    render_data->pending_mesh_uploads.fetch_add(1, std::memory_order_relaxed);
    const uint64_t mesh_job_serial = render_data->mesh_job_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    const float detail = compute_chunk_detail_level(chunk_x, chunk_y, chunk_z);
    render_data->last_built_detail_level = detail;
    uint64_t key = chunk_map->get_chunk_key(chunk_x, chunk_y, chunk_z);
    if (detail >= 1.0f) {
        if (lod_distance > 0 && last_player_chunk_x != INT32_MIN) {
            int32_t dx = chunk_x - last_player_chunk_x;
            int32_t dy = chunk_y - last_player_chunk_y;
            int32_t dz = chunk_z - last_player_chunk_z;
            int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
            if (dist <= lod_distance + 2) {
                active_full_detail_chunks_.insert(key);
            }
        }
    } else {
        active_full_detail_chunks_.erase(key);
    }
    const bool high_priority = mesh_queue.erase_urgent(key);

    // Update last built versions
    render_data->last_built_version = render_data->mesh_version;
    ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
    for (int i = 0; i < 6; ++i) {
        render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
    }

    const int32_t player_bx = last_player_block_x;
    const int32_t player_by = last_player_block_y;
    const int32_t player_bz = last_player_block_z;

    const bool far_mode = detail < 1.0f && is_chunk_far_mode(chunk_x, chunk_y, chunk_z);
    render_data->last_built_far_mode = far_mode;

    // Greedy vs per-face fallback (mirrors the worker's old decision).
    const bool player_known = player_bx != INT32_MIN && player_by != INT32_MIN && player_bz != INT32_MIN;
    int32_t pdx = 0, pdy = 0, pdz = 0;
    if (player_known) {
        int32_t cmin_x = chunk_x * CHUNK_WIDTH;
        int32_t cmax_x = cmin_x + CHUNK_WIDTH - 1;
        int32_t cmin_y = chunk_y * CHUNK_HEIGHT;
        int32_t cmax_y = cmin_y + CHUNK_HEIGHT - 1;
        int32_t cmin_z = chunk_z * CHUNK_DEPTH;
        int32_t cmax_z = cmin_z + CHUNK_DEPTH - 1;
        pdx = player_bx < cmin_x ? cmin_x - player_bx : player_bx > cmax_x ? player_bx - cmax_x : 0;
        pdy = player_by < cmin_y ? cmin_y - player_by : player_by > cmax_y ? player_by - cmax_y : 0;
        pdz = player_bz < cmin_z ? cmin_z - player_bz : player_bz > cmax_z ? player_bz - cmax_z : 0;
    }
    const bool greedy_enabled = !(player_known && std::max(pdx, std::max(pdy, pdz)) <= kGreedyDisableBlockRadius);

    // Only near-player chunks record quad caches (they are the only ones edited).
    const bool record_quads =
        player_known &&
        std::max(std::abs(chunk_x - last_player_chunk_x),
                 std::max(std::abs(chunk_y - last_player_chunk_y),
                          std::abs(chunk_z - last_player_chunk_z))) <= 2;

    // Partial remesh eligibility: own edits only (no neighbor edits), a partial
    // dirty mask, an existing quad cache, and unchanged mode/detail from the
    // build that produced that cache.
    const bool partial =
        record_quads && own_version_changed && !neighbor_version_changed &&
        render_data->dirty_subchunks != 0 && render_data->dirty_subchunks != 0xFF &&
        render_data->has_dirty_bbox() &&
        !render_data->cached_quads.empty() &&
        detail >= 1.0f && !far_mode &&
        render_data->last_built_detail_level == detail &&
        render_data->last_built_far_mode == far_mode &&
        render_data->last_built_greedy_mode == greedy_enabled;

    // Do NOT capture raw neighbor pointers. The worker looks up neighbors by coordinate
    // at build time. If a neighbor was unloaded, the mesh builder sees nullptr (air).
    // Only the center chunk is pinned via pending_mesh_builds.
    auto mesh_task = std::make_unique<MeshBuildTask>();
    mesh_task->chunk_map = chunk_map;
    mesh_task->chunk_scheduler = chunk_scheduler;
    mesh_task->async_epoch = async_epoch;
    mesh_task->render_data = render_data;
    mesh_task->chunk_x = chunk_x;
    mesh_task->chunk_y = chunk_y;
    mesh_task->chunk_z = chunk_z;
    mesh_task->epoch = epoch;
    mesh_task->mesh_job_serial = mesh_job_serial;
    mesh_task->player_bx = player_bx;
    mesh_task->player_by = player_by;
    mesh_task->player_bz = player_bz;
    mesh_task->high_priority = high_priority;
    mesh_task->smooth_lighting = smooth_lighting_enabled;
    mesh_task->detail_level = detail;
    mesh_task->far_mode = far_mode;
    mesh_task->greedy_enabled = greedy_enabled;
    mesh_task->record_quads = record_quads;
    mesh_task->partial = partial;
    mesh_task->prev_quads = render_data->cached_quads;
    mesh_task->prev_light_checksums = render_data->light_checksums;
    mesh_task->dirty_subchunks = render_data->dirty_subchunks;
    // NOTE: render_data->dirty_subchunks is NOT cleared here. It is cleared at
    // apply time (process_completed_meshes) with the snapshot mask, so edits that
    // land while the build is in flight stay marked for the next build.

    // Block-level dirty bbox: snapshot into the task, then clear it so later
    // edits accumulate their own bbox. The bitmask + mesh_version deferral above
    // still catches edits that land mid-build (they re-open their own bbox and
    // force a rebuild via the version mismatch).
    mesh_task->have_dirty_bounds = render_data->has_dirty_bbox();
    if (mesh_task->have_dirty_bounds) {
        mesh_task->dirty_bounds = {
            render_data->dirty_min_x,
            render_data->dirty_max_x + 1,
            render_data->dirty_min_y,
            render_data->dirty_max_y + 1,
            render_data->dirty_min_z,
            render_data->dirty_max_z + 1};
    }
    render_data->reset_dirty_bbox();

    thread_pool->enqueue_task(std::move(mesh_task), high_priority);
}

void MeshManager::rebuild_chunk_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch) {
    if (!chunk_map) return;
    ChunkRenderData* chunk_render_data = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z);
    if (!chunk_render_data) return;
    chunk_render_data->is_mesh_dirty = true;

    ChunkRenderData* neighbors[6] = {};
    chunk_map->get_neighbors(chunk_x, chunk_y, chunk_z, neighbors);

    rebuild_rendering_server_mesh(chunk_x, chunk_y, chunk_z, epoch, chunk_render_data,
                                  neighbors[0], neighbors[1],
                                  neighbors[2], neighbors[3],
                                  neighbors[4], neighbors[5]);
}

void MeshManager::rebuild_all_meshes_with_neighbors(uint64_t epoch) {
    if (!chunk_map) return;
    std::vector<std::tuple<int32_t, int32_t, int32_t>> dirty_chunks;
    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (render_data->is_mesh_dirty) {
            int32_t chunk_x, chunk_y, chunk_z;
            ChunkMap::decode_chunk_key(key, chunk_x, chunk_y, chunk_z);
            dirty_chunks.emplace_back(chunk_x, chunk_y, chunk_z);
        }
    });
    for (auto [chunk_x, chunk_y, chunk_z] : dirty_chunks) {
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z);
        ChunkRenderData* d_x_neg = chunk_map->get_chunk_render_data(chunk_x - 1, chunk_y, chunk_z);
        ChunkRenderData* d_x_pos = chunk_map->get_chunk_render_data(chunk_x + 1, chunk_y, chunk_z);
        ChunkRenderData* d_y_neg = chunk_map->get_chunk_render_data(chunk_x, chunk_y - 1, chunk_z);
        ChunkRenderData* d_y_pos = chunk_map->get_chunk_render_data(chunk_x, chunk_y + 1, chunk_z);
        ChunkRenderData* d_z_neg = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z - 1);
        ChunkRenderData* d_z_pos = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z + 1);
        if (render_data) {
            rebuild_rendering_server_mesh(chunk_x, chunk_y, chunk_z, epoch, render_data,
                                          d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos);
        }
    }
}

void MeshManager::queue_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    mesh_queue.queue_dirty_chunk(chunk_map->get_chunk_key(cx, cy, cz), dist_sq, false);
}

void MeshManager::queue_immediate_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    uint64_t key = chunk_map->get_chunk_key(cx, cy, cz);
    mesh_queue.queue_immediate_dirty_chunk(key, mesh_queue.is_pending(key));
}

void MeshManager::mark_chunk_urgent(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    mesh_queue.mark_urgent(chunk_map->get_chunk_key(cx, cy, cz));
}

void MeshManager::reprioritize(int32_t player_cx, int32_t player_cy, int32_t player_cz, const Frustum* frustum) {
    mesh_queue.reprioritize(player_cx, player_cy, player_cz, frustum);

    if (lod_distance <= 0 || !chunk_map) {
        refresh_far_region_visibility();
        return;
    }

    const int32_t lod = lod_distance;
    const int32_t lod_shell_min = lod > 0 ? lod - 1 : 0;
    const int32_t lod_shell_max = lod + 1;
    const int32_t vert_range = 10;

    int32_t queued = 0;
    constexpr int32_t kMaxLodRemeshPerFrame = 128;

    for (int32_t dx = -lod_shell_max; dx <= lod_shell_max && queued < kMaxLodRemeshPerFrame; ++dx) {
        for (int32_t dz = -lod_shell_max; dz <= lod_shell_max && queued < kMaxLodRemeshPerFrame; ++dz) {
            for (int32_t dy = -vert_range; dy <= vert_range && queued < kMaxLodRemeshPerFrame; ++dy) {
                int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                if (dist < lod_shell_min || dist > lod_shell_max) continue;

                int32_t cx = player_cx + dx;
                int32_t cy = player_cy + dy;
                int32_t cz = player_cz + dz;

                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                if (!render_data) continue;

                float target = compute_chunk_detail_level(cx, cy, cz);
                if (target != render_data->last_built_detail_level && !render_data->is_mesh_dirty) {
                    render_data->is_mesh_dirty = true;
                    render_data->mesh_version++;
                    mark_far_region_dirty_for_chunk(cx, cy, cz);
                    queue_dirty_chunk(cx, cy, cz);
                    ++queued;
                }
            }
        }
    }

    // Far shell: chunks crossing the third-tier boundary (dist ==
    // lod_far_distance..lod_far_distance+1) must switch between the mid-LOD
    // mesh and the far-mode heightmap-only mesh. The distance classifier only
    // crosses a shell ring when the player moves one chunk, so scanning these
    // two rings keeps the far tier in sync without a full-world sweep.
    if (lod_far_distance > lod + 1) {
        const int32_t far_shell_min = lod_far_distance;
        const int32_t far_shell_max = lod_far_distance + 1;
        for (int32_t dx = -far_shell_max; dx <= far_shell_max && queued < kMaxLodRemeshPerFrame; ++dx) {
            for (int32_t dz = -far_shell_max; dz <= far_shell_max && queued < kMaxLodRemeshPerFrame; ++dz) {
                for (int32_t dy = -vert_range; dy <= vert_range && queued < kMaxLodRemeshPerFrame; ++dy) {
                    const int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                    if (dist < far_shell_min || dist > far_shell_max) continue;

                    const int32_t cx = player_cx + dx;
                    const int32_t cy = player_cy + dy;
                    const int32_t cz = player_cz + dz;

                    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                    if (!render_data) continue;

                    const float target = compute_chunk_detail_level(cx, cy, cz);
                    const bool target_far_mode = is_chunk_far_mode(cx, cy, cz);
                    if ((target != render_data->last_built_detail_level || target_far_mode != render_data->last_built_far_mode) &&
                        !render_data->is_mesh_dirty) {
                        render_data->is_mesh_dirty = true;
                        render_data->mesh_version++;
                        mark_far_region_dirty_for_chunk(cx, cy, cz);
                        queue_dirty_chunk(cx, cy, cz);
                        ++queued;
                    }
                }
            }
        }
    }

    // Downgrade chunks that were built at full detail but are now beyond LOD threshold.
    for (auto it = active_full_detail_chunks_.begin(); it != active_full_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target >= 1.0f) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_full_detail_chunks_.erase(it);
    }

    refresh_far_region_visibility();
}

void MeshManager::mark_chunks_dirty_for_light(int32_t center_cx, int32_t center_cy, int32_t center_cz) {
    if (!chunk_map) return;
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dz = -1; dz <= 1; dz++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                const int32_t cx = center_cx + dx;
                const int32_t cy = center_cy + dy;
                const int32_t cz = center_cz + dz;
                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                if (render_data) {
                    render_data->is_mesh_dirty = true;
                    render_data->mesh_version++;
                }
                queue_dirty_chunk(cx, cy, cz);
            }
        }
    }
}

void MeshManager::process_far_region_queue(int32_t max_rebuilds) {
    if (max_rebuilds <= 0 || !thread_pool || !chunk_map) {
        far_regions_partial_missing_cache_last = 0;
        return;
    }

    int32_t partial_missing_cache = 0;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<int32_t, uint64_t>> candidates;
    candidates.reserve(far_regions.size());
    for (const auto& [region_key, region] : far_regions) {
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            continue;
        }
        // Debounce: give a burst of cache arrivals time to settle before
        // rebuilding, so streaming a region does not schedule N rebuilds.
        const double elapsed_ms = std::chrono::duration<double, std::milli>(now - region.last_dirty_at).count();
        if (elapsed_ms < kFarRegionDebounceMs) {
            continue;
        }
        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t center_x = rx * kFarRegionSizeXZ + kFarRegionSizeXZ / 2;
        const int32_t center_z = rz * kFarRegionSizeXZ + kFarRegionSizeXZ / 2;
        const int32_t dx = center_x - last_player_chunk_x;
        const int32_t dy = ry - last_player_chunk_y;
        const int32_t dz = center_z - last_player_chunk_z;
        candidates.emplace_back(dx * dx + dy * dy + dz * dz, region_key);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    int32_t scheduled = 0;
    for (const auto& candidate : candidates) {
        const uint64_t region_key = candidate.second;
        if (scheduled >= max_rebuilds) {
            break;
        }

        auto it = far_regions.find(region_key);
        if (it == far_regions.end()) {
            continue;
        }
        FarRegionRenderData& region = it->second;
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            continue;
        }

        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t base_cx = rx * kFarRegionSizeXZ;
        const int32_t base_cz = rz * kFarRegionSizeXZ;

        struct BuildSource {
            int32_t local_chunk_x = 0;
            int32_t local_chunk_z = 0;
            uint64_t chunk_key = 0;
            std::shared_ptr<CachedFarChunkMesh> cache;
        };

        std::vector<BuildSource> sources;
        std::vector<uint64_t> member_chunk_keys;
        bool missing_far_cache = false;
        int32_t eligible_chunk_count = 0;

        for (int32_t local_z = 0; local_z < kFarRegionSizeXZ; ++local_z) {
            for (int32_t local_x = 0; local_x < kFarRegionSizeXZ; ++local_x) {
                const int32_t cx = base_cx + local_x;
                const int32_t cz = base_cz + local_z;
                if (!should_use_far_region_for_chunk(cx, ry, cz)) {
                    continue;
                }

                ++eligible_chunk_count;
                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, ry, cz);
                if (!render_data) {
                    continue;
                }
                if (!render_data->far_mesh_cache) {
                    missing_far_cache = true;
                    continue;
                }

                const uint64_t chunk_key = chunk_map->get_chunk_key(cx, ry, cz);
                sources.push_back({local_x, local_z, chunk_key, render_data->far_mesh_cache});
                member_chunk_keys.push_back(chunk_key);
            }
        }

        if (missing_far_cache) {
            ++partial_missing_cache;
        }

        if (eligible_chunk_count < 2 || sources.size() < 2) {
            ensure_far_region_instance(region, region_key, false);
            sync_far_region_members_visibility(region);
            region.active_chunk_keys.clear();
            free_far_region_resources(region);
            region.dirty = false;
            continue;
        }

        region.dirty = false;
        const uint64_t revision = region.revision;
        region.pending_builds.fetch_add(1, std::memory_order_relaxed);

        const uint64_t build_epoch = async_epoch ? async_epoch->load(std::memory_order_acquire) : 0;
        thread_pool->fire_and_forget([this, sources = std::move(sources), member_chunk_keys = std::move(member_chunk_keys),
                                      region_key, revision, build_epoch]() mutable {
            int32_t rx_local, ry_local, rz_local;
            ChunkMap::decode_chunk_key(region_key, rx_local, ry_local, rz_local);
            const int32_t region_origin_x = rx_local * kFarRegionSizeXZ * CHUNK_WIDTH;
            const int32_t region_origin_z = rz_local * kFarRegionSizeXZ * CHUNK_DEPTH;

            CompletedRegionMesh completed;
            completed.region_key = region_key;
            completed.epoch = build_epoch;
            completed.revision = revision;
            completed.member_chunk_keys = std::move(member_chunk_keys);

            for (const BuildSource& source : sources) {
                const int32_t offset_x = source.local_chunk_x * CHUNK_WIDTH;
                const int32_t offset_z = source.local_chunk_z * CHUNK_DEPTH;
                append_packed_mesh_data(completed.mesh_data, source.cache->mesh_data, offset_x, 0, offset_z);
                append_packed_mesh_data(completed.water_mesh_data, source.cache->water_mesh_data, offset_x, 0, offset_z);
            }

            {
                std::lock_guard<std::mutex> lock(completed_far_region_meshes_mutex);
                completed_far_region_meshes.push(std::move(completed));
                completed_far_region_mesh_count.fetch_add(1, std::memory_order_relaxed);
            }
            (void)region_origin_x;
            (void)region_origin_z;
        });
        ++scheduled;
    }

    far_regions_partial_missing_cache_last = partial_missing_cache;
}

void MeshManager::process_queue(int32_t max_immediate, int32_t max_rebuilds, double budget_ms) {
    if (max_rebuilds <= 0) {
        return;
    }
    int32_t far_region_budget = max_rebuilds / kFarRegionBuildDivisor;
    if (max_rebuilds > 1) {
        far_region_budget = std::max(1, far_region_budget);
    }
    far_region_budget = std::min(far_region_budget, max_rebuilds);
    const int32_t chunk_budget = std::max(0, max_rebuilds - far_region_budget);

    int32_t mesh_rd_sq = mesh_render_distance ? mesh_render_distance * mesh_render_distance : INT32_MAX;
    int32_t pcx = last_player_chunk_x;
    int32_t pcy = last_player_chunk_y;
    int32_t pcz = last_player_chunk_z;
        mesh_queue.process(
            [this, mesh_rd_sq, pcx, pcy, pcz](int32_t cx, int32_t cy, int32_t cz) {
                int32_t dx = cx - pcx;
                int32_t dy = cy - pcy;
                int32_t dz = cz - pcz;
                if (dx*dx + dz*dz > mesh_rd_sq || std::abs(dy) > 10) {
                    return;
                }
                rebuild_chunk_mesh(cx, cy, cz, async_epoch ? async_epoch->load(std::memory_order_acquire) : 0);
            },
        max_immediate,
        chunk_budget,
        budget_ms
    );
    process_far_region_queue(far_region_budget);
}

void MeshManager::clear() {
    mesh_queue.clear();
    for (auto& entry : far_regions) {
        FarRegionRenderData& region = entry.second;
        free_far_region_resources(region);
    }
    far_regions.clear();
    {
        std::lock_guard<std::mutex> lock(completed_far_region_meshes_mutex);
        while (!completed_far_region_meshes.empty()) {
            completed_far_region_meshes.pop();
        }
    }
    completed_far_region_mesh_count.store(0, std::memory_order_relaxed);
    active_full_detail_chunks_.clear();
    far_regions_partial_missing_cache_last = 0;
    last_player_chunk_x = INT32_MIN;
    last_player_chunk_y = INT32_MIN;
    last_player_chunk_z = INT32_MIN;
    last_player_block_x = INT32_MIN;
    last_player_block_y = INT32_MIN;
    last_player_block_z = INT32_MIN;
}

void MeshManager::notify_chunk_unloaded(int32_t cx, int32_t cy, int32_t cz, const ChunkRenderData* render_data) {
    if (!render_data || !render_data->far_mesh_cache) {
        return;
    }
    const uint64_t region_key = get_far_region_key(cx, cy, cz);
    auto it = far_regions.find(region_key);
    if (it != far_regions.end()) {
        ensure_far_region_instance(it->second, region_key, false);
        sync_far_region_members_visibility(it->second);
    }
    mark_far_region_dirty_for_chunk(cx, cy, cz);
}


void MeshManager::mark_all_chunks_dirty() {
if (!chunk_map) return;
chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
if (render_data->data && !render_data->data->is_all_air()) {
}
render_data->is_mesh_dirty = true;
int32_t cx, cy, cz;
ChunkMap::decode_chunk_key(key, cx, cy, cz);
queue_dirty_chunk(cx, cy, cz);
});
}

bool MeshManager::has_pending_mesh_work() const {
    bool far_region_dirty = false;
    for (const auto& entry : far_regions) {
        const FarRegionRenderData& region = entry.second;
        if (region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            far_region_dirty = true;
            break;
        }
    }
    if (!chunk_scheduler) {
        return mesh_queue.size() > 0 ||
               mesh_queue.immediate_size() > 0 ||
               far_region_dirty ||
               completed_far_region_mesh_count.load(std::memory_order_relaxed) > 0;
    }
    return chunk_scheduler->completed_mesh_count() > 0 ||
           mesh_queue.size() > 0 ||
           mesh_queue.immediate_size() > 0 ||
           far_region_dirty ||
           completed_far_region_mesh_count.load(std::memory_order_relaxed) > 0;
}

WorldRenderStats MeshManager::gather_render_stats() {
    WorldRenderStats stats;
    if (!chunk_map) {
        return stats;
    }

    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        if (should_use_far_region_for_chunk(cx, cy, cz)) {
            ++stats.eligible_far_chunks;
            if (render_data->far_mesh_cache) {
                ++stats.cached_far_chunks;
            }
        }

        if (render_data->mesh_rid.is_valid()) {
            ++stats.mesh_rids;
            ++stats.chunk_mesh_rids;
        }
        if (!render_data->instance_rid.is_valid()) {
            return;
        }
        if (!is_chunk_within_render_distance(cx, cy, cz) ||
            is_far_region_active_for_chunk(cx, cy, cz)) {
            return;
        }
        if (render_data->mesh_rid.is_valid()) {
            ++stats.visible_instances;
            ++stats.chunk_instances;
        }
    });

    for (const auto& entry : far_regions) {
        const FarRegionRenderData& region = entry.second;
        if (region.mesh_rid.is_valid()) {
            ++stats.mesh_rids;
            ++stats.far_region_mesh_rids;
        }
        if (region.instance_rid.is_valid() && region.active) {
            ++stats.visible_instances;
            ++stats.far_region_instances;
            stats.active_region_member_chunks += static_cast<int32_t>(region.active_chunk_keys.size());
        }
    }

    stats.regions_partial_missing_cache = far_regions_partial_missing_cache_last;

    return stats;
}

float MeshManager::compute_chunk_detail_level(int32_t cx, int32_t cy, int32_t cz) const {
    if (lod_distance <= 0) return 1.0f;
    if (last_player_chunk_x == INT32_MIN) return 1.0f;
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});

    // Two-tier LOD: chunks within lod_distance+1 are always full-res.
    // The +1 ring acts as a skirt so that the first coarse ring (dist =
    // lod_distance+2) always borders a stride-1 neighbor, preventing
    // T-junction cracks at the transition boundary.
    // TODO: if more LOD tiers are added, replace with resolved-stride
    // propagation (compute nearest-first, store in ChunkRenderData, check
    // neighbor's actual resolved stride instead of recomputing from distance).
    if (dist <= lod_distance + 1) return 1.0f;

    // Third tier: beyond lod_far_distance, chunks drop to the far-mode
    // heightmap-only mesh (see is_chunk_far_mode / MeshBuilder::set_far_mode).
    if (lod_far_distance > lod_distance + 1 && dist > lod_far_distance) {
        return far_detail_level;
    }

    return lod_detail_level;
}

bool MeshManager::is_chunk_far_mode(int32_t cx, int32_t cy, int32_t cz) const {
    if (lod_far_distance <= 0 || lod_distance <= 0 || last_player_chunk_x == INT32_MIN) {
        return false;
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
    return dist > lod_far_distance;
}

} // namespace VoxelEngine
