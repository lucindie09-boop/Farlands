#ifndef FARLANDS_PERFORMANCE_TIMER_HPP
#define FARLANDS_PERFORMANCE_TIMER_HPP
#include <cstdint>
#include <chrono>
#include <array>
#include <cmath>
#include <string>
#include <algorithm>
#include <atomic>

namespace VoxelEngine {

#define TIMER_LIST(X) \
    X(UpdateCollision) \
    X(ChunkLoadUnload) \
    X(DirtyMeshQueue) \
    X(ProcessCompleted) \
    X(BuildMesh) \
    X(MeshArrayAlloc) \
    X(MeshDataCopy) \
    X(MeshGpuUpload) \
    X(ProcessTotal) \
    X(PlayerPosUpdate) \
    X(GenerateChunk) \
    X(ConvertToGodotMesh) \
    X(ProcessCompletedChunks) \
    X(ProcessCompletedMeshes) \
    X(GodotFrameOverhead) \
    X(WorldUpdate) \
    X(SceneUpdate) \
    X(RenderTime) \
    X(MeshUploadGpu) \
X(SolidCachePopulation) \
X(GreedyMeshHorizontal) \
X(GreedyMeshVertical) \
X(Voxelization) \
X(LodUpdate) \
X(GroupMeshBuild) \
X(GroupMeshUpload) \
X(PaletteWrite) \
X(LightPropagation)

enum class TimerID : uint8_t {
#define X(name) name,
    TIMER_LIST(X)
#undef X
    Count
};

class PerformanceTimer {
private:
    struct TimerData {
        std::atomic<uint64_t> total_ns{0};
        std::atomic<double> min_time{1e9};
        std::atomic<double> max_time{0.0};
        std::atomic<uint64_t> count{0};
        
        void add_sample(double time_ms) {
            uint64_t ns = static_cast<uint64_t>(time_ms * 1e6);
            total_ns.fetch_add(ns, std::memory_order_relaxed);
            double old_min = min_time.load(std::memory_order_relaxed);
            while (time_ms < old_min && !min_time.compare_exchange_weak(old_min, time_ms,
                                                                        std::memory_order_relaxed));
            double old_max = max_time.load(std::memory_order_relaxed);
            while (time_ms > old_max && !max_time.compare_exchange_weak(old_max, time_ms,
                                                                        std::memory_order_relaxed));
            count.fetch_add(1, std::memory_order_relaxed);
        }
        
        double get_avg() const {
            uint64_t c = count.load(std::memory_order_relaxed);
            return c > 0 ? (total_ns.load(std::memory_order_relaxed) / 1e6) / c : 0.0;
        }
    };
    
    std::array<TimerData, static_cast<size_t>(TimerID::Count)> timers;

    static constexpr const char* TIMER_NAMES[] = {
#define X(name) #name,
        TIMER_LIST(X)
#undef X
    };
    
public:
    PerformanceTimer() {
        reset_all();
    }
    
    void add_sample(TimerID id, double time_ms) {
        timers[static_cast<size_t>(id)].add_sample(time_ms);
    }
    
    double get_avg(TimerID id) const {
        return timers[static_cast<size_t>(id)].get_avg();
    }
    
    double get_min(TimerID id) const {
        return timers[static_cast<size_t>(id)].min_time.load(std::memory_order_relaxed);
    }
    
    double get_max(TimerID id) const {
        return timers[static_cast<size_t>(id)].max_time.load(std::memory_order_relaxed);
    }
    
    uint64_t get_count(TimerID id) const {
        return timers[static_cast<size_t>(id)].count.load(std::memory_order_relaxed);
    }
    
    void reset(TimerID id) {
        TimerData empty;
        auto& dst = timers[static_cast<size_t>(id)];
        dst.total_ns.store(empty.total_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.min_time.store(empty.min_time.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.max_time.store(empty.max_time.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst.count.store(empty.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    
    void reset_all() {
        for (auto& timer : timers) {
            timer.total_ns.store(0, std::memory_order_relaxed);
            timer.min_time.store(1e9, std::memory_order_relaxed);
            timer.max_time.store(0.0, std::memory_order_relaxed);
            timer.count.store(0, std::memory_order_relaxed);
        }
    }
    
    void reset_min_max(TimerID id) {
        auto& timer = timers[static_cast<size_t>(id)];
        timer.min_time.store(-1.0, std::memory_order_relaxed);
        timer.max_time.store(-1.0, std::memory_order_relaxed);
    }
    
    void reset_all_min_max() {
        for (auto& timer : timers) {
            timer.min_time.store(-1.0, std::memory_order_relaxed);
            timer.max_time.store(-1.0, std::memory_order_relaxed);
        }
    }
    
    const char* get_name(TimerID id) const {
        return TIMER_NAMES[static_cast<size_t>(id)];
    }
    
    std::string get_report() const {
        std::string report = "=== Performance Timings ===\n";
        for (size_t i = 0; i < static_cast<size_t>(TimerID::Count); i++) {
            const TimerData& data = timers[i];
            if (data.count.load(std::memory_order_relaxed) > 0) {
                report += TIMER_NAMES[i] + std::string(" avg: ") + std::to_string(data.get_avg()) + " ms ";
                report += "(min " + std::to_string(data.min_time.load(std::memory_order_relaxed)) + " ";
                report += "max " + std::to_string(data.max_time.load(std::memory_order_relaxed)) + ")\n";
            }
        }
        return report;
    }
};

// RAII scoped timer
class ScopedTimer {
private:
    PerformanceTimer& timer;
    TimerID id;
    std::chrono::high_resolution_clock::time_point start;
    
public:
    ScopedTimer(PerformanceTimer& t, TimerID timer_id)
        : timer(t), id(timer_id), start(std::chrono::high_resolution_clock::now()) {}
    
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        timer.add_sample(id, elapsed.count());
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_PERFORMANCE_TIMER_HPP