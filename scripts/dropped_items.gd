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
# deepest-overlap push-out shared between them (see _solve_item_pairs), so a
# dropped block lands on another and a stack settles as a stack instead of as two
# blocks in one place. The world's answer belongs to one body and the pair's to
# both: neither can be moved twice for the same overlap, and a throw hands its
# momentum over instead of passing through.
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
## The same, for the item-vs-item solve: fewer passes, because a pair of convex
## boxes has no face of nine points fighting itself.
const PAIR_SOLVER_ITERATIONS = 3
## How far outside its box a point still counts as touching another body's box, in
## blocks -- the same hair of tolerance the world's point contacts take.
const PAIR_CONTACT_RADIUS = 0.02
## How fast a body has to be moving to WAKE a sleeping one it touches. A body merely
## leaning on a sleeping neighbour moves by gravity's own tickle between substeps
## (well under this), and counting that as a hit would wake a stack every substep,
## so it could never come to rest.
const WAKE_SPEED = 0.75
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
	var boxes := _boxes_of(block_id)
	var centre: Vector3 = _body_centre(boxes)
	var size: Vector3 = _body_size(boxes, centre)
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
		# The body's own boxes, in its own space: the contacts that turn it and
		# the guard that keeps it out of the world are both built from these, so a
		# stair is a stair and not the block around it.
		"offsets": _box_offsets(boxes, centre),
		"halves": _box_halves(boxes),
		# The body's own points, in its own space: the contacts that turn it.
		"points": _body_points(boxes, centre),
		"inertia": _inertia_of(boxes, centre),
		# The body's own reach from its origin: the broad phase of the pair solve.
		"radius": _body_reach(boxes, centre),
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
## Every body moves inside a slice BEFORE the pairs are solved, so both halves of a
## stack see each other's motion at the same instant. Stepping one body to the end
## of the frame at a time would let it clear a body that had not moved yet.
func _step_all(delta: float) -> void:
	if _items.is_empty():
		return
	var slices := maxi(1, int(ceil(delta / MAX_SUBSTEP)))
	var slice := delta / float(slices)
	for i in range(slices):
		for item in _items:
			if item["asleep"]:
				continue
			_substep(item, slice)
		_solve_item_pairs(slice)


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

	# The exact convex guard, and the body's ONLY push-out. Points can only report
	# the places they were put, so a block sitting against the MIDDLE of an edge lies
	# between two of them and the edge passes straight through the world -- the
	# clipping that gets worse the further a spot is from a corner. This test has no
	# such gaps: it answers for the whole box, so it catches the middle of an edge
	# AND it is the one correction a face-wide contact cannot multiply. The turning
	# still comes from the points.
	#
	# Asked BEFORE the impulses, because a point contact is only useful while it
	# pushes the way the whole body has to go. A point that has sunk past the middle
	# of a thin block reports the face it is NEAREST, which can be the one it came in
	# through; an impulse along that face would fight the push-out instead of helping
	# it, so those contacts are dropped here.
	var guard: Dictionary = _turned_contact(item, pos, basis)
	var contacts: Array = _contacts(item, pos, basis)
	if guard.get("into", false):
		var separation: Vector3 = guard["normal"]
		var agreeing := []
		for contact in contacts:
			if contact["normal"].dot(separation) >= 0.0:
				agreeing.append(contact)
		contacts = agreeing

	# Solve the body's own points, several times, so the ones that touch agree on
	# one motion instead of fighting.
	#
	# These points answer MOTION and nothing else. The penetration is not pushed out
	# here: a face lying on the floor reports nine points at once, and moving the
	# body out of each would move it out nine times over -- and then again on every
	# iteration -- so a plate that touched the ground was thrown off it by the size
	# of its own contact set. One overlap, one correction: the guard answers for the
	# WHOLE body, so it cannot count the same overlap twice.
	for iteration in range(SOLVER_ITERATIONS):
		for contact in contacts:
			var r: Vector3 = contact["point"] - pos
			var normal: Vector3 = contact["normal"]
			# Answer the motion this point actually sees, at the point itself
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

	item["position"] = pos
	item["velocity"] = vel
	item["spin"] = spin
	_settle(item, delta, touching)


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
##
## Asked of every box the body is made of, so a stair is guarded as its two boxes
## rather than as the full block around them.
func _turned_contact(item: Dictionary, pos: Vector3, basis: Basis) -> Dictionary:
	if _chunk_manager == null or not _chunk_manager.has_method("turned_boxes_contact"):
		return {"into": false, "normal": Vector3.UP, "depth": 0.0}
	var offsets := PackedVector3Array()
	for o in item["offsets"]:
		offsets.append(o)
	var halves := PackedVector3Array()
	for h in item["halves"]:
		halves.append(h)
	return _chunk_manager.turned_boxes_contact(pos, offsets, halves, basis)


## The one place a body decides it has come to rest: slow, touching something, and
## staying that way for long enough. Called at the end of the world substep and
## again after the item-vs-item solve, because a body lying on ANOTHER body is as
## settled as one lying on the ground.
##
## Nothing else is applied to a resting body: no extra drag, no easing onto a flat
## orientation. A body that settles on its own is a body whose contacts held it, and
## a body told to lie flat is a body that cannot balance on an edge when the
## geometry says it should.
func _settle(item: Dictionary, delta: float, touching: bool) -> void:
	item["grounded"] = touching
	if touching and item["velocity"].length() < REST_SPEED and item["spin"].length() < REST_SPIN:
		item["rest"] = item["rest"] + delta
		if item["rest"] >= REST_TIME:
			item["asleep"] = true
			item["velocity"] = Vector3.ZERO
			item["spin"] = Vector3.ZERO
	else:
		item["rest"] = 0.0


## Item against item, once per substep: two bodies whose volumes overlap push each
## other apart along the shortest way out and answer the motion each sees where
## they touch, so a dropped block lands on another, a stack leans, and a throw
## hands its momentum over instead of one body passing through the other.
##
## The pair is a two-body contact in the same terms as the world's: the bodies' own
## surface points carry the turning, and ONE contact -- the deepest box overlap --
## carries the push-out, so the same overlap cannot be corrected twice.
##
## A sleeping body is an OBSTACLE, not a participant: it holds its place and its
## motion until a body MOVING faster than a resting one touches it, which is what
## lets a stack settle instead of being jogged awake by the weight above it.
func _solve_item_pairs(delta: float) -> void:
	for i in range(_items.size()):
		var a: Dictionary = _items[i]
		for j in range(i + 1, _items.size()):
			var b: Dictionary = _items[j]
			if a["asleep"] and b["asleep"]:
				continue
			_solve_pair(a, b, delta)


func _solve_pair(a: Dictionary, b: Dictionary, delta: float) -> void:
	# Broad phase: the bodies' own reaches, so the box math below only runs for the
	# pairs that could possibly touch.
	if (a["position"] - b["position"]).length() > float(a["radius"]) + float(b["radius"]):
		return
	var guard: Dictionary = _pair_guard(a, b)
	if not guard["into"]:
		return

	# A body that is moving has HIT the sleeping one; a body only leaning on it has
	# not, and the sleeping body stays an obstacle.
	var a_velocity: Vector3 = a["velocity"]
	var b_velocity: Vector3 = b["velocity"]
	if a["asleep"] and b_velocity.length() > WAKE_SPEED:
		_wake(a)
	if b["asleep"] and a_velocity.length() > WAKE_SPEED:
		_wake(b)

	# Only the contacts that agree with the guard's separation, for the same reason
	# the world solve drops them: a point sunk past the middle of the other body
	# reports the face it is nearest, which can be the one it came in through.
	var guard_normal: Vector3 = guard["normal"]
	var contacts: Array = _pair_point_contacts(a, b)
	if not contacts.is_empty():
		var agreeing := []
		for contact in contacts:
			if contact["normal"].dot(guard_normal) * float(contact["side"]) >= 0.0:
				agreeing.append(contact)
		contacts = agreeing
	for iteration in range(PAIR_SOLVER_ITERATIONS):
		for contact in contacts:
			_apply_pair_impulse(contact)

	# The pair's push-out, split between the bodies: both can be moved, so each
	# takes half, and an obstacle takes none of it.
	var normal: Vector3 = guard["normal"]
	var depth: float = float(guard["depth"])
	if depth > PUSH_SLOP:
		var correction := (depth - PUSH_SLOP) * PUSH_FRACTION
		var a_free: bool = not a["asleep"]
		var b_free: bool = not b["asleep"]
		if a_free and b_free:
			a["position"] = (a["position"] as Vector3) - normal * (correction * 0.5)
			b["position"] = (b["position"] as Vector3) + normal * (correction * 0.5)
		elif a_free:
			a["position"] = (a["position"] as Vector3) - normal * correction
		elif b_free:
			b["position"] = (b["position"] as Vector3) + normal * correction

	# Both bodies are touching something, so each is settled on the same terms as
	# the world solve: the one standing on the other can come to rest.
	if not a["asleep"]:
		_settle(a, delta, true)
	if not b["asleep"]:
		_settle(b, delta, true)


## A sleeping body that something moving touched: back in the solve from this
## substep on, with its rest timer cleared so it cannot fall asleep again while the
## other body is still moving against it.
func _wake(item: Dictionary) -> void:
	item["asleep"] = false
	item["rest"] = 0.0


## The shortest way two bodies overlap, over every pair of their boxes -- the guard
## of the pair solve, as turned_boxes_contact is the guard of the world one. The
## normal points from `a` to `b`, so moving `a` along -normal and `b` along +normal
## separates them.
func _pair_guard(a: Dictionary, b: Dictionary) -> Dictionary:
	var a_basis := Basis(a["node"].quaternion)
	var b_basis := Basis(b["node"].quaternion)
	var best := {"into": false, "normal": Vector3.UP, "depth": 0.0}
	for i in range(a["offsets"].size()):
		var a_centre: Vector3 = a["position"] + a_basis * a["offsets"][i]
		for j in range(b["offsets"].size()):
			var b_centre: Vector3 = b["position"] + b_basis * b["offsets"][j]
			var hit := _box_pair_overlap(a_centre, a["halves"][i], a_basis,
				b_centre, b["halves"][j], b_basis)
			if hit["overlap"] and hit["depth"] > float(best["depth"]):
				best = {"into": true, "normal": hit["normal"], "depth": hit["depth"]}
	return best


## Two turned boxes, by the separating axis test: they overlap while no axis
## separates them, and the axis they overlap along LEAST is the penetration, whose
## direction is the way out. All fifteen axes have to be tried, not just the six
## box faces: two boxes resting corner to corner are separated by the cross product
## of two edges and by nothing else. The normal points from `a` to `b`.
func _box_pair_overlap(a_centre: Vector3, a_half: Vector3, a_basis: Basis,
		b_centre: Vector3, b_half: Vector3, b_basis: Basis) -> Dictionary:
	var delta: Vector3 = b_centre - a_centre
	var best_depth := INF
	var best_normal := Vector3.UP
	var a_axes: Array[Vector3] = [a_basis.x, a_basis.y, a_basis.z]
	var b_axes: Array[Vector3] = [b_basis.x, b_basis.y, b_basis.z]
	var axes: Array[Vector3] = [a_basis.x, a_basis.y, a_basis.z,
		b_basis.x, b_basis.y, b_basis.z]
	for i in range(3):
		for j in range(3):
			axes.append(a_axes[i].cross(b_axes[j]))
	for axis in axes:
		if axis.length_squared() < 0.000001:
			continue
		var n := axis.normalized()
		var ra := absf(n.dot(a_axes[0])) * a_half.x + absf(n.dot(a_axes[1])) * a_half.y + absf(n.dot(a_axes[2])) * a_half.z
		var rb := absf(n.dot(b_axes[0])) * b_half.x + absf(n.dot(b_axes[1])) * b_half.y + absf(n.dot(b_axes[2])) * b_half.z
		var overlap := ra + rb - absf(n.dot(delta))
		if overlap <= 0.0:
			return {"overlap": false, "normal": Vector3.UP, "depth": 0.0}
		if overlap < best_depth:
			best_depth = overlap
			best_normal = n if n.dot(delta) >= 0.0 else -n
	return {"overlap": true, "normal": best_normal, "depth": best_depth}


## Every point of one body's surface that is inside one of the other's boxes, as a
## contact pushing the FIRST body out of it. Both directions are taken, so the
## turning is carried by whichever body's points are actually in the other.
func _pair_point_contacts(a: Dictionary, b: Dictionary) -> Array:
	var out := []
	var a_basis := Basis(a["node"].quaternion)
	var b_basis := Basis(b["node"].quaternion)
	# `side` is the sign the contact's normal must carry against the pair guard,
	# which points from a to b: a's contacts push a AWAY from b, b's the other way.
	_points_into_boxes(a, a_basis, b, b_basis, -1.0, out)
	_points_into_boxes(b, b_basis, a, a_basis, 1.0, out)
	return out


## `mover`'s own points against `other`'s boxes: each contact carries the point,
## the normal that pushes `mover` away from `other`, `side` (the sign that normal
## must carry against the pair guard), and the two bodies, so the impulse can be
## answered to both of them.
func _points_into_boxes(mover: Dictionary, mover_basis: Basis, other: Dictionary,
		other_basis: Basis, side: float, out: Array) -> void:
	for point in mover["points"]:
		var p: Vector3 = mover["position"] + mover_basis * point
		for i in range(other["offsets"].size()):
			var centre: Vector3 = other["position"] + other_basis * other["offsets"][i]
			var half: Vector3 = other["halves"][i]
			var local: Vector3 = other_basis.transposed() * (p - centre)
			if absf(local.x) > half.x + PAIR_CONTACT_RADIUS \
				or absf(local.y) > half.y + PAIR_CONTACT_RADIUS \
				or absf(local.z) > half.z + PAIR_CONTACT_RADIUS:
				continue
			var dx := half.x - absf(local.x)
			var dy := half.y - absf(local.y)
			var dz := half.z - absf(local.z)
			var normal := other_basis.x
			if dx <= dy and dx <= dz:
				normal = other_basis.x * (1.0 if local.x >= 0.0 else -1.0)
			elif dz <= dy and dz <= dx:
				normal = other_basis.z * (1.0 if local.z >= 0.0 else -1.0)
			else:
				normal = other_basis.y * (1.0 if local.y >= 0.0 else -1.0)
			out.append({"point": p, "normal": normal, "side": side, "mover": mover, "other": other})


## One point of a pair contact, answered for BOTH bodies: the same impulse, opposite
## ways, each body taking it at the point through its own lever arm. A sleeping body
## is static here -- infinite mass -- so a resting item is not pushed by a body that
## is only leaning on it, and does not have to be woken to hold it up.
func _apply_pair_impulse(contact: Dictionary) -> void:
	var mover: Dictionary = contact["mover"]
	var other: Dictionary = contact["other"]
	var normal: Vector3 = contact["normal"]
	var point: Vector3 = contact["point"]
	var mover_free: float = 0.0 if mover["asleep"] else 1.0
	var other_free: float = 0.0 if other["asleep"] else 1.0
	if mover_free == 0.0 and other_free == 0.0:
		return
	var mover_basis := Basis(mover["node"].quaternion)
	var other_basis := Basis(other["node"].quaternion)
	var r_mover: Vector3 = point - mover["position"]
	var r_other: Vector3 = point - other["position"]
	var mover_inv := _inv_inertia(mover["inertia"], mover_basis)
	var other_inv := _inv_inertia(other["inertia"], other_basis)
	var mover_velocity: Vector3 = mover["velocity"] + mover["spin"].cross(r_mover)
	var other_velocity: Vector3 = other["velocity"] + other["spin"].cross(r_other)
	var relative := mover_velocity - other_velocity
	var approach := relative.dot(normal)
	if approach >= 0.0:
		return
	var mover_rn := r_mover.cross(normal)
	var other_rn := r_other.cross(normal)
	var denom := mover_free + other_free \
		+ mover_rn.dot(mover_inv * mover_rn) + other_rn.dot(other_inv * other_rn)
	var restitution := BOUNCE if absf(approach) > RESTING_APPROACH else 0.0
	var impulse := -(1.0 + restitution) * approach / maxf(denom, 0.0001)
	mover["velocity"] = mover["velocity"] + normal * (impulse * mover_free)
	mover["spin"] = mover["spin"] + mover_inv * (r_mover.cross(normal * impulse)) * mover_free
	other["velocity"] = other["velocity"] - normal * (impulse * other_free)
	other["spin"] = other["spin"] - other_inv * (r_other.cross(normal * impulse)) * other_free
	# Friction, the tangential share of the same contact, through the same
	# effective mass: what stops two stacked blocks sliding across each other.
	var tangent := relative - normal * approach
	if tangent.length_squared() > 0.000001:
		var t := tangent.normalized()
		var mover_rt := r_mover.cross(t)
		var other_rt := r_other.cross(t)
		var t_denom := mover_free + other_free \
			+ mover_rt.dot(mover_inv * mover_rt) + other_rt.dot(other_inv * other_rt)
		var t_impulse := -minf(tangent.length() * FRICTION,
			FRICTION * absf(impulse)) / maxf(t_denom, 0.0001)
		mover["velocity"] = mover["velocity"] + t * (t_impulse * mover_free)
		mover["spin"] = mover["spin"] + mover_inv * (r_mover.cross(t * t_impulse)) * mover_free
		other["velocity"] = other["velocity"] - t * (t_impulse * other_free)
		other["spin"] = other["spin"] - other_inv * (r_other.cross(t * t_impulse)) * other_free


## How far a body reaches from its own origin: the broad phase of the item-vs-item
## test, so two items on opposite sides of the world cost one distance check.
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
