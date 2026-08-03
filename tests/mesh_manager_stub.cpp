// Standalone-test stub for MeshManager methods that light_propagator.cpp
// references but the test build never invokes (mesh_manager stays null in
// unit tests). The game library links the real mesh_manager.cpp instead.
#include "mesh/mesh_manager.hpp"

namespace VoxelEngine {
void MeshManager::mark_chunks_dirty_for_light(int32_t, int32_t, int32_t) {}
} // namespace VoxelEngine
