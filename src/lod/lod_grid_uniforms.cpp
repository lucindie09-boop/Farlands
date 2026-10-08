// What LodGrid tells its material, once per frame and only when it changed.
//
// Split out of lod/lod_grid.cpp, which is at the file-size cap (scons sizecheck): the
// pushes are a subject of their own -- the state the shader is told about, as opposed to
// the tiles it is asked to draw -- and they are the only place in the mode where a value
// crosses into shader-land rather than the other way round.
//
// Both are pushed on a CHANGE rather than every frame: a shader parameter write dirties
// the material, and the clip moves with the player, so a per-frame write would be a
// per-frame material update for a value that changed by nothing.
#include "lod/lod_grid.hpp"

#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>

namespace VoxelEngine {

using namespace godot;

void LodGrid::push_clip_uniforms() {
    if (material.is_null()) return;
    // The liquid quad's texture-array layer, which the shader uses as its water flag
    // (see shaders/lod_grid.gdshader). It cannot be a constant in the shader: the layer
    // is an index into an array built from the loaded TEXTURE PACK, so the same block
    // gets a different index under a different pack and a number in the shader is right
    // for one pack and wrong for the next. Pushed before the clip's early return, so a
    // pack that changed under a standing player still reaches the material.
    if (static_cast<int32_t>(water_layer) != last_water_layer) {
        material->set_shader_parameter("water_layer", static_cast<float>(water_layer));
        last_water_layer = static_cast<int32_t>(water_layer);
    }
    // The far field's inner boundary is the loaded world's DISC, and the disc is
    // centred on the player while the tiles are a square lattice that does not
    // move. Clipping in the fragment stage is what reconciles the two: a tile
    // straddling the edge is built once and stays correct as the player walks
    // across it, whereas dropping whole tiles (the first version) left the four
    // corners of the inner square as holes -- outside the disc, so the world did
    // not draw them either.
    //
    // `inner_radius_blocks` is the radius the world DRAWS to, not the radius it
    // streams at (see world_drawn_radius_blocks): the unload pass keeps its hysteresis
    // as loaded chunks and a loaded chunk keeps its mesh. The disc used to be cut from
    // the streaming radius instead, which is two chunks inside the world's own drawn
    // edge -- so the far field drew its cells over the world's retained rings, and two
    // surfaces over one piece of ground is fighting cells and covered chunks.
    if (player_position == last_clip_center && inner_radius_blocks == last_clip_radius) return;
    last_clip_center = player_position;
    last_clip_radius = inner_radius_blocks;
    // Half a chunk INSIDE the world's own drawn radius, so the two sides overlap rather
    // than meet: the world's coverage is a disc of whole chunk squares and can end a few
    // blocks inside the radius it draws to, and a clip exactly on that radius leaves a
    // sliver neither side draws (which is what the last few missing spots along the
    // world's edge were). Sixteen blocks is the whole of the overlap on purpose -- the
    // grid's cells are drawn over the world's outermost point and nowhere else.
    const float clip = static_cast<float>(std::max(inner_radius_blocks - 16, 0));
    material->set_shader_parameter("clip_center",
                                   Vector2(player_position.x, player_position.z));
    material->set_shader_parameter("clip_radius", clip);
}

} // namespace VoxelEngine
