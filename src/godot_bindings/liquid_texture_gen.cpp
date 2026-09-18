#include "godot_bindings/liquid_texture_gen.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

#include <cstring>
#include <string>

#include "render/liquid_texture.hpp"

using namespace godot;
using VoxelEngine::liquid::Kernel;
using VoxelEngine::liquid::Settings;
using VoxelEngine::liquid::Style;

namespace {

// Dictionary accessors with defaults, so a caller can pass a partial
// dictionary (the lab does exactly that: it mutates one key at a time).
[[nodiscard]] float num(const Dictionary& d, const char* key, float fallback) {
    if (!d.has(key)) return fallback;
    return static_cast<float>(static_cast<double>(d.get(key, fallback)));
}

[[nodiscard]] int32_t ival(const Dictionary& d, const char* key, int32_t fallback) {
    if (!d.has(key)) return fallback;
    return static_cast<int32_t>(static_cast<int64_t>(d.get(key, fallback)));
}

[[nodiscard]] bool flag(const Dictionary& d, const char* key, bool fallback) {
    if (!d.has(key)) return fallback;
    const Variant value = d.get(key, fallback);
    return static_cast<bool>(value);
}

[[nodiscard]] Kernel kernel_from(const Variant& value, Kernel fallback) {
    const String name = static_cast<String>(value);
    if (name == "box") return Kernel::Box;
    if (name == "warp") return Kernel::Warp;
    if (name == "plus") return Kernel::Plus;
    if (name == "row") return Kernel::Row;
    return fallback;
}

[[nodiscard]] Settings settings_from(const Dictionary& d) {
    Style style = Style::Water;
    const String style_name = static_cast<String>(d.get("style", String("water")));
    if (!VoxelEngine::liquid::style_from_name(std::string(style_name.utf8().get_data()), style)) {
        style = Style::Water;  // unknown name: take the water preset and move on
    }

    Settings s = VoxelEngine::liquid::style_settings(style, ival(d, "resolution", 16));
    s.resolution = ival(d, "resolution", s.resolution);
    s.frames = ival(d, "frames", s.frames);
    s.warmup_steps = ival(d, "warmup_steps", s.warmup_steps);
    s.steps_per_frame = ival(d, "steps_per_frame", s.steps_per_frame);
    s.frame_time = ival(d, "frame_time", s.frame_time);
    s.interpolate = ival(d, "interpolate", s.interpolate);
    s.kernel = kernel_from(d.get("kernel", String(VoxelEngine::liquid::kernel_name(s.kernel))), s.kernel);
    s.surface_divisor = num(d, "surface_divisor", s.surface_divisor);
    s.flow_coupling = num(d, "flow_coupling", s.flow_coupling);
    s.flow_box = flag(d, "flow_box", s.flow_box);
    s.flow_gain = num(d, "flow_gain", s.flow_gain);
    s.surge_decay = num(d, "surge_decay", s.surge_decay);
    s.excite_chance = num(d, "excite_chance", s.excite_chance);
    s.excite_strength = num(d, "excite_strength", s.excite_strength);
    s.warp_shift = num(d, "warp_shift", s.warp_shift);
    s.wrap = flag(d, "wrap", s.wrap);
    s.seed = static_cast<std::uint32_t>(static_cast<int64_t>(d.get("seed", static_cast<int64_t>(s.seed))));
    s.field_scale = num(d, "field_scale", s.field_scale);
    s.ramp_curve = num(d, "ramp_curve", s.ramp_curve);
    s.ramp_stops = ival(d, "ramp_stops", s.ramp_stops);
    s.posterize = ival(d, "posterize", s.posterize);
    s.grain = num(d, "grain", s.grain);
    s.shift_rows = ival(d, "shift_rows", s.shift_rows);
    s.shift_period = ival(d, "shift_period", s.shift_period);
    s.loop = flag(d, "loop", s.loop);
    s.loop_window = ival(d, "loop_window", s.loop_window);

    // The ramp travels as 16 floats (four RGBA stops); only the first
    // ramp_stops of them are read.
    if (d.has("ramp")) {
        const PackedFloat32Array ramp = d.get("ramp", PackedFloat32Array());
        for (int stop = 0; stop < VoxelEngine::liquid::kMaxRampStops; ++stop) {
            for (int channel = 0; channel < 4; ++channel) {
                const int index = stop * 4 + channel;
                if (index < ramp.size()) {
                    s.ramp[stop][channel] = ramp[index];
                }
            }
        }
    }
    return VoxelEngine::liquid::clamped(s);
}

[[nodiscard]] Dictionary settings_to_dict(const Settings& s, Style style) {
    Dictionary d;
    d["style"] = String(VoxelEngine::liquid::style_name(style));
    d["resolution"] = s.resolution;
    d["frames"] = s.frames;
    d["warmup_steps"] = s.warmup_steps;
    d["steps_per_frame"] = s.steps_per_frame;
    d["frame_time"] = s.frame_time;
    d["interpolate"] = s.interpolate;
    d["kernel"] = String(VoxelEngine::liquid::kernel_name(s.kernel));
    d["surface_divisor"] = s.surface_divisor;
    d["flow_coupling"] = s.flow_coupling;
    d["flow_box"] = s.flow_box;
    d["flow_gain"] = s.flow_gain;
    d["surge_decay"] = s.surge_decay;
    d["excite_chance"] = s.excite_chance;
    d["excite_strength"] = s.excite_strength;
    d["warp_shift"] = s.warp_shift;
    d["wrap"] = s.wrap;
    d["seed"] = static_cast<int64_t>(s.seed);
    d["field_scale"] = s.field_scale;
    d["ramp_curve"] = s.ramp_curve;
    d["ramp_stops"] = s.ramp_stops;
    d["posterize"] = s.posterize;
    d["grain"] = s.grain;
    d["shift_rows"] = s.shift_rows;
    d["shift_period"] = s.shift_period;
    d["loop"] = s.loop;
    d["loop_window"] = s.loop_window;
    PackedFloat32Array ramp;
    ramp.resize(static_cast<int64_t>(VoxelEngine::liquid::kMaxRampStops) * 4);
    for (int stop = 0; stop < VoxelEngine::liquid::kMaxRampStops; ++stop) {
        for (int channel = 0; channel < 4; ++channel) {
            ramp.set(static_cast<int64_t>(stop) * 4 + channel, s.ramp[stop][channel]);
        }
    }
    d["ramp"] = ramp;
    return d;
}

} // namespace

PackedStringArray LiquidTextureGen::style_names() {
    PackedStringArray names;
    names.append("water");
    names.append("lava");
    names.append("acid");
    return names;
}

Dictionary LiquidTextureGen::default_settings(const String& style, int32_t resolution) {
    Style parsed = Style::Water;
    if (!VoxelEngine::liquid::style_from_name(std::string(style.utf8().get_data()), parsed)) {
        parsed = Style::Water;
    }
    return settings_to_dict(VoxelEngine::liquid::style_settings(parsed, resolution), parsed);
}

Dictionary LiquidTextureGen::describe(const Dictionary& settings) {
    const Settings clamped = settings_from(settings);
    Style style = Style::Water;
    const String style_name = static_cast<String>(settings.get("style", String("water")));
    if (!VoxelEngine::liquid::style_from_name(std::string(style_name.utf8().get_data()), style)) {
        style = Style::Water;
    }
    Dictionary d = settings_to_dict(clamped, style);
    // The strip shape after interpolation, which is what the caller will get.
    d["strip_frames"] = clamped.frames * clamped.interpolate;
    d["strip_width"] = clamped.resolution;
    d["strip_height"] = clamped.resolution * clamped.frames * clamped.interpolate;
    // Whether the last frame repeats the first (so a player must wrap to index
    // 1, not 0, after it) — a single-frame strip cannot loop.
    d["looped"] = clamped.loop && clamped.frames >= 2;
    return d;
}

Ref<Image> LiquidTextureGen::generate_strip(const Dictionary& settings) {
    const VoxelEngine::liquid::Strip strip = VoxelEngine::liquid::generate(settings_from(settings));
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(strip.pixels.size()));
    if (!strip.pixels.empty()) {
        std::memcpy(bytes.ptrw(), strip.pixels.data(), strip.pixels.size());
    }
    return Image::create_from_data(strip.width, strip.height, false, Image::FORMAT_RGBA8, bytes);
}

void LiquidTextureGen::_bind_methods() {
    ClassDB::bind_static_method("LiquidTextureGen", D_METHOD("style_names"), &LiquidTextureGen::style_names);
    ClassDB::bind_static_method("LiquidTextureGen", D_METHOD("default_settings", "style", "resolution"),
                                &LiquidTextureGen::default_settings);
    ClassDB::bind_static_method("LiquidTextureGen", D_METHOD("describe", "settings"), &LiquidTextureGen::describe);
    ClassDB::bind_static_method("LiquidTextureGen", D_METHOD("generate_strip", "settings"),
                                &LiquidTextureGen::generate_strip);
}
