#include "mesh/mesh_manager_internal.hpp"

#include <algorithm>

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

using namespace godot;

// ---------------------------------------------------------------------------
// Region uploads are built on a fresh mesh and swapped in when the worker is
// finished. The old path cleared the visible RID and published the instance
// immediately, so the region drew an empty mesh while its members were hidden;
// on initial streaming that showed as chunks flashing for a second or two.
// ---------------------------------------------------------------------------

bool MeshManager::queue_far_region_upload(FarRegionRenderData& region, CompletedRegionMesh& completed,
                                          uint64_t region_key, std::vector<uint64_t> previous_members,
                                          const Ref<ShaderMaterial>& material,
                                          const Ref<ShaderMaterial>& water_material) {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs == nullptr || thread_pool == nullptr || !region_upload_channel) {
        return false;
    }
    if (completed.mesh_data.empty && completed.water_mesh_data.empty) {
        return false;
    }

    const RID upload_mesh_rid = rs->mesh_create();
    if (!upload_mesh_rid.is_valid()) {
        return false;
    }
    ++far_region_uploads_total;
    far_region_upload_vertices_total += completed.mesh_data.vertices.size() +
                                        completed.water_mesh_data.vertices.size();
    rs->mesh_set_custom_aabb(
        upload_mesh_rid,
        AABB(Vector3(0, 0, 0),
             Vector3(region.region_size_xz * CHUNK_WIDTH, CHUNK_HEIGHT, region.region_size_xz * CHUNK_DEPTH)));

    const RID solid_material = material.is_valid() ? material->get_rid() : RID();
    const RID water_material_rid = water_material.is_valid() ? water_material->get_rid() : RID();
    PackedBuiltMeshData solid = std::move(completed.mesh_data);
    PackedBuiltMeshData water = std::move(completed.water_mesh_data);
    std::vector<uint64_t> members = std::move(completed.member_chunk_keys);
    const uint64_t epoch = completed.epoch;
    const uint64_t revision = completed.revision;
    const uint64_t content_hash = completed.content_hash;
    const auto channel = region_upload_channel;

    ++region.pending_uploads;
    channel->in_flight.fetch_add(1, std::memory_order_relaxed);

    thread_pool->fire_and_forget(
        [solid = std::move(solid), water = std::move(water), upload_mesh_rid, solid_material,
         water_material_rid, channel, region_key, epoch, revision, content_hash,
         members = std::move(members), previous = std::move(previous_members)]() mutable {
            RenderingServer* worker_rs = RenderingServer::get_singleton();
            bool surfaces_added = false;
            if (worker_rs != nullptr && !channel->stopping.load(std::memory_order_acquire)) {
                Array arrays;
                arrays.resize(Mesh::ARRAY_MAX);
                int64_t fmt = 0;
                fmt |= RenderingServer::ARRAY_FORMAT_VERTEX;
                fmt |= RenderingServer::ARRAY_FORMAT_INDEX;
                fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM0;
                fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM)
                       << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT;
                fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM1;
                fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA8_UNORM)
                       << RenderingServer::ARRAY_FORMAT_CUSTOM1_SHIFT;
                fmt |= RenderingServer::ARRAY_FORMAT_CUSTOM2;
                fmt |= static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RG_HALF)
                       << RenderingServer::ARRAY_FORMAT_CUSTOM2_SHIFT;
                fmt |= RenderingServer::ARRAY_FLAG_COMPRESS_ATTRIBUTES;

                int surface_index = 0;
                if (!solid.empty) {
                    arrays[Mesh::ARRAY_VERTEX] = solid.vertices;
                    arrays[Mesh::ARRAY_INDEX] = solid.indices;
                    arrays[Mesh::ARRAY_CUSTOM0] = solid.custom0;
                    arrays[Mesh::ARRAY_CUSTOM1] = solid.custom1;
                    arrays[Mesh::ARRAY_CUSTOM2] = solid.custom2;
                    worker_rs->mesh_add_surface_from_arrays(
                        upload_mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                        BitField<RenderingServer::ArrayFormat>(fmt));
                    if (solid_material.is_valid()) {
                        worker_rs->mesh_surface_set_material(upload_mesh_rid, surface_index, solid_material);
                    }
                    ++surface_index;
                }
                if (!water.empty) {
                    arrays[Mesh::ARRAY_VERTEX] = water.vertices;
                    arrays[Mesh::ARRAY_INDEX] = water.indices;
                    arrays[Mesh::ARRAY_CUSTOM0] = water.custom0;
                    arrays[Mesh::ARRAY_CUSTOM1] = water.custom1;
                    arrays[Mesh::ARRAY_CUSTOM2] = water.custom2;
                    worker_rs->mesh_add_surface_from_arrays(
                        upload_mesh_rid, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                        BitField<RenderingServer::ArrayFormat>(fmt));
                    if (water_material_rid.is_valid()) {
                        worker_rs->mesh_surface_set_material(upload_mesh_rid, surface_index, water_material_rid);
                    }
                }
                surfaces_added = true;
            }

            CompletedRegionUpload done;
            done.region_key = region_key;
            done.epoch = epoch;
            done.revision = revision;
            done.content_hash = content_hash;
            done.mesh_rid = upload_mesh_rid;
            done.member_chunk_keys = std::move(members);
            done.previous_members = std::move(previous);
            done.surfaces_added = surfaces_added;
            {
                std::lock_guard<std::mutex> lock(channel->mutex);
                channel->completed.push(std::move(done));
                channel->in_flight.fetch_sub(1, std::memory_order_relaxed);
            }
            channel->idle_cv.notify_all();
        }, PoolTaskClass::Mesh);

    return true;
}

bool MeshManager::pop_completed_region_upload(CompletedRegionUpload& out) {
    const auto channel = region_upload_channel;
    if (!channel) {
        return false;
    }
    std::lock_guard<std::mutex> lock(channel->mutex);
    if (channel->completed.empty()) {
        return false;
    }
    out = std::move(channel->completed.front());
    channel->completed.pop();
    return true;
}

void MeshManager::process_completed_region_uploads(uint64_t epoch, int32_t max_uploads) {
    if (max_uploads <= 0) {
        return;
    }
    RenderingServer* rs = RenderingServer::get_singleton();
    const auto channel = region_upload_channel;
    if (rs == nullptr || !channel) {
        return;
    }
    bool published = false;

    for (int32_t processed = 0; processed < max_uploads; ++processed) {
        CompletedRegionUpload done;
        if (!pop_completed_region_upload(done)) {
            break;
        }

        auto it = far_regions.find(done.region_key);
        if (it == far_regions.end()) {
            if (rs != nullptr && done.mesh_rid.is_valid()) {
                rs->free_rid(done.mesh_rid);
            }
            continue;
        }

        FarRegionRenderData& region = it->second;
        const bool usable = done.surfaces_added && done.mesh_rid.is_valid() && done.epoch == epoch &&
                            done.revision == region.revision;
        if (!usable) {
            if (region.pending_uploads > 0) {
                --region.pending_uploads;
            }
            if (done.mesh_rid.is_valid()) {
                rs->free_rid(done.mesh_rid);
            }
            continue;
        }

        // The worker's RenderingServer command can still be queued when it
        // reports done. Making the instance visible now points it at a mesh
        // with no surface and hides the members behind it. Keep the previous
        // mesh on screen and retry after the renderer has applied the command.
        if (rs->mesh_get_surface_count(done.mesh_rid) == 0) {
            if (!channel->stopping.load(std::memory_order_acquire) && done.publish_retries < 30) {
                ++done.publish_retries;
                {
                    std::lock_guard<std::mutex> lock(channel->mutex);
                    channel->completed.push(std::move(done));
                }
                break;
            }
            if (region.pending_uploads > 0) {
                --region.pending_uploads;
            }
            rs->free_rid(done.mesh_rid);
            continue;
        }
        if (region.pending_uploads > 0) {
            --region.pending_uploads;
        }

        // Swap only now: the old mesh keeps the region drawn until this point,
        // so there is never a frame where the instance points at an empty RID.
        const RID old_mesh = region.mesh_rid;
        region.mesh_rid = done.mesh_rid;
        region.uploaded_content_hash = done.content_hash;
        region.active_chunk_keys = std::move(done.member_chunk_keys);
        ensure_far_region_instance(region, done.region_key, true);
        if (region.instance_rid.is_valid()) {
            rs->instance_set_base(region.instance_rid, region.mesh_rid);
        }
        if (old_mesh.is_valid() && old_mesh != region.mesh_rid) {
            rs->free_rid(old_mesh);
        }

        for (uint64_t old_key : done.previous_members) {
            if (std::find(region.active_chunk_keys.begin(), region.active_chunk_keys.end(), old_key) !=
                region.active_chunk_keys.end()) {
                continue;
            }
            region.visibility_extra_keys.push_back(old_key);
        }
        published = true;
    }

    if (published) {
        request_far_region_visibility_refresh();
    }
}

void MeshManager::discard_region_upload_completions() {
    const auto channel = region_upload_channel;
    if (!channel) {
        return;
    }
    RenderingServer* rs = RenderingServer::get_singleton();
    CompletedRegionUpload done;
    while (pop_completed_region_upload(done)) {
        if (rs != nullptr && done.mesh_rid.is_valid()) {
            rs->free_rid(done.mesh_rid);
        }
    }
}

void MeshManager::wait_for_region_uploads(int32_t timeout_ms) {
    const auto channel = region_upload_channel;
    if (!channel) {
        return;
    }
    std::unique_lock<std::mutex> lock(channel->mutex);
    channel->idle_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&channel]() {
        return channel->in_flight.load(std::memory_order_acquire) == 0;
    });
}

void MeshManager::shutdown_region_uploads() {
    const auto channel = region_upload_channel;
    if (!channel) {
        return;
    }
    // Stop queued uploads from touching the server, then wait out the ones that
    // already started and free the RIDs they were filling. The controller calls
    // this before it destroys the thread pool and the engine destroys the server.
    channel->stopping.store(true, std::memory_order_release);
    wait_for_region_uploads(2000);
    discard_region_upload_completions();
    // A world reset restarts the pool and keeps this manager; a final shutdown
    // has nothing left to upload, so clearing the flag is safe for both.
    channel->stopping.store(false, std::memory_order_release);
}

} // namespace VoxelEngine
