#ifndef FARLANDS_GODOT_BINDINGS_SKIN_PIXELS_HPP
#define FARLANDS_GODOT_BINDINGS_SKIN_PIXELS_HPP

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

// Static helpers that push the skin/block-maker pixel & noise math into C++.
// skin_manager.gd and the settings gallery keep their Godot state in
// GDScript and call these for the byte work, so the heavy double loops run
// native while the deterministic engine-exact PCG noise matches the old
// GDScript output byte for byte.
class SkinPixels : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(SkinPixels, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // RGBA8 `dim` x `dim` noise image: r = randf(seed), g = 0, b = 0, a = 255.
    static godot::Ref<godot::Image> make_noise_map(int32_t dim, int64_t seed);

    // Returns a copy of `base` with monochromatic grain applied from the red
    // channel of `noise_map`, amount in 0..~1 (the GDScript established a max
    // grain of 0.35). Works for any source format; output is RGBA8.
    static godot::Ref<godot::Image> apply_gray_noise(const godot::Ref<godot::Image>& base,
                                                     const godot::Ref<godot::Image>& noise_map,
                                                     double amount);

    // UV-space rectangle -> inclusive clamped texel range [x0, y0, x1, y1].
    // The range may be empty (x1 < x0 / y1 < y0) after clamping.
    static godot::PackedInt32Array uv_texel_bounds(double lo_x, double lo_y, double hi_x, double hi_y, int32_t dim);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_SKIN_PIXELS_HPP