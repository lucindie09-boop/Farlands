#pragma once

// The one piece of shape parsing that BOTH JSON readers need: copying a parsed
// BlockShape into a block and deriving its two static box lists from the parts.
// core/block_types_shapes.cpp defines it; core/block_types_load.cpp (the block
// definitions) calls it as well, so it cannot stay file-local.
//
// Guarded exactly like the two readers: under fuzzing there is no Godot JSON to
// parse, and these translation units compile to nothing.

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

#include <godot_cpp/variant/string.hpp>

#include "core/block_types.hpp"

namespace VoxelEngine {

void apply_shape_to_block(const BlockShape& shape, BlockType& bt, const godot::String& block_name,
                          const godot::String& shape_name);

} // namespace VoxelEngine

#endif  // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
