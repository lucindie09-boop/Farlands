extends "res://scripts/player_model.gd"

## The goblin's body, drawn as the player's body is.
##
## It IS the player's body: the same two clips, the same blend tree
## (scripts/player_model.gd -- the rest pose blended into the run by the walk amount,
## the idle added over it), the same walk-amount-from-speed rule, the same world light
## on a cell's own light. What differs is only what a body wears and how it hangs
## together, and both are here:
##
##   - the picture: Goblin2.png, not the skin (the skin is a 64x64 atlas with a
##     painter attached, and a goblin has neither).
##   - the rig: `Goblin2.obj` is ONE mesh of ELEVEN surfaces, authored in Blockbench
##     as eleven named boxes (`leg`, `leg`, `torso`, `arm`, `arm`, `head`, `ear`,
##     `ear`, `chin`, `nose`, `nose` -- the ears, chin and nose are the head's). A
##     surface cannot be turned on its own, so the boxes are split back into the
##     parts, and each part is hung from its own joint (_build_rig).
##   - the head: it is the goblin's own. `goblin.gd` turns the whole model to look
##     about, so there is nothing for a head to track (see _track_head_look).
##
## Everything else -- the walk's fade, the idle that is never re-entered, the lit
## material per part -- comes from the parent unchanged.

## The goblin's skin: one texture for all eleven surfaces (the obj's .mtl is not in
## the tree, so this is the whole look).
const GOBLIN_TEXTURE: Texture2D = preload("res://Assets/models/Goblin2.png")

## Surface index -> rig part, in the obj's own order (which is the import's order, one
## surface per `o` block). The parts are named what the clips and the parent class
## expect (`arm`/`leg` on the -x side, `arm2`/`leg2` on +x), so a clip's tracks find
## them without a remap: the goblin walks with the player's own clip paths.
const PART_SURFACES := {
	"leg": [0],
	"leg2": [1],
	"torso": [2],
	"arm": [3],
	"arm2": [4],
	"head": [5, 6, 7, 8, 9, 10],
}

## The mesh to split: Goblin2.obj, handed over by the spawn tool before the body is
## added to the tree (the rig is built from it at `_ready`).
var source_mesh: Mesh = null

## How far a neck turns: 75 degrees either side of where the body faces, and 40 up or
## down. Past that the goblin would be looking out of the back of its head, and the game
## clamps a mob's look the same way rather than letting a goal spin a head round.
const HEAD_YAW_LIMIT := 1.309
const HEAD_PITCH_LIMIT := 0.698
## Seconds for the head to close on a new point, about 10 degrees a tick -- a look that
## snaps is a teleport, one that crawls reads as a machine scanning.
const HEAD_EASE := 0.12

## Where the head is looking (a world point), and this frame's length, kept here so the
## parent's `_process(delta)` can drive an ease that is stated in seconds.
var _look_point := Vector3.ZERO
var _head_delta := 1.0 / 60.0
## Until the goblin picks something to look at, the head keeps the rig's own rest pose:
## the point starts at the origin, and a head that turned toward THAT would be facing a
## spot in the world it has no business knowing about.
var _has_look := false


## A world point the head should look at (a random point around the goblin while it
## stands, the way it is walking while it walks). `goblin.gd` is the only caller.
func look_at_point(point: Vector3) -> void:
	_look_point = point
	_has_look = true


func _process(delta: float) -> void:
	_head_delta = delta
	super._process(delta)


func _ready() -> void:
	# The parts have to exist before the parent does anything: it captures their rest
	# rotations to build the clips from, and lights every MeshInstance3D it can find.
	_build_rig()
	super._ready()


## The goblin's own picture, in place of the skin.
func _skin_albedo() -> Texture2D:
	return GOBLIN_TEXTURE


## Nothing aims a goblin's head but the goblin.
##
## The parent's head look is the PLAYER's: with no controller in the ancestor chain it
## falls back to the scene's Player node, which would turn every goblin's face toward
## wherever the player is looking. A goblin's head instead turns to the point its own
## look-around picked (`goblin.gd`'s LOOK_CHANCE, and the way it is walking when it is),
## while its body only ever faces where it walks -- which is the difference between a mob
## looking at something and a mob spinning on the spot.
func _track_head_look() -> void:
	if _head == null or not _has_look:
		return
	var to := _look_point - _head.global_position
	if to.length_squared() < 0.01:
		return
	# In the head's parent's frame, so the model's yaw and the body's stillness are both
	# already accounted for: this quaternion is the head's own turn relative to the rig.
	var parent := _head.get_parent() as Node3D
	if parent == null:
		return
	var local: Vector3 = parent.global_transform.basis.inverse() * to
	var yaw := clampf(atan2(-local.x, -local.z), -HEAD_YAW_LIMIT, HEAD_YAW_LIMIT)
	var pitch := clampf(atan2(local.y, Vector2(local.x, local.z).length()),
		-HEAD_PITCH_LIMIT, HEAD_PITCH_LIMIT)
	var want := Quaternion(Vector3.UP, yaw) * Quaternion(Vector3.RIGHT, pitch)
	_head.quaternion = _head.quaternion.slerp(want, 1.0 - exp(-_head_delta / HEAD_EASE))


## Eleven surfaces -> six hung parts, each its own joint.
##
## A part is a MeshInstance3D whose vertices are moved so its OWN pivot is the node's
## origin, standing at that pivot: turning the node then swings the box about the joint
## instead of about the model's feet (which is what a whole-mesh `rotation` does).
func _build_rig() -> void:
	if source_mesh == null:
		push_error("goblin_model: no source_mesh (the spawn tool hands over Goblin2.obj)")
		return
	for part in PART_SURFACES:
		var surfaces: Array = PART_SURFACES[part]
		var box := _surfaces_box(surfaces)
		if box.size == Vector3.ZERO:
			continue
		var pivot := _part_pivot(part, box)
		var mi := MeshInstance3D.new()
		mi.name = part
		mi.mesh = _part_mesh(surfaces, pivot)
		mi.position = pivot
		add_child(mi)


## The union of some of the source mesh's surfaces, in the mesh's own space.
func _surfaces_box(surfaces: Array) -> AABB:
	var box := AABB()
	var any := false
	for s in surfaces:
		if s < 0 or s >= source_mesh.get_surface_count():
			continue
		var arrays := source_mesh.surface_get_arrays(s)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for v in verts:
			box = AABB(v, Vector3.ZERO) if not any else box.expand(v)
			any = true
	return box


## Where a part hangs from, read off its own box so a re-modelled goblin keeps its
## joints: a leg turns at the top of its box (the hip), an arm at the top on the side
## the torso is (the shoulder -- the goblin's arms hang OUTSIDE its torso, so the inner
## edge is the body's side), and the torso and head at their own bottom centre.
func _part_pivot(part: String, box: AABB) -> Vector3:
	var centre := box.get_center()
	match part:
		"leg", "leg2":
			return Vector3(centre.x, box.end.y, centre.z)
		"arm":
			return Vector3(box.end.x, box.end.y, centre.z)
		"arm2":
			return Vector3(box.position.x, box.end.y, centre.z)
		_:
			return Vector3(centre.x, box.position.y, centre.z)


## One part's surfaces as a mesh of its own, with the pivot taken out of its vertices
## so the node can stand AT the pivot. Nothing else changes: the normals and the uvs
## are the obj's, which is what keeps the texture where it was drawn.
func _part_mesh(surfaces: Array, pivot: Vector3) -> ArrayMesh:
	var out := ArrayMesh.new()
	out.resource_name = "goblin_part"
	for s in surfaces:
		if s < 0 or s >= source_mesh.get_surface_count():
			continue
		var arrays := source_mesh.surface_get_arrays(s)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for i in verts.size():
			verts[i] -= pivot
		arrays[Mesh.ARRAY_VERTEX] = verts
		out.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return out
