// Standalone-test double for the MeshManager light marks that light_propagator.cpp
// and light_propagator_sky.cpp reference. The test build never links the real mesh
// manager, so these record the request instead of queueing a remesh: a test can
// then assert that a pass over an all-air band asks for nothing, while a pass that
// really moves light still reaches the manager.
#include "mesh_manager_stub.hpp"

#include "mesh/mesh_manager.hpp"

namespace mesh_manager_stub {

std::vector<LightMark>& light_marks() {
    static std::vector<LightMark> marks;
    return marks;
}

void clear_light_marks() {
    light_marks().clear();
}

} // namespace mesh_manager_stub

namespace VoxelEngine {

void MeshManager::mark_chunks_dirty_for_light(int32_t cx, int32_t cy, int32_t cz) {
    mesh_manager_stub::light_marks().push_back({cx, cy, cz});
}

void MeshManager::mark_chunk_dirty_for_light(int32_t cx, int32_t cy, int32_t cz) {
    mesh_manager_stub::light_marks().push_back({cx, cy, cz});
}

} // namespace VoxelEngine
