#include "doctest.h"

#include "core/lock_order_checker.hpp"

#include <atomic>
#include <thread>

using namespace VoxelEngine;

// =========================================================================
// Lock-order checker tests (src/core/lock_order_checker.hpp)
// =========================================================================

TEST_CASE("Lock-order checker: ascending acquisition succeeds") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(0);
    CHECK(held_.test(0));
    CHECK(max_held_ == 0);

    acquire(5);
    CHECK(held_.test(5));
    CHECK(max_held_ == 5);

    acquire(63);
    CHECK(held_.test(63));
    CHECK(max_held_ == 63);

    release(5);
    CHECK(!held_.test(5));
    CHECK(max_held_ == 63);

    release(63);
    CHECK(!held_.test(63));
    CHECK(max_held_ == 0);

    release(0);
    CHECK(!held_.test(0));
    CHECK(max_held_ == 0);
    CHECK(held_.none());
}

TEST_CASE("Lock-order checker: reset_all clears state") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(10);
    acquire(20);
    acquire(30);
    CHECK(held_.test(10));
    CHECK(held_.test(20));
    CHECK(held_.test(30));
    CHECK(max_held_ == 30);

    reset_all();
    CHECK(held_.none());
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: release recomputes max_held") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(3);
    acquire(7);
    acquire(12);
    CHECK(max_held_ == 12);

    release(12);
    CHECK(max_held_ == 7);

    release(3);
    CHECK(max_held_ == 7);

    release(7);
    CHECK(max_held_ == 0);
    CHECK(held_.none());
}

TEST_CASE("Lock-order checker: duplicate acquire is harmless") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(5);
    acquire(5);
    CHECK(max_held_ == 5);
    CHECK(held_.count() == 1);

    release(5);
    release(5);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: release of unheld shard is harmless") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(10);
    release(42);
    CHECK(max_held_ == 10);
    release(10);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: out-of-range shard indices are ignored") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(100);
    CHECK(held_.none());
    CHECK(max_held_ == 0);

    release(100);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: each thread has independent state") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(50);
    CHECK(max_held_ == 50);

    std::atomic<bool> child_acquired{false};
    std::atomic<bool> child_ok{false};
    std::thread t([&]() {
        acquire(3);
        child_ok.store(max_held_ == 3 && !held_.test(50), std::memory_order_relaxed);
        child_acquired.store(true, std::memory_order_release);
        release(3);
    });

    while (!child_acquired.load(std::memory_order_acquire))
        std::this_thread::yield();
    t.join();

    CHECK(child_ok.load());
    CHECK(max_held_ == 50);
    release(50);
}
