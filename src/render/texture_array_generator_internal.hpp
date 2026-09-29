#pragma once

// The image helpers every layer of both texture arrays goes through: the mipmap and
// format normalisers, the S3TC compression step and the magenta/black checker
// placeholder. Shared by render/texture_array_generator.cpp (the albedo array) and
// render/texture_array_generator_emissive.cpp, which is why they are a named
// namespace rather than the anonymous one they sat in while both builds were one
// file.

#include <godot_cpp/classes/image.hpp>

namespace VoxelEngine {
namespace texture_array_detail {

// Forces an image into the requested mipmap state: an image from
// texture->get_image() may already carry mipmaps from the 3D texture import, while
// one built by create_from_data never does, and create_from_images() refuses an
// array whose layers disagree.
void normalize_mipmaps(godot::Ref<godot::Image>& image, bool enabled);

// Normalizes an image to RGBA8: texture packs may carry mixed formats (RGB8, RGBA8),
// which would make create_from_images() fail.
void normalize_format(godot::Ref<godot::Image>& image);

// Applies GPU compression (S3TC = DXT1/DXT5, BC1/BC3) when enabled: ~4-6x less VRAM
// for lossy artifacts.
void apply_compression(godot::Ref<godot::Image>& image, bool enabled);

// The magenta/black checker used for every layer when textures are disabled.
godot::Ref<godot::Image> build_checker_image(int width, int height);

} // namespace texture_array_detail
} // namespace VoxelEngine
