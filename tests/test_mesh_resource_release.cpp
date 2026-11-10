// The three geometry-dropping branches -- all-air, buried, and an empty mesh --
// share release_mesh_resources(). test_mesh_builder's last-block case pins the
// state that opens the all-air branch, but the free itself needs a
// RenderingServer there. This build has none, so the release takes an injected
// callback and the test records what it was handed.
#include "doctest.h"

#include "mesh/mesh_resource_release.hpp"

using VoxelEngine::release_mesh_resources;

namespace {

struct Handle {
    int id = 0;
    [[nodiscard]] bool is_valid() const { return id != 0; }
};

} // namespace

TEST_CASE("release frees both mesh and instance and clears the handles") {
    Handle mesh;
    mesh.id = 7;
    Handle instance;
    instance.id = 9;
    int calls = 0;
    int last_id = 0;
    const int freed = release_mesh_resources(mesh, instance, [&](Handle handle) {
        ++calls;
        last_id = handle.id;
    });
    CHECK(freed == 2);
    CHECK(calls == 2);
    CHECK(last_id == 9);          // the instance is freed after the mesh
    CHECK_FALSE(mesh.is_valid());
    CHECK_FALSE(instance.is_valid());
}

TEST_CASE("release of an already-empty chunk frees nothing") {
    Handle mesh;
    Handle instance;
    int calls = 0;
    const int freed = release_mesh_resources(mesh, instance, [&](Handle) { ++calls; });
    CHECK(freed == 0);
    CHECK(calls == 0);
}

TEST_CASE("release frees only the handle that is still valid") {
    Handle mesh;
    mesh.id = 3;
    Handle instance;
    int calls = 0;
    const int freed = release_mesh_resources(mesh, instance, [&](Handle) { ++calls; });
    CHECK(freed == 1);
    CHECK(calls == 1);
    CHECK_FALSE(mesh.is_valid());
    CHECK_FALSE(instance.is_valid());
}
