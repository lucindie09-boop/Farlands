shader_type spatial;
render_mode unshaded, cull_back;

// A BODY -- the player's own, and every goblin -- on the world's own light model, drawn
// OPAQUE.
//
// It is the same model the dropped items and the held block run
// (Assets/shaders/item_lighting.gdshaderinc): the light of the CELL the body is in, its own
// face shading and nothing of the engine's sky. The one difference is compile-time and it is
// not cosmetic. item_shader.gdshader writes ALPHA in every fragment, and Godot compiles any
// shader that writes ALPHA into the TRANSPARENT pipeline. A body is closed opaque boxes --
// there is no texel to blend -- so being drawn as transparent cost it three things it should
// have had: the depth buffer (transparent draws are depth-sorted instead of depth-tested), the
// shadow pass, and the renderer's automatic instancing, which never batches a transparent
// material. A hundred goblins were a hundred separate sets of draws for exactly that reason,
// with the same six meshes and the same material (docs/rendering-notes.md).
//
// BODY_OPAQUE is a DEFINE rather than a uniform on purpose: a uniform branch is still seen by
// the shader compiler, so ALPHA would still be written and the material would stay in the
// transparent pipeline. Every body shares this one material, so the macro is the only thing
// that differs per body -- nothing here does.
#define BODY_OPAQUE
#include "res://Assets/shaders/item_lighting.gdshaderinc"
