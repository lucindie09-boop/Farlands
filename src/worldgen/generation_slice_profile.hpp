#ifndef FARLANDS_GENERATION_SLICE_PROFILE_HPP
#define FARLANDS_GENERATION_SLICE_PROFILE_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "world/generation_path_profile.hpp"

namespace VoxelEngine {

// Where a worker's per-chunk time goes. GenerationPathProfile splits a chunk's
// trip into wait/work/drain and leaves `work` as one blob; these are the slices
// inside it, so a change can be aimed at the part that is actually spending the
// ~3 ms instead of at the pipeline around it.
//
// Off unless FARLANDS_GEN_PROFILE=1 (the same flag as the path profile). Each
// slice is one relaxed atomic add per chunk and one clock read: eight reads
// against a 3 ms body is noise, and a mutex here would be the profile's own cost.
enum class GenSlice : int {
    Columns = 0,
    Density,
    Materials,
    Vegetation,
    EditMap,
    Light,
    Solid,
    Count
};

inline std::atomic<uint64_t>& gen_slice_ns(GenSlice slice) {
    static std::atomic<uint64_t> counters[static_cast<size_t>(GenSlice::Count)];
    return counters[static_cast<size_t>(slice)];
}

// One per generated chunk, so every slice reads as milliseconds per chunk.
inline std::atomic<uint64_t>& gen_slice_chunks() {
    static std::atomic<uint64_t> chunks{0};
    return chunks;
}

// Counted once at the worker lambda's entry: the slices are divided by this, and
// a chunk that returns early (a uniform fast path) must still count as a chunk.
inline void gen_slice_count_chunk() {
    if (!GenerationPathProfile::enabled()) return;
    gen_slice_chunks().fetch_add(1, std::memory_order_relaxed);
}

class GenSliceTimer {
public:
    explicit GenSliceTimer(GenSlice slice)
        : slice_(slice),
          start_(GenerationPathProfile::enabled() ? generation_path_now_ns() : 0) {}

    ~GenSliceTimer() { stop(); }

    // Ends the slice before this timer's scope does: the generator's phase
    // boundaries are inside one long function, so a timer that only stopped at
    // scope end would measure the rest of it. Idempotent, and the destructor
    // calls it, so an early return still records the time.
    void stop() {
        if (start_ == 0) return;
        gen_slice_ns(slice_).fetch_add(generation_path_now_ns() - start_,
                                       std::memory_order_relaxed);
        start_ = 0;
    }

    GenSliceTimer(const GenSliceTimer&) = delete;
    GenSliceTimer& operator=(const GenSliceTimer&) = delete;

private:
    GenSlice slice_;
    uint64_t start_;
};

[[nodiscard]] inline std::string generation_slice_report() {
    if (!GenerationPathProfile::enabled()) return "slices disabled";
    const uint64_t chunks = gen_slice_chunks().load(std::memory_order_relaxed);
    const auto per_chunk_ms = [chunks](GenSlice slice) {
        if (chunks == 0) return 0.0;
        return static_cast<double>(gen_slice_ns(slice).load(std::memory_order_relaxed)) /
               static_cast<double>(chunks) / 1.0e6;
    };
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "slices/chunk columns=%.2f density=%.2f materials=%.2f veget=%.2f "
        "edit=%.2f light=%.2f solid=%.2f chunks=%llu",
        per_chunk_ms(GenSlice::Columns),
        per_chunk_ms(GenSlice::Density), per_chunk_ms(GenSlice::Materials),
        per_chunk_ms(GenSlice::Vegetation), per_chunk_ms(GenSlice::EditMap),
        per_chunk_ms(GenSlice::Light), per_chunk_ms(GenSlice::Solid),
        static_cast<unsigned long long>(chunks));
    return std::string(buf);
}

} // namespace VoxelEngine

#endif // FARLANDS_GENERATION_SLICE_PROFILE_HPP
