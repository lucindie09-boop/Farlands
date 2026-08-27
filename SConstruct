#!/usr/bin/env python
import os, sys

env = SConscript("godot-cpp/SConstruct")
env.Append(CPPPATH=["src/"])

# Generate compile_commands.json for clang-tidy static analysis
env.Tool('compilation_db')
cdb = env.CompilationDatabase('compile_commands.json')

# Optional TSan support (Linux/GCC/Clang only)
tsan = ARGUMENTS.get("TSAN", "0")
if tsan == "1" and sys.platform != "win32":
    env.Append(CCFLAGS=["-fsanitize=thread", "-g", "-O1"])
    env.Append(LINKFLAGS=["-fsanitize=thread"])

# Optional ASan+UBSan support (Linux/GCC/Clang only)
asan = ARGUMENTS.get("ASAN", "0")
if asan == "1" and sys.platform != "win32":
    env.Append(CCFLAGS=["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1"])
    env.Append(LINKFLAGS=["-fsanitize=address,undefined"])

# Optional coverage support (Linux/GCC/Clang only)
coverage = ARGUMENTS.get("COVERAGE", "0")
if coverage == "1" and sys.platform != "win32":
    env.Append(CCFLAGS=["--coverage"])
    env.Append(LINKFLAGS=["--coverage"])

# Collect all .cpp files in src/ and subdirectories
sources = Glob("src/*.cpp") + Glob("src/*/*.cpp")

# Exclude standalone tools from the main library
lib_sources = [s for s in sources if os.path.basename(str(s)) not in ("terrain_debug.cpp", "benchmark.cpp")]

# Remove any non-existent .cpp files (like crc32.cpp which is header-only)
lib_sources = [s for s in lib_sources if os.path.exists(str(s))]

library = env.SharedLibrary("bin/libgdextension{}{}".format(env["suffix"], env["SHLIBSUFFIX"]), source=lib_sources)
Default(library, cdb)

# Pre-compile sources shared between the library and standalone targets once
# with env, so cloned envs (debug_env, bench_env, test_env) don't recompile
# them with potentially different flags (e.g. --coverage).
# Note: chunk_data.cpp is NOT in shared_sources because the library uses
# PaletteStorage while tests use the simpler ChunkData implementation.
# Note: light_propagator.cpp is NOT in shared_sources because it requires
# MeshManager which standalone tools don't have.
shared_sources = [
    "src/core/terrain_params.cpp",
    "src/worldgen/chunk_generator.cpp",
    "src/worldgen/biome_config.cpp",
    "src/worldgen/vegetation_config.cpp",
    "src/worldgen/vegetation_generator.cpp",
    "src/core/block_types.cpp",
    "src/core/inventory.cpp",
    "src/core/crafting.cpp",
    "src/core/item_registry.cpp",
    "src/core/edit_map.cpp",
    "src/mesh/mesh_builder.cpp",
    "src/mesh/mesh_builder_faces.cpp",
    "src/mesh/mesh_builder_greedy.cpp",
    "src/mesh/mesh_builder_solid.cpp",
    "src/mesh/chunk_neighbor_accessor.cpp",
    "src/mesh/ambient_occlusion.cpp",
    "src/mesh/smooth_lighting.cpp",
    "src/lighting/block_light_region.cpp",
    "src/engine/collision_resolver.cpp",
    "src/engine/player_controller.cpp",
    "src/render/texture_pack_manager.cpp",
    "src/render/minecraft_pack_converter.cpp",
]
# Remove any non-existent .cpp files (like crc32.cpp which is header-only)
shared_sources = [s for s in shared_sources if os.path.exists(str(s))]
shared_objects = env.Object(shared_sources)

# Object lookup for standalone tools that link only a subset of shared_sources.
# Keyed by source basename (extension-agnostic: .o on GCC/Clang, .obj on MSVC).
shared_obj_by_src = {os.path.splitext(os.path.basename(str(s)))[0]: o for s, o in zip(shared_sources, shared_objects)}

# Terrain-generation objects needed by standalone terrain tools (no mesh/lighting).
terrain_tool_objects = [shared_obj_by_src[n] for n in [
    "terrain_params", "chunk_generator", "biome_config", "vegetation_config",
    "vegetation_generator", "block_types", "inventory", "edit_map",
]]

# Pre-compile chunk_data.cpp once for non-test standalone tools (debug, bench, greedy, memory, repro)
# Give it a unique target name to avoid conflicts with test environment
chunk_data_object = env.Object("src/core/chunk_data_standalone", source="src/core/chunk_data.cpp")

# Debug terrain renderer (standalone executable).
# chunk_data.cpp is not in shared_sources (the library compiles its own copy),
# so standalone programs that link chunk_generator.o must add it explicitly.
debug_env = env.Clone()
debug_env.Append(LIBS=[])
debug_prog = debug_env.Program("bin/terrain_debug", ["tools/terrain_debug.cpp"] + terrain_tool_objects + [chunk_data_object])
Alias("debug", debug_prog)

# Performance benchmark (standalone executable)
bench_env = env.Clone()
bench_env.Append(CPPPATH=["src/"])
bench_env.Append(LIBS=[])
bench_prog = bench_env.Program("bin/benchmark", ["tools/benchmark.cpp"] + shared_objects + [chunk_data_object])
Alias("bench", bench_prog)

# Greedy meshing measurement on real generated terrain (standalone executable)
greedy_env = env.Clone()
greedy_env.Append(CPPPATH=["src/"])
greedy_env.Append(LIBS=[])
greedy_prog = greedy_env.Program("bin/greedy_measure", ["tools/greedy_measure.cpp"] + shared_objects + [chunk_data_object])
Alias("greedy_measure", greedy_prog)

# RAM/VRAM estimate at a given render distance (standalone executable)
mem_env = env.Clone()
mem_env.Append(CPPPATH=["src/"])
mem_env.Append(LIBS=[])
mem_prog = mem_env.Program("bin/memory_estimate", ["tools/memory_estimate.cpp"] + shared_objects + [chunk_data_object])
Alias("memory_estimate", mem_prog)

# Stride-2 greedy repro (null vs real neighbors)
repro_env = env.Clone()
repro_env.Append(CPPPATH=["src/"])
repro_env.Append(LIBS=[])
repro_prog = repro_env.Program("bin/repro_stride2", ["tools/repro_stride2.cpp"] + shared_objects + [chunk_data_object])
Alias("repro_stride2", repro_prog)

# Unit tests (standalone executable, no Godot runtime needed)
test_env = env.Clone()
test_env.Append(CPPPATH=["src/", "tests/"])
test_env.Append(LIBS=[])
# Disable doctest's internal threading to avoid ThreadSanitizer false positives
# in doctest's thread pool when built with TSan (Linux CI)
test_env.Append(CPPDEFINES=["DOCTEST_NO_MULTITHREADED", "DEBUG_ENABLED"])
# Add chunk_data for integration test (not in shared_sources because library uses PaletteStorage)
# Compile separately with the DOCTEST_NO_MULTITHREADED flag
test_chunk_data_object = test_env.Object("src/core/chunk_data_test", source="src/core/chunk_data.cpp")
# Add light_propagator for light removal tests (not in shared_sources because it requires MeshManager)
# Compile separately with the DOCTEST_NO_MULTITHREADED flag
test_light_propagator_object = test_env.Object("src/lighting/light_propagator_test", source="src/lighting/light_propagator.cpp")
# Note: edit_map.cpp and block_light_region.cpp are already in shared_sources via library build
test_prog = test_env.Program("bin/run_tests", Glob("tests/*.cpp") + shared_objects + [test_chunk_data_object, test_light_propagator_object])
Alias("test", test_prog)

# LibFuzzer harnesses (Clang-only, Linux/macOS)
# Build with: scons fuzz  (requires clang++)
if sys.platform != "win32":
    # Create fresh environment to avoid godot-cpp GCC-specific flags
    fuzz_env = Environment()
    fuzz_env["CC"] = "clang"
    fuzz_env["CXX"] = "clang++"
    fuzz_env.Append(CPPPATH=["src/"])
    fuzz_env.Append(CPPDEFINES=["FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION"])
    fuzz_env.Append(CCFLAGS=["-std=c++17", "-fsanitize=fuzzer,address,undefined", "-fno-omit-frame-pointer", "-g", "-O1"])
    fuzz_env.Append(LINKFLAGS=["-fsanitize=fuzzer,address,undefined"])
    # Pre-compile chunk_data.cpp for fuzz harnesses with unique target name
    fuzz_chunk_data_object = fuzz_env.Object("src/core/chunk_data_fuzz", source="src/core/chunk_data.cpp")
    # Reference source files directly to avoid VariantDir file locking
    fuzz_sources_common = [fuzz_chunk_data_object, "src/core/block_types.cpp", "src/core/inventory.cpp", "src/core/edit_map.cpp", "src/lighting/block_light_region.cpp"]
    fuzz_palette = fuzz_env.Program("bin/fuzz_palette", ["tools/fuzz_palette.cpp"] + fuzz_sources_common)
    fuzz_chunk = fuzz_env.Program("bin/fuzz_chunk_load", ["tools/fuzz_chunk_load.cpp"] + ["src/core/edit_map.cpp", "src/core/block_types.cpp"])
    fuzz_recovery = fuzz_env.Program("bin/fuzz_chunk_recovery", ["tools/fuzz_chunk_recovery.cpp"] + ["src/core/edit_map.cpp", "src/core/block_types.cpp"])
    fuzz_light = fuzz_env.Program("bin/fuzz_light_propagation", ["tools/fuzz_light_propagation.cpp"] + fuzz_sources_common)
    fuzz_mesh_sources = fuzz_sources_common + [
        "src/mesh/mesh_builder.cpp",
        "src/mesh/mesh_builder_faces.cpp",
        "src/mesh/mesh_builder_greedy.cpp",
        "src/mesh/mesh_builder_solid.cpp",
        "src/mesh/chunk_neighbor_accessor.cpp",
        "src/mesh/ambient_occlusion.cpp",
        "src/mesh/smooth_lighting.cpp",
    ]
    fuzz_mesh = fuzz_env.Program("bin/fuzz_mesh_builder", ["tools/fuzz_mesh_builder.cpp"] + fuzz_mesh_sources)
    Alias("fuzz", [fuzz_palette, fuzz_chunk, fuzz_recovery, fuzz_light, fuzz_mesh])
