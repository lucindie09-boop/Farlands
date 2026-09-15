#ifndef FARLANDS_GODOT_BINDINGS_LIQUID_TEXTURE_GEN_HPP
#define FARLANDS_GODOT_BINDINGS_LIQUID_TEXTURE_GEN_HPP

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// GDScript face of render/liquid_texture.hpp — the procedural generator behind
// the Liquid Texture Lab (liquid_texture_lab.gd). Every setting travels as a
// Dictionary so the lab can build its widgets, save them to JSON and hand them
// back without a second copy of the schema in GDScript; the keys are exactly
// the LiquidSettings field names. Static-only, like BlockTextures and
// ViewmodelMeshes: no instance state lives here.
class LiquidTextureGen : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(LiquidTextureGen, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // The built-in palettes: "water", "lava", "acid".
    static godot::PackedStringArray style_names();

    // Full settings dictionary for a style, at the requested frame resolution.
    // Every key is present, so callers can mutate one key and pass it back.
    static godot::Dictionary default_settings(const godot::String& style, int32_t resolution);

    // The settings the generator would actually use (every value clamped to the
    // legal range), as a dictionary. The lab shows this: a slider that asked
    // for 900 frames is told the truth.
    static godot::Dictionary describe(const godot::Dictionary& settings);

    // Generates the whole animation strip: RGBA8, `resolution` wide and
    // `resolution * frames` tall, one frame per horizontal band, frames stacked
    // downwards. Missing keys fall back to the style defaults.
    static godot::Ref<godot::Image> generate_strip(const godot::Dictionary& settings);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_LIQUID_TEXTURE_GEN_HPP
