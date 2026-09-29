// The emissive array build. Split from render/texture_array_generator.cpp, which
// owns the albedo array and the lookups: the two builds share only the image
// helpers (render/texture_array_generator_internal.hpp) and the class's static
// layer tables, so this is the peer of that file rather than a part of it.

#include "render/texture_array_generator.hpp"
#include "render/texture_array_generator_internal.hpp"

#include "core/block_types.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <optional>
#include <set>

namespace VoxelEngine {

using namespace texture_array_detail;

godot::Ref<godot::Texture2DArray> TextureArrayGenerator::generate_emissive_texture_array() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    // Collect unique emissive texture names (excluding empty)
    std::set<godot::String> unique_emissive;
    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        for (int f = 0; f < 6; ++f) {
            if (!bt.emissive_texture_names[f].empty()) {
                unique_emissive.insert(godot::String(bt.emissive_texture_names[f].c_str()));
            }
        }
    }

    s_global_emissive_array.instantiate();
    s_global_emissive_name_to_index.clear();

    // Determine target resolution from an albedo texture (reuse width/height)
    int target_width = 16;
    int target_height = 16;
    if (s_global_texture_array.is_valid() && s_global_texture_array->get_layers() > 0) {
        godot::Ref<godot::Image> ref_img = s_global_texture_array->get_layer_data(0);
        if (ref_img.is_valid()) {
            target_width = ref_img->get_width();
            target_height = ref_img->get_height();
        }
    }

    // Layer 0 = solid black (no emissive contribution)
    godot::PackedByteArray black_data;
    const int64_t black_size = static_cast<int64_t>(target_width) * target_height * 4;
    black_data.resize(black_size);
    if (black_data.size() != black_size) {
        ERR_PRINT("Failed to allocate emissive black layer");
        target_width = 16;
        target_height = 16;
        black_data.resize(static_cast<int64_t>(16) * 16 * 4);
    }
    black_data.fill(0);
    // Alpha = 255 so emissive.rgb * emissive.a doesn't multiply by zero-alpha edge cases
    for (int i = 3; i < static_cast<int>(black_data.size()); i += 4) {
        black_data[i] = 255;
    }
    godot::Ref<godot::Image> black_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, black_data);
    normalize_format(black_image);
    normalize_mipmaps(black_image, mipmaps_enabled_);
    apply_compression(black_image, compression_enabled_);

    godot::Array images;
    images.append(black_image);

    if (unique_emissive.empty()) {
        s_global_emissive_array->create_from_images(images);
        s_global_emissive_initialized = true;
        return s_global_emissive_array;
    }

    godot::ResourceLoader* loader = godot::ResourceLoader::get_singleton();

    for (const godot::String& tex_name : unique_emissive) {
        // Packs may override emissive textures too, but a missing emissive is
        // NOT a stone fallback — it means "no glow" (black below).
        std::optional<godot::String> path = TexturePackManager::get_instance().resolve_optional(tex_name);
        godot::Ref<godot::Image> emissive_image;

        if (textures_enabled_ && path.has_value()) {
            if (path->begins_with("user://")) {
                emissive_image = godot::Image::load_from_file(*path);
            } else {
                godot::Ref<godot::Texture2D> tex = loader->load(*path);
                if (tex.is_valid()) {
                    emissive_image = tex->get_image();
                }
            }
        }

        if (!emissive_image.is_valid()) {
            // Missing emissive texture → fall back to black (no glow)
            godot::PackedByteArray fb_data;
            const int64_t fb_size = static_cast<int64_t>(target_width) * target_height * 4;
            fb_data.resize(fb_size);
            if (fb_data.size() != fb_size) {
                ERR_PRINT("Failed to allocate emissive fallback layer");
                continue;
            }
            fb_data.fill(0);
            for (int i = 3; i < static_cast<int>(fb_data.size()); i += 4) {
                fb_data[i] = 255;
            }
            emissive_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, fb_data);
        } else {
            // Validate image dimensions before resize
            if (emissive_image->get_width() <= 0 || emissive_image->get_height() <= 0) {
                WARN_PRINT("Invalid emissive image dimensions for: " + tex_name + ", using fallback");
                godot::PackedByteArray fb_data;
                const int64_t fb_size = static_cast<int64_t>(target_width) * target_height * 4;
                fb_data.resize(fb_size);
                fb_data.fill(0);
                for (int i = 3; i < static_cast<int>(fb_data.size()); i += 4) {
                    fb_data[i] = 255;
                }
                emissive_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, fb_data);
            } else if (emissive_image->get_width() != target_width || emissive_image->get_height() != target_height) {
                emissive_image->resize(target_width, target_height, godot::Image::INTERPOLATE_NEAREST);
            }
        }

        if (!emissive_image.is_valid()) {
            WARN_PRINT("Emissive image became invalid during processing: " + tex_name + ", skipping layer");
            continue;
        }

        normalize_format(emissive_image);
        normalize_mipmaps(emissive_image, mipmaps_enabled_);
        apply_compression(emissive_image, compression_enabled_);

        const int layer_index = static_cast<int>(images.size());
        images.append(emissive_image);

        const godot::String& file_name = tex_name;
        s_global_emissive_name_to_index[file_name] = layer_index;
    }

    s_global_emissive_array->create_from_images(images);
    s_global_emissive_initialized = true;

    return s_global_emissive_array;
}

} // namespace VoxelEngine
