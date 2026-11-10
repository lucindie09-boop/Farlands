#ifndef FARLANDS_CROSS_BOUNDARY_QUEUE_HPP
#define FARLANDS_CROSS_BOUNDARY_QUEUE_HPP

#include "core/chunk_coords.hpp"

#include <mutex>
#include <vector>

namespace VoxelEngine {

// Chunks whose neighbours received a cross-boundary vegetation write. The
// producer is a generation worker and the consumer is the main-thread completion
// drain; the world reset clears it so the next fill cannot mesh the previous
// world's rim. Kept as a small type so the clear itself is testable without a
// Godot runtime.
class CrossBoundaryRemeshQueue {
public:
    void push(const ChunkPos& pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.push_back(pos);
    }

    // Hands the whole list to the caller. The queue is empty on return, so the
    // caller can execute the remeshes without holding the queue lock.
    std::vector<ChunkPos> take_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ChunkPos> out;
        out.swap(chunks_);
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.clear();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chunks_.size();
    }

private:
    std::vector<ChunkPos> chunks_;
    mutable std::mutex mutex_;
};

} // namespace VoxelEngine

#endif // FARLANDS_CROSS_BOUNDARY_QUEUE_HPP