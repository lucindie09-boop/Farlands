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
# Landing together is also how two drops of one kind become ONE: the frame after
# they come to lie next to each other their counts add -- up to the stack cap the
# inventory uses -- and the pile is the size, the shape and the motion of the two
# from then on (merge_pass). The drawn size pops once when it grows, which is the
# only part of a merge that is animation.
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

## How big a dropped item is, as a fraction of the cell its shape was authored in.
## A block that leaves the hand is smaller than a placed one: a dropped cube is
## half a block on a side, a dropped slab is that same slab at half the scale, and
## a sprite item is its own silhouette at it. The factor goes on the BODY as well
## as on the mesh -- the boxes the native solve is handed are the boxes the mesh
## draws -- so the thing the player sees and the thing the world collides with are
## always the same size.
const BASE_SCALE := 0.5

## How much bigger a stack of more than one is DRAWN -- and SOLVED -- than a
## single item. A pile grows with what is in it, but sublinearly and with a cap:
## the curve is
##
##     1 + MERGE_SCALE_STEP * log2(count)
##
## up to MERGE_SCALE_MAX, so a pair is 1.25x, four 1.5x, eight 1.75x, and sixteen
## or more sits at twice the size. A cube root -- the pile as its own volume --
## would make 64 items four times the size: too big to see past or stand beside.
## The factor goes on the BODY as well as on the mesh, exactly like BASE_SCALE: a
## bigger pile is bigger to the world too -- it rests on the ground it covers and
## bumps into what it reaches -- and the cap keeps the biggest pile at the size of
## the shape it came from and no bigger.
const MERGE_SCALE_STEP := 0.25
const MERGE_SCALE_MAX := 2.0

## The inventory's own stack cap (src/core/inventory.cpp): what one pile can hold,
## and so the most two items can merge into. A pair whose counts add up past this
## stays two piles however close they lie.
const MAX_STACK := 64

## How close two items of one kind must be to merge. MERGE_GAP is the air allowed
## between their boxes sideways and MERGE_DROP_GAP the air allowed vertically, so a
## block coming down onto a pile joins it as it lands, two laid out apart keep their
## distance, and the same kind a storey below is left where it is.
const MERGE_GAP := 0.5
const MERGE_DROP_GAP := 0.25

## Under this speed a body counts as lying still, whatever its rest timer says: the
## solver's rest decision is a moment behind the motion, not ahead of it, and a
## pile that has just stopped must not halve the speed of the one that hits it.
const MERGE_STILL_SPEED := 0.1

## The pop a merge plays on the DRAWN size: the mesh overshoots the size the body
## took by POP_GROWTH for POP_TIME seconds and settles back onto it (a half sine,
## which starts and ends flat). The pop is on the mesh alone, because the body is
## the pile's size from the frame it merges and the world must not be moved by a
## bounce that is only there to be seen.
const POP_TIME := 0.2
const POP_GROWTH := 0.22

## The share of POP_GROWTH a pop plays once the pile is already as big as a pile
## gets (MERGE_SCALE_MAX): half. Such a merge has no size to show -- two heaps at
## the cap stay the size they were -- so its bounce is a pulse rather than a swell,
## and it never promises growth that is not coming.
const POP_GROWTH_AT_MAX := 0.5

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

## How long an item's light takes to play out, in seconds (the time constant of
## the ease): the light of the cell an item is in is a STEP function of where the
## item is -- one value per block -- so a body rolling, sliding or being shoved
## across a boundary would otherwise change brightness in one frame. The same
## ease, over the eye's cell, is what the viewmodel pushes (viewmodel.gd).
## 0 disables it.
const LIGHT_EASE_TIME := 0.15

# One entry per item: the mesh, the body's own geometry, and its state.
var _items: Array = []
var _meshes: Dictionary = {}
var _materials: Dictionary = {}

var _chunk_manager: Node = null
var _player: Node3D = null
## Whether the world can solve an item's whole substep (ChunkManager.solve_item_bodies).
## Looked up once: it is the same answer every substep, and has_method is not free.
var _body_solve_available := false

# The shape table the native solver is handed: every kind of block that has been
# dropped at a given count, once -- a pile's body is as big as the pile (spawn), so
# the same block dropped once and dropped sixty-four times is two shapes.
# `_shape_rows` maps a block id and a count to a row; the arrays are the rows
# flattened (row s owns boxes [_shape_box_start[s], +_shape_box_count[s]) and
# points [_shape_point_start[s], +_shape_point_count[s])). A shape never moves, so
# this is written only when a stack of that kind and count is dropped for the
# first time.
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
	# The pile's own size: BASE_SCALE, the size every dropped item is, times the
	# count's merge factor. It goes on the body's boxes as well as on the mesh, so
	# the thing drawn and the thing solved are one box at every count, and a stack
	# that reads bigger to the eye is bigger to the world too.
	var body_scale := BASE_SCALE * _merge_scale(count)
	var boxes := _boxes_of(block_id, body_scale)
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
	# The mesh's own centre sits at the node's origin, so the node turns the item
	# about the middle of the box it draws, and that box is the body's own: the same
	# boxes _boxes_of returns, at the same `body_scale`. (A node's transform scales
	# its vertices and THEN translates them, so the centring offset is the scaled
	# one: without that, a shaped mesh whose centre is not the cell's -- a slab, a
	# stair -- would drift off the origin by the shrink and be drawn beside the box
	# it is solved as.)
	mesh_instance.scale = Vector3.ONE * body_scale
	mesh_instance.position = -_mesh_centre(block_id) * body_scale
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
		# points, flattened once for every block id at every count (the boxes are
		# the pile's own size). They are not kept on the item as well -- nothing
		# reads them off it any more.
		"shape": _shape_row(block_id, count, offsets, halves, points),
		# World space. `position` is the CENTRE of the box, which is also the
		# node's own origin.
		"position": from,
		"velocity": vel,
		"spin": spin,
		"age": 0.0,
		"grounded": false,
		"asleep": false,
		# Seconds of merge pop still owed to the drawn size (POP_TIME).
		"pop": 0.0,
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
	# The merges come after the move, where the pairs actually are, and before the
	# pickup, so walking over a pair that has just met collects one pile and not two.
	merge_pass()
	for i in range(_items.size() - 1, -1, -1):
		var item: Dictionary = _items[i]
		if _try_pickup(item):
			_remove(i)
			continue
		_draw_item(item, delta)


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


## The shape row for a block id at a count, added to the table the first time that
## kind of block is dropped with that many in its pile. A shape's boxes and points
## never move, so every item of the same kind and count shares one row and the
## native pair solve is handed the same table every substep.
func _shape_row(block_id: int, count: int, offsets: Array, halves: Array, points: Array) -> int:
	var key := Vector2i(block_id, count)
	if _shape_rows.has(key):
		return _shape_rows[key]
	var row: int = _shape_box_count.size()
	_shape_rows[key] = row
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


## The frame's merges: two items of one kind lying next to each other become the one
## pile their counts add up to, when that fits in a stack (MAX_STACK). It runs after
## the bodies have moved, so a pair is merged where it has actually come to rest --
## and public, so a probe can set two items down exactly where it wants them and ask
## for this one step alone.
##
## A merge is not an animation: the survivor is the pile from the next solve on,
## with the count, the size, the shape and the motion of the two, and the other item
## is gone in the same frame.
func merge_pass() -> void:
	if _items.size() < 2:
		return
	var i := 0
	while i < _items.size():
		# The item at `i` keeps taking on the items after it. Each join makes it
		# bigger, and the next candidate is judged against the size it has now, so
		# three of one kind in a heap become one pile in this one pass.
		var first: Dictionary = _items[i]
		var j := i + 1
		while j < _items.size():
			var second: Dictionary = _items[j]
			if not _can_merge(first, second):
				j += 1
				continue
			# The fuller pile is the one that STAYS: its place, its lean and its
			# tumble are what the merged body keeps, and the other is the one that
			# joins it. Equal counts leave the older item where it lies, and two
			# the same size and age leave the one that was there first.
			var survivor := first
			var joining := second
			if _is_more(second, first):
				survivor = second
				joining = first
				_items[i] = second
				_items[j] = first
				first = second
			_join(survivor, joining)
			_remove(j)
		i += 1


## Whether two items are one pile waiting to happen: the same kind, counts that add
## up inside a stack, and their boxes lying close enough to read as touching. The
## test is on the boxes the world solves -- a pile is bigger than a single item --
## so two heaps meet where they nearly touch, not where their centres are half a
## block from each other; and the vertical air is measured against MERGE_DROP_GAP
## alone, so a block coming down onto a pile joins it as it lands while the same
## kind a storey below is left alone.
func _can_merge(a: Dictionary, b: Dictionary) -> bool:
	if a["block_id"] != b["block_id"]:
		return false
	if a["count"] + b["count"] > MAX_STACK:
		return false
	var pa: Vector3 = a["position"]
	var pb: Vector3 = b["position"]
	var ha: Vector3 = a["half"]
	var hb: Vector3 = b["half"]
	var gap_x := maxf(absf(pa.x - pb.x) - ha.x - hb.x, 0.0)
	var gap_z := maxf(absf(pa.z - pb.z) - ha.z - hb.z, 0.0)
	if Vector2(gap_x, gap_z).length() > MERGE_GAP:
		return false
	return maxf(absf(pa.y - pb.y) - ha.y - hb.y, 0.0) <= MERGE_DROP_GAP


## Which of a pair takes the other on: the fuller pile, and on a tie the older one.
## A full tie goes to the item that was there first -- a comparison answers this,
## never a swap (merge_pass), so two drops of the same size and age stay where the
## first one fell.
func _is_more(a: Dictionary, b: Dictionary) -> bool:
	if a["count"] != b["count"]:
		return a["count"] > b["count"]
	return a["age"] > b["age"]


## The merge: the counts add, the pile takes the younger of the two clocks, and the
## survivor's body becomes the pile's own -- the size, the boxes, the shape row and
## the mesh, exactly as if this many had been dropped at once (spawn), so a merge and
## a spawn can never leave two piles that answer differently. The body takes the new
## size in the frame it merges, and the guard that pushes a landing body out of the
## world pushes a pile out of whatever it grew into.
func _join(survivor: Dictionary, joining: Dictionary) -> void:
	var count: int = survivor["count"] + joining["count"]
	# The motion first, while both counts are still the two parts of the pile.
	_combine_motion(survivor, joining)
	survivor["count"] = count
	# An item's age is both its pickup delay and its lifetime, and the pile takes
	# the younger of the two: a merge cannot be collected the instant the newer part
	# lands, and the whole pile lives out the newer part's own time.
	survivor["age"] = minf(survivor["age"], joining["age"])
	var block_id: int = survivor["block_id"]
	var body_scale := BASE_SCALE * _merge_scale(count)
	var boxes := _boxes_of(block_id, body_scale)
	var centre: Vector3 = _body_centre(boxes)
	var offsets := _box_offsets(boxes, centre)
	var halves := _box_halves(boxes)
	var points := _body_points(boxes, centre)
	survivor["size"] = _body_size(boxes, centre)
	survivor["half"] = survivor["size"] * 0.5
	survivor["inertia"] = _inertia_of(boxes, centre)
	survivor["radius"] = _body_reach(boxes, centre)
	survivor["shape"] = _shape_row(block_id, count, offsets, halves, points)
	var node: Node3D = survivor["node"]
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance != null:
		mesh_instance.scale = Vector3.ONE * body_scale
		mesh_instance.position = -_mesh_centre(block_id) * body_scale
	# The merged body is bigger than whatever it was resting on and has just been
	# handed a motion: it is awake again, and the next slice finds its new rest.
	survivor["asleep"] = false
	# And the news is drawn: the size pops past the new one and settles onto it
	# (_draw_item).
	survivor["pop"] = POP_TIME
	_bodies_static_dirty = true


## What the merged body does with the two motions. When both were moving, the
## momenta add -- each weighted by its own count -- so two stacks meeting carry on
## somewhere between the two instead of either of them suddenly doubling. When only
## one was moving it carries, because a pile lying on the ground has no business
## pulling a moving stack to a stop. Below MERGE_STILL_SPEED a body counts as still
## however it is flagged: the solver's rest decision is a moment behind the motion,
## not ahead of it.
func _combine_motion(survivor: Dictionary, joining: Dictionary) -> void:
	var va: Vector3 = survivor["velocity"]
	var vb: Vector3 = joining["velocity"]
	var a_moving := va.length() > MERGE_STILL_SPEED
	var b_moving := vb.length() > MERGE_STILL_SPEED
	if a_moving and b_moving:
		var weight_a := float(survivor["count"])
		var weight_b := float(joining["count"])
		var total := weight_a + weight_b
		survivor["velocity"] = (va * weight_a + vb * weight_b) / total
		survivor["spin"] = (survivor["spin"] * weight_a + joining["spin"] * weight_b) / total
	elif b_moving and not a_moving:
		survivor["velocity"] = vb
		survivor["spin"] = joining["spin"]


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


## How strong a pop plays for a pile of `count`: full while the pile still grows
## with its count, half once it is at its largest (MERGE_SCALE_MAX). The tolerance
## is for the curve's own arithmetic -- log2(16) lands a hair off four in floating
## point -- so "at the cap" is a band and not a bit-for-bit comparison.
func _pop_growth(count: int) -> float:
	if _merge_scale(count) >= MERGE_SCALE_MAX - 0.001:
		return POP_GROWTH * POP_GROWTH_AT_MAX
	return POP_GROWTH


## Put the node where the body is. The node's origin IS the body's centre, so this
## is the position the solve has been working in.
func _draw_item(item: Dictionary, delta: float) -> void:
	var node: Node3D = item["node"]
	if not is_instance_valid(node):
		return
	node.global_position = item["position"]
	var fade := 1.0
	var left: float = LIFETIME - item["age"]
	if left < FADE_TIME:
		fade = clampf(left / FADE_TIME, 0.0, 1.0)
	# The cell the body is in, eased rather than taken raw (LIGHT_EASE_TIME): the
	# cell light is a step function of position, so a body that drifts across a
	# boundary -- or has its cell relit under it -- changes in a single frame
	# otherwise. The eased value lives on the item, like the fade: every item's
	# light is its own.
	var cell_light: Vector4 = _light_at(item["position"])
	var light: Vector4 = item.get("light", cell_light)
	light = _ease_light(light, cell_light, delta)
	item["light"] = light
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance != null:
		# The drawn size, written every frame: the body's own size (spawn, _join)
		# times the pop a merge is playing. One assignment, and the mesh can never
		# be left behind by whatever changed the pile.
		var count: int = item["count"]
		var draw_scale := BASE_SCALE * _merge_scale(count)
		var pop: float = item.get("pop", 0.0)
		if pop > 0.0:
			pop = maxf(pop - delta, 0.0)
			item["pop"] = pop
			draw_scale *= 1.0 + _pop_growth(count) * sin(PI * (1.0 - pop / POP_TIME))
		mesh_instance.scale = Vector3.ONE * draw_scale
		# Per instance, not per material: every dropped block of one kind shares
		# one material, and each item's fade and the cell it lies in are its own.
		mesh_instance.set_instance_shader_parameter("item_fade", fade)
		mesh_instance.set_instance_shader_parameter("item_light", light)


## Framerate-independent exponential ease toward `target`: LIGHT_EASE_TIME is the
## time constant, so a step takes the same time to play out at any frame rate.
## probes/probe_item_light_smooth.gd holds this rule, for both consumers, to the
## value the shader is actually handed.
func _ease_light(current: Vector4, target: Vector4, delta: float) -> Vector4:
	if LIGHT_EASE_TIME <= 0.0:
		return target
	return current.lerp(target, 1.0 - exp(-delta / LIGHT_EASE_TIME))


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


## The boxes a body is drawn and solved as, at `body_scale` (_scaled_boxes): the
## block's OWN shape boxes, so a slab is a half block, a stair is its two boxes,
## and neither is the full block around them. An item is its own mesh's bounds --
## the sprite's silhouette at the width its mesh was extruded to -- so a stick is a
## block long and a torch is a quarter of one, each as thick as it is drawn. The
## mesh is built from these same boxes, so the two at one factor are one thing.
func _boxes_of(block_id: int, body_scale: float) -> Array:
	var out := []
	if BlockTextures.is_item(block_id):
		var mesh := _mesh_of(block_id, BlockTextures.get_texture(block_id))
		var size: Vector3 = mesh.get_aabb().size if mesh != null else Vector3.ONE
		size = Vector3(maxf(size.x, ITEM_THICKNESS), maxf(size.y, ITEM_THICKNESS),
			maxf(size.z, ITEM_THICKNESS))
		var half := size * 0.5
		out.append({"lo": -half, "hi": half})
		return _scaled_boxes(out, body_scale)
	if _chunk_manager != null and _chunk_manager.has_method("get_selection_boxes"):
		for b in _chunk_manager.get_selection_boxes(block_id):
			out.append({"lo": Vector3(b[0], b[1], b[2]), "hi": Vector3(b[3], b[4], b[5])})
	if out.is_empty():
		out.append({"lo": Vector3.ZERO, "hi": Vector3.ONE})
	return _scaled_boxes(out, body_scale)


## A shape's own boxes at `body_scale` -- BASE_SCALE, the size every dropped item
## is, times the pile's own factor -- shrunk about the centre of their bounds: the
## same shape, at the size the drop is drawn and solved. Scaling about that centre
## is what lets the mesh and the body share one origin: the node's origin IS the
## centre (_body_centre), and the mesh is drawn about it too (spawn), so the drawn
## thing and the solved thing stay the same box in the same place whatever the
## factor is.
func _scaled_boxes(boxes: Array, body_scale: float) -> Array:
	var centre := _body_centre(boxes)
	var out := []
	for box in boxes:
		out.append({
			"lo": centre + (box["lo"] - centre) * body_scale,
			"hi": centre + (box["hi"] - centre) * body_scale,
		})
	return out


## The factor a stack of `count` is drawn at, on top of BASE_SCALE: a single item
## is 1, and a pile grows by MERGE_SCALE_STEP per doubling of the count, up to
## MERGE_SCALE_MAX (the constants above). log(count)/log(2) is the log2 the curve is
## written in terms of.
func _merge_scale(count: int) -> float:
	if count <= 1:
		return 1.0
	var grown := 1.0 + MERGE_SCALE_STEP * log(float(count)) / log(2.0)
	return minf(grown, MERGE_SCALE_MAX)


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
