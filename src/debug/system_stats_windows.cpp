#ifdef _WIN32

#include "debug/system_stats.hpp"

#include <windows.h>

#include "debug/smbios_memory.hpp"  // the RAM's own description (parser)
// The GPU Engine counters are the same PDH counters Task Manager reads; they
// exist on Windows 10 1709 and later with a WDDM 2.0 driver, and their absence
// is reported as "unknown", never as zero.
#include <dxgi.h>  // the adapter's dedicated memory: Godot has no VRAM capacity monitor
#include <pdh.h>
#include <pdhmsg.h>  // PDH_MORE_DATA, the size-probe result of the array read

#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace farlands {
namespace {

// One query for the whole process: a PDH query is a kernel object, and the GPU
// Engine counters carry an instance per process, so one query is widened by the
// pid filter below rather than opened per reader. A machine without the
// counters (or with the counter name unavailable) fails once and is never asked
// again.
struct GpuQuery {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    // "pid_<this process>_": every instance this process owns starts with it, so
    // the sum can exclude every other application's engines.
    std::wstring own_pid_prefix;
    bool open = false;
    bool unavailable = false;
};

GpuQuery& gpu_query() {
    // Function-local static of plain Win32 handles and a std::wstring: safe to
    // construct before the engine exists, unlike anything holding a godot-cpp
    // type (AGENTS.md).
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
    query.own_pid_prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
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
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return false;
    }
    // 100 ns units for this process only; the kernel half of GetSystemTimes
    // contains the machine's idle time, which is why that read is not reused here.
    const std::uint64_t busy = filetime_ticks(kernel) + filetime_ticks(user);
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (QueryPerformanceFrequency(&frequency) == 0 || QueryPerformanceCounter(&counter) == 0 ||
        frequency.QuadPart <= 0) {
        return false;
    }
    // The performance counter in the 100 ns unit the process times already use;
    // both sides are 64 bits of a monotonic counter, so their difference is the
    // interval the share is taken over.
    const double seconds =
        static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
    const auto wall = static_cast<std::uint64_t>(seconds * 1.0e7);
    out = process_cpu_sample(busy, wall, std::thread::hardware_concurrency());
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
        if (value.CStatus != ERROR_SUCCESS || !(value.doubleValue > 0.0) || items[i].szName == nullptr) {
            continue;
        }
        // Another application's engines are in the same counter; only ours count.
        if (std::wcsncmp(items[i].szName, query.own_pid_prefix.c_str(),
                         query.own_pid_prefix.size()) != 0) {
            continue;
        }
        sum += value.doubleValue;
    }
    out = clamp_gpu_percent(sum);
    return true;
}

bool platform_memory_spec(RamSummary& out) {
    // 'RSMB' is the raw SMBIOS provider; the first call reports the size it needs.
    const UINT needed = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (needed <= 8) {
        return false;
    }
    std::vector<unsigned char> raw(needed);
    if (GetSystemFirmwareTable('RSMB', 0, raw.data(), needed) != needed) {
        return false;
    }
    // RawSMBIOSData: four version bytes, then a DWORD length, then the table.
    const auto table_length = static_cast<std::uint32_t>(raw[4]) |
                              (static_cast<std::uint32_t>(raw[5]) << 8) |
                              (static_cast<std::uint32_t>(raw[6]) << 16) |
                              (static_cast<std::uint32_t>(raw[7]) << 24);
    if (table_length == 0 || 8 + static_cast<std::size_t>(table_length) > raw.size()) {
        return false;
    }
    return parse_smbios_memory(raw.data() + 8, table_length, out);
}

bool platform_vram_total_bytes(std::uint64_t& out) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return false;
    }
    std::uint64_t best = 0;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) != S_OK || adapter == nullptr) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // The Basic Render Driver is the software fallback, and its "dedicated"
            // memory is the system's; on a machine with both an integrated and a
            // discrete adapter the largest real one is the card a game runs on.
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (!software && desc.DedicatedVideoMemory > best) {
                best = desc.DedicatedVideoMemory;
            }
        }
        adapter->Release();
    }
    factory->Release();
    if (best == 0) {
        return false;
    }
    out = best;
    return true;
}

}  // namespace farlands

#endif  // _WIN32
