#ifndef FARLANDS_BLOCK_LIGHT_REGION_HPP
#define FARLANDS_BLOCK_LIGHT_REGION_HPP
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "lighting/light_propagation.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace VoxelEngine {

class BlockLightRegion {
public:
    explicit BlockLightRegion(ChunkData* (&chunks)[3][3][3]) {
        for (int32_t dz = 0; dz < 3; ++dz) {
            for (int32_t dy = 0; dy < 3; ++dy) {
                for (int32_t dx = 0; dx < 3; ++dx) {
                    region_[dx][dy][dz] = chunks[dx][dy][dz];
                }
            }
        }
    }

    // Not noexcept: the per-slot snapshot allocates on first touch.
    void clear_block_light() {
        for (int32_t dz = 0; dz < 3; ++dz) {
            for (int32_t dy = 0; dy < 3; ++dy) {
                for (int32_t dx = 0; dx < 3; ++dx) {
                    cleared_slot_[dx][dy][dz] = true;
                    ChunkData* c = region_[dx][dy][dz];
                    if (!c) continue;
                    take_snapshot(dx - 1, dy - 1, dz - 1);
                    (void)c->clear_block_light();
                }
            }
        }
    }

    // Wipes only the part a change in the CENTRE chunk can reach: that chunk and its
    // six faces. A chunk is 32 wide and block light travels 15, so the edge and
    // corner slots of the 3x3x3 cannot have changed at all — clearing and rebuilding
    // them is four fifths of the pass for nothing, and measured, that four fifths is
    // 11.3 -> 5.3 ms with one emitter a chunk (29.6 -> 10.3 ms with four).
    //
    // This is only correct together with the boundary seeding in propagate_additive:
    // on its own it comes out DIMMER than the full clear (measured: up to 9 levels on
    // a channel), because light from an emitter in an untouched chunk travels through
    // cells that already hold their steady value and the search only continues from a
    // cell it WROTE an improvement to. Seeding the untouched side of the boundary with
    // its own light is what lets that light back in.
    void clear_block_light_affected() {
        static constexpr int8_t kAffected[7][3] = {
            {0, 0, 0},
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1},
        };
        for (int32_t dz = 0; dz < 3; ++dz)
            for (int32_t dy = 0; dy < 3; ++dy)
                for (int32_t dx = 0; dx < 3; ++dx) cleared_slot_[dx][dy][dz] = false;
        for (const auto& offset : kAffected) {
            cleared_slot_[offset[0] + 1][offset[1] + 1][offset[2] + 1] = true;
            ChunkData* c = at(offset[0], offset[1], offset[2]);
            if (!c) continue;
            take_snapshot(offset[0], offset[1], offset[2]);
            (void)c->clear_block_light();
        }
    }

    // Which of the 27 slots this pass actually CHANGED, as a 27-bit mask (bit
    // (dx+1) + (dy+1)*3 + (dz+1)*9). A pass over a region whose chunks hold no
    // block light and no emissive sources changes nothing at all, and the caller
    // marks chunks for a remesh from this.
    //
    // "Changed" is decided by comparing each touched slot's light bytes against a
    // snapshot taken before this pass first wrote to it, not by which slots were
    // written. A clear-and-repropagate normally lands back on exactly the values it
    // wiped -- an arriving chunk's neighbours hold light from their own sources and
    // come out identical -- so marking them would queue a rebuild of a mesh that is
    // already correct. That is 26 rebuilds per arriving chunk, and it is what made
    // loading a large build slower than building it.
    [[nodiscard]] uint32_t modified_mask() const {
        uint32_t mask = 0;
        for (int32_t dz = 0; dz < 3; ++dz) {
            for (int32_t dy = 0; dy < 3; ++dy) {
                for (int32_t dx = 0; dx < 3; ++dx) {
                    const ChunkData* c = region_[dx][dy][dz];
                    if (!c) continue;
                    const int32_t slot = slot_index(dx - 1, dy - 1, dz - 1);
                    if (!snapshot_taken_[slot]) continue;  // never written: unchanged
                    if (!c->light_state_equals(light_before_[slot])) {
                        mask |= slot_bit(dx - 1, dy - 1, dz - 1);
                    }
                }
            }
        }
        return mask;
    }

    [[nodiscard]] static constexpr uint32_t slot_bit(int32_t dx, int32_t dy, int32_t dz) noexcept {
        return 1u << static_cast<uint32_t>((dx + 1) + (dy + 1) * 3 + (dz + 1) * 9);
    }

    // `only_cleared` is for the caller that wiped just the affected seven: a source in
    // a chunk the pass did NOT wipe has already lit the cells around it, including the
    // boundary cells whose light seeds this pass, so re-seeding it from here would only
    // repeat work that is already in the data.
    void collect_emissive_sources(std::vector<EmissiveSource>& out_sources,
                                  bool only_cleared = false) const {
        const BlockRegistry& registry = BlockRegistry::get_instance();
        out_sources.clear();

        for (int8_t rdz = -1; rdz <= 1; ++rdz) {
            for (int8_t rdy = -1; rdy <= 1; ++rdy) {
                for (int8_t rdx = -1; rdx <= 1; ++rdx) {
                    if (only_cleared && !cleared_slot_[rdx + 1][rdy + 1][rdz + 1]) continue;
                    ChunkData* chunk = at(rdx, rdy, rdz);
                    if (!chunk || chunk->get_emissive_count() == 0) {
                        continue;
                    }

                    for (int32_t s = 0; s < CHUNK_SECTIONS; ++s) {
                        if (chunk->is_section_all_air(s)) {
                            continue;
                        }
                        const int32_t y0 = s * SECTION_HEIGHT;
                        const int32_t y1 = y0 + SECTION_HEIGHT;
                        for (int32_t y = y0; y < y1; ++y) {
                            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                                    const BlockID block_id = chunk->get_block_unsafe(x, y, z);
                                    if (block_id == BlockIDs::AIR) {
                                        continue;
                                    }
                                    const BlockType& type = registry.get_block(block_id);
                                    if (!HasProperty(type.properties, BlockProperty::Emissive)) {
                                        continue;
                                    }
                                    const uint8_t lr = type.light_r;
                                    const uint8_t lg = type.light_g;
                                    const uint8_t lb = type.light_b;
                                    if (lr == 0 && lg == 0 && lb == 0) {
                                        continue;
                                    }
                                    out_sources.push_back({
                                        rdx, rdy, rdz,
                                        static_cast<int16_t>(x),
                                        static_cast<int16_t>(y),
                                        static_cast<int16_t>(z),
                                        lr, lg, lb,
                                        type.light_pattern
                                    });
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // `already_cleared` is for the caller that has just cleared the region itself
    // (which it must, because an empty source list means "this region has no block
    // light" and this function returns early rather than clearing). Clearing twice
    // was measured at half the cost of a whole region pass on sky-lit chunks.
    void propagate_additive(const std::vector<EmissiveSource>& sources, bool already_cleared = false) {
        // Nothing to rebuild from and nothing outside a partial clear to carry light
        // back in: the region is dark, and saying so is the cheap answer — a region of
        // daylight terrain holds no block light and no sources at all.
        if (sources.empty() && !has_uncleared_slot()) {
            return;
        }

        if (!already_cleared) {
            clear_block_light();
        }

        struct Node {
            int8_t rdx = 0;
            int8_t rdy = 0;
            int8_t rdz = 0;
            int16_t x = 0;
            int16_t y = 0;
            int16_t z = 0;
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
        };

        std::vector<Node> queue;
        queue.reserve(sources.size() * 2);

        for (const EmissiveSource& source : sources) {
            ChunkData* src_chunk = at(source.rdx, source.rdy, source.rdz);
            if (!src_chunk) continue;
            take_snapshot(source.rdx, source.rdy, source.rdz);
            src_chunk->set_light_rgb_unsafe(source.x, source.y, source.z, source.r, source.g, source.b);
            queue.push_back({
                source.rdx, source.rdy, source.rdz,
                source.x, source.y, source.z,
                source.r, source.g, source.b
            });
        }

        // Boundary seeding, for a PARTIAL clear (see clear_block_light_affected).
        // Light that lives in the untouched chunks has to be let back in: the loop
        // below only continues from a cell it IMPROVED, and a cell in an untouched
        // chunk already holds its steady value, so without these seeds the path stops
        // dead at the boundary and the wiped chunks come out dimmer than they are. A
        // seed is the untouched cell at its own value; the loop's own arithmetic (one
        // level a step, less the destination's opacity) is what makes the light that
        // arrives inside the wiped volume correct. Only the planes facing a wiped
        // neighbour are walked — the boundary shell, not the whole chunk.
        if (has_uncleared_slot()) {
            static constexpr int8_t kDirs[6][3] = {
                {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
            };
            for (int32_t sdz = -1; sdz <= 1; ++sdz) {
                for (int32_t sdy = -1; sdy <= 1; ++sdy) {
                    for (int32_t sdx = -1; sdx <= 1; ++sdx) {
                        if (cleared_slot_[sdx + 1][sdy + 1][sdz + 1]) continue;
                        ChunkData* chunk = at(sdx, sdy, sdz);
                        if (chunk == nullptr) continue;
                        for (const auto& dir : kDirs) {
                            const int32_t ndx = sdx + dir[0];
                            const int32_t ndy = sdy + dir[1];
                            const int32_t ndz = sdz + dir[2];
                            if (ndx < -1 || ndx > 1 || ndy < -1 || ndy > 1 || ndz < -1 || ndz > 1) continue;
                            if (!cleared_slot_[ndx + 1][ndy + 1][ndz + 1]) continue;
                            // This chunk's plane facing the wiped neighbour. The axes
                            // are the only ones a 32-wide chunk has, so the free pair
                            // is picked per direction.
                            const int32_t axis = dir[0] != 0 ? 0 : (dir[1] != 0 ? 1 : 2);
                            const int32_t fixed = (dir[0] + dir[1] + dir[2]) > 0
                                                      ? (axis == 0 ? CHUNK_WIDTH : (axis == 1 ? CHUNK_HEIGHT : CHUNK_DEPTH)) - 1
                                                      : 0;
                            const int32_t a_max = axis == 0 ? CHUNK_HEIGHT : CHUNK_WIDTH;
                            const int32_t b_max = axis == 2 ? CHUNK_HEIGHT : CHUNK_DEPTH;
                            for (int32_t a = 0; a < a_max; ++a) {
                                for (int32_t b = 0; b < b_max; ++b) {
                                    int32_t x = 0, y = 0, z = 0;
                                    if (axis == 0) { x = fixed; y = a; z = b; }
                                    else if (axis == 1) { x = a; y = fixed; z = b; }
                                    else { x = a; y = b; z = fixed; }
                                    const uint8_t r = chunk->get_light_r_unsafe(x, y, z);
                                    const uint8_t g = chunk->get_light_g_unsafe(x, y, z);
                                    const uint8_t bl = chunk->get_light_b_unsafe(x, y, z);
                                    if (r <= 1 && g <= 1 && bl <= 1) continue;
                                    queue.push_back({static_cast<int8_t>(sdx), static_cast<int8_t>(sdy),
                                                     static_cast<int8_t>(sdz),
                                                     static_cast<int16_t>(x), static_cast<int16_t>(y),
                                                     static_cast<int16_t>(z), r, g, bl});
                                }
                            }
                        }
                    }
                }
            }
        }

        size_t head = 0;
        while (head < queue.size()) {
            const Node node = queue[head++];

            if (node.r <= 1 && node.g <= 1 && node.b <= 1) continue;

            uint8_t next_r = node.r > 1 ? static_cast<uint8_t>(node.r - 1) : 0;
            uint8_t next_g = node.g > 1 ? static_cast<uint8_t>(node.g - 1) : 0;
            uint8_t next_b = node.b > 1 ? static_cast<uint8_t>(node.b - 1) : 0;

            if (next_r == 0 && next_g == 0 && next_b == 0) continue;

            for (int i = 0; i < 6; ++i) {
                const int16_t* offset = kDiamondOffsets[i];

                int8_t nrdx = node.rdx;
                int8_t nrdy = node.rdy;
                int8_t nrdz = node.rdz;
                int16_t nx = static_cast<int16_t>(node.x + offset[0]);
                int16_t ny = static_cast<int16_t>(node.y + offset[1]);
                int16_t nz = static_cast<int16_t>(node.z + offset[2]);

                if (!wrap_local_to_region(nx, ny, nz, nrdx, nrdy, nrdz)) continue;

                ChunkData* dst_chunk = at(nrdx, nrdy, nrdz);
                if (!dst_chunk) {
                    continue;
                }

                const BlockID block_id = dst_chunk->get_block_unsafe(nx, ny, nz);
                const BlockType& block_type = BlockRegistry::get_instance().get_block(block_id);
                if (HasProperty(block_type.properties, BlockProperty::Opaque)) {
                    continue;
                }

                // Same opacity cost as the chunk-map BFS: light crossing a
                // translucent block drops by its opacity before landing.
                const int extra = block_type.light_opacity;
                const uint8_t cur_r = dst_chunk->get_light_r_unsafe(nx, ny, nz);
                const uint8_t cur_g = dst_chunk->get_light_g_unsafe(nx, ny, nz);
                const uint8_t cur_b = dst_chunk->get_light_b_unsafe(nx, ny, nz);

                const uint8_t out_r = std::max(cur_r, extra > 0 ? static_cast<uint8_t>(std::max(0, next_r - extra)) : next_r);
                const uint8_t out_g = std::max(cur_g, extra > 0 ? static_cast<uint8_t>(std::max(0, next_g - extra)) : next_g);
                const uint8_t out_b = std::max(cur_b, extra > 0 ? static_cast<uint8_t>(std::max(0, next_b - extra)) : next_b);

                if (out_r != cur_r || out_g != cur_g || out_b != cur_b) {
                    take_snapshot(nrdx, nrdy, nrdz);
                    dst_chunk->set_light_rgb_unsafe(nx, ny, nz, out_r, out_g, out_b);
                    queue.push_back({nrdx, nrdy, nrdz, nx, ny, nz, out_r, out_g, out_b});
                }
            }
        }
    }

private:
    ChunkData* region_[3][3][3]{};
    // Which slots the caller's clear wiped. A slot left false is intact, and is a
    // source of light for the wiped ones (see the seeding in propagate_additive).
    bool cleared_slot_[3][3][3] = {};
    // Light bytes of each slot as of the last moment before this pass wrote to it.
    // Taken at first touch, so a pass that touches nothing allocates nothing, and a
    // pristine chunk snapshots to a few dozen bytes because its sections are uniform.
    std::vector<uint8_t> light_before_[27];
    bool snapshot_taken_[27] = {};

    [[nodiscard]] bool has_uncleared_slot() const noexcept {
        for (int32_t dz = 0; dz < 3; ++dz)
            for (int32_t dy = 0; dy < 3; ++dy)
                for (int32_t dx = 0; dx < 3; ++dx)
                    if (!cleared_slot_[dx][dy][dz]) return true;
        return false;
    }

    [[nodiscard]] static constexpr int32_t slot_index(int32_t dx, int32_t dy, int32_t dz) noexcept {
        return (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9;
    }

    void take_snapshot(int8_t rdx, int8_t rdy, int8_t rdz) {
        ChunkData* c = at(rdx, rdy, rdz);
        if (!c) return;
        const int32_t slot = slot_index(rdx, rdy, rdz);
        if (snapshot_taken_[slot]) return;
        snapshot_taken_[slot] = true;
        light_before_[slot].clear();
        c->append_light_state(light_before_[slot]);
    }

    [[nodiscard]] static std::size_t grid_index(int32_t dx, int32_t dy, int32_t dz) noexcept {
        return static_cast<std::size_t>(dz) * 9 + static_cast<std::size_t>(dy) * 3 + static_cast<std::size_t>(dx);
    }

    [[nodiscard]] ChunkData* at(int8_t rdx, int8_t rdy, int8_t rdz) const noexcept {
        if (rdx < -1 || rdx > 1 || rdy < -1 || rdy > 1 || rdz < -1 || rdz > 1) {
            return nullptr;
        }
        return region_[rdx + 1][rdy + 1][rdz + 1];
    }

    [[nodiscard]] static std::size_t cell_index(int32_t x, int32_t y, int32_t z) noexcept {
        return static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * CHUNK_WIDTH + static_cast<std::size_t>(z) * CHUNK_WIDTH * CHUNK_HEIGHT;
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_LIGHT_REGION_HPP