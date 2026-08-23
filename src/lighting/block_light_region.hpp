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

    void clear_block_light() noexcept {
        for (int32_t dz = 0; dz < 3; ++dz) {
            for (int32_t dy = 0; dy < 3; ++dy) {
                for (int32_t dx = 0; dx < 3; ++dx) {
                    if (region_[dx][dy][dz]) {
                        region_[dx][dy][dz]->clear_block_light();
                    }
                }
            }
        }
    }

    void collect_emissive_sources(std::vector<EmissiveSource>& out_sources) const {
        const BlockRegistry& registry = BlockRegistry::get_instance();
        out_sources.clear();

        for (int8_t rdz = -1; rdz <= 1; ++rdz) {
            for (int8_t rdy = -1; rdy <= 1; ++rdy) {
                for (int8_t rdx = -1; rdx <= 1; ++rdx) {
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

    void propagate_additive(const std::vector<EmissiveSource>& sources) {
        if (sources.empty()) {
            return;
        }

        clear_block_light();

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
            src_chunk->set_light_rgb_unsafe(source.x, source.y, source.z, source.r, source.g, source.b);
            queue.push_back({
                source.rdx, source.rdy, source.rdz,
                source.x, source.y, source.z,
                source.r, source.g, source.b
            });
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

                const uint8_t cur_r = dst_chunk->get_light_r_unsafe(nx, ny, nz);
                const uint8_t cur_g = dst_chunk->get_light_g_unsafe(nx, ny, nz);
                const uint8_t cur_b = dst_chunk->get_light_b_unsafe(nx, ny, nz);

                const uint8_t out_r = std::max(cur_r, next_r);
                const uint8_t out_g = std::max(cur_g, next_g);
                const uint8_t out_b = std::max(cur_b, next_b);

                if (out_r != cur_r || out_g != cur_g || out_b != cur_b) {
                    dst_chunk->set_light_rgb_unsafe(nx, ny, nz, out_r, out_g, out_b);
                    queue.push_back({nrdx, nrdy, nrdz, nx, ny, nz, out_r, out_g, out_b});
                }
            }
        }
    }

private:
    ChunkData* region_[3][3][3]{};

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