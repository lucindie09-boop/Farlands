extends "res://scripts/player_model.gd"

## The goblin's body, drawn as the player's body is.
##
## It IS the player's body: the same two clips, the same blend tree
## (scripts/player_model.gd -- the rest pose blended into the run by the walk amount,
## the idle added over it), the same walk-amount-from-speed rule, the same world light
## on a cell's own light. What differs is only what a body wears and how it hangs
## together, and both are here:
##
##   - the picture: Goblin.png, not the skin (the skin is a 64x64 atlas with a
##     painter attached, and a goblin has neither).
##   - the rig: `Goblin.obj` is ONE mesh of ELEVEN surfaces, authored in Blockbench
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
const GOBLIN_TEXTURE: Texture2D = preload("res://assets/models/Goblin.png")

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

## The surfaces whose box a part's JOINT is read from, when that is not simply all of them.
## The head is the case: its ears, chin and nose are pieces ATTACHED to it, so they ride
## with the turn -- but a joint averaged over them lands in front of the neck (the nose
## pushes it forward) and below it (the chin drags it down), and a head turning about a
## point in front of its neck sweeps sideways instead of turning where it sits. The joint
## is the head box's own, which is the neck: the bottom of it, on the body's centre line.
const PART_JOINT_SURFACES := {"head": [5]}

## The batch every goblin is DRAWN through (scripts/body_batch.gd): one MultiMesh per part,
## shared by the whole crowd. Forward+ instances MeshInstance3Ds that share a mesh and a
## material, but only up to a batch's own limit -- past it a hundred bodies cost ~30 draws
## instead of six (probes/probe_goblin_drawcalls.gd measures both) -- so the body goes through
## one MultiMesh per part, and each body writes its own six transforms into its own slot.
const BodyBatch := preload("res://scripts/body_batch.gd")
const BATCH_SHADER: Shader = preload("res://assets/shaders/body_batch.gdshader")
## The batch node and the material the crowd is drawn with: one of each, made on first use.
static var _batch: Node3D = null
static var _batch_material_cache: ShaderMaterial = null

## This body's slot in the batch ("-1" when it is drawn as its own MeshInstance3Ds: a preview,
## or a build with no world to light against).
var _batch_slot := -1

## The mesh to split: Goblin.obj, handed over by the spawn tool before the body is
## added to the tree (the rig is built from it at `_ready`).
var source_mesh: Mesh = null

## The arm the offhand torch hangs from: `arm` is the -x side, and the mesh looks down
## its own -Z, so with up +Y its right is cross(-Z, +Y) = +X. That makes `arm2` (+x) the
## goblin's right hand and `arm` (-x) its LEFT -- the hand an offhand belongs in. (The
## player's rig faces +Z, which mirrors it: its `arm` is the right hand, see
## player_model.gd's HELD_ARM.)
const OFFHAND_ARM := "arm"
## The goblin faces -Z where the player's rig faces +Z, so the seat and the sprite's turn
## are mirrored -- player_model.gd's HELD_MIRROR_YAW, and probes/probe_goblin.gd holds
## the drawn torch to the mesh's face side.
const OFFHAND_FACING := -1.0

## The arm the MAIN hand's item hangs from: `arm2` is the +x side, which is the goblin's
## RIGHT -- the mirror of OFFHAND_ARM above, for the same reason (up +Y, looking down -Z, so
## the right hand is cross(-Z, +Y) = +X). A goblin holds what it picks up here: one item, and
## nothing in an inventory.
const MAIN_ARM := "arm2"
## The same mirrored seat the offhand takes, because it is the same mesh: the hand is the arm
## box's other end and the artwork is turned with it.
const MAIN_FACING := -1.0

## Which way a raised arm turns for the inspection pose (player_model.gd's `inspect_pose`):
## the pose is `rest * Quaternion(RIGHT, pitch * sign)`, and the sign is the MESH's rather
## than a copied number -- the player's rig faces +Z and comes forward on -18 degrees, while
## this one faces -Z, so the same "forward" is the opposite turn. Measured, not assumed:
## probes/probe_goblin.gd holds the raised hand in front of the mesh (negative z), above its
## rest, and inside the neck's own pitch limit.
const INSPECT_SIGN := 1.0

## What is drawn in the offhand right now (`scripts/goblin.gd`'s roll, as last read), so a
## frame that has not changed it costs one comparison and no work.
var _offhand_drawn := 0
## The same, for the main hand: what the body says it is holding there (`main_hand_item`).
var _main_drawn := 0

## How far a neck turns: 75 degrees either side of where the body faces, and 40 up or
## down. Past that the goblin would be looking out of the back of its head, and the game
## clamps a mob's look the same way rather than letting a goal spin a head round.
const HEAD_YAW_LIMIT := 1.309
const HEAD_PITCH_LIMIT := 0.698
## How fast a neck turns: 10 degrees a TICK, the game's own limit for a mob's head, which is
## a constant 200 degrees a second -- a whole 75-degree turn takes 7.5 ticks, about a third
## of a second. It is a RATE and not an ease: the head travels at the same speed the whole
## way. An ease races off the mark and creeps into the finish, which is what reads as a
## camera settling rather than a neck turning.
const HEAD_TURN_PER_TICK := 0.1745
## Seconds in a tick, and how long the DRAWN head takes to close on the tick's own step.
## The neck is the shape the rest of the body already has: a value that moves on the 20 Hz
## ticks and a drawn pose interpolated between them. Turning the drawn head at a flat rate
## instead gave a head that crawled frame by frame beside a body that glided.
const HEAD_STEP_SECONDS := 0.05
const HEAD_DRAW_EASE := 0.02

## Where the head is looking (a world point), and this frame's length, kept here so the
## parent's `_process(delta)` can drive an ease that is stated in seconds.
var _look_point := Vector3.ZERO
var _head_delta := 1.0 / 60.0
## Until the goblin picks something to look at, the head keeps the rig's own rest pose:
## the point starts at the origin, and a head that turned toward THAT would be facing a
## spot in the world it has no business knowing about.
var _has_look := false
## The neck as the ticks see it (at most HEAD_TURN_PER_TICK a tick) and how far the last
## frame under a tick's worth of clock has got, which is what the drawn head eases onto.
var _step_q := Quaternion.IDENTITY
var _head_accum := 0.0


## A world point the head should look at (a random point around the goblin while it
## stands, the way it is walking while it walks). `goblin.gd` is the only caller.
func look_at_point(point: Vector3) -> void:
	_look_point = point
	_has_look = true


## Back to straight ahead. The head eases there, nothing snaps: a glance that has run out
## is a head turning back, not a head being reset.
func clear_look() -> void:
	_has_look = false


func _process(delta: float) -> void:
	_head_delta = delta
	_sync_offhand()
	_sync_main_hand()
	super._process(delta)
	_push_batch()


## Draw whatever the body says is in its offhand, this frame.
##
## The body owns the roll (`scripts/goblin.gd`'s `offhand_item`), and it is read here
## rather than handed over once at load: a child's `_ready` runs BEFORE its parent's, so a
## value the body sets in `_ready` would be too late for a one-shot read. Reading it
## lazily is also what lets a probe force a roll by writing the field. A body with no such
## field -- a stub, or this model under a host that is not a goblin -- holds nothing.
func _sync_offhand() -> void:
	var body := get_parent()
	var id := 0
	if body != null and body.has_method("get_offhand_item"):
		id = int(body.call("get_offhand_item"))
	if id == _offhand_drawn:
		return
	_offhand_drawn = id
	hold_offhand(OFFHAND_ARM, id, OFFHAND_FACING)


## Draw whatever the body says is in its MAIN hand, this frame -- the offhand's rule, for the
## offhand's reason: the field is read lazily (a child's `_ready` runs before its parent's), a
## body with no such field holds nothing, and a probe can force a hand by writing the field.
## Two things fill it: an item taken off the ground mid-loot, and a tool the goblin decided to
## keep. Clearing it is the same one comparison, and the hand empties.
func _sync_main_hand() -> void:
	var body := get_parent()
	var id := 0
	if body != null and body.has_method("get_main_hand_item"):
		id = int(body.call("get_main_hand_item"))
	if id == _main_drawn:
		return
	_main_drawn = id
	hold_main(MAIN_ARM, id, MAIN_FACING)


## The arm an inspection raises is the main hand's (player_model.gd's `inspect_pose`), and it
## turns the other way because this mesh looks down -Z.
func _inspect_arm() -> String:
	return MAIN_ARM


## The arm this body holds things in, so the shoulder pivot and the held-item pose are on the
## MAIN hand (player_model.gd's `_held_arm_name`): a kept tool hangs at arm2 tilted forward by
## the same 18 degrees and swinging half as far as the walk, exactly as an item in the player's
## hand does, and the LEFT arm keeps its plain swing for the torch.
func _held_arm_name() -> String:
	return MAIN_ARM


func _inspect_sign() -> float:
	return INSPECT_SIGN


func _ready() -> void:
	# The parts have to exist before the parent does anything: it captures their rest
	# rotations to build the clips from, and lights every MeshInstance3D it can find.
	_build_rig()
	super._ready()
	# ...and only after it: the batch draws the body with that material, so the material has to
	# exist before the body joins it.
	_init_batch()


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
	if _head == null:
		return
	# With nothing picked, straight ahead IS the rest: the rig's own pose, which is what a
	# mob reads as most of the time. A glance that has run out eases back here rather than
	# staying turned until something else asks.
	var want := Quaternion.IDENTITY
	if _has_look:
		var to := _look_point - _head.global_position
		# In the head's parent's frame, so the model's yaw and the body's stillness are both
		# already accounted for: this quaternion is the head's own turn relative to the rig.
		var parent := _head.get_parent() as Node3D
		if parent != null and to.length_squared() >= 0.01:
			var local: Vector3 = parent.global_transform.basis.inverse() * to
			# Yaw about the model's own up, then pitch about its own right: two axes, in
			# that order, so a look cannot tilt the head sideways (a roll would be a third
			# and nothing here asks for one).
			var yaw := clampf(atan2(-local.x, -local.z), -HEAD_YAW_LIMIT, HEAD_YAW_LIMIT)
			var pitch := clampf(atan2(local.y, Vector2(local.x, local.z).length()),
				-HEAD_PITCH_LIMIT, HEAD_PITCH_LIMIT)
			want = Quaternion(Vector3.UP, yaw) * Quaternion(Vector3.RIGHT, pitch)
	_turn_head(want)


## One frame's worth of neck. The STEP is the tick's: at most HEAD_TURN_PER_TICK per 20 Hz
## tick, about the shortest arc to `want`, which is the game's own limit for a mob's head.
## The drawn quaternion then eases onto that step every frame, so the turn is a turn at 20
## steps a second drawn smoothly rather than a flat crawl at the display's rate.
func _turn_head(want: Quaternion) -> void:
	_head_accum += _head_delta
	var steps := 0
	while _head_accum >= HEAD_STEP_SECONDS:
		_head_accum -= HEAD_STEP_SECONDS
		steps += 1
	if steps > 0:
		var gap := _step_q.angle_to(want)
		var step := HEAD_TURN_PER_TICK * float(steps)
		_step_q = want if gap <= step or gap < 0.0001 else _step_q.slerp(want, step / gap)
	_head.quaternion = _head.quaternion.slerp(
		_step_q, 1.0 - exp(-_head_delta / HEAD_DRAW_EASE))


## Eleven surfaces -> six hung parts, each its own joint.
##
## A part is a MeshInstance3D whose vertices are moved so its OWN pivot is the node's
## origin, standing at that pivot: turning the node then swings the box about the joint
## instead of about the model's feet (which is what a whole-mesh `rotation` does). The
## MESHES are the rig's and are shared (see `_rig`): the node is the body's, because the
## clips turn it.
func _build_rig() -> void:
	if source_mesh == null:
		push_error("goblin_model: no source_mesh (the spawn tool hands over Goblin.obj)")
		return
	var rig := _rig()
	for part in rig:
		var entry: Dictionary = rig[part]
		var mi := MeshInstance3D.new()
		mi.name = part
		mi.mesh = entry["mesh"]
		mi.position = entry["pivot"]
		add_child(mi)


## The rig every goblin of one source mesh draws: part name -> {mesh, pivot}.
##
## Split ONCE. Splitting the obj is eleven surface reads, six vertex rewrites and six mesh
## uploads, and every goblin draws the same six boxes -- built per body, a hundred goblins paid
## for it a hundred times and held a hundred copies of the same 1.1 KB of geometry, which also
## means no two bodies shared a mesh RID and the renderer could not batch any of them
## (Forward+ instances MeshInstance3Ds that share BOTH a mesh and a material).
##
## Keyed by the source mesh, which is kept alive by the entry: an instance id can be reused
## once the object it named is freed, and this table must not answer a new mesh with an old rig.
static var _rig_cache := {}


## The shared rig for this body's source mesh, built on first use.
func _rig() -> Dictionary:
	if source_mesh == null:
		return {}
	var key := source_mesh.get_instance_id()
	if _rig_cache.has(key):
		return _rig_cache[key]["parts"]
	var parts := {}
	for part in PART_SURFACES:
		var surfaces: Array = PART_SURFACES[part]
		if _surfaces_box(surfaces).size == Vector3.ZERO:
			continue
		# The joint comes from the part's own box, or from the boxes named in
		# PART_JOINT_SURFACES when something rides on the part.
		var joint: Array = PART_JOINT_SURFACES.get(part, surfaces)
		var pivot := _part_pivot(part, _surfaces_box(joint))
		parts[part] = {"mesh": _part_mesh(surfaces, pivot), "pivot": pivot}
	_rig_cache[key] = {"source": source_mesh, "parts": parts}
	return parts


## Hand the body's six meshes to the shared batch, and stop drawing them as six
## MeshInstance3Ds of their own.
##
## The NODES stay: the clips turn them, the grip maths reads their boxes and a probe reads
## their meshes. They are simply not what is drawn -- each part is drawn at its node's world
## transform, so the walk, the idle, the head's turn and the body's yaw all still arrive.
##
## A body with no world-light material (a preview in a SubViewport) keeps its own
## MeshInstance3Ds: it is one body, and it has no world uniform to share.
func _init_batch() -> void:
	if _body_material == null or _body_meshes.is_empty():
		return
	var node := _shared_batch()
	if node == null:
		return
	_batch_slot = int(node.call("register", _part_meshes(), _batch_material()))
	if _batch_slot < 0:
		return
	for mi in _body_meshes:
		mi.visible = false


## The one batch node every goblin writes into, made on first use under the ROOT at identity:
## the transforms it is handed are world transforms.
func _shared_batch() -> Node3D:
	if _batch != null and is_instance_valid(_batch):
		return _batch
	var tree := get_tree()
	if tree == null or tree.root == null:
		return null
	_batch = Node3D.new()
	_batch.name = "BodyBatch"
	_batch.set_script(BodyBatch)
	tree.root.add_child(_batch)
	return _batch


## Every part's mesh, by part name -- what the batch builds its six MultiMeshes from.
func _part_meshes() -> Dictionary:
	var meshes := {}
	for mi in _body_meshes:
		if mi.mesh != null:
			meshes[String(mi.name)] = mi.mesh
	return meshes


## The material the BATCH draws with: the same picture and the same light model on
## assets/shaders/body_batch.gdshader, which reads each body's cell light from the MultiMesh's
## per-instance custom data instead of an instance uniform. One material for the whole crowd.
func _batch_material() -> ShaderMaterial:
	if _batch_material_cache == null:
		_batch_material_cache = ShaderMaterial.new()
		_batch_material_cache.shader = BATCH_SHADER
		_batch_material_cache.set_shader_parameter("albedo_texture", GOBLIN_TEXTURE)
		# Closed opaque boxes: there is no sprite texel here to cut away.
		_batch_material_cache.set_shader_parameter("alpha_scissor", 0.0)
	return _batch_material_cache


## This body's six parts into its slot: after the clips have turned them (the tree runs at
## TREE_PRIORITY, before this) and after the light has been eased (super._process).
func _push_batch() -> void:
	if _batch_slot < 0 or _batch == null or not is_instance_valid(_batch):
		return
	# Once a frame for the whole crowd, not once per body (_feed_material dedupes by frame).
	_feed_material(_batch_material())
	for mi in _body_meshes:
		_batch.call("write", _batch_slot, String(mi.name), mi.global_transform, _body_light)


## Out of the crowd: the slot goes back, and nothing is drawn where this body was.
func _exit_tree() -> void:
	if _batch_slot >= 0 and _batch != null and is_instance_valid(_batch):
		_batch.call("unregister", _batch_slot)
	_batch_slot = -1
	super._exit_tree()


## The arms are drawn by the batch, so a hand item cannot hang from one: it would vanish with
## the node that is no longer drawn (player_model.gd's `_hand_parent` stands a node in for it).
func _arms_are_batched() -> bool:
	return _batch_slot >= 0


## The material every goblin's body is drawn with, made once.
##
## Shared on purpose, and it is the other half of what lets a crowd batch: Forward+ instances
## MeshInstance3Ds that share BOTH a mesh and a material, so one material RID for every goblin
## is one instanced draw per part instead of eleven per body. Nothing about a goblin's look is
## per-body -- Goblin.png is the same picture on all of them -- and the light each one is seen in
## is the `item_light` INSTANCE uniform, which is per MeshInstance3D rather than per material, so
## sharing costs nothing (docs/rendering-notes.md).
##
## The player's own body does NOT share this: its skin is repaintable.
static var _body_material_cache: ShaderMaterial = null


func _make_body_material() -> ShaderMaterial:
	if _body_material_cache == null:
		_body_material_cache = super._make_body_material()
	return _body_material_cache


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
## edge is the body's side), and the torso and head at their own bottom centre -- for the
## head that is the neck, because its box is the head and not its chin or its nose
## (see PART_JOINT_SURFACES).
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


## One part's surfaces as ONE surface of its own, with the pivot taken out of its vertices
## so the node can stand AT the pivot. Nothing else changes: the normals, tangents and uvs are
## the obj's, which is what keeps the texture where it was drawn.
##
## The part's source surfaces are MERGED rather than added one per source surface, and that is
## most of the body's draw calls: a mesh draws once per surface, so the head -- six boxes, the
## ears, chin and nose riding on it -- was six draws and a goblin eleven. Merged, a goblin is
## six, and six shared meshes is what the renderer can instance (docs/rendering-notes.md).
## Merging is an append per attribute plus the running vertex offset on the indices; no vertex,
## normal or uv is touched beyond the pivot subtraction every part gets.
func _part_mesh(surfaces: Array, pivot: Vector3) -> ArrayMesh:
	var out := ArrayMesh.new()
	out.resource_name = "goblin_part"
	var pieces: Array = []
	var have := {Mesh.ARRAY_NORMAL: true, Mesh.ARRAY_TANGENT: true, Mesh.ARRAY_TEX_UV: true}
	for s in surfaces:
		if s < 0 or s >= source_mesh.get_surface_count():
			continue
		var arrays := source_mesh.surface_get_arrays(s)
		if arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for i in verts.size():
			verts[i] -= pivot
		arrays[Mesh.ARRAY_VERTEX] = verts
		# An attribute only some of the part's boxes carry cannot be merged: it would have
		# to be invented for the others, which is a guess about their shading.
		for kind in have:
			if arrays[kind] == null:
				have[kind] = false
		pieces.append(arrays)
	if pieces.is_empty():
		return out
	var merged := []
	merged.resize(Mesh.ARRAY_MAX)
	var verts_out := PackedVector3Array()
	var normals_out := PackedVector3Array()
	var tangents_out := PackedFloat32Array()
	var uvs_out := PackedVector2Array()
	var indices_out := PackedInt32Array()
	var offset := 0
	for arrays in pieces:
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		verts_out.append_array(verts)
		if have[Mesh.ARRAY_NORMAL]:
			var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
			normals_out.append_array(normals)
		if have[Mesh.ARRAY_TANGENT]:
			var tangents: PackedFloat32Array = arrays[Mesh.ARRAY_TANGENT]
			tangents_out.append_array(tangents)
		if have[Mesh.ARRAY_TEX_UV]:
			var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
			uvs_out.append_array(uvs)
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		if indices.is_empty():
			# A non-indexed box, appended as its own triangle list.
			indices.resize(verts.size())
			for i in verts.size():
				indices[i] = i
		for i in indices.size():
			indices[i] += offset
		indices_out.append_array(indices)
		offset += verts.size()
	merged[Mesh.ARRAY_VERTEX] = verts_out
	merged[Mesh.ARRAY_INDEX] = indices_out
	if have[Mesh.ARRAY_NORMAL]:
		merged[Mesh.ARRAY_NORMAL] = normals_out
	if have[Mesh.ARRAY_TANGENT]:
		merged[Mesh.ARRAY_TANGENT] = tangents_out
	if have[Mesh.ARRAY_TEX_UV]:
		merged[Mesh.ARRAY_TEX_UV] = uvs_out
	out.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, merged)
	return out
