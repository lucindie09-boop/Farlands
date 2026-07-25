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

library = env.SharedLibrary("bin/libgdextension{}{}".format(env["suffix"], env["SHLIBSUFFIX"]), source=lib_sources)
Default(library, cdb)

# Pre-compile sources shared between the library and standalone targets once
# with env, so cloned envs (debug_env, bench_env, test_env) don't recompile
# them with potentially different flags (e.g. --coverage).
shared_sources = [
    "src/worldgen/chunk_generator.cpp",
    "src/worldgen/vegetation_generator.cpp",
    "src/core/chunk_data.cpp",
    "src/core/block_types.cpp",
    "src/mesh/mesh_builder.cpp",
    "src/mesh/mesh_builder_faces.cpp",
    "src/mesh/mesh_builder_greedy.cpp",
    "src/mesh/chunk_neighbor_accessor.cpp",
    "src/mesh/ambient_occlusion.cpp",
    "src/mesh/smooth_lighting.cpp",
    "src/lighting/block_light_region.cpp",
    "src/engine/collision_resolver.cpp",
    "src/engine/player_controller.cpp",
]
shared_objects = env.Object(shared_sources)

# Debug terrain renderer (standalone executable)
debug_env = env.Clone()
debug_env.Append(LIBS=[])
debug_prog = debug_env.Program("bin/terrain_debug", ["tools/terrain_debug.cpp"] + shared_objects[:4])
Alias("debug", debug_prog)

# Performance benchmark (standalone executable)
bench_env = env.Clone()
bench_env.Append(CPPPATH=["src/"])
bench_env.Append(LIBS=[])
bench_prog = bench_env.Program("bin/benchmark", ["tools/benchmark.cpp"] + shared_objects)
Alias("bench", bench_prog)

# Unit tests (standalone executable, no Godot runtime needed)
test_env = env.Clone()
test_env.Append(CPPPATH=["src/", "tests/"])
test_env.Append(LIBS=[])
test_prog = test_env.Program("bin/run_tests", Glob("tests/*.cpp") + shared_objects)
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
    # Reference source files directly to avoid VariantDir file locking
    fuzz_sources_common = ["src/core/chunk_data.cpp", "src/core/block_types.cpp", "src/lighting/block_light_region.cpp"]
    fuzz_palette = fuzz_env.Program("bin/fuzz_palette", ["tools/fuzz_palette.cpp"] + fuzz_sources_common)
    fuzz_chunk = fuzz_env.Program("bin/fuzz_chunk_load", ["tools/fuzz_chunk_load.cpp"] + fuzz_sources_common)
    fuzz_light = fuzz_env.Program("bin/fuzz_light_propagation", ["tools/fuzz_light_propagation.cpp"] + fuzz_sources_common)
    fuzz_mesh_sources = fuzz_sources_common + [
        "src/mesh/mesh_builder.cpp",
        "src/mesh/mesh_builder_faces.cpp",
        "src/mesh/mesh_builder_greedy.cpp",
        "src/mesh/chunk_neighbor_accessor.cpp",
        "src/mesh/ambient_occlusion.cpp",
        "src/mesh/smooth_lighting.cpp",
    ]
    fuzz_mesh = fuzz_env.Program("bin/fuzz_mesh_builder", ["tools/fuzz_mesh_builder.cpp"] + fuzz_mesh_sources)
    Alias("fuzz", [fuzz_palette, fuzz_chunk, fuzz_light, fuzz_mesh])
