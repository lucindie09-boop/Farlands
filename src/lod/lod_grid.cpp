#include "lod/lod_grid.hpp"

#include "core/block_types.hpp"
#include "core/chunk_types.hpp"
#include "core/thread_pool.hpp"
#include "lod/lod_tile_policy.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace VoxelEngine {

using namespace godot;

namespace {

double ms_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

LodGrid::LodGrid() : sink(std::make_shared<Sink>()) {}
LodGrid::~LodGrid() {
    free_all_tiles();
    // The per-level instances are the mode's own, so they are freed here rather
    // than left to the scenario: a world that enables and disables the mode would
    // otherwise accumulate one set per session. Null-guarded because at shutdown
    // the server can already be gone.
    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs) {
        for (Bucket& bucket : buckets) {
            if (bucket.instance_rid.is_valid()) {
                rs->free_rid(bucket.instance_rid);
                bucket.instance_rid = RID();
            }
        }
    }
}

uint64_t LodGrid::tile_key(int32_t tx, int32_t tz) {
    // Tiles are unbounded horizontally, so the key has to keep a negative
    // coordinate from colliding with a positive one (the packing the chunk map
    // uses, at tile granularity).
    return (static_cast<uint64_t>(static_cast<uint32_t>(tx)) << 32) |
           static_cast<uint32_t>(tz);
}

int32_t LodGrid::player_tile_x() const {
    return static_cast<int32_t>(std::floor(player_position.x / kTileBlocks));
}

int32_t LodGrid::player_tile_z() const {
    return static_cast<int32_t>(std::floor(player_position.z / kTileBlocks));
}

int32_t LodGrid::level_for_distance(int32_t tile_distance) const {
    // Two rules, both in lod_tile_policy.hpp so a test can pin them: nothing
    // inside the world's disc (it would only be clipped away), and the spacing
    // ladder out from the disc's edge. Tiles that STRADDLE the edge are wanted --
    // they are what fills the corners of the lattice the disc cannot reach.
    if (lod::lod_tile_hidden_by_world(tile_distance, inner_radius_blocks)) return -1;
    return lod::lod_level_for_distance(tile_distance, lod::lod_inner_tiles(inner_radius_blocks),
                                       rings_per_level, outer_rings);
}

int32_t LodGrid::get_outer_radius_blocks() const {
    return lod::lod_outer_radius_blocks(inner_radius_blocks, rings_per_level, outer_rings);
}

void LodGrid::set_config(int32_t new_seed, const TerrainParams& params,
                         const BiomeConfig& biomes) {
    seed = new_seed;
    terrain_params = params;
    biome_config = biomes;
    refresh_layers();
    // Every existing tile was sampled from the old configuration.
    free_all_tiles();
    tiles_dirty = true;
}

void LodGrid::refresh_layers() {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    for (int32_t i = 0; i < static_cast<int32_t>(BiomeType::Count); ++i) {
        const BlockID surface = biome_config.surfaces[static_cast<size_t>(i)].surface;
        // Face index 2 is +Y: the face you see from above, which is the only one
        // a top-down surface mesh shows.
        biome_layers[static_cast<size_t>(i)] =
            static_cast<uint8_t>(registry.get_block_fast(surface).texture_indices[2]);
    }
    underwater_layer = static_cast<uint8_t>(
        registry.get_block_fast(biome_config.underwater_surface).texture_indices[2]);
    water_layer = static_cast<uint8_t>(registry.get_block_fast(BlockIDs::WATER).texture_indices[2]);
}

void LodGrid::set_enabled(bool value) {
    if (enabled == value) return;
    enabled = value;
    if (!enabled) {
        free_all_tiles();
        // Turning the mode off has to give the camera's far plane back: it is the
        // player's camera, and a plane left at a far field's horizon is a change to
        // the whole world's rendering made by a setting that is now off.
        restore_camera_far();
    }
    tiles_dirty = true;
}

void LodGrid::set_base_spacing(int32_t blocks) {
    // Snapped, not clamped: a spacing that does not divide the tile makes every
    // tile of the level come back empty (see lod_snap_spacing), so accepting one
    // would be accepting a request the mode cannot draw. The nearest power of two
    // is what the slider shows as well, so the number on screen is the number the
    // geometry is built at.
    const int32_t snapped = lod::lod_snap_spacing(blocks);
    if (snapped == base_spacing) return;
    base_spacing = snapped;
    free_all_tiles();
    tiles_dirty = true;
}

void LodGrid::set_rings_per_level(int32_t rings) {
    const int32_t clamped = std::clamp(rings, 1, 8);
    if (clamped == rings_per_level) return;
    rings_per_level = clamped;
    free_all_tiles();
    tiles_dirty = true;
}

void LodGrid::set_outer_rings(int32_t rings) {
    // Twelve rings is three kilometres of horizon past an already-shown ladder: the
    // ceiling exists so a settings slider cannot turn a nearly-free knob into a
    // tile scan nobody asked for. Nothing is freed here on purpose: the levels
    // inside the outer one have not moved, so a growing reach only adds tiles and a
    // shrinking one drops them in refresh_wanted_tiles' own pass (the tiles whose
    // level comes back -1 are freed there).
    const int32_t clamped = std::clamp(rings, 0, kMaxOuterRings);
    if (clamped == outer_rings) return;
    outer_rings = clamped;
    tiles_dirty = true;
}

void LodGrid::set_inner_radius_blocks(int32_t blocks) {
    if (blocks == inner_radius_blocks) return;
    inner_radius_blocks = std::max(0, blocks);
}

void LodGrid::set_epoch(uint64_t value) {
    if (value == epoch) return;
    epoch = value;
    free_all_tiles();
    tiles_dirty = true;
}

void LodGrid::set_budgets(int32_t builds_in_flight, int32_t uploads_per_frame) {
    max_builds_in_flight = std::max(1, builds_in_flight);
    max_uploads_per_frame = std::max(1, uploads_per_frame);
}

void LodGrid::reset() {
    free_all_tiles();
    tiles_dirty = true;
    last_player_tile_x = INT32_MIN;
    last_player_tile_z = INT32_MIN;
}

void LodGrid::free_tile(Tile& tile) {
    // The geometry lives in the level's merged mesh, so a tile leaving is a reason
    // to merge that level again rather than a resource to free. The instance and
    // the mesh are the bucket's, and there are kLevels of them for the whole mode.
    if (static_cast<size_t>(tile.level) < buckets.size()) {
        buckets[static_cast<size_t>(tile.level)].dirty = true;
    }
    tile.raw.vertices.clear();
    tile.raw.vertices.shrink_to_fit();
    tile.has_mesh = false;
    tile.quads = 0;
    tile.in_flight = false;
    tile.settled = false;
}

void LodGrid::free_all_tiles() {
    for (auto& [key, tile] : tiles) {
        free_tile(tile);
    }
    const int32_t dropped = static_cast<int32_t>(tiles.size());
    stats.tiles_dropped += dropped;
    tiles.clear();
    // The merged meshes go with them, and they go NOW rather than at the next
    // update: the mode being off is exactly the case where update() returns before
    // it would rebuild anything, so an instance left visible would outlive the
    // setting that turned it off.
    RenderingServer* rs = RenderingServer::get_singleton();
    for (Bucket& bucket : buckets) {
        if (bucket.instance_rid.is_valid()) {
            rs->instance_set_visible(bucket.instance_rid, false);
        }
        bucket.mesh = Ref<ArrayMesh>();
        bucket.mesh_rid = RID();
        bucket.dirty = false;
        bucket.tiles = 0;
        bucket.quads = 0;
        bucket.vertices = 0;
    }
    // The shared column table goes with them. Every call that drops every tile is a
    // reason to distrust a column: a new seed (set_config), a new world (set_epoch,
    // reset), a different spacing or reach (the lattice the asks land on changes),
    // and the mode going off. A table kept across one of those is at best stale work
    // and at worst terrain from the world before it.
    sink->cache.clear();
    // Results that finished but were never drained go with them, and so do the
    // requests the dropped tiles were waiting on: there is no count of those to
    // keep in step (see the Sink comment), so nothing can be left counting them.
    std::lock_guard<std::mutex> lock(sink->mutex);
    std::queue<CompletedTile> empty;
    std::swap(sink->completed, empty);
}

void LodGrid::refresh_wanted_tiles() {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const int32_t ptx = player_tile_x();
    const int32_t ptz = player_tile_z();
    const int32_t inner_tiles = lod::lod_inner_tiles(inner_radius_blocks);
    const int32_t outer_tiles = lod::lod_outer_tiles(inner_radius_blocks, rings_per_level, outer_rings);

    // Drop what has left the annulus or changed spacing, then add what is missing.
    for (auto it = tiles.begin(); it != tiles.end();) {
        Tile& tile = it->second;
        const int32_t distance = std::max(std::abs(tile.tx - ptx), std::abs(tile.tz - ptz));
        const int32_t level = level_for_distance(distance);
        if (level < 0 || spacing_for_level(level) != tile.spacing) {
            free_tile(tile);
            ++stats.tiles_dropped;
            it = tiles.erase(it);
        } else {
            ++it;
        }
    }

    for (int32_t tz = ptz - outer_tiles; tz <= ptz + outer_tiles; ++tz) {
        for (int32_t tx = ptx - outer_tiles; tx <= ptx + outer_tiles; ++tx) {
            const int32_t distance = std::max(std::abs(tx - ptx), std::abs(tz - ptz));
            const int32_t level = level_for_distance(distance);
            if (level < 0) continue;
            const uint64_t key = tile_key(tx, tz);
            if (tiles.find(key) != tiles.end()) continue;
            Tile tile;
            tile.tx = tx;
            tile.tz = tz;
            tile.level = level;
            tile.spacing = spacing_for_level(level);
            tiles.emplace(key, std::move(tile));
        }
    }
    // The tiles that straddle the loaded world's edge: the ones that fill the
    // corners of the lattice the world's disc cannot reach, reported so a caller
    // can tell "the ring is up" from "the ring is up and the seams are closed".
    int32_t straddling = 0;
    for (const auto& [key, tile] : tiles) {
        const int32_t dx = std::abs(tile.tx - ptx);
        const int32_t dz = std::abs(tile.tz - ptz);
        if (std::max(dx, dz) <= inner_tiles) ++straddling;
    }
    stats.straddling_tiles = straddling;
    stats.tiles_live = static_cast<int32_t>(tiles.size());
    stats.last_schedule_ms = ms_since(start);
    stats.spacing_blocks = base_spacing;
    stats.outer_spacing_blocks = spacing_for_level(kLevels - 1);
    stats.outer_radius_blocks = get_outer_radius_blocks();
}

std::array<int32_t, 4> LodGrid::neighbour_spacing_for(int32_t tx, int32_t tz, int32_t ptx,
                                                     int32_t ptz) const {
    const int32_t inner_tiles = lod::lod_inner_tiles(inner_radius_blocks);
    std::array<int32_t, 4> out{};
    for (int32_t edge = 0; edge < 4; ++edge) {
        const int32_t nx = tx + (edge == 0 ? -1 : (edge == 1 ? 1 : 0));
        const int32_t nz = tz + (edge == 2 ? -1 : (edge == 3 ? 1 : 0));
        const int32_t distance = std::max(std::abs(nx - ptx), std::abs(nz - ptz));
        if (lod::lod_tile_hidden_by_world(distance, inner_radius_blocks)) continue;
        // The reach knob belongs here too: a neighbour in the outer band owns the
        // level-3 spacing, and reading the ladder alone would call it "no tile" and
        // leave the edge unsnapped against a neighbour that is a coarser surface.
        const int32_t level =
            lod::lod_level_for_distance(distance, inner_tiles, rings_per_level, outer_rings);
        if (level < 0) continue;
        out[static_cast<size_t>(edge)] = spacing_for_level(level);
    }
    return out;
}

void LodGrid::push_clip_uniforms() {
    if (material.is_null()) return;
    // The far field's inner boundary is the loaded world's DISC, and the disc is
    // centred on the player while the tiles are a square lattice that does not
    // move. Clipping in the fragment stage is what reconciles the two: a tile
    // straddling the edge is built once and stays correct as the player walks
    // across it, whereas dropping whole tiles (the first version) left the four
    // corners of the inner square as holes -- outside the disc, so the world did
    // not draw them either.
    //
    // `inner_radius_blocks` is the radius the world DRAWS to, not the radius it
    // streams at (see world_drawn_radius_blocks): the unload pass keeps its hysteresis
    // as loaded chunks and a loaded chunk keeps its mesh. The disc used to be cut from
    // the streaming radius instead, which is two chunks inside the world's own drawn
    // edge -- so the far field drew its cells over the world's retained rings, and two
    // surfaces over one piece of ground is fighting cells and covered chunks.
    if (player_position == last_clip_center && inner_radius_blocks == last_clip_radius) return;
    last_clip_center = player_position;
    last_clip_radius = inner_radius_blocks;
    // Half a chunk INSIDE the world's own drawn radius, so the two sides overlap rather
    // than meet: the world's coverage is a disc of whole chunk squares and can end a few
    // blocks inside the radius it draws to, and a clip exactly on that radius leaves a
    // sliver neither side draws (which is what the last few missing spots along the
    // world's edge were). Sixteen blocks is the whole of the overlap on purpose -- the
    // grid's cells are drawn over the world's outermost point and nowhere else.
    const float clip = static_cast<float>(std::max(inner_radius_blocks - 16, 0));
    material->set_shader_parameter("clip_center",
                                   Vector2(player_position.x, player_position.z));
    material->set_shader_parameter("clip_radius", clip);
}

void LodGrid::schedule_builds(double /*delta*/) {
    if (!thread_pool || !owner) return;
    // Nearest first, and only up to the in-flight ceiling: the mode is background
    // work on a pool the world is already using, so it never takes more than its
    // share and never queues a burst it cannot drain.
    const double now = now_seconds();
    // Reused rather than rebuilt: this is one entry per live tile, so at the far end
    // of the reach it is tens of thousands of pairs, and a fresh vector plus a fresh
    // sort every frame is main-thread work the frame cannot afford for the whole fill.
    std::vector<std::pair<int32_t, uint64_t>>& pending = pending_scratch;
    pending.clear();
    if (pending.capacity() < tiles.size()) pending.reserve(tiles.size());
    const int32_t ptx = player_tile_x();
    const int32_t ptz = player_tile_z();
    // The count of builds in flight is taken from the tiles, which is the only
    // place it can be right: a live request is a flag on a tile, and a tile that
    // is not there cannot be in flight. The version of this that kept a counter
    // beside the queue could disagree with the map after a world reset threw away
    // finished results, and then it never scheduled anything again.
    int32_t in_flight = 0;
    for (const auto& [key, tile] : tiles) {
        if (tile.in_flight) {
            if (now - tile.in_flight_since < kBuildTimeoutSeconds) {
                ++in_flight;
                continue;
            }
            // Not coming back: re-ask for it rather than leave a hole where this
            // tile should be.
        }
        if (tile.has_mesh || tile.settled) continue;
        const int32_t dx = tile.tx - ptx;
        const int32_t dz = tile.tz - ptz;
        pending.emplace_back(std::max(std::abs(dx), std::abs(dz)), key);
    }
    if (pending.empty()) return;
    int32_t budget = max_builds_in_flight - in_flight;
    // Only the nearest few are ever dispatched, so the sort is a partial one: at the
    // far end of the reach the list is tens of thousands long and the budget is a
    // dozen, and sorting all of it to read the front was most of this function.
    const size_t wanted = static_cast<size_t>(std::clamp(budget, 0, static_cast<int32_t>(pending.size())));
    std::partial_sort(pending.begin(), pending.begin() + wanted, pending.end());
    for (const auto& [distance, key] : pending) {
        if (budget <= 0) break;
        (void)distance;
        auto it = tiles.find(key);
        if (it == tiles.end()) continue;
        Tile& tile = it->second;
        if (tile.has_mesh || tile.settled) continue;
        auto result = std::make_shared<CompletedTile>();
        result->tx = tile.tx;
        result->tz = tile.tz;
        result->spacing = tile.spacing;
        result->epoch = epoch;
        result->request = next_request++;
        result->neighbour_spacing = neighbour_spacing_for(tile.tx, tile.tz, ptx, ptz);
        tile.in_flight = true;
        tile.in_flight_since = now;
        tile.request = result->request;
        --budget;
        auto* grid = this;
        thread_pool->fire_and_forget([grid, result]() { grid->build_tile(result); });
    }
}

void LodGrid::update(double delta) {
    if (!enabled) return;
    const int32_t ptx = player_tile_x();
    const int32_t ptz = player_tile_z();
    if (tiles_dirty || ptx != last_player_tile_x || ptz != last_player_tile_z ||
        inner_radius_blocks != last_inner_blocks) {
        last_player_tile_x = ptx;
        last_player_tile_z = ptz;
        last_inner_blocks = inner_radius_blocks;
        tiles_dirty = false;
        refresh_wanted_tiles();
    }
    drain_completed(max_uploads_per_frame);
    // Only the levels whose tiles arrived or left are merged again, and the merge is
    // what the frame pays instead of a draw call per tile for the rest of it. Timed
    // because it is the one thing this mode does on the main thread, and a fill's
    // frames are the only frames in which it costs anything (see Stats).
    //
    // ...and a level that is still changing waits its turn (kMergeIntervalFrames): a
    // dirty level is one that has had tiles arrive SINCE the last merge, and during a
    // fill that is every level on every frame. The dirty flag alone therefore re-uploads
    // the whole far field four times a frame, which is what a filling frame spends its
    // time on. The cooldown is only counted down while the level is dirty, so a settled
    // reach answers a lone arriving tile on the first frame after it and not on the
    // fourth.
    const std::chrono::steady_clock::time_point merge_start = std::chrono::steady_clock::now();
    int32_t merged_levels = 0;
    for (int32_t level = 0; level < static_cast<int32_t>(buckets.size()); ++level) {
        Bucket& bucket = buckets[static_cast<size_t>(level)];
        // Counted down every frame, dirty or not, so a settled level's cooldown reaches
        // zero and stays there: the next tile to arrive is merged on the following
        // frame, and only a level that is still changing is held to the interval.
        if (bucket.cooldown > 0) --bucket.cooldown;
        if (!bucket.dirty || bucket.cooldown > 0) continue;
        rebuild_bucket(level);
        bucket.cooldown = kMergeIntervalFrames;
        ++merged_levels;
    }
    if (merged_levels > 0) {
        sink->merge_ms_window.store(sink->merge_ms_window.load(std::memory_order_relaxed) +
                                    ms_since(merge_start), std::memory_order_relaxed);
        // Counted on the frames that merged and not on every frame: the number the
        // stats call "per frame" is what a merging frame costs, which is the one worth
        // reporting (see Stats::merge_ms_per_frame).
        sink->merge_frames.fetch_add(1, std::memory_order_relaxed);
    }
    schedule_builds(delta);
    push_clip_uniforms();
    // ...and the camera has to be able to SEE that far. Cheap and idempotent: it
    // compares against the plane already set.
    push_camera_far();
}

LodGrid::Stats LodGrid::gather_stats() {
    stats.enabled = enabled;
    // Derived rather than cached: these describe the configuration, so a caller
    // reading the stats before the first tile refresh still gets the answer the
    // mode will use (which is what a settings row wants to show).
    stats.spacing_blocks = base_spacing;
    stats.outer_spacing_blocks = spacing_for_level(kLevels - 1);
    stats.outer_radius_blocks = get_outer_radius_blocks();
    stats.tiles_live = static_cast<int32_t>(tiles.size());
    stats.clip_radius_blocks = inner_radius_blocks;
    // What the far field costs to draw, which is the number this mode is judged on:
    // one instance per level that has geometry, whatever the tile count.
    int32_t draw_calls = 0;
    int32_t quads = 0;
    int32_t vertices = 0;
    for (const Bucket& bucket : buckets) {
        if (bucket.vertices <= 0) continue;
        ++draw_calls;
        quads += bucket.quads;
        vertices += bucket.vertices;
    }
    stats.draw_calls = draw_calls;
    stats.quads = quads;
    stats.vertices = vertices;
    stats.columns_sampled = sink->columns_sampled.load(std::memory_order_relaxed);
    stats.cache_hits = sink->cache.hits();
    const int64_t merged_frames = sink->merge_frames.exchange(0, std::memory_order_relaxed);
    const double merged_ms = sink->merge_ms_window.exchange(0.0, std::memory_order_relaxed);
    stats.merge_ms_per_frame =
        merged_frames > 0 ? merged_ms / static_cast<double>(merged_frames) : 0.0;
    return stats;
}

} // namespace VoxelEngine
