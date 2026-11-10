#ifndef FARLANDS_MESH_RESOURCE_RELEASE_HPP
#define FARLANDS_MESH_RESOURCE_RELEASE_HPP

namespace VoxelEngine {

// Release a chunk's two GPU resources through the caller's free function and
// clear both handles, so "nothing of this chunk is on the GPU any more" is one
// operation instead of four lines repeated at every branch that drops geometry
// (all-air, buried, an empty mesh).
//
// The free function is a parameter, not a RenderingServer call, for two
// reasons: the standalone test build has no RenderingServer, and the branch
// that drops the geometry is worth pinning on its own rather than only through
// the state that opens it. In production the caller passes
// `[rs](RID rid) { rs->free_rid(rid); }`.
//
// A handle must default-construct invalid and expose `is_valid()`; godot::RID
// satisfies both. Returns how many handles were freed.
//
// The order is mesh then instance, matching the branches this replaced. Godot
// may make instance-first cheaper, since the mesh would then have no dependents
// to notify, but that is unmeasured; do not change the order without an arm of
// the release measurement, and do not let a unit test pin it as an optimum.
template <typename Handle, typename FreeFn>
inline int release_mesh_resources(Handle& mesh, Handle& instance, FreeFn&& free_one) {
    int freed = 0;
    if (mesh.is_valid()) {
        free_one(mesh);
        mesh = Handle();
        ++freed;
    }
    if (instance.is_valid()) {
        free_one(instance);
        instance = Handle();
        ++freed;
    }
    return freed;
}

} // namespace VoxelEngine

#endif // FARLANDS_MESH_RESOURCE_RELEASE_HPP
