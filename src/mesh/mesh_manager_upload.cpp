#include "mesh/mesh_manager_internal.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

using namespace godot;
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
            AABB(Vector3(0, 0, 0), Vector3(region.region_size_xz * CHUNK_WIDTH, CHUNK_HEIGHT, region.region_size_xz * CHUNK_DEPTH)));

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

} // namespace VoxelEngine
