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
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VoxelEngine {

// What the frame currently draws for the in-frustum far-tier pending set. The
// categories partition the set: total = no_chunk + empty + no_geometry +
// hidden + drawn_chunk + drawn_region. `solid` and `enclosed` refine
// no_geometry; they are not additional categories.
struct PendingFarGeometry {
    size_t total = 0;
    size_t no_chunk = 0;      // the queue entry has no resident chunk
    size_t empty = 0;         // resident and all air: there is nothing to draw
    size_t no_geometry = 0;   // resident and non-empty, but no instance and no region
    size_t solid = 0;         // of no_geometry: the chunk data is fully solid
    size_t enclosed = 0;      // of solid: the buried test would build no faces
    size_t hidden = 0;        // geometry exists, but nothing currently draws it
    size_t drawn_chunk = 0;   // the chunk's own instance is visible
    size_t drawn_region = 0;  // an active far region lists the chunk
};

// The potential far-tier holes (not drawn, and not a chunk the mesher would
// skip as empty) that a camera-ray pass finds visible through the blocks the
// frame is actually drawing. visible + occluded == total.
struct VisibleFarHoles {
    size_t total = 0;
    size_t visible = 0;
    size_t occluded = 0;
    // Of the visible holes, those whose projected AABB contains at least one
    // unoccluded screen sample. `screen_pixels` is the union of the sample cells
    // they cover (4x4 blocks), so overlapping holes merge instead of adding up.
    size_t screen_visible = 0;
    double screen_pixels = 0.0;
};

class MeshManager {
public:
    void set_chunk_map(ChunkMap* cm) { chunk_map = cm; }
    void set_chunk_scheduler(ChunkScheduler* cs) { chunk_scheduler = cs; }
    void set_thread_pool(ThreadPool* tp) { thread_pool = tp; }
    void set_performance_timer(PerformanceTimer* pt) { perf_timer = pt; }
    void set_async_epoch(std::atomic<uint64_t>* ae) { async_epoch = ae; }
    void set_owner(godot::Node* node) { owner = node; }

    void set_player_chunk(int32_t cx, int32_t cy, int32_t cz);
    // Incremental far-region visibility refresh: processes a bounded slice of the
    // regions whose members' far/full visibility can change as the player moves.
    // The old full walk over every region and every active member cost ~2.4 ms
    // on the crossing frame, twice (set_player_chunk and reprioritize).
    void process_far_region_visibility(double budget_ms);

    // SPIKE (env FARLANDS_REGION_SPIKE=<regions>): the far-region worker
    // snapshots each member chunk one at a time under its own shard lock and
    // meshes the snapshot at far detail with a throwaway builder, so the read is
    // measured under live streaming and the data path is proven before any
    // region-source rewrite. Never uploads or touches the visible mesh.
    void region_snapshot_spike(const std::vector<uint64_t>& chunk_keys, float detail_level,
                               bool smooth_lighting);
    godot::Dictionary get_region_spike_stats() const;

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
    // A light pass's 27-bit modified mask (BlockLightRegion::slot_bit layout,
    // relative to the center): only the chunks it wrote, one chunk-map lookup
    // each instead of the two the per-chunk form spends.
    void mark_light_modified_chunks(int32_t center_cx, int32_t center_cy, int32_t center_cz, uint32_t mask);
    void process_queue(int32_t max_immediate, int32_t max_rebuilds, double budget_ms);
    void notify_chunk_unloaded(int32_t cx, int32_t cy, int32_t cz, const ChunkRenderData* render_data);
    void clear();
    // Waits out region uploads that are already on the pool and frees the RIDs
    // they were building. Called on the way out while the RenderingServer is
    // still alive, so a worker can never wake up against a dead singleton.
    void shutdown_region_uploads();
    size_t size() const { return mesh_queue.size(); }
    size_t visible_pending_chunk_mesh_count() const { return mesh_queue.visible_pending_count(); }
    PendingLodSplit visible_pending_lod_split() const {
        return mesh_queue.visible_pending_split(lod_distance, far_lod_distance);
    }
    PendingFarGeometry pending_far_geometry() const;
    VisibleFarHoles visible_far_holes(const godot::Vector3& eye) const;
    VisibleFarHoles visible_far_holes(const godot::Transform3D& camera,
                                      const godot::Projection& projection,
                                      const godot::Vector2& viewport) const;
    int32_t last_rebuild_budget() const { return last_rebuild_budget_; }
    int64_t queue_stop_counts_view() const { return mesh_queue.stop_counts_view(); }
    int64_t queue_stop_counts_backlog() const { return mesh_queue.stop_counts_backlog(); }
    int64_t queue_builds() const { return mesh_queue.builds(); }
    int64_t queue_rejects() const { return mesh_queue.rejects(); }
    bool erase_urgent(uint64_t key) { return mesh_queue.erase_urgent(key); }

    void set_smooth_lighting(bool enabled) { smooth_lighting_enabled = enabled; }
    bool is_smooth_lighting_enabled() const { return smooth_lighting_enabled; }
    void mark_all_chunks_dirty();
    // Why the world still counts as unsettled, one bit per reason: 0 is "quiet".
    // has_pending_mesh_work() is exactly "reasons != 0", so the two cannot
    // disagree, and a probe can name the reason instead of inferring it from the
    // chunk heaps - which hold only one of the six.
    enum PendingMeshReason : uint32_t {
        kPendingQueue = 1u << 0,           // dirty_mesh_queue + backlog_queue
        kPendingImmediate = 1u << 1,       // immediate_dirty_mesh_queue (not in size())
        kPendingFarVisibility = 1u << 2,   // the incremental far-visibility walk
        kPendingFarRegion = 1u << 3,       // a far region dirty, building or uploading
        kPendingCompletedMesh = 1u << 4,   // completed chunk meshes awaiting upload
        kPendingCompletedRegion = 1u << 5, // completed far-region meshes awaiting upload
    };
    // Why a queue pop did or did not become a mesh build. The tail's drain is
    // mostly pops that rebuild_rendering_server_mesh answers without building
    // (a version already accounted for, a buried chunk, a build already in
    // flight), so this is the count that says whether skipping them earlier can
    // buy anything. Main-thread only: every field is written from the queue
    // callback, never from a worker.
    struct MeshPopOutcomes {
        uint64_t callbacks = 0;    // rebuild_chunk_mesh entries (the queue path)
        uint64_t total = 0;        // rebuild_rendering_server_mesh entries
        uint64_t missing = 0;      // no render data (chunk no longer resident)
        uint64_t no_pipeline = 0;  // pool/scheduler/map gone (shutdown)
        uint64_t distance = 0;     // beyond render distance + 2
        uint64_t clean = 0;        // own and neighbour versions already built
        uint64_t air = 0;          // all-air chunk: resources freed, no build
        uint64_t buried = 0;       // fully solid and enclosed: no build
        uint64_t in_flight = 0;    // a build is already running for it
        uint64_t built = 0;        // handed to the thread pool
    };
    [[nodiscard]] const MeshPopOutcomes& mesh_pop_outcomes() const { return mesh_pop_outcomes_; }
    [[nodiscard]] uint32_t pending_mesh_reasons() const;
    [[nodiscard]] bool has_pending_mesh_work() const { return pending_mesh_reasons() != 0; }
    [[nodiscard]] uint64_t solid_probe_hits() const { return solid_probe_hits_; }
    [[nodiscard]] uint64_t solid_probe_misses() const { return solid_probe_misses_; }
    [[nodiscard]] uint64_t solid_probe_resident() const { return solid_probe_resident_; }
    WorldRenderStats gather_render_stats();

    // Optional predicate: "would the chunk at (cx, cy, cz) be entirely solid
    // if generated?" Used by buried-chunk culling to treat ungenerated
    // underground neighbors as opaque instead of rendering box walls into
    // the void. Set by the engine controller; may be null.
    void set_chunk_would_be_solid_fn(std::function<bool(int32_t, int32_t, int32_t)> fn) {
        chunk_would_be_solid_fn_ = std::move(fn);
        // A new predicate means a new world (or a new seed): every memoised
        // answer was for the old one.
        solid_probe_cache_.clear();
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
        // Hash of the source keys and their child content hashes. Equal means
        // the region mesh already on the GPU has the same geometry.
        uint64_t content_hash = 0;
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
        // An active region with a far chunk missing from its current mesh has a
        // visible coverage gap, and one with a full-detail chunk still in its
        // mesh draws that chunk twice. Skip the coalescing delay for both.
        bool needs_immediate_rebuild = false;
        std::atomic<int> pending_builds{0};
        uint64_t revision = 0;
        std::vector<uint64_t> active_chunk_keys;
        // The incremental visibility pass walks this region's members a few at a
        // time; the full walk was measured at 4.2 ms on one crossing frame.
        size_t member_sync_cursor = 0;
        bool member_sync_pending = false;
        // Every far-cached child of this region, maintained by the dirty marks
        // themselves. The scheduler used to rescan all 64 chunk-map shards per
        // build, which cost ~3 ms on a crossing frame.
        std::unordered_map<uint64_t, std::shared_ptr<CachedFarChunkMesh>> far_sources;
        // Chunks that left the active mesh in the last upload. They must be
        // shown again on the incremental pass rather than by a 64-member walk
        // inside the upload itself.
        std::vector<uint64_t> visibility_extra_keys;
        size_t visibility_extra_cursor = 0;
        // What the region's mesh RID currently holds. Reset by
        // free_far_region_resources, so a skipped upload can never leave the
        // region without geometry.
        uint64_t uploaded_content_hash = 0;
        // Uploads in flight for this region. A newer build must not clear or
        // reuse the RID that is on screen; the region keeps drawing the last
        // complete mesh until the replacement is finished and swapped in.
        int32_t pending_uploads = 0;
        std::chrono::steady_clock::time_point last_dirty_at{};
        int32_t region_size_xz = 4; // 4 for mid LOD, 8 for far LOD
    };

    struct CompletedRegionUpload {
        uint64_t region_key = 0;
        uint64_t epoch = 0;
        uint64_t revision = 0;
        uint64_t content_hash = 0;
        godot::RID mesh_rid;
        std::vector<uint64_t> member_chunk_keys;
        std::vector<uint64_t> previous_members;
        // False when the worker could not touch the RenderingServer (shutdown).
        // The RID still has to come back to main so it can be freed.
        bool surfaces_added = false;
        // Frames spent waiting for the server to apply the worker's surface
        // command. Publishing before that is the loading flash.
        int32_t publish_retries = 0;
    };

    struct RegionUploadChannel {
        std::mutex mutex;
        std::queue<CompletedRegionUpload> completed;
        std::atomic<int32_t> in_flight{0};
        std::condition_variable idle_cv;
        std::atomic<bool> stopping{false};
    };

    bool queue_far_region_upload(FarRegionRenderData& region, CompletedRegionMesh& completed,
                                 uint64_t region_key, std::vector<uint64_t> previous_members,
                                 const godot::Ref<godot::ShaderMaterial>& material,
                                 const godot::Ref<godot::ShaderMaterial>& water_material);
    void process_completed_region_uploads(uint64_t epoch, int32_t max_uploads);
    bool pop_completed_region_upload(CompletedRegionUpload& out);
    void discard_region_upload_completions();
    void wait_for_region_uploads(int32_t timeout_ms);

    void hide_chunk_instance(ChunkRenderData* render_data);
    void show_chunk_instance(ChunkRenderData* render_data, int32_t cx, int32_t cy, int32_t cz);
    void process_completed_region_meshes(uint64_t epoch, int32_t max_uploads, double budget_ms,
                                         const godot::Ref<godot::ShaderMaterial>& material,
                                         const godot::Ref<godot::ShaderMaterial>& water_material);

    // The world's two geometry effects and the frustum.
    // `cull_for` is the box an instance should be culled against; `cull_apply`
    // pushes one and remembers how far it was grown. The slack the refresh
    // leaves is what makes a refresh per camera block rather than per frame safe.
    godot::AABB cull_for(const godot::AABB& local_aabb, const godot::Vector3& origin) const;
    void cull_apply(const godot::RID& instance, godot::Vector2& remembered, const godot::AABB& local_aabb,
                    const godot::Vector3& origin);
    void refresh_world_cull_region(FarRegionRenderData& region, uint64_t region_key);
    void process_far_region_queue(int32_t max_rebuilds, double budget_ms);
    void mark_far_region_dirty_for_chunk(int32_t cx, int32_t cy, int32_t cz,
                                         const std::shared_ptr<CachedFarChunkMesh>& cache);
    void request_far_region_visibility_refresh();
    bool should_use_far_region_for_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    bool is_chunk_within_render_distance(int32_t cx, int32_t cy, int32_t cz) const;
    uint64_t get_far_region_key(int32_t cx, int32_t cy, int32_t cz) const;
    bool is_far_region_active_for_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    void sync_far_region_members_visibility(FarRegionRenderData& region);
    void sync_far_region_member(FarRegionRenderData& region, uint64_t chunk_key);
    void sync_visibility_extra_member(uint64_t chunk_key);
    void ensure_far_region_instance(FarRegionRenderData& region, uint64_t region_key, bool visible);
    void free_far_region_resources(FarRegionRenderData& region);

    // One chunk of a light mark, with the mark's single chunk-map lookup.
    void queue_light_dirty_chunk(int32_t cx, int32_t cy, int32_t cz);

    ChunkMap* chunk_map = nullptr;
    ChunkScheduler* chunk_scheduler = nullptr;
    ThreadPool* thread_pool = nullptr;
    PerformanceTimer* perf_timer = nullptr;
    std::atomic<uint64_t>* async_epoch = nullptr;
    godot::Node* owner = nullptr;
    // The culling state for both of the world's geometry effects: the knobs, the
    // camera they were last worked out at, and whether a refresh is owed
    // regardless (a knob changed, or an effect was just switched off).
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
    std::vector<uint64_t> far_visibility_keys_;
    size_t far_visibility_cursor_ = 0;
    bool far_visibility_pending_ = false;
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
    // Shared with the upload task, so a worker finishing after the manager is
    // torn down still has somewhere safe to deposit its RID.
    std::shared_ptr<RegionUploadChannel> region_upload_channel = std::make_shared<RegionUploadChannel>();
    int32_t far_regions_partial_missing_cache_last = 0;
    // Chunks remeshed because their data held liquid while the GPU held no liquid
    // geometry for them. Main-thread only (upload path).
    int32_t liquid_geometry_repairs = 0;
    MeshPopOutcomes mesh_pop_outcomes_;
    // Upload bookkeeping, cumulative (upload path, main thread):
    //   uploads   — meshes handed to the RenderingServer
    //   skips     — uploads the content hash judged unchanged
    //   swallowed — skips where the WATER mesh differed anyway, i.e. the change
    //               the user could not see. Must stay 0: the hash covers both
    //               surfaces precisely so a liquid-only change is never skipped.
    int32_t mesh_uploads = 0;
    int32_t mesh_upload_dedup_skips = 0;
    int32_t mesh_upload_swallowed_water_changes = 0;
    // Far-region upload volume, cumulative (upload path, main thread). Counted
    // here rather than derived from the live RIDs: a region re-uploads its whole
    // merged surface on every rebuild, so the number of queued surfaces and
    // their vertices is the volume a coalescing window has to reduce.
    uint64_t far_region_uploads_total = 0;
    uint64_t far_region_upload_vertices_total = 0;
    static constexpr int32_t kFarRegionSizeXZ = 8;

    float compute_chunk_detail_level(int32_t cx, int32_t cy, int32_t cz) const;

    std::function<bool(int32_t, int32_t, int32_t)> chunk_would_be_solid_fn_;
    // Memo for chunk_would_be_solid_fn_. The predicate is deterministic for a
    // world seed and is asked the same frontier coordinates over and over as the
    // world streams in; one sample cost ~25 us on the main thread and the probes
    // were 91% of the mesh queue's time (1.07 s of 1.18 s in the fill probe).
    // Main-thread only: read and written by the queue drain. Reaching the limit
    // drops everything rather than tracking recency, because a re-probe is
    // merely slower, never wrong.
    //
    // Measured: 8k -> 64k moved nothing (the tail's buried test stayed 0.78-0.85 s
    // over 5.5k pops), so the 0.15 ms a pop is not memo misses and the limit stays
    // where the fill's first profile put it.
    std::unordered_map<uint64_t, uint8_t> solid_probe_cache_;
    static constexpr size_t kSolidProbeCacheLimit = 8192;
    // Where the six neighbour tests of a buried pop resolve. Main-thread only, like
    // the memo: the drain writes, the report reads. `resident` is a loaded chunk
    // answering from its cached flag; hits and misses are the ungenerated-neighbour
    // predicate, and the split says whether its 25 us sample is being reused at all.
    uint64_t solid_probe_hits_ = 0;
    uint64_t solid_probe_misses_ = 0;
    uint64_t solid_probe_resident_ = 0;
    // The rebuild cap the last process_queue() ran under (8 gameplay, 64 once
    // the load settles). Exposed so the fill probe can tell a tail paced by the
    // cap from one paced by the queue budget.
    int32_t last_rebuild_budget_ = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_MESH_MANAGER_HPP
