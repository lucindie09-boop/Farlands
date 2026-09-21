// The dirty mesh queue's off-screen backlog, pinned at the level the artifact was
// reported: "it forgets to mesh stuff until you look at it".
//
// During streaming the queue is permanently saturated — every installed chunk
// dirties itself and its neighbours, far more than the 16/frame active budget
// drains — so while the frustum tier was an unbounded priority, a chunk that was
// merely off-screen never reached the front at all. It stayed un-meshed until the
// player turned round, at which point it jumped the queue and appeared. These
// tests pin the property that fixes it: every frame spends part of its budget in
// plain distance order, so off-screen work is delayed, never starved.
#include "doctest.h"
#include "mesh/mesh_queue.hpp"

#include <array>
#include <cstdint>

using VoxelEngine::ChunkMap;
using VoxelEngine::Frustum;
using VoxelEngine::MeshQueue;

namespace {

constexpr int32_t kPlayerCx = 0;
constexpr int32_t kPlayerCy = 3;
constexpr int32_t kPlayerCz = 0;

// Chunk keys are a pure packing of the coordinates. Mirrored here (there is no
// static encoder to call), with `key_of_round_trips` below proving this copy
// agrees with the engine's own decoder rather than silently misplacing chunks.
uint64_t key_of(int32_t x, int32_t y, int32_t z) {
    constexpr uint32_t OFFSET = 1u << 20;
    constexpr uint32_t MASK = 0x1FFFFF;
    const uint64_t ux = static_cast<uint32_t>(x) + OFFSET & MASK;
    const uint64_t uy = static_cast<uint32_t>(y) + OFFSET & MASK;
    const uint64_t uz = static_cast<uint32_t>(z) + OFFSET & MASK;
    return (ux << 42) | (uy << 21) | uz;
}

bool key_of_round_trips(int32_t x, int32_t y, int32_t z) {
    int32_t rx = 0, ry = 0, rz = 0;
    ChunkMap::decode_chunk_key(key_of(x, y, z), rx, ry, rz);
    return rx == x && ry == y && rz == z;
}

// A frustum that culls everything at cz >= 1 when the player is at cz = 0, i.e. a
// camera looking down -Z, built from one half-space plane. The chunk AABB test is
// conservative at the boundary (a box spanning z = -32..0 is kept), so the
// clusters below sit at cz <= -2 and cz >= 1 rather than on the seam.
// The other five planes are the zero plane, which passes everything.
Frustum frustum_looking_negative_z() {
    Frustum f;
    std::array<godot::Plane, 6> planes{
        godot::Plane(godot::Vector3(0.0f, 0.0f, 1.0f), 16.0f),  // keeps z < 0
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
    };
    f.update(planes);
    return f;
}

Frustum frustum_looking_positive_z() {
    Frustum f;
    std::array<godot::Plane, 6> planes{
        godot::Plane(godot::Vector3(0.0f, 0.0f, -1.0f), 16.0f),  // keeps z > 0
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
        godot::Plane(godot::Vector3(0.0f, 0.0f, 0.0f), 0.0f),
    };
    f.update(planes);
    return f;
}

// A grid of distinct chunk keys in front of (cz = -2) or behind (cz = +1) the
// player. Distinctness matters: the queue dedups by key.
uint64_t cluster_key(int32_t index, int32_t cz) {
    const int32_t cx = index % 20;
    const int32_t cy = index / 20 % 20;
    return key_of(cx, cy, cz);
}

struct FrameCounts {
    int32_t front = 0;   // built chunks with cz < kPlayerCz
    int32_t behind = 0;  // built chunks with cz > kPlayerCz
};

FrameCounts run_frames(MeshQueue& queue, int32_t frames, int32_t budget_per_frame) {
    FrameCounts counts;
    for (int32_t frame = 0; frame < frames; ++frame) {
        queue.process(
            [&](int32_t, int32_t, int32_t cz) -> bool {
                if (cz < kPlayerCz) {
                    ++counts.front;
                } else {
                    ++counts.behind;
                }
                return true;
            },
            0,          // no immediate rebuilds in these tests
            budget_per_frame,
            1000.0);    // a time budget that cannot bind
    }
    return counts;
}

} // namespace

TEST_CASE("the test's key packing agrees with the engine's decoder") {
    CHECK(key_of_round_trips(0, 0, 0));
    CHECK(key_of_round_trips(-2, 3, 1));
    CHECK(key_of_round_trips(-1000, 200, -37));
    CHECK(key_of(0, -2, 1) != 0);
    CHECK(cluster_key(0, -2) != cluster_key(1, -2));
}

TEST_CASE("a saturated view-first queue still meshes off-screen chunks every frame") {
    MeshQueue queue;
    const Frustum frustum = frustum_looking_negative_z();
    queue.reprioritize(kPlayerCx, kPlayerCy, kPlayerCz, &frustum);

    // Every visible chunk is strictly nearer than every off-screen one, so pure
    // distance ordering would pick the visible set every single time and the
    // off-screen chunks would never be reached at all.
    for (int32_t i = 0; i < 400; ++i) {
        queue.queue_dirty_chunk(cluster_key(i, kPlayerCz - 2), 1, false);
    }
    for (int32_t i = 0; i < 400; ++i) {
        queue.queue_dirty_chunk(cluster_key(i, kPlayerCz + 1), 100000, false);
    }

    const FrameCounts one = run_frames(queue, 1, 16);
    // The reserve is a quarter of the budget, and this frame's visible work fills
    // its share, so exactly that quarter goes to the off-screen backlog.
    CHECK(one.front == 12);
    CHECK(one.behind == 4);

    // ...and it keeps happening, rather than the backlog waiting for the visible
    // set to empty, which is what "until you look at it" was.
    const FrameCounts more = run_frames(queue, 5, 16);
    CHECK(more.behind == 20);
    CHECK(more.front == 60);
}

TEST_CASE("a queue with nothing visible to build spends its whole budget") {
    MeshQueue queue;
    const Frustum frustum = frustum_looking_negative_z();
    queue.reprioritize(kPlayerCx, kPlayerCy, kPlayerCz, &frustum);
    for (int32_t i = 0; i < 40; ++i) {
        queue.queue_dirty_chunk(cluster_key(i, kPlayerCz + 1), 1000 + i, false);
    }

    // The view pass has nothing to do, so its unused share is handed to the
    // backlog: the reserve is a floor for off-screen work, not a ceiling.
    const FrameCounts counts = run_frames(queue, 1, 16);
    CHECK(counts.behind == 16);
    CHECK(counts.front == 0);
}

TEST_CASE("without a frustum the whole budget drains by distance") {
    // The headless path (benchmarks, tools) never feeds a frustum. Everything must
    // then be buildable in one frame's budget: a reserve that only applies when a
    // view exists must not throttle it.
    MeshQueue queue;
    queue.reprioritize(kPlayerCx, kPlayerCy, kPlayerCz, nullptr);
    for (int32_t i = 0; i < 40; ++i) {
        queue.queue_dirty_chunk(cluster_key(i, kPlayerCz + 1), 1000 + i, false);
    }
    const FrameCounts counts = run_frames(queue, 1, 16);
    CHECK(counts.behind == 16);
}

TEST_CASE("a chunk that comes into view is built in that same frame") {
    // Turning to face a backlog entry must not make it wait behind the reserve:
    // it belongs to the view tier the moment it is visible again.
    MeshQueue queue;
    Frustum facing_front = frustum_looking_negative_z();
    queue.reprioritize(kPlayerCx, kPlayerCy, kPlayerCz, &facing_front);
    for (int32_t i = 0; i < 40; ++i) {
        queue.queue_dirty_chunk(cluster_key(i, kPlayerCz - 2), 1, false);
    }
    CHECK(run_frames(queue, 1, 8).front == 6);  // 8 - reserve(2), all visible

    // Turn around. The 34 chunks still queued are now behind the player, and one
    // far chunk in the new view direction is enqueued with a huge distance.
    Frustum facing_back = frustum_looking_positive_z();
    queue.reprioritize(kPlayerCx, kPlayerCy, kPlayerCz, &facing_back);
    queue.queue_dirty_chunk(key_of(5, kPlayerCy, kPlayerCz + 1), 1000000, false);

    const FrameCounts counts = run_frames(queue, 1, 8);
    CHECK(counts.behind == 1);                       // the newly visible far chunk
    CHECK(counts.front == 7);                        // and 7 off-screen anyway
    CHECK(counts.front + counts.behind <= 8);        // budget respected
}
