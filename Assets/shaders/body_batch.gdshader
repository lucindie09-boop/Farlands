shader_type spatial;
render_mode unshaded, cull_back;

// The same body the player has (Assets/shaders/body_shader.gdshader), drawn from a MultiMesh
// (scripts/body_batch.gd) instead of from one MeshInstance3D per goblin.
//
// Two differences, and neither changes what a body looks like:
//
//   * BODY_OPAQUE, so the material stays in the opaque pass (see body_shader.gdshader).
//   * BODY_BATCH, so the cell light a body is seen in comes from the MultiMesh's PER-INSTANCE
//     custom data rather than an instance uniform: a MultiMesh carries one material for every
//     instance, and its per-instance state is `INSTANCE_CUSTOM`, which the batch writes from
//     the body's own eased `item_light` every frame. This is why the batch is a variant of the
//     single light model (item_lighting.gdshaderinc) rather than a second copy of it.
#define BODY_OPAQUE
#define BODY_BATCH
#include "res://Assets/shaders/item_lighting.gdshaderinc"
