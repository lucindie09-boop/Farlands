shader_type spatial;
render_mode unshaded, cull_back, depth_draw_always, depth_test_disabled;

// The first-person arm. Same light model as the held and dropped items (the
// include), drawn over the world like the rest of the viewmodel: with depth
// testing off, a wall the player is standing against cannot clip the hand.
#include "res://shaders/item_lighting.gdshaderinc"
