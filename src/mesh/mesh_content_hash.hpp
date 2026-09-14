#ifndef FARLANDS_MESH_CONTENT_HASH_HPP
#define FARLANDS_MESH_CONTENT_HASH_HPP

// -----------------------------------------------------------------------------
// Upload deduplication hash.
//
// A chunk's mesh RID carries up to two surfaces — opaque terrain and the liquid
// surface — and they are uploaded together, in one `mesh_add_surface_from_arrays`
// pair, guarded by a single "did the content change?" comparison. So the hash has
// to cover BOTH of them. It used to cover only the opaque vertices, and that is
// the exact shape of a bug that is hard to see: a liquid is transparent, so
// flowing water changes the water mesh while every opaque vertex stays
// byte-identical (same blocks, same light) — the comparison then said "nothing
// changed", the whole upload was skipped, and the water existed in the world
// (collision, outline, swimming) with no geometry on screen until some unrelated
// edit finally moved an opaque vertex.
//
// Two properties the test pins, because both are easy to get wrong:
//   * a change confined to the water mesh changes the result;
//   * opaque A + water B never hashes the same as opaque B + water A (the
//     separator byte below is what makes that so), and neither does
//     opaque-only-A + water-B collide with opaque-A+B + water-only.
// -----------------------------------------------------------------------------

#include "core/hash_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace VoxelEngine {

// Hash of everything one upload carries. `0` means "nothing to upload" and is
// also the "no previous upload" sentinel the caller compares against, so an
// empty mesh never deduplicates against a real one.
template <typename VertexContainer, typename IndexContainer>
[[nodiscard]] inline uint64_t mesh_content_hash(const VertexContainer& solid_vertices,
                                                const IndexContainer& solid_indices,
                                                const VertexContainer& water_vertices,
                                                const IndexContainer& water_indices) {
    using SolidValue = typename VertexContainer::value_type;
    using IndexValue = typename IndexContainer::value_type;

    uint64_t hash = 0;
    if (!solid_vertices.empty() && !solid_indices.empty()) {
        hash = fnv1a_hash_bytes(solid_vertices.data(),
                                solid_vertices.size() * sizeof(SolidValue));
        hash = fnv1a_hash_bytes(solid_indices.data(),
                                solid_indices.size() * sizeof(IndexValue), hash);
    }
    if (!water_vertices.empty() && !water_indices.empty()) {
        // A byte between the two surfaces, so a boundary between them cannot be
        // mistaken for the other surface's content (see the header comment).
        static constexpr uint8_t kSurfaceSeparator = 0xFF;
        hash = fnv1a_hash_bytes(&kSurfaceSeparator, sizeof(kSurfaceSeparator), hash);
        hash = fnv1a_hash_bytes(water_vertices.data(),
                                water_vertices.size() * sizeof(SolidValue), hash);
        hash = fnv1a_hash_bytes(water_indices.data(),
                                water_indices.size() * sizeof(IndexValue), hash);
    }
    return hash;
}

} // namespace VoxelEngine

#endif // FARLANDS_MESH_CONTENT_HASH_HPP
