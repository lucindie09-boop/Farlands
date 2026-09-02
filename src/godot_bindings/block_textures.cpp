#include "godot_bindings/block_textures.hpp"

#include "core/block_types.hpp"
#include "core/item_registry.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>
#include <vector>

using namespace godot;
using namespace VoxelEngine;

constexpr int kItemIdBase = static_cast<int>(ItemRegistry::FIRST_ITEM_ID);
constexpr int kSideFaceIndex = 0;

void BlockTextures::_bind_methods() {
    ClassDB::bind_static_method("BlockTextures", D_METHOD("is_item", "block_id"), &BlockTextures::is_item);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("get_side_texture_name", "block_id"), &BlockTextures::get_side_texture_name);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("get_block_id_by_name", "block_name"), &BlockTextures::get_block_id_by_name);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("get_block_names"), &BlockTextures::get_block_names);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("is_hidden", "block_id"), &BlockTextures::is_hidden);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("get_texture", "block_id"), &BlockTextures::get_texture);
    ClassDB::bind_static_method("BlockTextures", D_METHOD("invalidate_cache"), &BlockTextures::invalidate_cache);
}

void BlockTextures::ensure_lookup() {
    if (s_lookup_built) {
        return;
    }
    s_lookup_built = true;

    const BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();
    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        if (bt.name == nullptr) {
            continue;
        }
        s_name_to_id[godot::String(bt.name).to_lower().utf8().get_data()] = static_cast<int>(i);
        if (i > 0 && !registry.is_hidden(static_cast<BlockID>(i))) {
            s_block_names.push_back(bt.name);
        }
    }

    const ItemRegistry& items = ItemRegistry::get_instance();
    for (size_t i = 0; i < items.get_item_count(); ++i) {
        const BlockID id = static_cast<BlockID>(kItemIdBase + i);
        const char* name = items.get_item_name(id);
        if (name == nullptr) {
            continue;
        }
        s_name_to_id[godot::String(name).to_lower().utf8().get_data()] = static_cast<int>(id);
        s_block_names.push_back(name);
    }
}

bool BlockTextures::is_item(int block_id) {
    return block_id >= kItemIdBase && ItemRegistry::get_instance().is_item(static_cast<BlockID>(block_id));
}

String BlockTextures::get_side_texture_name(int block_id) {
    const ItemRegistry& items = ItemRegistry::get_instance();
    if (block_id >= kItemIdBase && items.is_item(static_cast<BlockID>(block_id))) {
        const char* tex = items.get_item_texture(static_cast<BlockID>(block_id));
        return tex != nullptr ? String(tex) : String();
    }
    const BlockRegistry& registry = BlockRegistry::get_instance();
    if (block_id < 0 || static_cast<size_t>(block_id) >= registry.get_count()) {
        return String();
    }
    const std::string& tex = registry.get_block_fast(static_cast<BlockID>(block_id)).texture_names[kSideFaceIndex];
    return tex.empty() ? String() : String(tex.c_str());
}

int BlockTextures::get_block_id_by_name(const String& block_name) {
    ensure_lookup();
    const std::string key = block_name.to_lower().utf8().get_data();
    auto it = s_name_to_id.find(key);
    return it != s_name_to_id.end() ? it->second : -1;
}

PackedStringArray BlockTextures::get_block_names() {
    ensure_lookup();
    PackedStringArray result;
    result.resize(static_cast<int64_t>(s_block_names.size()));
    for (size_t i = 0; i < s_block_names.size(); ++i) {
        result[i] = String(s_block_names[i].c_str());
    }
    return result;
}

bool BlockTextures::is_hidden(int block_id) {
    if (block_id < 0) {
        return false;
    }
    return BlockRegistry::get_instance().is_hidden(static_cast<BlockID>(block_id));
}

Ref<Texture2D> BlockTextures::load_texture(const String& path) {
    // Prefer the import system so imported (mipmapped/compressed) textures are
    // used. Newly added PNGs may not have gone through the editor import step
    // yet (no .import/.ctex), so ResourceLoader can't see them — read the file
    // directly instead, same path the pack textures outside res:// already take.
    ResourceLoader* loader = ResourceLoader::get_singleton();
    if (loader != nullptr && loader->exists(path)) {
        Ref<Texture2D> tex = loader->load(path);
        if (tex.is_valid()) {
            return tex;
        }
    }
    Ref<Image> image = Image::load_from_file(path);
    if (image.is_valid()) {
        return ImageTexture::create_from_image(image);
    }
    return Ref<Texture2D>();
}

Ref<Texture2D> BlockTextures::get_texture(int block_id) {
    auto cached = s_texture_cache.find(block_id);
    if (cached != s_texture_cache.end()) {
        return cached->second;
    }

    Ref<Texture2D> texture;
    const String name = get_side_texture_name(block_id);
    if (!name.is_empty()) {
        // Items resolve straight to textures/items/ — they are not part of
        // the block texture array, and the pack resolver's built-in fallback
        // would otherwise return stone.png for any unknown name.
        String path;
        if (is_item(block_id)) {
            path = "res://textures/items/" + name + ".png";
        } else {
            path = TexturePackManager::get_instance().resolve(name);
        }
        texture = load_texture(path);
    }

    s_texture_cache[block_id] = texture;
    return texture;
}

void BlockTextures::invalidate_cache() {
    s_texture_cache.clear();
}