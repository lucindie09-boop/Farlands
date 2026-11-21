#ifndef _WIN32

#include "debug/system_stats.hpp"

#include "debug/smbios_memory.hpp"  // the RAM's own description (parser)

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <iterator>
#include <thread>
#include <unistd.h>
#include <vector>

namespace farlands {
namespace {

// Fields 14 and 15 of /proc/self/stat, utime and stime in USER_HZ ticks, read
// after the executable name's closing paren: field 2 is that name in
// parentheses and may itself contain spaces or parens, so counting tokens from
// the start of the line is how a reader gets the field numbers wrong.
bool read_self_times(std::uint64_t& ticks_out) {
    std::ifstream stat("/proc/self/stat");
    if (!stat.is_open()) {
        return false;
    }
    std::string line;
    if (!std::getline(stat, line)) {
        return false;
    }
    const std::size_t close = line.rfind(')');
    if (close == std::string::npos) {
        return false;
    }
    std::istringstream fields(line.substr(close + 1));
    // Fields 3 through 15: state, ppid, ... utime (14), stime (15).
    constexpr int kFieldCount = 13;
    std::string tokens[kFieldCount];
    for (auto& token : tokens) {
        if (!(fields >> token)) {
            return false;
        }
    }
    ticks_out = std::strtoull(tokens[11].c_str(), nullptr, 10) +
                std::strtoull(tokens[12].c_str(), nullptr, 10);
    return true;
}

}  // namespace

bool platform_cpu_sample(CpuSample& out) {
    std::uint64_t ticks = 0;
    if (!read_self_times(ticks)) {
        return false;
    }
    const long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) {
        return false;
    }
    // USER_HZ to the 100 ns unit the Windows side works in, so the two platforms
    // feed percent_since() the same scale.
    const auto busy = static_cast<std::uint64_t>(
        static_cast<unsigned long long>(ticks) * (10'000'000ull / static_cast<std::uint64_t>(ticks_per_second)));
    using steady = std::chrono::steady_clock;
    const auto wall = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now().time_since_epoch())
            .count() /
        100);
    out = process_cpu_sample(busy, wall, std::thread::hardware_concurrency());
    return true;
}

bool platform_gpu_percent(double& out) {
    (void)out;
    // The DRM gpu_busy_percent file is the whole CARD's share, not this process's,
    // and a per-process figure would need the driver-specific drm-engine-* lines
    // in this process's /proc/self/fdinfo. Unknown is the honest answer: the row
    // draws '-', where the card-wide number would move with other applications.
    return false;
}

bool platform_memory_spec(RamSummary& out) {
    // The same table Windows exposes, with no header in front of it -- and
    // root-only (mode 0400) on most distributions, so this usually reports
    // unknown rather than a size it guessed.
    std::ifstream table("/sys/firmware/dmi/tables/DMI", std::ios::binary);
    if (!table.is_open()) {
        return false;
    }
    const std::vector<unsigned char> raw{std::istreambuf_iterator<char>(table),
                                         std::istreambuf_iterator<char>()};
    return parse_smbios_memory(raw.data(), raw.size(), out);
}

bool platform_vram_total_bytes(std::uint64_t& out) {
    // amdgpu and radeon publish the card's total here; Intel's and NVIDIA's drivers
    // do not, so those machines report unknown rather than a size they guessed.
    constexpr int kMaxCards = 16;
    for (int card = 0; card < kMaxCards; ++card) {
        const std::string path = "/sys/class/drm/card" + std::to_string(card) +
                                 "/device/mem_info_vram_total";
        std::ifstream total(path);
        std::uint64_t bytes = 0;
        if (total.is_open() && (total >> bytes) && bytes > 0) {
            out = bytes;
            return true;
        }
    }
    return false;
}

}  // namespace farlands

#endif  // _WIN32
