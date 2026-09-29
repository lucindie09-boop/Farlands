// The rendering side of the binding: the WorldEnvironment/sun nodes this node owns
// in the scene tree, and the texture array's layers (the lab's live edits read a
// layer, fit a frame onto it, push it back, and restore it). Kept apart from
// godot_bindings/chunk_manager.cpp, which is lifecycle and bindings only.

#include "godot_bindings/chunk_manager.hpp"

#include "godot_bindings/cached_node.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "render/texture_array_generator.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;
using namespace VoxelEngine;

namespace {
// The lab's generated frames are RGBA8 and whatever resolution the user picked;
// an array layer is fixed at the pack's base resolution with the array's format
// and mipmap state. This copies the frame onto that shape (never mutating the
// caller's image, which the lab keeps for its preview).
//
// `want_mipmaps` cannot come from the array: Godot 4.7's TextureLayered returns
// NULL from get_layer_data() even for a freshly built array, so there is nothing
// to inspect. It comes from the generator's own mipmap flag instead, which is the
// flag that built the array in the first place.
godot::Ref<godot::Image> fit_frame_to_array(const godot::Ref<godot::Image>& frame,
                                            const godot::Ref<godot::Texture2DArray>& array,
                                            bool want_mipmaps) {
    if (frame.is_null() || frame->is_empty() || array.is_null()) return godot::Ref<godot::Image>();
    const int width = array->get_width();
    const int height = array->get_height();
    if (width <= 0 || height <= 0) return godot::Ref<godot::Image>();

    godot::Ref<godot::Image> out = godot::Image::create(width, height, false, godot::Image::FORMAT_RGBA8);
    if (out.is_null()) return godot::Ref<godot::Image>();
    if (frame->get_format() != godot::Image::FORMAT_RGBA8) {
        // copy_from() requires a matching format, so convert the source first.
        godot::Ref<godot::Image> converted = frame->duplicate();
        if (converted.is_null()) return godot::Ref<godot::Image>();
        converted->convert(godot::Image::FORMAT_RGBA8);
        if (converted->get_width() != width || converted->get_height() != height) {
            converted->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
        }
        out->copy_from(converted);
    } else {
        out->copy_from(frame);
        if (out->get_width() != width || out->get_height() != height) {
            out->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
        }
    }

    // A mipmapped array layer rejects a frame without mipmaps.
    if (want_mipmaps && !out->has_mipmaps()) {
        out->generate_mipmaps();
    }
    return out;
}
} // namespace

Dictionary ChunkManager::get_texture_layer_info(const String& texture_name) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    // find_texture_layer reads the table built by the generate call above.
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    Dictionary out;
    out["found"] = layer >= 0;
    out["index"] = layer;
    out["layers"] = array.is_valid() ? array->get_layers() : 0;
    if (array.is_valid()) {
        out["width"] = array->get_width();
        out["height"] = array->get_height();
        out["format"] = static_cast<int>(array->get_format());
        out["mipmaps"] = TextureArrayGenerator::is_mipmaps_enabled();
        // A compressed array cannot take an uncompressed frame; the lab says so
        // up front, and rebuilds uncompressed, instead of pushing frames that
        // Godot silently drops on the format mismatch.
        out["writable"] = array->get_format() == Image::FORMAT_RGBA8;
    } else {
        out["width"] = 0;
        out["height"] = 0;
        out["format"] = -1;
        out["mipmaps"] = false;
        out["writable"] = false;
    }
    return out;
}

Ref<Image> ChunkManager::get_texture_layer_image(const String& texture_name) {
    // NOTE: Godot 4.7's TextureLayered::get_layer_data() returns null here even
    // for an array built from images in this same process, so this is best
    // effort — it answers non-null only on builds that keep the CPU copies.
    // Nothing in the engine depends on it; fit_texture_frame() is the verifiable
    // half of the live-preview path.
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return Ref<Image>();
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return Ref<Image>();
    return array->get_layer_data(layer);
}

Ref<Image> ChunkManager::fit_texture_frame(const String& texture_name, const Ref<Image>& frame) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return Ref<Image>();
    if (TextureArrayGenerator::find_texture_layer(texture_name) < 0) return Ref<Image>();
    if (array->get_format() != Image::FORMAT_RGBA8) return Ref<Image>();
    return fit_frame_to_array(frame, array, TextureArrayGenerator::is_mipmaps_enabled());
}

bool ChunkManager::push_texture_frame(const String& texture_name, const Ref<Image>& frame) {
    TextureArrayGenerator& generator = TextureArrayGenerator::get_instance();
    Ref<Texture2DArray> array = generator.get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return false;
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return false;
    if (array->get_format() != Image::FORMAT_RGBA8) {
        WARN_PRINT("push_texture_frame: texture array is compressed; disable texture compression to preview animated textures.");
        return false;
    }
    const Ref<Image> fitted = fit_frame_to_array(frame, array, TextureArrayGenerator::is_mipmaps_enabled());
    if (fitted.is_null()) return false;
    array->update_layer(fitted, layer);
    return true;
}

bool ChunkManager::restore_texture_layer(const String& texture_name) {
    Ref<Texture2DArray> array = TextureArrayGenerator::get_instance().get_texture_array();
    if (array.is_null() || array->get_layers() <= 0) return false;
    const int layer = TextureArrayGenerator::find_texture_layer(texture_name);
    if (layer < 0) return false;

    // Resolution order matches the array build: active pack override first, then
    // the built-in set. Pack PNGs live outside the import system.
    const String path = TexturePackManager::get_instance().resolve(texture_name);
    Ref<Image> original;
    if (path.begins_with("user://")) {
        original = Image::load_from_file(path);
    } else if (ResourceLoader* loader = ResourceLoader::get_singleton(); loader != nullptr) {
        const Ref<Texture2D> texture = loader->load(path);
        if (texture.is_valid()) {
            original = texture->get_image();
        }
    }
    if (original.is_null() || original->is_empty()) return false;
    const Ref<Image> fitted = fit_frame_to_array(original, array, TextureArrayGenerator::is_mipmaps_enabled());
    if (fitted.is_null()) return false;
    array->update_layer(fitted, layer);
    return true;
}

// -------------------------------------------------------------------------
// Scene tree manipulation — Godot-specific, lives in the binding layer
// -------------------------------------------------------------------------

void ChunkManager::update_environment() {
    Node* parent = get_parent();
    if (!parent) return;
    // The parent is compared by ID for the same reason the nodes below are
    // resolved from IDs: a reparent could free the object the old pointer
    // belonged to, and comparing a dangling pointer is not a question anyone
    // can answer. An ID comparison is always sound.
    const uint64_t parent_id = parent->get_instance_id();
    WorldEnvironment* world_env = resolve_cached<WorldEnvironment>(cached_world_env_id);
    if (parent_id != cached_env_parent_id || world_env == nullptr) {
        cached_env_parent_id = parent_id;
        world_env = Object::cast_to<WorldEnvironment>(
            parent->get_node_or_null(NodePath("WorldEnvironment"))
        );
        cache_object(cached_world_env_id, world_env);
        DirectionalLight3D* sun = Object::cast_to<DirectionalLight3D>(
            parent->get_node_or_null(NodePath("SunLight"))
        );
        cache_object(cached_sun_light_id, sun);
    }
    if (world_env == nullptr) return;
    Ref<Environment> env = world_env->get_environment();
    if (!env.is_valid()) return;

    auto& ec = controller->get_environment_controller();
    const auto& day_night = ec.get_day_night_cycle();
    const float blend = day_night.get_blend();
    const float elevation = day_night.get_sun_elevation();
    const Color horizon_color = day_night.get_horizon_color();
    const Color sun_color = day_night.get_sun_color();
    const Vector3 sun_dir = day_night.get_sun_direction();

    ec.get_sky_controller().update(env.ptr(), blend, static_cast<float>(day_night.get_raw_time()),
                                   sun_color, sun_dir,
                                   day_night.get_moon_phase(), 1.0f, day_night.get_sky_turbidity(), 1.0f,
                                   ec.get_fog_controller().get_fog_scatter(blend, elevation));
    ec.get_fog_controller().update(env.ptr(), blend, horizon_color,
                                   ec.get_fog_controller().get_fog_color(blend, horizon_color, elevation, sun_color, day_night.get_sky_turbidity()),
                                   ec.get_fog_controller().get_fog_scatter(blend, elevation));

    env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
    env->set_ambient_light_color(day_night.get_ambient_color());
    env->set_ambient_light_energy(day_night.get_ambient_intensity());

    // The sun is re-acquired on its own, not only inside the guard above: a
    // SunLight can be swapped under an unchanged parent (the world environment
    // stays valid, so the guard does not re-run), and a sun that resolved to
    // nullptr and was simply skipped would leave the day/night cycle driving a
    // light that no longer exists - the world would keep its last sun
    // transform instead of tracking the sun.
    DirectionalLight3D* sun_light = resolve_cached<DirectionalLight3D>(cached_sun_light_id);
    if (sun_light == nullptr) {
        sun_light = Object::cast_to<DirectionalLight3D>(parent->get_node_or_null(NodePath("SunLight")));
        cache_object(cached_sun_light_id, sun_light);
    }
    if (sun_light) {
        Vector3 light_pos = sun_light->get_global_position();
        sun_light->look_at(light_pos - sun_dir, Vector3(0, 0, 1));

        float sun_visible = std::clamp((elevation + 0.08f) / 0.16f, 0.0f, 1.0f);
        float moon_visible = (1.0f - sun_visible) * (1.0f - blend);

        if (sun_visible > 0.0f) {
            sun_light->set_color(sun_color);
            sun_light->set_param(Light3D::PARAM_ENERGY, 3.0f * sun_visible * day_night.get_day_intensity());
            sun_light->set_shadow(false);
            sun_light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else if (moon_visible > 0.0f) {
            sun_light->set_color(Color(1.0f, 1.0f, 1.0f));
            sun_light->set_param(Light3D::PARAM_ENERGY, 0.25f * moon_visible * day_night.get_night_intensity());
            sun_light->set_shadow(false);
            sun_light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else {
            sun_light->set_param(Light3D::PARAM_ENERGY, 0.0f);
            sun_light->set_shadow(false);
        }
    }
}
