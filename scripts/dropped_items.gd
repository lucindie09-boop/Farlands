extends Node3D

# Items that have been thrown out of the inventory, in the world.
#
# The inventory half of a drop is not here: the hotbar takes the item out of the
# slot it was dropped from (hotbar.gd, the Q binding) and the shatter that plays
# as the stack drains is the hotbar's own. This node is only the part that leaves
# the HUD -- a stack that has left the player's hands and is now falling,
# tumbling, lying somewhere, and waiting to be walked over.
#
# A dropped item is a RIGID BODY, solved here rather than by an engine, because
# the voxel world has no Godot physics bodies to fall onto: there is no collider
# anywhere in it, and the player and the K-key dummy both move through ChunkManager
# with their own integrator. An item is the first body in the game that TURNS, so
# it needs the parts a sliding box does not: an inertia tensor, and the points at
# which it touches the world.
#
# Everything about the motion comes from those contacts. The spin is not rolled at
# random and there is no "settle" animation: a throw gives the item an angular
# velocity because the throw's own offset from its centre is a lever arm, gravity
# acting on a part of the body that is over an edge is what makes it topple, and
# it comes to rest when its contacts have actually stopped it.
#
# Two things the world is asked for, and they do different jobs:
#
#   * contacts_for_points() -- which of the body's own points are inside solid
#     geometry. These are what the solve TURNS the body with, because a contact's
#     torque comes from where it is, and a sweep's "which axis hit" cannot say.
#   * turned_box_contact() -- the exact convex overlap of the whole turned box
#     with the world. Points can only report the places they were put, so a block
#     sitting against the MIDDLE of an edge lies between two of them and the edge
#     passes through the world untouched: the clipping that gets worse the further
#     a spot is from a corner. This test has no such gaps, so it is the guard that
#     pushes the body out of anything the points missed.
#
# Bodies collide with the WORLD that way, and with each other the same way in
# miniature: an overlapping PAIR answers with the two bodies' own points and one
# deepest-overlap push-out shared between them, so a dropped block lands on
# another and a stack settles as a stack instead of as two blocks in one place.
# The world's answer belongs to one body and the pair's to both: neither can be
# moved twice for the same overlap, and a throw hands its momentum over instead
# of passing through.
#
# None of that is solved HERE any more. One substep is one native call over every
# body at once -- the gravity and the damping, the turn and the move, the world's
# contacts and their impulses, the exact guard, the pairs, and the rest decision
# (ChunkManager.solve_item_bodies: engine/item_body_solver.cpp and
# engine/item_pair_solver.cpp). It is the same maths, in flat arrays, and it moved
# because a pile made the old cost visible: a substep per body was two native
# queries wrapped in an impulse loop that read and walked a dictionary per
# contact, and the more blocks were in the pile the more times over it paid. This
# script gathers the bodies into the arrays and reads them back, and that is all.
#
# An item's node origin is the CENTRE of its box, so that the rotation turns the
# block about its middle; `position` is that same centre, in world space.

## The item shader: the world's own light model running on a mesh that belongs to
## no single block cell (see shaders/item_lighting.gdshaderinc). Unshaded, so the
## engine's sky and ambient cannot light an item that the world around it lights
## itself -- the mismatch this shader exists to remove.
const ITEM_SHADER: Shader = preload("res://shaders/item_shader.gdshader")

## The node under Main that owns the world. Set in Main.tscn.
@export var chunk_manager_path: NodePath = NodePath("../ChunkManager")

## How far along the aim the item starts, and how far below the eye. The offset is
## what makes the throw read as leaving the hand rather than the face, and the same
## offset is the lever arm the throw's spin comes from.
const SPAWN_FORWARD = 0.45
const SPAWN_DOWN = 0.12

## The throw, at full strength.
const THROW_SPEED = 6.0
const THROW_VARY = 0.20       # fraction of that, per drop, so two drops differ
const THROW_UP = 0.35         # fraction of the throw spent lifting the item

## How finely each of a body's boxes has its surface laid out in points, per axis.
## These are the points the solve turns the body with, so a stair is turned by the
## corners of its two boxes rather than by the corners of the block around them.
const SURFACE_POINTS = 3

## The least thickness an item is collided with, in blocks. This is a numerical
## floor and not a shape: an item is a real 3D mesh, extruded from its sprite
## (ViewmodelMeshes builds it with front, back and rim faces, 0.05 thick), and its
## own bounds are what it is collided as. The floor only exists so a mesh that came
## back degenerate could not produce a body with no volume at all.
const ITEM_THICKNESS = 0.01

## A body is stepped in slices this long at most, so a fast throw cannot pass
## through a block between two frames: the contacts are found at the position the
## body is actually at, and a 60-block-a-second fall moves a whole block a frame.
##
## Everything a slice is stepped BY -- gravity, damping, the bounce and the
## friction, how many times the contacts are solved over, the push-out, and what
## counts as being at rest -- is the native solver's, in
## src/engine/item_body_solver.cpp (world) and item_pair_solver.cpp (pairs).
const MAX_SUBSTEP = 1.0 / 120.0

## The item is picked up when the player's feet are within this of it.
const PICKUP_RADIUS = 1.2
## A freshly thrown item cannot be picked up for this long: without it the throw
## that put it on the ground is also the step that collects it again.
const PICKUP_DELAY = 0.6

## How long an item lies there before it is gone. Not persisted across a save in
## this version: an item on the ground does not survive a quit.
const LIFETIME = 300.0
const FADE_TIME = 8.0          # the last stretch of that, spent fading out

# One entry per item: the mesh, the body's own geometry, and its state.
var _items: Array = []
var _meshes: Dictionary = {}
var _materials: Dictionary = {}

var _chunk_manager: Node = null
var _player: Node3D = null
## Whether the world can solve an item's whole substep (ChunkManager.solve_item_bodies).
## Looked up once: it is the same answer every substep, and has_method is not free.
var _body_solve_available := false

# The shape table the native solver is handed: every kind of block that has
# been dropped, once. `_shape_rows` maps a block id to its row; the arrays are
# the rows flattened (row s owns boxes [_shape_box_start[s], +_shape_box_count[s])
# and points [_shape_point_start[s], +_shape_point_count[s])). A shape never
# moves, so this is written only when a block id is dropped for the first time.
var _shape_rows: Dictionary = {}
var _shape_box_offsets := PackedVector3Array()
var _shape_box_halves := PackedVector3Array()
var _shape_box_start := PackedInt32Array()
var _shape_box_count := PackedInt32Array()
var _shape_points := PackedVector3Array()
var _shape_point_start := PackedInt32Array()
var _shape_point_count := PackedInt32Array()

# The bodies of one substep, in the same order as _items: the arrays the native
# solver works in. The half that does not move -- inertia, reach, shape -- is
# gathered only when the item list changes; the live half is gathered every
# substep, because every substep is somewhere new.
var _bodies_static_dirty := true
var _body_positions := PackedVector3Array()
var _body_rotations := PackedVector4Array()
var _body_velocities := PackedVector3Array()
var _body_spins := PackedVector3Array()
var _body_inertia := PackedVector3Array()
var _body_reaches := PackedFloat32Array()
var _body_shapes := PackedInt32Array()
var _body_asleep := PackedByteArray()
## Seconds each body has been slow and touching: the solver's rest timer, kept
## between substeps for exactly the reason the velocity is.
var _body_rest := PackedFloat32Array()
var _body_grounded := PackedByteArray()


func _ready() -> void:
	_chunk_manager = get_node_or_null(chunk_manager_path)
	_player = get_node_or_null("../Player")
	_body_solve_available = _chunk_manager != null \
		and _chunk_manager.has_method("solve_item_bodies")


## Throw `count` of `block_id` into the world from `eye` (the camera), along
## `direction` (the aim). The caller has already taken them out of the inventory,
## and passes the eye rather than a hand position: the offsets that turn one into
## the other are the throw's own geometry, so they live here with the rest of it.
func spawn(block_id: int, count: int, from_eye: Vector3, direction: Vector3) -> void:
	if block_id <= 0 or count <= 0:
		return
	var tex := BlockTextures.get_texture(block_id)
	if tex == null:
		return
	var dir := direction.normalized()
	if dir.length_squared() < 0.5:
		dir = Vector3.FORWARD
	# Out of the hand, which is below the eye and a little ahead of it.
	var from := from_eye + Vector3.DOWN * SPAWN_DOWN + dir * SPAWN_FORWARD
	var boxes := _boxes_of(block_id)
	var centre: Vector3 = _body_centre(boxes)
	var size: Vector3 = _body_size(boxes, centre)
	var offsets := _box_offsets(boxes, centre)
	var halves := _box_halves(boxes)
	var points := _body_points(boxes, centre)
	var node := Node3D.new()
	node.name = "Drop"
	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = _mesh_of(block_id, tex)
	mesh_instance.material_override = _material_of(block_id, tex)
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	# The mesh's own centre sits at the node's origin, so the node turns the block
	# about its middle and the body below is the boxes the mesh draws.
	mesh_instance.position = -_mesh_centre(block_id)
	node.add_child(mesh_instance)
	add_child(node)

	var speed := THROW_SPEED * randf_range(1.0 - THROW_VARY, 1.0 + THROW_VARY)
	var vel := dir * speed + Vector3.UP * (speed * THROW_UP)
	# The spin is not rolled: it is what the throw does to the body. The hand is
	# BELOW and BEHIND the centre by `from - from_eye`, so a throw along `dir`
	# pulls the body about that offset -- the cross product is the lever arm, and
	# the angular velocity follows from it. A body whose offset is along the throw
	# gets no spin, which is right: it was pushed through its own centre.
	var lever := from - from_eye
	var spin := lever.cross(vel) / maxf(lever.length_squared(), 0.04)
	_items.append({
		"block_id": block_id,
		"count": count,
		"node": node,
		"size": size,
		"half": size * 0.5,
		"inertia": _inertia_of(boxes, centre),
		# The body's own reach from its origin: the broad phase of the pair solve.
		"radius": _body_reach(boxes, centre),
		# The body's shape in the table the native solver is handed: its boxes and
		# points, flattened once for every item of this block id. They are not kept
		# on the item as well -- nothing reads them off it any more.
		"shape": _shape_row(block_id, offsets, halves, points),
		# World space. `position` is the CENTRE of the box, which is also the
		# node's own origin.
		"position": from,
		"velocity": vel,
		"spin": spin,
		"age": 0.0,
		"grounded": false,
		"asleep": false,
	})
	_bodies_static_dirty = true


func _process(delta: float) -> void:
	_push_world_lighting()
	# Age and expire first, then move every body, then collect and draw. The pair
	# solve needs the whole set before anything is taken out of it, and an item
	# picked up this frame must not also be pushed around by one beside it.
	for i in range(_items.size() - 1, -1, -1):
		var item: Dictionary = _items[i]
		item["age"] = item["age"] + delta
		if item["age"] >= LIFETIME:
			_remove(i)
	_step_all(delta)
	for i in range(_items.size() - 1, -1, -1):
		var item: Dictionary = _items[i]
		if _try_pickup(item):
			_remove(i)
			continue
		_draw_item(item)


## One frame of every body's flight, in slices short enough that nothing can pass
## through a block -- or through another body -- between two of them.
##
## Every body moves inside a slice BEFORE the pairs are solved, so both halves of
## a stack see each other's motion at the same instant. Stepping one body to the end
## of the frame at a time would let it clear a body that had not moved yet.
func _step_all(delta: float) -> void:
	if _items.is_empty():
		return
	var slices := maxi(1, int(ceil(delta / MAX_SUBSTEP)))
	var slice := delta / float(slices)
	for i in range(slices):
		_step_items(slice)


## One slice of every body, solved NATIVELY in one call
## (ChunkManager.solve_item_bodies, engine/item_body_solver.cpp): the gravity and
## the damping, the turn and the move, the world's contacts and the impulses that
## answer them, the exact guard and its push-out, then the bodies against EACH
## OTHER, then the one rest decision per body. That is everything this script
## used to do in _substep and _solve_item_pairs, per body, in GDScript.
##
## This function is the gather and the read-back, and nothing else: the bodies go
## into flat packed arrays, come back solved, and are written back into the item
## dictionaries and the nodes. A pile of blocks costs one native call a slice
## whatever its size, where it used to cost a dictionary or two per contact.
func _step_items(delta: float) -> void:
	if not _body_solve_available:
		return
	var count := _items.size()
	if count == 0:
		return
	# A pile that is entirely asleep has nothing to solve -- and it is the common
	# case, so it does not pay for the gather below.
	var awake := false
	for item in _items:
		if not item["asleep"]:
			awake = true
			break
	if not awake:
		return
	if _bodies_static_dirty:
		_gather_static_bodies()
	for i in range(count):
		var item: Dictionary = _items[i]
		var node: Node3D = item["node"]
		_body_positions[i] = item["position"]
		# The body's orientation is the node's: the solver turns it and this is
		# where its own rotation is kept between substeps.
		var rotation: Quaternion = node.quaternion
		_body_rotations[i] = Vector4(rotation.x, rotation.y, rotation.z, rotation.w)
		_body_velocities[i] = item["velocity"]
		_body_spins[i] = item["spin"]
		_body_asleep[i] = 1 if item["asleep"] else 0
	var state := {
		"positions": _body_positions,
		"rotations": _body_rotations,
		"velocities": _body_velocities,
		"spins": _body_spins,
		"inertia": _body_inertia,
		"reach": _body_reaches,
		"shapes": _body_shapes,
		"asleep": _body_asleep,
		"rest": _body_rest,
		"box_offsets": _shape_box_offsets,
		"box_halves": _shape_box_halves,
		"box_start": _shape_box_start,
		"box_count": _shape_box_count,
		"points": _shape_points,
		"point_start": _shape_point_start,
		"point_count": _shape_point_count,
	}
	_chunk_manager.solve_item_bodies(state, delta)
	# The solved arrays come back as NEW ones (the native pass writes its own
	# copies of what it is handed), so they are read from the dictionary and not
	# from the members that were gathered into it.
	_body_positions = state["positions"]
	_body_rotations = state["rotations"]
	_body_velocities = state["velocities"]
	_body_spins = state["spins"]
	_body_asleep = state["asleep"]
	_body_rest = state["rest"]
	_body_grounded = state["grounded"]
	for i in range(count):
		var item: Dictionary = _items[i]
		var node: Node3D = item["node"]
		item["position"] = _body_positions[i]
		item["velocity"] = _body_velocities[i]
		item["spin"] = _body_spins[i]
		item["asleep"] = _body_asleep[i] != 0
		item["grounded"] = _body_grounded[i] != 0
		var rotation: Vector4 = _body_rotations[i]
		node.quaternion = Quaternion(rotation.x, rotation.y, rotation.z, rotation.w)


## The half of the body state that does not move -- inertia, reach, shape row --
## gathered when the item list changes, and one of the places the live half is
## SIZED: the gather in _step_items fills by index and does not resize.
##
## Some of this is the item dictionaries' own answers, and some of it is the rest
## timer: a body that arrives or leaves restarts the pile's timers, which is a
## fraction of a second of grace and never a body that should keep sleeping.
func _gather_static_bodies() -> void:
	var count := _items.size()
	_body_positions.resize(count)
	_body_rotations.resize(count)
	_body_velocities.resize(count)
	_body_spins.resize(count)
	_body_asleep.resize(count)
	_body_rest.resize(count)
	_body_inertia.resize(count)
	_body_reaches.resize(count)
	_body_shapes.resize(count)
	for i in range(count):
		var item: Dictionary = _items[i]
		_body_rest[i] = 0.0
		_body_inertia[i] = item["inertia"]
		_body_reaches[i] = item["radius"]
		_body_shapes[i] = item["shape"]
	_bodies_static_dirty = false


## The shape row for a block id, added to the table the first time that kind of
## block is dropped. A shape's boxes and points never move, so every item of the
## same block shares one row and the native pair solve is handed the same table
## every substep.
func _shape_row(block_id: int, offsets: Array, halves: Array, points: Array) -> int:
	if _shape_rows.has(block_id):
		return _shape_rows[block_id]
	var row: int = _shape_box_count.size()
	_shape_rows[block_id] = row
	_shape_box_start.append(_shape_box_offsets.size())
	_shape_box_count.append(offsets.size())
	for box_offset in offsets:
		_shape_box_offsets.append(box_offset)
	for box_half in halves:
		_shape_box_halves.append(box_half)
	_shape_point_start.append(_shape_points.size())
	_shape_point_count.append(points.size())
	for point in points:
		_shape_points.append(point)
	return row


## How far a body reaches from its own origin: the broad phase of the native pair
## solve, so two items on opposite sides of the world cost one distance check.
func _body_reach(boxes: Array, centre: Vector3) -> float:
	var reach := 0.0
	for box in boxes:
		var box_centre: Vector3 = (box["lo"] + box["hi"]) * 0.5 - centre
		var half: Vector3 = (box["hi"] - box["lo"]) * 0.5
		reach = maxf(reach, box_centre.length() + half.length())
	return reach


## A body's points: every box's surface laid out SURFACE_POINTS to an axis, in the
## body's own space. The corners and edges are where a box meets the world, so that
## is where they are needed; the inside of a box never touches anything.
func _body_points(boxes: Array, centre: Vector3) -> Array:
	var out := []
	var n := SURFACE_POINTS
	for box in boxes:
		var box_centre: Vector3 = (box["lo"] + box["hi"]) * 0.5 - centre
		var half: Vector3 = (box["hi"] - box["lo"]) * 0.5
		for ix in range(n):
			for iy in range(n):
				for iz in range(n):
					# Only the shell: a point in the middle of an axis is interior.
					if ix != 0 and ix != n - 1 and iy != 0 and iy != n - 1 and iz != 0 and iz != n - 1:
						continue
					out.append(box_centre + Vector3(
						half.x * (float(ix) / float(n - 1) * 2.0 - 1.0),
						half.y * (float(iy) / float(n - 1) * 2.0 - 1.0),
						half.z * (float(iz) / float(n - 1) * 2.0 - 1.0)))
	return out


## The body's inertia tensor about its own origin, as a diagonal in its own space:
## each box's own formula, moved out to where that box sits (the parallel axis
## theorem), summed over the boxes. Its inverse is what a contact needs -- how much
## angular velocity an impulse about a lever arm makes.
##
## The tensor is for UNIT MASS, not for the body's volume, because the solve's
## impulse denominator is `1 + ...` -- it has already taken 1/m to be 1. A tensor
## that still carried the volume would answer a contact with 1/volume times too
## much spin. A full block hides that, since its volume is exactly 1; an item does
## not. A torch is a thousandth of a block, so its first touch turned it with a
## thousand times the spin the contact was worth, which is what threw it at the
## floor. Dividing by the volume is what makes a body turn by its SHAPE and not by
## how small it happens to be drawn.
func _inertia_of(boxes: Array, centre: Vector3) -> Vector3:
	var total := Vector3.ZERO
	var volume := 0.0
	for box in boxes:
		var box_centre: Vector3 = (box["lo"] + box["hi"]) * 0.5 - centre
		var half: Vector3 = (box["hi"] - box["lo"]) * 0.5
		var m := maxf(half.x * half.y * half.z * 8.0, 0.0001)
		volume += m
		total.x += m * (half.y * half.y + half.z * half.z) / 3.0 + m * (box_centre.y * box_centre.y + box_centre.z * box_centre.z)
		total.y += m * (half.x * half.x + half.z * half.z) / 3.0 + m * (box_centre.x * box_centre.x + box_centre.z * box_centre.z)
		total.z += m * (half.x * half.x + half.y * half.y) / 3.0 + m * (box_centre.x * box_centre.x + box_centre.y * box_centre.y)
	total = total / maxf(volume, 0.0001)
	# A numerical floor per axis, as a share of the body's own largest one. A body
	# with one very thin axis -- a torch, whose long axis has almost nothing to turn
	# against -- would otherwise take a contact the way a bare rod does and drill.
	var floor_m := maxf(total.x, maxf(total.y, total.z)) * 0.1
	return Vector3(maxf(total.x, floor_m), maxf(total.y, floor_m), maxf(total.z, floor_m))


## Whether the player is close enough to take the item back. The inventory's own
## answer decides: a full inventory leaves the item on the ground rather than
## destroying it, which is the same rule the mined-block collect uses.
##
## Asked with can_add_block() before give_block(), and never the other way round.
## give_block() fills what it can and only then reports whether ALL of it fitted, so
## a refused pickup would have kept the part that did -- and the item, left on the
## ground because it was refused, would hand that part over again on the next frame.
## The read-only question has to be the gate; the write only follows it.
func _try_pickup(item: Dictionary) -> bool:
	if item["age"] < PICKUP_DELAY or _player == null:
		return false
	var at: Vector3 = item["position"]
	var feet := _player.global_position
	var flat := Vector2(at.x - feet.x, at.z - feet.z).length()
	if flat > PICKUP_RADIUS or absf(at.y - feet.y) > PICKUP_RADIUS + 1.0:
		return false
	if not _player.has_method("can_add_block") or not _player.has_method("give_block"):
		return false
	var block_id: int = item["block_id"]
	var count: int = item["count"]
	if not _player.can_add_block(block_id, count):
		return false
	_player.give_block(block_id, count)
	return true


## Put the node where the body is. The node's origin IS the body's centre, so this
## is the position the solve has been working in.
func _draw_item(item: Dictionary) -> void:
	var node: Node3D = item["node"]
	if not is_instance_valid(node):
		return
	node.global_position = item["position"]
	var fade := 1.0
	var left: float = LIFETIME - item["age"]
	if left < FADE_TIME:
		fade = clampf(left / FADE_TIME, 0.0, 1.0)
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance != null:
		# Per instance, not per material: every dropped block of one kind shares
		# one material, and each item's fade and the cell it lies in are its own.
		mesh_instance.set_instance_shader_parameter("item_fade", fade)
		mesh_instance.set_instance_shader_parameter("item_light", _light_at(item["position"]))


## The mesh a block drops as: its own shape, built once per block id.
##
## A block is a box mesh of the shape's own boxes, so a slab drops as a slab and a
## stair as a stair. An item -- a stick, a torch -- has no boxes to speak of and is
## drawn as the flat sprite mesh the viewmodel uses for it.
func _mesh_of(block_id: int, tex: Texture2D) -> ArrayMesh:
	if _meshes.has(block_id):
		return _meshes[block_id]
	var mesh: ArrayMesh
	if BlockTextures.is_item(block_id):
		mesh = _mesh_from(ViewmodelMeshes.build_item_mesh(tex))
	else:
		var boxes := []
		if _chunk_manager != null and _chunk_manager.has_method("get_selection_boxes"):
			for box in _chunk_manager.get_selection_boxes(block_id):
				var b := PackedFloat32Array()
				for i in range(mini(box.size(), 6)):
					b.append(float(box[i]))
				if b.size() == 6:
					boxes.append(b)
		if boxes.is_empty():
			# No world to ask (a scene without a ChunkManager), or a block with no
			# boxes at all: a full cube is what the registry answers with too.
			boxes.append(PackedFloat32Array([0.0, 0.0, 0.0, 1.0, 1.0, 1.0]))
		mesh = _mesh_from(ViewmodelMeshes.build_shaped_mesh(boxes))
	_meshes[block_id] = mesh
	return mesh


## The boxes a body is drawn and solved as: the block's OWN shape boxes, so a slab
## is a half block, a stair is its two boxes, and neither is the full block around
## them. An item is its own mesh's bounds -- the sprite's silhouette at the width
## its mesh was extruded to -- so a stick is a block long and a torch is a quarter
## of one, each as thick as it is drawn.
func _boxes_of(block_id: int) -> Array:
	var out := []
	if BlockTextures.is_item(block_id):
		var mesh := _mesh_of(block_id, BlockTextures.get_texture(block_id))
		var size: Vector3 = mesh.get_aabb().size if mesh != null else Vector3.ONE
		size = Vector3(maxf(size.x, ITEM_THICKNESS), maxf(size.y, ITEM_THICKNESS),
			maxf(size.z, ITEM_THICKNESS))
		var half := size * 0.5
		out.append({"lo": -half, "hi": half})
		return out
	if _chunk_manager != null and _chunk_manager.has_method("get_selection_boxes"):
		for b in _chunk_manager.get_selection_boxes(block_id):
			out.append({"lo": Vector3(b[0], b[1], b[2]), "hi": Vector3(b[3], b[4], b[5])})
	if out.is_empty():
		out.append({"lo": Vector3.ZERO, "hi": Vector3.ONE})
	return out


## Where the body's own boxes are centred, so the body turns about the middle of
## the shape it draws rather than about the corner of its cell.
func _body_centre(boxes: Array) -> Vector3:
	var lo: Vector3 = boxes[0]["lo"]
	var hi: Vector3 = boxes[0]["hi"]
	for box in boxes:
		lo = lo.min(box["lo"])
		hi = hi.max(box["hi"])
	return (lo + hi) * 0.5


func _body_size(boxes: Array, centre: Vector3) -> Vector3:
	var lo: Vector3 = boxes[0]["lo"]
	var hi: Vector3 = boxes[0]["hi"]
	for box in boxes:
		lo = lo.min(box["lo"])
		hi = hi.max(box["hi"])
	return (hi - lo).abs().max(Vector3(0.01, 0.01, 0.01))


func _box_offsets(boxes: Array, centre: Vector3) -> Array:
	var out := []
	for box in boxes:
		out.append((box["lo"] + box["hi"]) * 0.5 - centre)
	return out


func _box_halves(boxes: Array) -> Array:
	var out := []
	for box in boxes:
		out.append(((box["hi"] - box["lo"]) * 0.5).max(Vector3(0.01, 0.01, 0.01)))
	return out


## The mesh's own centre, so the mesh can be moved to sit on the body's origin.
func _mesh_centre(block_id: int) -> Vector3:
	var mesh := _mesh_of(block_id, BlockTextures.get_texture(block_id))
	return mesh.get_aabb().get_center() if mesh != null else Vector3.ZERO


func _mesh_from(data: Dictionary) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	if data.is_empty():
		return mesh
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


## The block's own texture, running the world's light model instead of the
## engine's: a dropped item has to look like the ground it lands on at noon, at
## midnight and in a cave, and Godot's own sky/ambient know about none of those.
## The day/night half of the model is pushed into this material once a frame
## (_push_world_lighting); the light of the cell an item is lying in is per item
## (`item_light`, set in _draw_item).
func _material_of(block_id: int, tex: Texture2D) -> ShaderMaterial:
	if _materials.has(block_id):
		return _materials[block_id]
	var mat := ShaderMaterial.new()
	mat.shader = ITEM_SHADER
	mat.set_shader_parameter("albedo_texture", tex)
	# An item's sprite mesh needs its transparent texels cut away; a block's
	# textures are fully opaque and the scissor can only punch holes in them where
	# a UV rounds onto a texel boundary.
	mat.set_shader_parameter("alpha_scissor", 0.5 if BlockTextures.is_item(block_id) else 0.0)
	_materials[block_id] = mat
	return mat


## The day/night half of the item light model, into every material in use: the
## values are the ones the terrain was just lit with, so an item and the ground it
## lies on can never disagree about the hour.
func _push_world_lighting() -> void:
	if _chunk_manager == null or _materials.is_empty():
		return
	for mat in _materials.values():
		_chunk_manager.apply_item_lighting(mat)


## The world's light where an item is: the block light and sky light of the cell
## its centre occupies, mapped the way terrain vertices are (see
## ChunkManager.get_light_at).
func _light_at(position: Vector3) -> Vector4:
	if _chunk_manager == null:
		return Vector4(0.0, 0.0, 0.0, 1.0)
	return _chunk_manager.get_light_at(floori(position.x), floori(position.y), floori(position.z))


func _remove(index: int) -> void:
	var item: Dictionary = _items[index]
	var node: Node3D = item["node"]
	if is_instance_valid(node):
		node.queue_free()
	_items.remove_at(index)
	# The body arrays are indexed like _items, so the row that just left has to be
	# gathered again before the next pair solve.
	_bodies_static_dirty = true


## Everything currently on the ground, for probes and tests: one entry per item with
## the block, the count and where it is.
func get_items() -> Array:
	var out := []
	for item in _items:
		out.append({
			"block_id": item["block_id"],
			"count": item["count"],
			"position": item["position"],
			"velocity": item["velocity"],
			"spin": item["spin"],
			"grounded": item["grounded"],
			"asleep": item["asleep"],
			"age": item["age"],
		})
	return out
