#include "doctest.h"
#include "debug/smbios_memory.hpp"
#include "debug/system_stats.hpp"

#include <limits>
#include <vector>

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

TEST_CASE("a process sample is its own CPU time over the machine's, by core count") {
    // Four cores' worth of process time over one core's worth of wall time on an
    // eight-core box is half the machine, not 400%.
    const CpuSample before = process_cpu_sample(1000, 1000, 8);
    const CpuSample after = process_cpu_sample(5000, 2000, 8);
    CHECK(after.percent_since(before) == doctest::Approx(50.0f));

    // One core saturated on a one-core box is the whole machine, and a busy time
    // past every core floors idle rather than overshooting the percentage.
    const CpuSample empty = process_cpu_sample(0, 0, 1);
    CHECK(process_cpu_sample(1000, 1000, 1).percent_since(empty) == doctest::Approx(100.0f));
    CHECK(process_cpu_sample(9000, 1000, 8).percent_since(empty) == doctest::Approx(100.0f));

    // A zero core count is one core, not a divide by a zero width.
    CHECK(process_cpu_sample(0, 0, 0).total_ticks == 0);
    CHECK(process_cpu_sample(500, 1000, 0).percent_since(process_cpu_sample(0, 0, 0)) ==
          doctest::Approx(50.0f));
}

TEST_CASE("the ram spec names the generation, the speed and equal modules") {
    RamSummary modules;
    modules.type = "DDR4";
    modules.speed_mts = 3200;
    modules.modules = 2;
    modules.module_mb = 16384;
    modules.total_mb = 32768;
    CHECK(format_memory_spec(modules) == "DDR4-3200 (2x16 GB)");

    // One module: the row's own capacity already says its size.
    RamSummary single;
    single.type = "DDR5";
    single.speed_mts = 4800;
    single.modules = 1;
    single.module_mb = 32768;
    single.total_mb = 32768;
    CHECK(format_memory_spec(single) == "DDR5-4800");

    // Modules of different sizes cannot be "2x" anything.
    RamSummary mixed;
    mixed.type = "DDR4";
    mixed.speed_mts = 3200;
    mixed.modules = 2;
    mixed.total_mb = 24576;
    CHECK(format_memory_spec(mixed) == "DDR4-3200");

    // Nothing readable is an empty string rather than a guessed specification.
    CHECK(format_memory_spec(RamSummary{}).empty());
}

namespace {

void put_word(std::vector<unsigned char>& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<unsigned char>(value & 0xFF);
    bytes[at + 1] = static_cast<unsigned char>(value >> 8);
}

// One Type 17 structure as a 3.0-era firmware lays it out, with an empty string
// set after the formatted area (a double NUL).
std::vector<unsigned char> memory_device(std::uint16_t size_mb, std::uint8_t ram_type,
                                         std::uint16_t speed_mts) {
    std::vector<unsigned char> record(0x22, 0);
    record[0] = 17;
    record[1] = 0x22;
    put_word(record, 0x0C, size_mb);
    record[0x12] = ram_type;
    put_word(record, 0x15, speed_mts);
    record.push_back(0);
    record.push_back(0);
    return record;
}

}  // namespace

TEST_CASE("the smbios parser reads the memory devices the firmware describes") {
    std::vector<unsigned char> table;
    for (int module = 0; module < 2; ++module) {
        const auto fitted = memory_device(16384, 0x1A, 3200);
        table.insert(table.end(), fitted.begin(), fitted.end());
    }
    const auto empty_slot = memory_device(0, 0, 0);  // not fitted
    table.insert(table.end(), empty_slot.begin(), empty_slot.end());

    RamSummary ram;
    CHECK(parse_smbios_memory(table.data(), table.size(), ram));
    CHECK(ram.modules == 2);
    CHECK(ram.speed_mts == 3200);
    CHECK(ram.type == "DDR4");
    CHECK(ram.total_mb == 32768);
    CHECK(format_memory_spec(ram) == "DDR4-3200 (2x16 GB)");

    // A table whose only device is an empty slot is no reading at all.
    RamSummary unread;
    CHECK_FALSE(parse_smbios_memory(empty_slot.data(), empty_slot.size(), unread));
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
