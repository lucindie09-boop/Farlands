// The far mode's upload half: a finished tile's geometry is kept, and the spacing
// level it belongs to is merged into the one mesh that level's one instance draws.
//
// Split out of lod_grid.cpp the way this repository asks (nothing above 500 lines,
// the original name keeping its subject), and the subject here is batched drawing:
// draw calls are what a far field costs in this engine, so how many instances exist
// is the whole of this file's job. Nothing here is a member's private business --
// the class declared it, so any translation unit can define it.
#include "lod/lod_grid.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>
#include <queue>

namespace VoxelEngine {

using namespace godot;

void LodGrid::drain_completed(int32_t max_uploads) {
    std::queue<CompletedTile> ready;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        while (!sink->completed.empty()) {
            ready.push(std::move(sink->completed.front()));
            sink->completed.pop();
        }
    }
    int32_t uploaded = 0;
    while (!ready.empty()) {
        CompletedTile result = std::move(ready.front());
        ready.pop();
        auto it = tiles.find(tile_key(result.tx, result.tz));
        const bool stale = result.epoch != epoch || it == tiles.end() ||
                           it->second.spacing != result.spacing ||
                           it->second.request != result.request;
        stats.columns_sampled = sink->columns_sampled.load(std::memory_order_relaxed);
        if (stale) {
            ++stats.tiles_failed;
            continue;
        }
        if (uploaded >= max_uploads) {
            // Put it back rather than dropping it: it is a finished tile.
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->completed.push(std::move(result));
            break;
        }
        Tile& tile = it->second;
        tile.in_flight = false;
        if (result.mesh.vertices.empty()) {
            // An empty answer is FINAL, not a failure to retry (see Tile::settled in
            // the header): the sampler is a pure function of the configuration, so a
            // second pass finds the same hole -- and the tiles of a refused level,
            // which are the nearest ones, would otherwise be re-sampled every frame
            // and spend the build budget on themselves while the reach's own outer
            // rings never got built at all.
            tile.settled = true;
            ++stats.tiles_failed;
            continue;
        }

        ++uploaded;
        ++stats.uploads;
        ++stats.tiles_built;
        stats.last_build_ms = result.build_ms;
        // The tile's geometry is kept and its level's mesh is merged again below:
        // one draw call per level instead of one per tile.
        tile.raw = std::move(result.mesh);
        tile.quads = tile.raw.terrain_quads + tile.raw.water_quads;
        tile.has_mesh = true;
        if (static_cast<size_t>(tile.level) < buckets.size()) {
            buckets[static_cast<size_t>(tile.level)].dirty = true;
        }
    }
}

void LodGrid::rebuild_bucket(int32_t level) {
    if (level < 0 || static_cast<size_t>(level) >= buckets.size()) return;
    Bucket& bucket = buckets[static_cast<size_t>(level)];
    bucket.dirty = false;

    int32_t count = 0;
    int32_t quads = 0;
    int32_t tiles_in_bucket = 0;
    float min_x = 1.0e30f;
    float max_x = -1.0e30f;
    float min_z = 1.0e30f;
    float max_z = -1.0e30f;
    float min_y = 1.0e30f;
    float max_y = -1.0e30f;
    for (const auto& [key, tile] : tiles) {
        if (tile.level != level || !tile.has_mesh || tile.raw.vertices.empty()) continue;
        count += static_cast<int32_t>(tile.raw.vertices.size());
        quads += tile.raw.terrain_quads + tile.raw.water_quads;
        ++tiles_in_bucket;
        min_x = std::min(min_x, tile.raw.min_x);
        max_x = std::max(max_x, tile.raw.max_x);
        min_z = std::min(min_z, tile.raw.min_z);
        max_z = std::max(max_z, tile.raw.max_z);
        min_y = std::min(min_y, tile.raw.min_y);
        max_y = std::max(max_y, tile.raw.max_y);
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (count == 0) {
        // Nothing of this level is wanted: the instance stays, hidden, because the
        // clip and the light are pushed to the material rather than to each tile.
        if (bucket.instance_rid.is_valid()) {
            rs->instance_set_visible(bucket.instance_rid, false);
        }
        bucket.tiles = 0;
        bucket.quads = 0;
        bucket.vertices = 0;
        return;
    }

    PackedVector3Array points;
    PackedColorArray colors;
    PackedVector2Array uvs;
    PackedVector2Array layers;
    points.resize(count);
    colors.resize(count);
    uvs.resize(count);
    layers.resize(count);
    int32_t w = 0;
    for (const auto& [key, tile] : tiles) {
        if (tile.level != level || !tile.has_mesh || tile.raw.vertices.empty()) continue;
        for (const lod::LodVertex& v : tile.raw.vertices) {
            points.set(w, Vector3(v.x, v.y, v.z));
            // The baked face constant rides in the vertex colour, so the shader has one
            // multiplier to apply and no normal to derive. Its alpha was the water flag
            // that UV2.y already carries, so it was dead -- the biome blend weight goes
            // there instead, and the vertex format does not grow a byte for it.
            colors.set(w, Color(v.shade, v.shade, v.shade, v.mix));
            uvs.set(w, Vector2(v.u, v.v));
            // UV2: the pair a cell blends between and the water flag, in two floats.
            //   x = the cell's own layer, y = water + 2 * (the layer it blends toward)
            // Both parts are small exact integers, so a shader can take them apart again
            // and a cell's value survives interpolation -- which it has to, because a
            // layer index interpolated between two different layers would sample a
            // texture that is neither of them.
            layers.set(w, Vector2(v.layer, v.water + 2.0f * v.layer2));
            ++w;
        }
    }

    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = points;
    arrays[Mesh::ARRAY_COLOR] = colors;
    arrays[Mesh::ARRAY_TEX_UV] = uvs;
    arrays[Mesh::ARRAY_TEX_UV2] = layers;
    bucket.mesh = Ref<ArrayMesh>();
    bucket.mesh.instantiate();
    bucket.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    if (material.is_valid()) {
        bucket.mesh->surface_set_material(0, material);
    }
    bucket.mesh_rid = bucket.mesh->get_rid();

    if (!bucket.instance_rid.is_valid()) {
        bucket.instance_rid = rs->instance_create();
        rs->instance_set_scenario(bucket.instance_rid, scenario());
        rs->instance_geometry_set_cast_shadows_setting(
            bucket.instance_rid, RenderingServer::SHADOW_CASTING_SETTING_OFF);
        // The vertices are WORLD coordinates (see lod/lod_surface.hpp), so the
        // transform must stay identity. It was set to a tile origin here once, when
        // there was an instance per tile, which drew every tile but the first at
        // double its offset: a ring of tiles and exactly one of them visible.
        rs->instance_set_transform(bucket.instance_rid, Transform3D());
    }
    rs->instance_set_base(bucket.instance_rid, bucket.mesh_rid);
    // The cull box is the union of the geometry merged in, taken from the meshes
    // themselves rather than derived from the tile indices, so it cannot describe
    // somewhere the geometry is not.
    AABB box;
    box.position = Vector3(min_x, min_y - 1.0f, min_z);
    box.size = Vector3(std::max(max_x - min_x, 1.0f), std::max(max_y - min_y, 2.0f),
                       std::max(max_z - min_z, 1.0f));
    rs->instance_set_custom_aabb(bucket.instance_rid, box);
    rs->instance_set_visible(bucket.instance_rid, true);

    bucket.tiles = tiles_in_bucket;
    bucket.quads = quads;
    bucket.vertices = count;
}


} // namespace VoxelEngine
