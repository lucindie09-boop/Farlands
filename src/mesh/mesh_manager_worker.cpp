#include "mesh/mesh_manager_internal.hpp"

#include "core/hash_utils.hpp"
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <cstring>

namespace VoxelEngine {

using namespace godot;
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


void MeshBuildTask::execute() {
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
        // Acquire a shared lock over the center chunk and its 26 neighbors for
        // the whole data read. Writers (block edits, light region recomputes,
        // player light) hold exclusive locks on the same 3x3x3 neighborhood, so
        // this serializes the build against any concurrent mutation of block or
        // light data. The previous pin-only scheme only protected against chunk
        // erasure and raced with those writers: on restart, chunks containing
        // light-source blocks triggered a recompute worker that mutated a
        // neighbor's section palettes while this build read them.
        std::unique_ptr<ChunkMap::ShardLock> build_lock;
        if (chunk_map) {
            uint64_t build_keys[27] = {};
            build_keys[0] = chunk_map->get_chunk_key(chunk_x, chunk_y, chunk_z);
            for (int i = 0; i < 26; i++) {
                build_keys[i + 1] = chunk_map->get_chunk_key(
                    chunk_x + kNeighborOffsets[i][0],
                    chunk_y + kNeighborOffsets[i][1],
                    chunk_z + kNeighborOffsets[i][2]);
            }
            build_lock.reset(new ChunkMap::ShardLock(chunk_map->lock_keys(build_keys)));
            for (int i = 0; i < 26; i++) {
                all_neighbors[i] = chunk_map->get_chunk_render_data_fast(
                    chunk_x + kNeighborOffsets[i][0],
                    chunk_y + kNeighborOffsets[i][1],
                    chunk_z + kNeighborOffsets[i][2]);
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
}

} // namespace VoxelEngine
