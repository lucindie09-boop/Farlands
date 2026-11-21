#ifdef _WIN32

#include "debug/system_stats.hpp"

#include <windows.h>
// The GPU Engine counters are the same PDH counters Task Manager reads; they
// exist on Windows 10 1709 and later with a WDDM 2.0 driver, and their absence
// is reported as "unknown", never as zero.
#include <pdh.h>
#include <pdhmsg.h>  // PDH_MORE_DATA, the size-probe result of the array read

#include <vector>

namespace farlands {
namespace {

// One query for the whole process: a PDH query is a kernel object, and the GPU
// Engine counters are per process so one query answers for every reader. A
// machine without the counters (or with the counter name unavailable) fails once
// and is never asked again.
struct GpuQuery {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool open = false;
    bool unavailable = false;
};

GpuQuery& gpu_query() {
    // Function-local static of plain Win32 handles: safe to construct before the
    // engine exists, unlike anything holding a godot-cpp type (AGENTS.md).
    static GpuQuery query;
    return query;
}

// True when the query is open and has a baseline. The first successful call
// collects that baseline and still reports no reading, which is the PDH rule:
// a rate counter needs two collections.
bool ensure_gpu_query(GpuQuery& query) {
    if (query.open) {
        return true;
    }
    if (query.unavailable) {
        return false;
    }
    if (PdhOpenQueryW(nullptr, 0, &query.query) != ERROR_SUCCESS) {
        query.unavailable = true;
        return false;
    }
    // The English counter path: on a non-English Windows the localized name is
    // not something this code can know, and the English one is always resolvable.
    if (PdhAddEnglishCounterW(query.query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &query.counter) != ERROR_SUCCESS) {
        PdhCloseQuery(query.query);
        query.query = nullptr;
        query.counter = nullptr;
        query.unavailable = true;
        return false;
    }
    PdhCollectQueryData(query.query);
    query.open = true;
    return false;
}

std::uint64_t filetime_ticks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
           static_cast<std::uint64_t>(time.dwLowDateTime);
}

}  // namespace

bool platform_cpu_sample(CpuSample& out) {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) {
        return false;
    }
    out.idle_ticks = filetime_ticks(idle);
    // Kernel time already contains the idle time, so kernel + user is the whole
    // machine's elapsed time and idle over that is its idle share.
    out.total_ticks = filetime_ticks(kernel) + filetime_ticks(user);
    return true;
}

bool platform_gpu_percent(double& out) {
    GpuQuery& query = gpu_query();
    if (!ensure_gpu_query(query)) {
        return false;
    }
    if (PdhCollectQueryData(query.query) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(query.counter, PDH_FMT_DOUBLE, &size, &count, nullptr) !=
        PDH_MORE_DATA) {
        return false;
    }
    std::vector<unsigned char> storage(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    if (PdhGetFormattedCounterArrayW(query.counter, PDH_FMT_DOUBLE, &size, &count, items) !=
        ERROR_SUCCESS) {
        return false;
    }
    double sum = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        const PDH_FMT_COUNTERVALUE& value = items[i].FmtValue;
        if (value.CStatus == ERROR_SUCCESS && value.doubleValue > 0.0) {
            sum += value.doubleValue;
        }
    }
    out = clamp_gpu_percent(sum);
    return true;
}

}  // namespace farlands

#endif  // _WIN32
