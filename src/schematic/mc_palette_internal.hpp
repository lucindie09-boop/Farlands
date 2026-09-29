#pragma once

// The one piece of the Minecraft block table that both halves of the palette
// need, and the reason it is not file-local: a "names" row is parsed while the
// table loads (mc_palette.cpp) and its {wood} placeholder is substituted while a
// state is resolved (mc_palette_resolve.cpp).

#include "schematic/mc_palette.hpp"

namespace VoxelEngine {
namespace schematic {
namespace mc_palette_detail {

// The placeholder a "names" row uses for the species its pattern captured.
constexpr const char* kWoodPlaceholder = "{wood}";

// Whether `text` carries that placeholder at all.
[[nodiscard]] bool has_placeholder(const std::string& text) noexcept;

} // namespace mc_palette_detail
} // namespace schematic
} // namespace VoxelEngine
