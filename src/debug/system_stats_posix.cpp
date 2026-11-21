#ifndef _WIN32

#include "debug/system_stats.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace farlands {
namespace {

// The first line of /proc/stat: "cpu <user> <nice> <system> <idle> <iowait>
// <irq> <softirq> <steal> ..." in USER_HZ ticks since boot. Only the first eight
// fields are the machine's own time; the rest (guest, guest_nice) are already
// counted inside user/nice on Linux.
bool read_proc_stat(CpuSample& out) {
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        return false;
    }
    std::string line;
    if (!std::getline(stat, line)) {
        return false;
    }
    std::istringstream fields(line);
    std::string label;
    if (!(fields >> label) || label != "cpu") {
        return false;
    }
    constexpr int kFieldCount = 8;
    std::uint64_t values[kFieldCount] = {};
    std::uint64_t total = 0;
    for (int i = 0; i < kFieldCount; ++i) {
        if (!(fields >> values[i])) {
            return false;
        }
        total += values[i];
    }
    out.idle_ticks = values[3] + values[4];  // idle + iowait
    out.total_ticks = total;
    return total > 0;
}

}  // namespace

bool platform_cpu_sample(CpuSample& out) {
    return read_proc_stat(out);
}

bool platform_gpu_percent(double& out) {
    // amdgpu and i915 publish a busy percentage here. NVIDIA's driver does not,
    // so that machine reports "unknown" rather than a zero it did not measure.
    constexpr int kMaxCards = 16;
    for (int card = 0; card < kMaxCards; ++card) {
        const std::string path =
            "/sys/class/drm/card" + std::to_string(card) + "/device/gpu_busy_percent";
        std::ifstream busy(path);
        int percent = 0;
        if (busy.is_open() && (busy >> percent)) {
            out = clamp_gpu_percent(static_cast<double>(percent));
            return true;
        }
    }
    return false;
}

}  // namespace farlands

#endif  // _WIN32
