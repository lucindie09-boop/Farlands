// Implementation half of render/texture_array_generator.hpp: the albedo array
// build, the resolved-path memo, the layer-index lookups and the two entry points
// that only exist to serve the build (populate_block_registry / force_regenerate).
// The emissive array build — the second half of the same idea — is in
// render/texture_array_generator_emissive.cpp, and the image helpers both use are
// declared in render/texture_array_generator_internal.hpp.
//
// All of this used to be inline bodies in the header, so the four files that include
// it — material_manager, chunk_manager_render, player_controller_inventory and
// register_types — recompiled the whole build whenever a line of it changed. The
// header now keeps the class, the inline singleton accessor and the two in-class
// one-liners (cleanup and the path-cache invalidation).

#include "render/texture_array_generator.hpp"
#include "render/texture_array_generator_internal.hpp"

#include "core/block_types.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace VoxelEngine {

// The image helpers below are declared in the internal header; this file and the
// emissive build both call them unqualified.
using namespace texture_array_detail;

namespace texture_array_detail {

// Forces an image into the requested mipmap state. Images returned by
// texture->get_image() may already carry mipmaps from the 3D texture import
// (while create_from_data images never do), so every image must be normalized
// explicitly or create_from_images() rejects the array as mixed usage.
void normalize_mipmaps(godot::Ref<godot::Image>& image, bool enabled) {
    if (image.is_null()) return;
    if (enabled) {
        if (!image->has_mipmaps()) {
            image->generate_mipmaps();
        }
    } else {
        if (image->has_mipmaps()) {
            image->clear_mipmaps();
        }
    }
}

// Normalizes image format to RGBA8. Texture packs may contain mixed formats
// (RGB8, RGBA8, etc.) which causes create_from_images() to fail. This ensures
// all layers share a consistent format.
void normalize_format(godot::Ref<godot::Image>& image) {
    if (image.is_null()) return;
    if (image->get_format() != godot::Image::FORMAT_RGBA8) {
        image->convert(godot::Image::FORMAT_RGBA8);
    }
}

// Applies GPU compression to an image if enabled. Uses S3TC (DXT1/DXT5, BC1/BC3)
// which provides ~4:1–6:1 compression at the cost of lossy artifacts.
void apply_compression(godot::Ref<godot::Image>& image, bool enabled) {
    if (image.is_null() || !enabled) return;
    // COMPRESS_S3TC uses DXT1/DXT5 (BC1/BC3), not BC7. For better quality
    // (especially gradients), use COMPRESS_BPTC instead, but S3TC has wider support.
    image->compress(godot::Image::COMPRESS_S3TC);
}

// Magenta/black checker placeholder used when textures are disabled.
godot::Ref<godot::Image> build_checker_image(int width, int height) {
    if (width <= 0 || height <= 0) {
        ERR_PRINT("Invalid checker image dimensions");
        width = 16;
        height = 16;
    }
    
    const int cell = 4;
    godot::PackedByteArray data;
    const int64_t expected_size = static_cast<int64_t>(width) * height * 4;
    data.resize(expected_size);
    
    if (data.size() != expected_size) {
        ERR_PRINT("Failed to allocate checker image data");
        return godot::Image::create(width, height, false, godot::Image::FORMAT_RGBA8);
    }
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int64_t idx = (static_cast<int64_t>(y) * width + x) * 4;
            if (idx + 3 >= data.size()) continue; // Safety check
            const bool magenta = ((x / cell + y / cell) % 2) == 0;
            if (magenta) {
                data[idx] = 255;
                data[idx + 1] = 0;
                data[idx + 2] = 255;
            } else {
                data[idx] = 0;
                data[idx + 1] = 0;
                data[idx + 2] = 0;
            }
            data[idx + 3] = 255;
        }
    }
    return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGBA8, data);
}

} // namespace texture_array_detail

godot::String TextureArrayGenerator::get_safe_texture_path(const godot::String& texture_name) {
    auto it = texture_path_cache.find(texture_name);
    if (it != texture_path_cache.end()) {
        return it->second;
    }

    // Resolution stack (see TexturePackManager): active pack textures override
    // the built-in res://textures/blocks set, which itself falls back to
    // stone.png. The manager owns the path decision; the array build below is
    // unchanged.
    godot::String result = TexturePackManager::get_instance().resolve(texture_name);
    if (texture_name != "stone" && result == "res://textures/blocks/stone.png") {
        WARN_PRINT("Texture not found: " + texture_name + ", falling back to stone.png");
    }
    texture_path_cache.emplace(texture_name, result);
    return result;
}

godot::Ref<godot::Texture2DArray> TextureArrayGenerator::generate_texture_array() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    // Collect unique texture names from all registered blocks
    std::set<godot::String> unique_textures;
    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        for (int f = 0; f < 6; ++f) {
            if (!bt.texture_names[f].empty()) {
                unique_textures.insert(godot::String(bt.texture_names[f].c_str()));
            }
        }
    }
    
    godot::PackedStringArray texture_paths;
    for (const godot::String& texture_name : unique_textures) {
        texture_paths.append(get_safe_texture_path(texture_name));
    }

    s_global_texture_array.instantiate();
    godot::ResourceLoader* loader = godot::ResourceLoader::get_singleton();

    if (texture_paths.size() == 0) {
        ERR_PRINT("No block textures available to load.");
        return s_global_texture_array;
    }

    // Array layer resolution comes from the active pack's declared
    // base_resolution (default 16), not from whichever texture sorts first.
    // Pack and built-in layers are all resized to it with nearest-neighbour.
    const int width  = TexturePackManager::get_instance().get_base_resolution();
    const int height = width;

    godot::Array textures;
    s_global_texture_name_to_index.clear();

    for (int i = 0; i < static_cast<int>(texture_paths.size()); ++i) {
        const godot::String& path = texture_paths[i];
        
        godot::Ref<godot::Image> image;
        if (textures_enabled_) {
            if (path.begins_with("user://")) {
                // Pack textures live outside the import system: load the raw
                // PNG directly so we don't hit the ResourceLoader cache or the
                // exported .pck import remap.
                image = godot::Image::load_from_file(path);
            } else {
                godot::Ref<godot::Texture2D> texture = loader->load(path);
                if (texture.is_valid()) {
                    image = texture->get_image();
                }
            }
            
            if (!image.is_valid()) {
                // Corrupt/truncated pack PNG: degrade to the built-in texture
                // instead of dropping the layer (which would map the name to
                // layer 0/stone and look wrong).
                const godot::String builtin = "res://textures/blocks/" + texture_paths[i].get_file().get_basename() + ".png";
                godot::Ref<godot::Texture2D> fallback = loader->load(builtin);
                if (fallback.is_valid()) {
                    image = fallback->get_image();
                }
            }
            if (!image.is_valid()) {
                WARN_PRINT("Failed to load texture: " + texture_paths[i] + ", skipping layer");
                continue;
            }

            // Validate image dimensions before resize
            const int img_width = image->get_width();
            const int img_height = image->get_height();
            
            if (img_width <= 0 || img_height <= 0) {
                WARN_PRINT("Invalid image dimensions for: " + texture_paths[i] + ", skipping layer");
                continue;
            }

            if (img_width != width || img_height != height) {
                image->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
            }
        } else {
            image = build_checker_image(width, height);
        }

        if (!image.is_valid()) {
            WARN_PRINT("Image became invalid during processing: " + texture_paths[i] + ", skipping layer");
            continue;
        }

        // Normalize format to RGBA8
        normalize_format(image);
        
        // Force mipmap consistency: all images must have the same mipmap state
        if (mipmaps_enabled_) {
            if (!image->has_mipmaps()) {
                image->generate_mipmaps();
            }
        } else {
            if (image->has_mipmaps()) {
                // Create a new image without mipmaps
                godot::Ref<godot::Image> new_image = godot::Image::create(image->get_width(), image->get_height(), false, image->get_format());
                new_image->copy_from(image);
                image = new_image;
            }
        }
        
        apply_compression(image, compression_enabled_);

        const int layer_index = static_cast<int>(textures.size());
        textures.append(image);

        godot::String file_name = texture_paths[i].get_file().get_basename();
        s_global_texture_name_to_index[file_name] = layer_index;
    }

    if (textures.size() > 0) {
        s_global_texture_array->create_from_images(textures);
    }

    return s_global_texture_array;
}


bool TextureArrayGenerator::is_mipmaps_enabled() {
    return get_instance().mipmaps_enabled_;
}

void TextureArrayGenerator::set_mipmaps_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.mipmaps_enabled_ == enabled) return;
    gen.mipmaps_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
}

bool TextureArrayGenerator::is_textures_enabled() {
    return get_instance().textures_enabled_;
}

void TextureArrayGenerator::set_textures_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.textures_enabled_ == enabled) return;
    gen.textures_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
}

bool TextureArrayGenerator::is_compression_enabled() {
    return get_instance().compression_enabled_;
}

void TextureArrayGenerator::set_compression_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.compression_enabled_ == enabled) return;
    gen.compression_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
}

godot::Ref<godot::Texture2DArray> TextureArrayGenerator::get_texture_array() {
    if (!s_global_texture_array.is_valid() || !s_global_texture_initialized) {
        generate_texture_array();
        populate_block_registry();
        s_global_texture_initialized = true;
    } else {
        populate_block_registry();
    }
    return s_global_texture_array;
}

godot::Ref<godot::Texture2DArray> TextureArrayGenerator::get_emissive_texture_array() {
    if (!s_global_emissive_array.is_valid() || !s_global_emissive_initialized) {
        generate_emissive_texture_array();
    }
    return s_global_emissive_array;
}

int TextureArrayGenerator::get_texture_index(const godot::String& texture_name) {
    auto it = s_global_texture_name_to_index.find(texture_name);
    if (it != s_global_texture_name_to_index.end()) {
        return it->second;
    }

    godot::String safe_fallback = get_safe_texture_path(texture_name).get_file().get_basename();
    it = s_global_texture_name_to_index.find(safe_fallback);
    if (it != s_global_texture_name_to_index.end()) {
        return it->second;
    }

    return 0;
}

int TextureArrayGenerator::find_texture_layer(const godot::String& texture_name) {
    const auto it = s_global_texture_name_to_index.find(texture_name);
    if (it != s_global_texture_name_to_index.end()) {
        return it->second;
    }
    return -1;
}

int TextureArrayGenerator::get_emissive_texture_index(const godot::String& texture_name) {
    auto it = s_global_emissive_name_to_index.find(texture_name);
    if (it != s_global_emissive_name_to_index.end()) {
        return it->second;
    }
    return 0;
}

int TextureArrayGenerator::get_block_texture_index(const godot::String& block_name, const godot::String& face) {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        if (bt.name && block_name == bt.name) {
            int face_idx = 2; // default to top
            if (face == "right")  face_idx = 0;
            if (face == "left")   face_idx = 1;
            if (face == "top")    face_idx = 2;
            if (face == "bottom") face_idx = 3;
            if (face == "front")  face_idx = 4;
            if (face == "back")   face_idx = 5;

            if (bt.texture_names[face_idx].empty()) return 0;
            return get_texture_index(godot::String(bt.texture_names[face_idx].c_str()));
        }
    }
    return 0;
}

void TextureArrayGenerator::populate_block_registry() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    if (block_count == last_registry_count) {
        return;
    }
    last_registry_count = block_count;

    for (size_t i = 0; i < block_count; ++i) {
        BlockType* block = registry.get_block_mutable(static_cast<BlockID>(i));
        if (!block || !block->name) {
            continue;
        }

        for (int f = 0; f < 6; ++f) {
            if (block->texture_names[f].empty()) {
                block->texture_indices[f] = 0;
            } else {
                block->texture_indices[f] = get_texture_index(
                    godot::String(block->texture_names[f].c_str()));
            }

            if (block->emissive_texture_names[f].empty()) {
                block->emissive_texture_indices[f] = 0;
            } else {
                block->emissive_texture_indices[f] = get_emissive_texture_index(
                    godot::String(block->emissive_texture_names[f].c_str()));
            }
        }
    }
}

void TextureArrayGenerator::force_regenerate() {
    s_global_texture_array.unref();
    s_global_texture_initialized = false;
    s_global_emissive_array.unref();
    s_global_emissive_initialized = false;
    last_registry_count = 0;
    generate_texture_array();
    generate_emissive_texture_array();
    populate_block_registry();
    s_global_texture_initialized = true;
    s_global_emissive_initialized = true;
}

} // namespace VoxelEngine
