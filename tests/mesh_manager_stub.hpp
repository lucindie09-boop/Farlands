#ifndef FARLANDS_TESTS_MESH_MANAGER_STUB_HPP
#define FARLANDS_TESTS_MESH_MANAGER_STUB_HPP

#include <cstdint>
#include <vector>

// What the standalone test MeshManager was asked to remesh. The stub does not
// queue anything; recording the request is how a test can tell a light pass that
// marked nothing (an all-air band) from one whose marks never reached the manager.
namespace mesh_manager_stub {

struct LightMark {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
};

std::vector<LightMark>& light_marks();
void clear_light_marks();

} // namespace mesh_manager_stub

#endif // FARLANDS_TESTS_MESH_MANAGER_STUB_HPP
