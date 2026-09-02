#ifndef FARLANDS_GODOT_BINDINGS_BLOCK_TEXTURES_HPP
#define FARLANDS_GODOT_BINDINGS_BLOCK_TEXTURES_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <unordered_map>

// Shared block/item -> icon texture lookup. Replaces the old GDScript
// block_textures.gd. Data comes from the C++ registries (BlockRegistry =
// block_definitions.json, ItemRegistry = items.json); this binding only adds
// the name lookups and the Texture2D load/cache layer on top. All methods are
// static and bound with bind_static_method so existing GDScript call sites
// (BlockTextures.get_texture(...), etc.) keep working unchanged.
class BlockTextures : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(BlockTextures, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Item ids start at 1024 (ItemRegistry::FIRST_ITEM_ID).
    static bool is_item(int block_id);
    // Side-face texture filename (index 0). Empty string when unknown.
    static godot::String get_side_texture_name(int block_id);
    // Case-insensitive name -> id. Blocks resolve first, then items. -1 when
    // unknown (Matches the old GDScript dictionary default).
    static int get_block_id_by_name(const godot::String& block_name);
    // Non-hidden block names (id > 0) followed by item names, in id order.
    static godot::PackedStringArray get_block_names();
    static bool is_hidden(int block_id);
    static godot::Ref<godot::Texture2D> get_texture(int block_id);
    static void invalidate_cache();

protected:
    static void _bind_methods();

private:
    static void ensure_lookup();
    static godot::Ref<godot::Texture2D> load_texture(const godot::String& path);

    static inline std::unordered_map<int, godot::Ref<godot::Texture2D>> s_texture_cache;
    static inline bool s_lookup_built = false;
    static inline std::unordered_map<std::string, int> s_name_to_id;
    // NOTE: must be a std type, not godot::PackedStringArray. Godot-typed
    // statics are constructed at DLL load, but their constructors call into the
    // gdextension interface table (a null function pointer until the engine
    // initializes the extension) — that aborts loading with WinError 1114.
    static inline std::vector<std::string> s_block_names;
};

#endif // FARLANDS_GODOT_BINDINGS_BLOCK_TEXTURES_HPP