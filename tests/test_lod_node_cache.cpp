// The far field's shared column table (src/lod/lod_node_cache.hpp).
//
// The cache exists because the mode was paying four times over for its own ground:
// at the outermost level a tile is ONE cell, so each of its four nodes is wanted by
// up to four tiles, and the occlusion ring asks for four more nodes per node. The
// things worth pinning are therefore the sharing itself (a column is sampled once
// however many tiles want it, and across spacings, because a sample does not depend
// on the spacing it is wanted at), the keys (two columns cannot collide), and the
// bound (over the limit the table is emptied rather than aged, and a caller still
// gets a correct sample out of it).
#include "doctest.h"
#include "lod/lod_node_cache.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using VoxelEngine::lod::NodeCache;
using VoxelEngine::lod::SurfaceSample;

namespace {

// How many columns the concurrent case asks for, and how many workers ask. Both are
// namespace scope because a lambda in the same test captures neither by default.
constexpr int32_t kConcurrentNodes = 256;
constexpr int32_t kConcurrentWorkers = 4;

// A sampler with no terrain behind it: the height IS the column, so a wrong entry
// cannot be mistaken for a right one. Counts its calls.
struct HeightByColumn {
    std::atomic<int64_t> calls{0};
    std::atomic<int64_t> last_x{0};

    SurfaceSample operator()(int32_t x, int32_t z) {
        ++calls;
        last_x = x;
        SurfaceSample s;
        s.valid = true;
        s.height = static_cast<float>(x) + static_cast<float>(z) * 0.5f;
        s.layer = 3;
        return s;
    }

    NodeCache::Sample sampler() {
        return [this](int32_t x, int32_t z) { return (*this)(x, z); };
    }
};

} // namespace

TEST_CASE("a node is sampled once however many tiles want it") {
    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;

    const SurfaceSample first = cache.get(0, 0, sample);
    const SurfaceSample second = cache.get(0, 0, sample);
    CHECK(terrain.calls.load() == 1);
    CHECK(first.height == doctest::Approx(second.height));
    CHECK(cache.hits() == 1);
    CHECK(cache.misses() == 1);
    CHECK(cache.size() == 1);

    // ...and the sharing is across the whole table, not per node: four corners of one
    // cell are four separate columns.
    for (int32_t z = 0; z <= 256; z += 256) {
        for (int32_t x = 0; x <= 256; x += 256) {
            cache.get(x, z, sample);
        }
    }
    CHECK(terrain.calls.load() == 4);
    CHECK(cache.size() == 4);
}

TEST_CASE("the table has no spacing in it: one column, every level") {
    // A column's sample does not depend on the spacing a caller wants it at, so the
    // fine levels and the coarse ones share one entry. This is why the table is keyed
    // by the column alone, and it is what makes the occlusion ring free: its four
    // nodes per node are the neighbouring tiles' own corners, asked for at the same
    // spacing, so they are hits and not columns.
    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;

    for (int32_t spacing : {32, 64, 128, 256}) {
        const SurfaceSample s = cache.get(512, -256, sample);
        CHECK(s.height == doctest::Approx(512.0f - 128.0f));
        (void)spacing;
    }
    CHECK(terrain.calls.load() == 1);
    CHECK(cache.hits() == 3);
}

TEST_CASE("a hole is a sample too") {
    // The sampler refusing a column (outside the world's vertical extent) is an
    // answer: a tile that gets it draws nothing there, and re-asking for the same
    // column is exactly the repeat the table exists to remove. An invalid sample
    // carries no height, so nothing may read it as one.
    bool valid = false;
    const NodeCache::Sample sample = [&valid](int32_t, int32_t) {
        SurfaceSample s;
        s.valid = valid;
        s.height = 64.0f;
        return s;
    };
    NodeCache cache;
    const SurfaceSample hole = cache.get(0, 0, sample);
    CHECK_FALSE(hole.valid);
    CHECK(cache.get(0, 0, sample).valid == false);
    CHECK(cache.size() == 1);
}

TEST_CASE("two columns cannot share a key") {
    // The packing is the tile map's, at node granularity: a negative coordinate keeps
    // a column (and a row) of its own, so a node west of the origin cannot be answered
    // with the height of one south of it.
    CHECK(NodeCache::node_key(-1, 0) != NodeCache::node_key(0, -1));
    CHECK(NodeCache::node_key(1234567, -987654) != NodeCache::node_key(-987654, 1234567));
    CHECK(NodeCache::node_key(0, 0) == NodeCache::node_key(0, 0));
    CHECK(NodeCache::node_key(-5, 7) == NodeCache::node_key(-5, 7));

    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;
    const SurfaceSample west = cache.get(-256, 0, sample);
    const SurfaceSample south = cache.get(0, -256, sample);
    CHECK(west.height == doctest::Approx(-256.0f));
    CHECK(south.height == doctest::Approx(-128.0f));
    CHECK(terrain.calls.load() == 2);
}

TEST_CASE("over the limit the table is emptied, and the answer is still right") {
    // Wholesale rather than aged out: the ring the player wants at any moment is one
    // contiguous band, an eviction policy would have to be told which part of it that
    // is, and the cost of being wrong is re-sampling one band. The one thing that may
    // never happen is a wrong sample coming back.
    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;
    cache.set_limit(4);

    for (int32_t x = 0; x < 4; ++x) {
        const SurfaceSample s = cache.get(x * 256, 0, sample);
        CHECK(s.height == doctest::Approx(static_cast<float>(x * 256)));
    }
    CHECK(cache.size() == 4);

    const SurfaceSample overflow = cache.get(1024, 0, sample);
    CHECK(overflow.height == doctest::Approx(1024.0f));
    CHECK(cache.size() == 1);
    // The node that just went in is still answered from the table...
    const int64_t calls_after_insert = terrain.calls.load();
    CHECK(cache.get(1024, 0, sample).height == doctest::Approx(1024.0f));
    CHECK(terrain.calls.load() == calls_after_insert);
    // ...and one that was emptied out is sampled again rather than invented.
    CHECK(cache.get(0, 0, sample).height == doctest::Approx(0.0f));
    CHECK(terrain.calls.load() > calls_after_insert);
}

TEST_CASE("clearing the table is a new world") {
    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;
    cache.get(128, 128, sample);
    CHECK(cache.size() == 1);
    cache.clear();
    CHECK(cache.size() == 0);
    cache.get(128, 128, sample);
    CHECK(terrain.calls.load() == 2);
    // The counters are not part of the world: they measure the table's whole life.
    CHECK(cache.hits() == 0);
    CHECK(cache.misses() == 2);
    cache.reset_counters();
    CHECK(cache.misses() == 0);
}

TEST_CASE("workers asking at once all get the column they asked for") {
    // The table is shared by the build tasks, and the contract under contention is
    // correctness rather than economy: two workers may both sample a node that is
    // missing (the sampler runs outside the lock, or the table serializes every
    // worker behind a millisecond of terrain work), and the second insert wins. What
    // may never happen is a wrong height coming back for a column, or an entry
    // disappearing because two threads inserted at once.
    HeightByColumn terrain;
    const NodeCache::Sample sample = terrain.sampler();
    NodeCache cache;

    std::atomic<int32_t> wrong{0};
    std::vector<std::thread> workers;
    for (int32_t w = 0; w < kConcurrentWorkers; ++w) {
        workers.emplace_back([&cache, &sample, &wrong]() {
            for (int32_t i = 0; i < kConcurrentNodes; ++i) {
                const int32_t x = i * 256;
                const SurfaceSample s = cache.get(x, 0, sample);
                if (std::abs(s.height - static_cast<float>(x)) > 0.001f) ++wrong;
            }
        });
    }
    for (std::thread& t : workers) t.join();

    CHECK(wrong.load() == 0);
    CHECK(cache.size() == static_cast<size_t>(kConcurrentNodes));
    // Every node was asked for four times and sampled between once and four times
    // (see above); the fourfold repeat is exactly what the table is for.
    CHECK(terrain.calls.load() >= kConcurrentNodes);
    CHECK(terrain.calls.load() <= 4 * kConcurrentNodes);
    CHECK(cache.hits() + cache.misses() ==
          static_cast<int64_t>(kConcurrentWorkers * kConcurrentNodes));
}
