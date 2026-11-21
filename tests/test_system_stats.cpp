#include "doctest.h"
#include "debug/system_stats.hpp"

#include <limits>

using namespace farlands;

TEST_CASE("cpu percent between samples is the busy share of the interval") {
    CpuSample previous;
    previous.idle_ticks = 1000;
    previous.total_ticks = 4000;

    // 1000 idle ticks out of 4000 elapsed: three quarters busy.
    CpuSample quarter_idle;
    quarter_idle.idle_ticks = 2000;
    quarter_idle.total_ticks = 8000;
    CHECK(quarter_idle.percent_since(previous) == doctest::Approx(75.0f));

    CpuSample fully_idle;
    fully_idle.idle_ticks = 5000;
    fully_idle.total_ticks = 8000;
    CHECK(fully_idle.percent_since(previous) == doctest::Approx(0.0f));

    CpuSample fully_busy;
    fully_busy.idle_ticks = 1000;
    fully_busy.total_ticks = 8000;
    CHECK(fully_busy.percent_since(previous) == doctest::Approx(100.0f));
}

TEST_CASE("cpu percent treats a stale or reset counter as no time passing") {
    CpuSample previous;
    previous.idle_ticks = 1000;
    previous.total_ticks = 4000;

    // The same reading twice: no interval, so no reading rather than a division
    // by zero.
    const CpuSample unchanged = previous;
    CHECK(unchanged.percent_since(previous) == doctest::Approx(0.0f));

    // Counters that went backwards (a reset, or a different machine's numbers).
    CpuSample backwards;
    backwards.idle_ticks = 900;
    backwards.total_ticks = 3000;
    CHECK(backwards.percent_since(previous) == doctest::Approx(0.0f));

    // An idle counter that moved further than the total one cannot mean less than
    // zero busy time.
    CpuSample idle_backwards;
    idle_backwards.idle_ticks = 500;
    idle_backwards.total_ticks = 5000;
    CHECK(idle_backwards.percent_since(previous) == doctest::Approx(0.0f));
}

TEST_CASE("cpu percent clamps an idle jump past the elapsed time to zero busy") {
    CpuSample previous;
    previous.idle_ticks = 1000;
    previous.total_ticks = 4000;

    // 5000 idle ticks in a 1000-tick interval: a counter that reset mid-interval.
    CpuSample impossible;
    impossible.idle_ticks = 6000;
    impossible.total_ticks = 5000;
    CHECK(impossible.percent_since(previous) == doctest::Approx(0.0f));
}

TEST_CASE("gpu percent is the sum of the engine counters, capped at a full machine") {
    CHECK(clamp_gpu_percent(-5.0) == doctest::Approx(0.0f));
    CHECK(clamp_gpu_percent(0.0) == doctest::Approx(0.0f));
    CHECK(clamp_gpu_percent(41.5) == doctest::Approx(41.5f));
    // Several processes sharing the same engines can sum past 100; a full machine
    // is the most the meter can say.
    CHECK(clamp_gpu_percent(180.0) == doctest::Approx(100.0f));
    CHECK(clamp_gpu_percent(std::numeric_limits<double>::quiet_NaN()) == doctest::Approx(0.0f));
}
