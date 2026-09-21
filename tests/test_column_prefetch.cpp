// The sweep's band frontier used to derive every column's content bounds itself,
// which is a rigorous chunk height range (~181 us cold) and 70% of the sweep's
// wall time. It now asks a worker for them ahead of time and only computes a
// column when no answer has landed.
//
// What is tested here is the part that can be wrong in a way the game would show
// as either a stall or a hole, and neither shows up as a crash:
//
//   * an answer is delivered to the exact key that asked for it, once, and a
//     second take of the same key does not find it again;
//   * a request is never duplicated, so the pool cannot be handed the same column
//     twice by one scan;
//   * an answer computed under retired terrain is REFUSED rather than cached, and
//     its request is still retired, because a column left looking in flight
//     forever is a column the prefetch silently stops covering;
//   * a request made against a retired epoch is refused, which is what stops a
//     scan that was reading a stale list from occupying a key with no task
//     behind it.
#include "doctest.h"
#include "world/column_prefetch.hpp"

using namespace VoxelEngine;

namespace {

ColumnBounds bounds_of(float land_h, float top_h) {
    ColumnBounds b;
    b.land_h = land_h;
    b.top_h = top_h;
    return b;
}

} // namespace

TEST_CASE("a published answer is claimed once, by the key that requested it") {
    ColumnPrefetch prefetch;
    const auto key = ColumnPrefetch::key_of(4, -7);

    CHECK(prefetch.request(key, prefetch.epoch()));
    ColumnBounds out;
    CHECK_FALSE(prefetch.try_take(key, out));  // nothing has landed yet

    prefetch.publish(key, prefetch.epoch(), bounds_of(64.0f, 96.0f));
    CHECK(prefetch.try_take(key, out));
    CHECK(out.land_h == doctest::Approx(64.0f));
    CHECK(out.top_h == doctest::Approx(96.0f));
    // Claimed means gone: the frontier has to fall back to the cache, not hand the
    // same value to whoever asks next.
    CHECK_FALSE(prefetch.try_take(key, out));
    CHECK(prefetch.stats().taken == 1);
    CHECK(prefetch.stats().published == 1);
    CHECK(prefetch.outstanding() == 0);
}

TEST_CASE("a key is only ever requested once at a time") {
    ColumnPrefetch prefetch;
    const auto key = ColumnPrefetch::key_of(-3, 9);

    CHECK(prefetch.request(key, prefetch.epoch()));
    CHECK_FALSE(prefetch.request(key, prefetch.epoch()));  // already out
    CHECK(prefetch.stats().requested == 1);

    prefetch.publish(key, prefetch.epoch(), bounds_of(0.0f, 0.0f));
    // And not again while its answer is still waiting to be claimed.
    CHECK_FALSE(prefetch.request(key, prefetch.epoch()));
    ColumnBounds out;
    CHECK(prefetch.try_take(key, out));
    // Now that it has been claimed, it can be asked for again.
    CHECK(prefetch.request(key, prefetch.epoch()));
}

TEST_CASE("columns are keyed so that negative and positive coordinates cannot collide") {
    CHECK(ColumnPrefetch::key_of(1, 0) != ColumnPrefetch::key_of(0, 1));
    CHECK(ColumnPrefetch::key_of(-1, 0) != ColumnPrefetch::key_of(0, -1));
    CHECK(ColumnPrefetch::key_of(-1, 0) != ColumnPrefetch::key_of(1, 0));
    CHECK(ColumnPrefetch::key_of(-1, -1) != ColumnPrefetch::key_of(1, 1));
    // Same coordinates, same key — the frontier and the scan must agree.
    CHECK(ColumnPrefetch::key_of(-12, 34) == ColumnPrefetch::key_of(-12, 34));
}

TEST_CASE("an answer from retired terrain is refused, and still retires its request") {
    ColumnPrefetch prefetch;
    const auto key = ColumnPrefetch::key_of(2, 2);
    const uint32_t old_epoch = prefetch.epoch();

    CHECK(prefetch.request(key, old_epoch));
    prefetch.set_config(ColumnPrefetch::Config{});
    CHECK(prefetch.epoch() != old_epoch);

    // The worker was already running when the terrain changed: its answer
    // describes terrain that no longer exists.
    prefetch.publish(key, old_epoch, bounds_of(70.0f, 90.0f));
    ColumnBounds out;
    CHECK_FALSE(prefetch.try_take(key, out));
    CHECK(prefetch.stats().stale == 1);
    CHECK(prefetch.stats().published == 0);
    // The request must not still be counted as out: a key stuck in flight is
    // never requested again, so the prefetch would quietly stop covering it.
    CHECK(prefetch.outstanding() == 0);
    CHECK(prefetch.request(key, prefetch.epoch()));
}

TEST_CASE("a request against a retired epoch is refused") {
    ColumnPrefetch prefetch;
    const uint32_t old_epoch = prefetch.epoch();
    prefetch.set_config(ColumnPrefetch::Config{});

    // A scan reading a list built for the old terrain: accepting the request would
    // record a key that no task is behind.
    CHECK_FALSE(prefetch.request(ColumnPrefetch::key_of(5, 5), old_epoch));
    CHECK(prefetch.outstanding() == 0);
    CHECK(prefetch.stats().requested == 0);
}

TEST_CASE("a configuration change drops what is waiting as well as what is in flight") {
    ColumnPrefetch prefetch;
    const auto waiting = ColumnPrefetch::key_of(1, 1);
    const auto flying = ColumnPrefetch::key_of(2, 2);
    const uint32_t epoch = prefetch.epoch();

    CHECK(prefetch.request(flying, epoch));
    CHECK(prefetch.request(waiting, epoch));
    prefetch.publish(waiting, epoch, bounds_of(10.0f, 20.0f));

    prefetch.set_config(ColumnPrefetch::Config{});
    ColumnBounds out;
    CHECK_FALSE(prefetch.try_take(waiting, out));
    CHECK(prefetch.outstanding() == 0);
}

TEST_CASE("a worker copies the configuration once per epoch, never per column") {
    ColumnPrefetch prefetch;
    ColumnPrefetch::Config config;
    uint32_t seen = 0;
    bool copied = false;

    const uint32_t epoch = prefetch.epoch();
    CHECK(prefetch.worker_config(epoch, seen, config, copied));
    CHECK(copied);  // first column on this thread configures the generator
    CHECK(prefetch.worker_config(epoch, seen, config, copied));
    CHECK_FALSE(copied);  // the rest reuse it — the copy is the expensive part

    prefetch.set_config(ColumnPrefetch::Config{});
    const uint32_t next_epoch = prefetch.epoch();
    CHECK(prefetch.worker_config(next_epoch, seen, config, copied));
    CHECK(copied);
    // And the retired epoch is refused from here on.
    CHECK_FALSE(prefetch.worker_config(epoch, seen, config, copied));
}

TEST_CASE("answers for a consumer that stopped asking are discarded, not accumulated") {
    ColumnPrefetch prefetch;
    const uint32_t epoch = prefetch.epoch();
    // The cap exists so a consumer that stops claiming cannot make the producer
    // grow without bound. Fill it through the real path, then check the next one.
    constexpr size_t kCapacity = 8192;
    for (size_t i = 0; i < kCapacity; ++i) {
        const auto key = ColumnPrefetch::key_of(static_cast<int32_t>(i), 0);
        CHECK(prefetch.request(key, epoch));
        prefetch.publish(key, epoch, bounds_of(0.0f, 1.0f));
    }
    CHECK(prefetch.stats().published == kCapacity);
    CHECK(prefetch.stats().dropped == 0);

    const auto overflow = ColumnPrefetch::key_of(static_cast<int32_t>(kCapacity), 0);
    CHECK(prefetch.request(overflow, epoch));
    prefetch.publish(overflow, epoch, bounds_of(0.0f, 1.0f));
    CHECK(prefetch.stats().dropped == 1);
    ColumnBounds out;
    CHECK_FALSE(prefetch.try_take(overflow, out));
    // The dropped request is retired too, so the column can be asked for again
    // once the consumer drains what is waiting.
    CHECK(prefetch.outstanding() == 0);
    for (size_t i = 0; i < kCapacity; ++i) {
        CHECK(prefetch.try_take(ColumnPrefetch::key_of(static_cast<int32_t>(i), 0), out));
    }
    CHECK(prefetch.request(overflow, epoch));
}
