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
# An item's node origin is the CENTRE of its box, so that the rotation turns the
# block about its middle; `position` is that same centre, in world space.

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

## How finely a body's points are laid out on its surface: the same count on every
## axis, so a slab's contacts are its own surface and a cube's are its own. These
## are the points the solve turns the body with, so a body is turned as exactly the
## shape it is guarded as.
const SURFACE_POINTS = 3

## Item physics, in blocks and seconds. The gravity is a touch under the player's,
## and the damping is what stops a throw sailing: without it nothing in the air
## would ever slow down.
const GRAVITY = 16.0
const MAX_SPEED = 60.0         # terminal speed, so a long fall cannot tunnel
const LINEAR_DAMPING = 0.25    # fraction of the velocity shed per second, in the air
const ANGULAR_DAMPING = 0.30   # and of the spin
const BOUNCE = 0.25            # how much of the approach speed a contact gives back
## Below this approach speed a contact does not bounce at all. Restitution applied
## to a resting body is a perpetual trampoline: gravity adds a hair of downward
## speed every substep, the contact hands a share of it straight back, and the body
## buzzes off the ground forever instead of lying on it.
const RESTING_APPROACH = 0.5
const FRICTION = 0.6           # how much of the tangential motion a contact takes

## A body is stepped in slices this long at most, so a fast throw cannot pass
## through a block between two frames: the contacts are found at the position the
## body is actually at, and a 60-block-a-second fall moves a whole block a frame.
const MAX_SUBSTEP = 1.0 / 120.0

## How many times the contact set is solved over per substep. One pass is a body
## whose corners fight each other -- pushing one out pushes another in, and the
## body buzzes on the spot. Iterating lets the contacts agree on one motion, which
## is what a resting body needs to be still.
const SOLVER_ITERATIONS = 6
## How much of a penetration is corrected per pass, and how deep one is ignored.
## Correcting all of it in one pass overshoots; leaving a slop lets a resting body
## sit still instead of being pushed out and pulled back every substep.
const PUSH_FRACTION = 0.3
const PUSH_SLOP = 0.001

## What counts as being at rest: slow enough, touching something, and staying that
## way for long enough. A body that is asleep stops being integrated until
## something disturbs it -- without that, a block lying on the ground is solved
## forever.
##
## Nothing else is applied to a resting body: no extra drag, no easing onto a flat
## orientation. A body that settles on its own is a body whose contacts held it, and
## a body told to lie flat is a body that cannot balance on an edge when the
## geometry says it should.
const REST_SPEED = 0.12
const REST_SPIN = 0.35
const REST_TIME = 0.3

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


func _ready() -> void:
	_chunk_manager = get_node_or_null(chunk_manager_path)
	_player = get_node_or_null("../Player")


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
	var shape := _shape_of(block_id)
	var size: Vector3 = shape["size"]
	var node := Node3D.new()
	node.name = "Drop"
	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = _mesh_of(block_id, tex)
	mesh_instance.material_override = _material_of(block_id, tex)
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	# The mesh's own centre sits at the node's origin, so the node turns the block
	# about its middle and the body below is the box the mesh draws.
	mesh_instance.position = -shape["centre"]
	node.add_child(mesh_instance)
	add_child(node)

	var speed := THROW_SPEED * randf_range(1.0 - THROW_VARY, 1.0 + THROW_VARY)
	var vel := dir * speed + Vector3.UP * (speed * THROW_UP)
	var half := size * 0.5
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
		"half": half,
		# The body's own points, in its own space: the contacts that turn it.
		"points": _surface_points(half),
		"inertia": _inertia(half),
		# World space. `position` is the CENTRE of the box, which is also the
		# node's own origin.
		"position": from,
		"velocity": vel,
		"spin": spin,
		"age": 0.0,
		"grounded": false,
		"rest": 0.0,
		"asleep": false,
	})


func _process(delta: float) -> void:
	for i in range(_items.size() - 1, -1, -1):
		var item: Dictionary = _items[i]
		item["age"] = item["age"] + delta
		if item["age"] >= LIFETIME:
			_remove(i)
			continue
		_step(item, delta)
		if _try_pickup(item):
			_remove(i)
			continue
		_draw_item(item)


## One frame of a body's flight, in slices short enough that it cannot pass through
## a block between two of them.
func _step(item: Dictionary, delta: float) -> void:
	if item["asleep"]:
		return
	var slices := maxi(1, int(ceil(delta / MAX_SUBSTEP)))
	var slice := delta / float(slices)
	for i in range(slices):
		_substep(item, slice)
		if item["asleep"]:
			return


## One slice: gravity, then the contacts the world reports at the position the body
## is now at, each one pushing it out and taking its share of the motion.
func _substep(item: Dictionary, delta: float) -> void:
	var pos: Vector3 = item["position"]
	var vel: Vector3 = item["velocity"]
	var spin: Vector3 = item["spin"]
	var node: Node3D = item["node"]

	vel.y -= GRAVITY * delta
	vel = vel * maxf(0.0, 1.0 - LINEAR_DAMPING * delta)
	spin = spin * maxf(0.0, 1.0 - ANGULAR_DAMPING * delta)
	if vel.length() > MAX_SPEED:
		vel = vel.normalized() * MAX_SPEED

	# Turn first, then move, then solve where the body ended up. The rotation has
	# to be integrated BEFORE the contacts are found: done after, the body turns
	# into the world every substep with nothing solving it, which is what a block
	# resting on its edge clips through the floor with.
	if spin.length_squared() > 0.000001:
		node.quaternion = (Quaternion(spin.normalized(), spin.length() * delta) * node.quaternion).normalized()
	pos = pos + vel * delta
	var basis := Basis(node.quaternion)

	var inv_inertia := _inv_inertia(item["inertia"], basis)
	var touching := false

	# Solve the body's own points, several times, so the ones that touch agree on
	# one motion instead of fighting.
	var contacts: Array = _contacts(item, pos, basis)
	for iteration in range(SOLVER_ITERATIONS):
		for contact in contacts:
			var r: Vector3 = contact["point"] - pos
			var normal: Vector3 = contact["normal"]
			var depth: float = float(contact["depth"])
			# Resolve the penetration as a POSITION correction, not an impulse: an
			# impulse of the whole overlap would hand the body the bounce speed of
			# how deep it is, every substep, which launches it off the floor.
			if depth > PUSH_SLOP:
				pos = pos + normal * (depth - PUSH_SLOP) * PUSH_FRACTION
			# Then answer the motion this point actually sees, at the point itself
			# -- not the body's centre. This is the difference between a box that
			# slides and a body that turns, and it is what makes a corner catch and
			# topple instead of being ignored.
			var point_vel := vel + spin.cross(r)
			var approach := point_vel.dot(normal)
			if approach < 0.0:
				touching = true
				# Bounce only a real impact. A body already resting closes on the
				# surface by gravity's own fall every substep, and returning a share
				# of THAT is a trampoline; below the threshold the motion is simply
				# absorbed, which is what lets a body lie still.
				var restitution := BOUNCE if absf(approach) > RESTING_APPROACH else 0.0
				var rn := r.cross(normal)
				var denom := 1.0 + rn.dot(inv_inertia * rn)
				var impulse := -(1.0 + restitution) * approach / maxf(denom, 0.0001)
				vel = vel + normal * impulse
				spin = spin + inv_inertia * r.cross(normal * impulse)
				# Friction, as the tangential part of the same contact, through the
				# same effective mass: the surface is what stops a body sliding and
				# what stops it turning when it is set down on a face.
				var tangent := point_vel - normal * approach
				if tangent.length_squared() > 0.000001:
					var t := tangent.normalized()
					var rt := r.cross(t)
					var tdenom := 1.0 + rt.dot(inv_inertia * rt)
					var t_impulse := -minf(tangent.length() * FRICTION,
						FRICTION * absf(impulse)) / maxf(tdenom, 0.0001)
					vel = vel + t * t_impulse
					spin = spin + inv_inertia * r.cross(t * t_impulse)

	# The exact convex guard. Points can only report the places they were put, so a
	# block sitting against the MIDDLE of an edge lies between two of them and the
	# edge passes straight through the world -- the clipping that gets worse the
	# further a spot is from a corner. This test has no such gaps: it answers for the
	# whole box, so whatever the points missed is caught here and pushed back out. It
	# only resolves the penetration; the turning already came from the contacts.
	var guard: Dictionary = _turned_contact(item, pos, basis)
	if guard.get("into", false):
		touching = true
		var guard_normal: Vector3 = guard["normal"]
		var guard_depth: float = float(guard["depth"])
		if guard_depth > PUSH_SLOP:
			pos = pos + guard_normal * (guard_depth - PUSH_SLOP) * PUSH_FRACTION
		# Take the motion into the surface out too, or the body keeps its speed into
		# the world and is pushed out again every substep.
		var closing := vel.dot(guard_normal)
		if closing < 0.0:
			vel = vel - guard_normal * closing

	item["grounded"] = touching
	# Asleep when the contacts have actually stopped it: slow, touching, and staying
	# that way. Nothing else is applied to it -- see REST_SPEED.
	if touching and vel.length() < REST_SPEED and spin.length() < REST_SPIN:
		item["rest"] = item["rest"] + delta
		if item["rest"] >= REST_TIME:
			item["asleep"] = true
			item["velocity"] = Vector3.ZERO
			item["spin"] = Vector3.ZERO
	else:
		item["rest"] = 0.0
	item["position"] = pos
	item["velocity"] = vel
	item["spin"] = spin


## The body's own points that are inside the world: where they touch, which way the
## world pushes each, and how deep. These are what the solve turns the body with,
## because a contact's torque comes from WHERE it is.
func _contacts(item: Dictionary, pos: Vector3, basis: Basis) -> Array:
	if _chunk_manager == null or not _chunk_manager.has_method("contacts_for_points"):
		return []
	var local: Array = item["points"]
	var world := PackedVector3Array()
	world.resize(local.size())
	for i in range(local.size()):
		world[i] = pos + basis * local[i]
	# A small radius, so a body that is merely touching is a contact at depth 0
	# rather than one that only appears a hair later.
	return _chunk_manager.contacts_for_points(world, 0.02)


## The exact convex guard: how deep the WHOLE body is in the world, and the normal
## that pushes it out. See where it is used -- it is what catches the middle of an
## edge, which no set of points can promise.
func _turned_contact(item: Dictionary, pos: Vector3, basis: Basis) -> Dictionary:
	if _chunk_manager == null or not _chunk_manager.has_method("turned_box_contact"):
		return {"into": false, "normal": Vector3.UP, "depth": 0.0}
	return _chunk_manager.turned_box_contact(pos, item["half"], basis)


## A body's points: SURFACE_POINTS along each axis, on the box's surface. The
## corners and edges are where a box meets the world, so that is where they are
## needed; the inside of the box never touches anything.
func _surface_points(half: Vector3) -> Array:
	var out := []
	var n := SURFACE_POINTS
	for ix in range(n):
		for iy in range(n):
			for iz in range(n):
				# Only the shell: a point in the middle of an axis is interior.
				if ix != 0 and ix != n - 1 and iy != 0 and iy != n - 1 and iz != 0 and iz != n - 1:
					continue
				out.append(Vector3(
					half.x * (float(ix) / float(n - 1) * 2.0 - 1.0),
					half.y * (float(iy) / float(n - 1) * 2.0 - 1.0),
					half.z * (float(iz) / float(n - 1) * 2.0 - 1.0)))
	return out


## The body's inertia tensor about its middle, as a diagonal in its own space: the
## box's own formula, scaled by the mass. Its inverse is what a contact needs --
## how much angular velocity an impulse about a lever arm produces.
func _inertia(half: Vector3) -> Vector3:
	var m := maxf(half.x * half.y * half.z * 8.0, 0.001)
	var x := m * (half.y * half.y + half.z * half.z) / 3.0
	var y := m * (half.x * half.x + half.z * half.z) / 3.0
	var z := m * (half.x * half.x + half.y * half.y) / 3.0
	return Vector3(maxf(x, 0.0001), maxf(y, 0.0001), maxf(z, 0.0001))


## The inverse of the inertia tensor, applied in world space: the tensor is diagonal
## in the body's own space, so it is divided per axis there and turned back out into
## the world by the body's own orientation.
func _inv_inertia(inertia: Vector3, basis: Basis) -> Basis:
	var inv := Basis(Vector3(1.0 / inertia.x, 0.0, 0.0),
		Vector3(0.0, 1.0 / inertia.y, 0.0),
		Vector3(0.0, 0.0, 1.0 / inertia.z))
	return basis * inv * basis.transposed()


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
		var mat := mesh_instance.material_override as StandardMaterial3D
		if mat != null and mat.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED:
			mat.albedo_color.a = fade


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


## The box a body is drawn and solved as: the mesh's own bounds, so a slab is a half
## block and collides as one.
func _shape_of(block_id: int) -> Dictionary:
	var mesh := _mesh_of(block_id, BlockTextures.get_texture(block_id))
	var aabb := mesh.get_aabb() if mesh != null else AABB()
	var size := aabb.size
	if size.length_squared() < 0.0001:
		size = Vector3.ONE
	return {"size": size, "centre": aabb.get_center()}


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


## The block's own texture, lit by the world like any other 3D object in it. The
## same material the viewmodel gives a held block, minus the depth-test tricks that
## belong to a first-person hand.
func _material_of(block_id: int, tex: Texture2D) -> StandardMaterial3D:
	if _materials.has(block_id):
		return _materials[block_id]
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = tex
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	mat.roughness = 1.0
	mat.metallic = 0.0
	# An item's sprite mesh needs the alpha test for its transparent silhouette; a
	# block's textures are fully opaque and the scissor can only punch holes in them
	# where a UV rounds onto a texel boundary.
	if BlockTextures.is_item(block_id):
		mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	else:
		mat.transparency = BaseMaterial3D.TRANSPARENCY_DISABLED
	_materials[block_id] = mat
	return mat


func _remove(index: int) -> void:
	var item: Dictionary = _items[index]
	var node: Node3D = item["node"]
	if is_instance_valid(node):
		node.queue_free()
	_items.remove_at(index)


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
