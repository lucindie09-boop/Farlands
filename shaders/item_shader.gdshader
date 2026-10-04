shader_type spatial;
render_mode unshaded, cull_back, depth_draw_always;

// Dropped items, and the held block/item. Unshaded on purpose: the whole light
// model is the world's own, in the include, from the cell the mesh occupies --
// see shaders/item_lighting.gdshaderinc. Depth behaves like any other mesh in
// the world, because these are.
#include "res://shaders/item_lighting.gdshaderinc"
