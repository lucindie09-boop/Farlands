#include "godot_bindings/register_types.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "godot_bindings/player_controller.hpp"
#include "render/texture_array_generator.hpp"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;
using namespace VoxelEngine;

void initialize_chunk_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    
    ClassDB::register_class<VoxelEngine::ChunkManager>();
    ClassDB::register_class<::PlayerController>();
}

void terminate_chunk_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    TextureArrayGenerator::cleanup();
}

extern "C" {
    GDExtensionBool GDE_EXPORT gdextension_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
        godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
        
        init_obj.register_initializer(initialize_chunk_module);
        init_obj.register_terminator(terminate_chunk_module);
        init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
        
        return init_obj.init();
    }
}
