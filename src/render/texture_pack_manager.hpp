#ifndef FUK_MINECRAFT_TEXTURE_PACK_MANAGER_HPP
#define FUK_MINECRAFT_TEXTURE_PACK_MANAGER_HPP

#include <godot_cpp/variant/string.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace VoxelEngine {

// One loaded texture pack: a folder with pack.json + textures/ keyed by
// engine texture names (see data/block_definitions.json). Pure data (no Godot
// types) so the resolution core is unit-testable in the standalone binary.
struct TexturePack {
    std::string name;
    std::string root_dir;        // "user://packs/<folder>/" (trailing slash)
    int schema = 1;
    int min_supported = 1;       // inclusive schema range this engine reads
    int max_supported = 1;
    int base_resolution = 16;    // target texture-array layer resolution
    std::string author;          // optional, from pack.json
};

using PackPathExistsFn = std::function<bool(const std::string& path)>;

// Whether this engine's schema range accepts the pack's schema.
inline bool schema_supported(const TexturePack& pack) {
    return pack.schema >= pack.min_supported && pack.schema <= pack.max_supported;
}

// Resolution stack (highest-priority active pack first):
//   user://packs/<p>/textures/<name>.png  ->  res://textures/blocks/<name>.png
//   -> res://textures/blocks/stone.png
// One path per name: two packs both overriding "stone" collapse to the first
// active pack that provides it, so the texture array can never gain duplicate
// layers for the same name.
inline std::string resolve_texture(const std::vector<TexturePack>& packs,
                                   const std::vector<size_t>& active,
                                   const std::string& texture_name,
                                   const PackPathExistsFn& exists) {
    for (size_t idx : active) {
        if (idx >= packs.size()) continue;
        const std::string pack_path = packs[idx].root_dir + "textures/" + texture_name + ".png";
        if (exists(pack_path)) return pack_path;
    }
    const std::string builtin = "res://textures/blocks/" + texture_name + ".png";
    if (exists(builtin)) return builtin;
    return "res://textures/blocks/stone.png";
}

// Strict resolution for emissive textures: returns std::nullopt when the name
// exists neither in a pack nor as a built-in (caller falls back to black).
inline std::optional<std::string> resolve_texture_optional(
    const std::vector<TexturePack>& packs,
    const std::vector<size_t>& active,
    const std::string& texture_name,
    const PackPathExistsFn& exists) {
    for (size_t idx : active) {
        if (idx >= packs.size()) continue;
        const std::string pack_path = packs[idx].root_dir + "textures/" + texture_name + ".png";
        if (exists(pack_path)) return pack_path;
    }
    const std::string builtin = "res://textures/blocks/" + texture_name + ".png";
    if (exists(builtin)) return builtin;
    return std::nullopt;
}

// Declared base resolution: the highest-priority active pack's value, else 16.
inline int base_resolution_for(const std::vector<TexturePack>& packs,
                               const std::vector<size_t>& active) {
    if (!active.empty() && active[0] < packs.size()) {
        return packs[active[0]].base_resolution;
    }
    return 16;
}

class TexturePackManager {
public:
    static TexturePackManager& get_instance();

    // Scans <root_dir>/*/pack.json. Missing/unparseable/unsupported packs are
    // skipped with a warning. Call at startup (VoxelEngineController).
    void load_packs(const godot::String& root_dir);

    // Directly install a pack list (load_packs builds one; also a test seam so
    // standalone tests never touch FileAccess/DirAccess).
    void set_packs(std::vector<TexturePack> packs);

    // Active set (highest priority first). v1 activates exactly one pack by
    // name; returns false when the name is unknown.
    bool set_active_pack(const godot::String& name);
    void clear_active_pack();
    [[nodiscard]] bool has_active_pack() const;
    [[nodiscard]] std::vector<TexturePack> packs() const { return packs_; }
    [[nodiscard]] std::vector<size_t> active_packs() const { return active_; }

    // Godot-facing wrappers over the pure resolver core.
    godot::String resolve(const godot::String& texture_name) const;
    std::optional<godot::String> resolve_optional(const godot::String& texture_name) const;
    [[nodiscard]] int get_base_resolution() const;

    // Injectable existence check (default: godot::FileAccess::file_exists).
    // Tests replace it with an in-memory fake file map.
    void set_path_exists(PackPathExistsFn fn) { path_exists_ = std::move(fn); }

private:
    TexturePackManager();
    std::vector<TexturePack> packs_;
    std::vector<size_t> active_;
    PackPathExistsFn path_exists_;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_TEXTURE_PACK_MANAGER_HPP
