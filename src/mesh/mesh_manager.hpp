#ifndef FARLANDS_MESH_MANAGER_HPP
#define FARLANDS_MESH_MANAGER_HPP
#include "core/chunk_map.hpp"
#include "mesh/chunk_render_data.hpp"
#include "core/frustum.hpp"
#include "core/world_cull.hpp"
#include "world/chunk_scheduler.hpp"
#include "mesh/mesh_queue.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/thread_pool.hpp"
#include "core/performance_timer.hpp"
#include "render/world_render_stats.hpp"
#include <godot_cpp/classes/shader_material.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VoxelEngine {

class MeshManager {
public:
    void set_chunk_map(ChunkMap* cm) { chunk_map = cm; }
    void set_chunk_scheduler(ChunkScheduler* cs) { chunk_scheduler = cs; }
    void set_thread_pool(ThreadPool* tp) { thread_pool = tp; }
    void set_performance_timer(PerformanceTimer* pt) { perf_timer = pt; }
    void set_async_epoch(std::atomic<uint64_t>* ae) { async_epoch = ae; }
    void set_owner(godot::Node* node) { owner = node; }

    void set_player_chunk(int32_t cx, int32_t cy, int32_t cz);

    void set_player_block(int32_t bx, int32_t by, int32_t bz) {
        last_player_block_x = bx;
        last_player_block_y = by;
        last_player_block_z = bz;
    }

    void set_mesh_render_distance(int32_t rd) { mesh_render_distance = rd; }

    // The Shaders menu's two geometry effects are vertex shaders, so the engine
    // decides what to draw from boxes that describe where the world was before
    // the shader moved it: a chunk the bend pulls into view is culled anyway,
    // and the far field has a hole in it, and the ground the curve sinks below
    // the frustum is thrown away with it. These are the compensation, one setter
    // per effect (each knob is one the world materials carry, pushed here by
    // shader_overlay.gd as well as to the shader) and one per-frame call that
    // grows every resident chunk's and far region's cull box by how far the two
    // of them could move it. With both off it does nothing at all.
    void set_world_bend(bool enabled, float amount, float radius, float rise);
    void set_world_horizon(bool enabled, float radius);
    void update_world_cull(const godot::Vector3& camera_position);

    void process_completed_meshes(uint64_t epoch, double budget_ms, int32_t max_uploads,
                                   const godot::Ref<godot::ShaderMaterial>& material,
                                   const godot::Ref<godot::ShaderMaterial>& water_material);

    void rebuild_rendering_server_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                         ChunkRenderData* render_data,
                                         ChunkRenderData* d_x_neg,
                                         ChunkRenderData* d_x_pos,
                                         ChunkRenderData* d_y_neg,
                                         ChunkRenderData* d_y_pos,
                                         ChunkRenderData* d_z_neg,
                                         ChunkRenderData* d_z_pos);
    void rebuild_chunk_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch);
    void rebuild_all_meshes_with_neighbors(uint64_t epoch);
    void queue_dirty_chunk(int32_t cx, int32_t cy, int32_t cz);
    // Same as queue_dirty_chunk but uses the caller-held shard lock (must hold
    // the lock covering cx/cy/cz before calling; never acquires its own).
    void queue_dirty_chunk_fast(int32_t cx, int32_t cy, int32_t cz);
    void queue_immediate_dirty_chunk(int32_t cx, int32_t cy, int32_t cz);
    // Invariant check run on every applied mesh: a chunk whose data holds liquid
    // must have liquid geometry on the GPU. Repairs (one remesh per mesh_version)
    // and counts the case where it does not. See mesh_manager_upload.cpp.
    void note_liquid_geometry(int32_t cx, int32_t cy, int32_t cz, ChunkRenderData& render_data);
    void mark_chunk_urgent(int32_t cx, int32_t cy, int32_t cz);
    void reprioritize(int32_t player_cx, int32_t player_cy, int32_t player_cz, const Frustum* frustum = nullptr);
    void mark_chunks_dirty_for_light(int32_t center_cx, int32_t center_cy, int32_t center_cz);
    // One chunk, for a light pass that reports exactly which chunks it wrote.
    void mark_chunk_dirty_for_light(int32_t cx, int32_t cy, int32_t cz);
    void process_queue(int32_t max_immediate, int32_t max_rebuilds, double budget_ms);
    void notify_chunk_unloaded(int32_t cx, int32_t cy, int32_t cz, const ChunkRenderData* render_data);
    void clear();
    size_t size() const { return mesh_queue.size(); }
    bool erase_urgent(uint64_t key) { return mesh_queue.erase_urgent(key); }

    void set_smooth_lighting(bool enabled) { smooth_lighting_enabled = enabled; }
    bool is_smooth_lighting_enabled() const { return smooth_lighting_enabled; }
    void mark_all_chunks_dirty();
    [[nodiscard]] bool has_pending_mesh_work() const;
    WorldRenderStats gather_render_stats();

    // Optional predicate: "would the chunk at (cx, cy, cz) be entirely solid
    // if generated?" Used by buried-chunk culling to treat ungenerated
    // underground neighbors as opaque instead of rendering box walls into
    // the void. Set by the engine controller; may be null.
    void set_chunk_would_be_solid_fn(std::function<bool(int32_t, int32_t, int32_t)> fn) {
        chunk_would_be_solid_fn_ = std::move(fn);
    }

    void set_lod_distance(int32_t d) { lod_distance = d; }
    int32_t get_lod_distance() const { return lod_distance; }
    void set_lod_detail_level(float l) { lod_detail_level = l; }
    float get_lod_detail_level() const { return lod_detail_level; }
    void set_far_lod_distance(int32_t d) { far_lod_distance = d; }
    int32_t get_far_lod_distance() const { return far_lod_distance; }
    void set_far_lod_detail_level(float l) { far_lod_detail_level = l; }
    float get_far_lod_detail_level() const { return far_lod_detail_level; }

private:
    struct CompletedRegionMesh {
        uint64_t region_key = 0;
        uint64_t epoch = 0;
        uint64_t revision = 0;
        PackedBuiltMeshData mesh_data;
        PackedBuiltMeshData water_mesh_data;
        std::vector<uint64_t> member_chunk_keys;
    };

    struct FarRegionRenderData {
        godot::RID mesh_rid;
        godot::RID instance_rid;
        // As on ChunkRenderData: how far the compensation has grown this
        // region's cull box, so an unchanged margin costs no RenderingServer
        // call. Negative = never touched.
        godot::Vector2 cull_growth = godot::Vector2(-1.0f, -1.0f);
        bool dirty = false;
        bool active = false;
        std::atomic<int> pending_builds{0};
        uint64_t revision = 0;
        std::vector<uint64_t> active_chunk_keys;
        std::chrono::steady_clock::time_point last_dirty_at{};
        int32_t region_size_xz = 4; // 4 for mid LOD, 8 for far LOD
    };

    void hide_chunk_instance(ChunkRenderData* render_data);
    void show_chunk_instance(ChunkRenderData* render_data, int32_t cx, int32_t cy, int32_t cz);
    void process_completed_region_meshes(uint64_t epoch, int32_t max_uploads,
                                         const godot::Ref<godot::ShaderMaterial>& material,
                                         const godot::Ref<godot::ShaderMaterial>& water_material);

    // The world's two geometry effects and the frustum (mesh_manager_cull.cpp).
    // `cull_for` is the box an instance should be culled against; `cull_apply`
    // pushes one and remembers how far it was grown. The slack the refresh
    // leaves is what makes a refresh per camera block rather than per frame safe.
    godot::AABB cull_for(const godot::AABB& local_aabb, const godot::Vector3& origin) const;
    void cull_apply(const godot::RID& instance, godot::Vector2& remembered, const godot::AABB& local_aabb,
                    const godot::Vector3& origin);
    void refresh_world_cull_region(FarRegionRenderData& region, uint64_t region_key);
    void process_far_region_queue(int32_t max_rebuilds);
    void mark_far_region_dirty_for_chunk(int32_t cx, int32_t cy, int32_t cz);
    void refresh_far_region_visibility();
    bool should_use_far_region_for_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    bool is_chunk_within_render_distance(int32_t cx, int32_t cy, int32_t cz) const;
    uint64_t get_far_region_key(int32_t cx, int32_t cy, int32_t cz) const;
    bool is_far_region_active_for_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    void sync_far_region_members_visibility(FarRegionRenderData& region);
    void ensure_far_region_instance(FarRegionRenderData& region, uint64_t region_key, bool visible);
    void free_far_region_resources(FarRegionRenderData& region);

    ChunkMap* chunk_map = nullptr;
    ChunkScheduler* chunk_scheduler = nullptr;
    ThreadPool* thread_pool = nullptr;
    PerformanceTimer* perf_timer = nullptr;
    std::atomic<uint64_t>* async_epoch = nullptr;
    godot::Node* owner = nullptr;
    // The culling state for both of the world's geometry effects
    // (mesh_manager_cull.cpp): the knobs, the camera they were last worked out
    // at, and whether a refresh is owed regardless (a knob changed, or an effect
    // was just switched off).
    WorldBendParams world_bend;
    WorldHorizonParams world_horizon;
    godot::Vector3 cull_camera;
    bool cull_owed = true;
    bool cull_on = false;
    MeshQueue mesh_queue;
    int32_t last_player_chunk_x = INT32_MIN;
    int32_t last_player_chunk_y = INT32_MIN;
    int32_t last_player_chunk_z = INT32_MIN;
    int32_t last_player_block_x = INT32_MIN;
    int32_t last_player_block_y = INT32_MIN;
    int32_t last_player_block_z = INT32_MIN;
    int32_t mesh_render_distance = 0;
    bool smooth_lighting_enabled = false;
    int32_t lod_distance = 0;
    float lod_detail_level = 0.5f;
    // Third LOD tier (identical stride-reduction mechanism to the mid tier):
    // render start far_lod_distance, detail far_lod_detail_level.
    int32_t far_lod_distance = 16;
    float far_lod_detail_level = 0.25f;
    std::unordered_map<uint64_t, FarRegionRenderData> far_regions;
    std::unordered_set<uint64_t> active_full_detail_chunks_;
    // Chunks currently built at the mid tier, used to remesh mid->far
    // downgrades that fall outside the reprioritize transition shells.
    std::unordered_set<uint64_t> active_mid_detail_chunks_;
    // Chunks currently built at the far detail tier. Tracked so the active-set
    // sweep can detect far→mid upgrades that are missed by the shell scans.
    std::unordered_set<uint64_t> active_far_detail_chunks_;
    std::queue<CompletedRegionMesh> completed_far_region_meshes;
    mutable std::mutex completed_far_region_meshes_mutex;
    std::atomic<int32_t> completed_far_region_mesh_count{0};
    int32_t far_regions_partial_missing_cache_last = 0;
    // Chunks remeshed because their data held liquid while the GPU held no liquid
    // geometry for them. Main-thread only (upload path).
    int32_t liquid_geometry_repairs = 0;
    // Upload bookkeeping, cumulative (upload path, main thread):
    //   uploads   — meshes handed to the RenderingServer
    //   skips     — uploads the content hash judged unchanged
    //   swallowed — skips where the WATER mesh differed anyway, i.e. the change
    //               the user could not see. Must stay 0: the hash covers both
    //               surfaces precisely so a liquid-only change is never skipped.
    int32_t mesh_uploads = 0;
    int32_t mesh_upload_dedup_skips = 0;
    int32_t mesh_upload_swallowed_water_changes = 0;
    static constexpr int32_t kFarRegionSizeXZ = 8;

    float compute_chunk_detail_level(int32_t cx, int32_t cy, int32_t cz) const;

    std::function<bool(int32_t, int32_t, int32_t)> chunk_would_be_solid_fn_;
};

} // namespace VoxelEngine

#endif // FARLANDS_MESH_MANAGER_HPP
