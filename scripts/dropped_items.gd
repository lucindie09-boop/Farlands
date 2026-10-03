extends Node3D

# Items that have been thrown out of the inventory, in the world.
#
# The inventory half of a drop is not here: the hotbar takes the item out of the
# slot it was dropped from (hotbar.gd, the Q binding) and the shatter that plays
# as the stack drains is the hotbar's own. This node is only the part that leaves
# the HUD -- a stack that has left the player's hands and is now falling,
# tumbling, lying somewhere, and waiting to be walked over.
#
# A dropped item is a real 3D object: the block's own mesh, in the block's own
# shape (a slab drops as a slab, a stair as a stair), textured with the block's
# own texture and lit by the world's light. It is thrown with a spin, it tumbles
# about its centre, it bounces off what it lands on, and it settles flat.
#
# The physics is this project's own, because the voxel world has no Godot physics
# bodies to fall onto: there is no collider anywhere in the world, and the player
# and the K-key dummy both move through ChunkManager.resolve_voxel_collision()
# with their own integrator. An item does the same thing, with the one part
# neither of those needs -- an angular velocity, integrated as a real rotation,
# so a thrown block turns over as it flies instead of sliding.
#
# An item's node origin is the CENTRE of its box, not its feet, so that the
# rotation above turns the block about its middle. The resolver wants the
# Minecraft-style position -- X/Z at the footprint's centre, Y at the feet -- so
# the two are converted at the one place the world is asked, and the items dict
# keeps the resolver's form, which is also what get_items() reports.

## The node under Main that owns the world. Set in Main.tscn.
@export var chunk_manager_path: NodePath = NodePath("../ChunkManager")

## How far along the aim the item starts, and how far below the eye. The offset
## is what makes the throw read as leaving the hand rather than the face.
const SPAWN_FORWARD = 0.45
const SPAWN_DOWN = 0.12

## The smallest box an item is collided as. A torch's shape is a thin pole, and a
## box that thin tunnels through a block boundary at any real speed.
const MIN_SIZE = 0.25

## The throw, at full strength. Vanilla throws an item at about 0.3 blocks/tick
## (six blocks a second) with a lift on top, and the spin is what makes it read
## as thrown rather than pushed.
const THROW_SPEED = 6.0
const THROW_VARY = 0.20       # fraction of that, per drop, so two drops differ
const THROW_UP = 0.35         # fraction of the throw spent lifting the item
const SPIN_MIN = 4.0          # radians/s the item turns, at least
const SPIN_MAX = 13.0         # and at most

## Item physics, in blocks and seconds, following the vanilla numbers the dummy
## uses: gravity a touch under the player's, and drag on every axis so a throw
## slows in the air instead of sailing.
const GRAVITY = 16.0
const MAX_FALL = 60.0         # terminal speed, so a long fall cannot tunnel a thin floor
const DRAG = 0.6              # fraction of the velocity shed per second, in the air
const BOUNCE = 0.30           # how much of the fall a landing gives back
const GROUND_FRICTION = 6.0   # horizontal speed shed per second while touching the ground
const ANGULAR_DRAG = 2.5      # spin shed per second while touching the ground
const AIR_ANGULAR_DRAG = 0.4  # and the little bit shed in the air

## What counts as having come to rest: a slow, grounded item with almost no spin
## stops being integrated and eases into its settled orientation.
const REST_SPEED = 0.35
const REST_SPIN = 0.7
const SETTLE_TIME = 0.15      # seconds spent easing onto the settled rotation

## The item is picked up when the player's feet are within this of it.
const PICKUP_RADIUS = 1.2
## A freshly thrown item cannot be picked up for this long: without it the throw
## that put it on the ground is also the step that collects it again.
const PICKUP_DELAY = 0.6

## How long an item lies there before it is gone. Not persisted across a save in
## this version: an item on the ground does not survive a quit.
const LIFETIME = 300.0
const FADE_TIME = 8.0          # the last stretch of that, spent fading out

# One entry per item: the mesh it is drawn with, where its box is, how fast it is
# going and how fast it is turning, and how long it has left.
var _items: Array = []
# The mesh and material for a block id, built once: a drop is then an instance of
# them rather than a fresh mesh per throw.
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
	var node := Node3D.new()
	node.name = "Drop"
	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = _mesh_of(block_id, tex)
	mesh_instance.material_override = _material_of(block_id, tex)
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	# The mesh's own centre sits at the node's origin, so the node turns the block
	# about its middle and the box below is the box the mesh draws.
	mesh_instance.position = -shape["centre"]
	node.add_child(mesh_instance)
	add_child(node)
	# A short toss out of the hand: along the aim, with a lift, and a little
	# spread so two items of one drop do not travel as one.
	var speed := THROW_SPEED * randf_range(1.0 - THROW_VARY, 1.0 + THROW_VARY)
	var vel := dir * speed + Vector3.UP * (speed * THROW_UP)
	_items.append({
		"block_id": block_id,
		"count": count,
		"node": node,
		"size": shape["size"],
		"centre": shape["centre"],
		# The resolver's form: X/Z at the footprint's centre, Y at the feet.
		"position": from,
		"velocity": vel,
		# The spin, as a world-space angular velocity vector: its length is the
		# turn rate and its direction is the axis turned about.
		"spin": _random_spin(),
		"rotation": node.quaternion,
		"age": 0.0,
		"grounded": false,
		"settling": 0.0,
		"settled": false,
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
		_draw_item(item, delta)


## One frame of an item's flight: gravity and drag on the linear velocity, the
## world's answer to where that motion actually puts it, and the same for the
## rotation. The world's answer is what makes an item land on terrain instead of
## through it, and stop against a wall instead of inside one.
func _step(item: Dictionary, delta: float) -> void:
	if item["settled"]:
		return
	var vel: Vector3 = item["velocity"]
	var spin: Vector3 = item["spin"]
	var grounded: bool = item["grounded"]

	vel.y = maxf(vel.y - GRAVITY * delta, -MAX_FALL)
	# Air drag on everything, and the ground's own friction on top of it: an item
	# that lands should skid to a stop, not slide forever.
	var drag := DRAG + (GROUND_FRICTION if grounded else 0.0)
	vel.x = move_toward(vel.x, 0.0, drag * delta)
	vel.z = move_toward(vel.z, 0.0, drag * delta)
	spin = spin * maxf(0.0, 1.0 - (ANGULAR_DRAG if grounded else AIR_ANGULAR_DRAG) * delta)

	var size := Vector3(maxf(item["size"].x, MIN_SIZE), maxf(item["size"].y, MIN_SIZE),
		maxf(item["size"].z, MIN_SIZE))
	var from: Vector3 = item["position"]
	var node: Node3D = item["node"]
	if _chunk_manager == null:
		# No world to collide with (a scene without a ChunkManager): the item
		# still falls and turns, so the throw is visible, it just has no ground.
		item["position"] = from + vel * delta
		item["velocity"] = vel
		item["spin"] = spin
		_turn(item, delta)
		return
	var result: Dictionary = _chunk_manager.resolve_voxel_collision(from, vel * delta, size)
	var at: Vector3 = result.get("position", from + vel * delta)
	var on_floor: bool = result.get("on_floor", false)
	item["position"] = at
	# A landing gives some of the fall back and kills the rest, and a wall stops
	# the axis that hit it. `collided_*` is the resolver saying which axes the
	# world actually stopped this frame.
	if result.get("collided_y", false):
		if vel.y < 0.0:
			vel.y = -vel.y * BOUNCE
			if absf(vel.y) < REST_SPEED:
				vel.y = 0.0
	if result.get("collided_x", false):
		vel.x = 0.0
	if result.get("collided_z", false):
		vel.z = 0.0
	item["velocity"] = vel
	item["spin"] = spin
	item["grounded"] = on_floor
	_turn(item, delta)

	# Come to rest: on something, barely moving, barely turning. The rotation then
	# eases onto the settled one over SETTLE_TIME instead of stopping wherever the
	# tumble happened to leave it, which is what puts a slab down flat.
	if on_floor and vel.length() < REST_SPEED and spin.length() < REST_SPIN:
		item["settling"] = item["settling"] + delta
		var settle_from: Quaternion = item["rotation"]
		var t := clampf(item["settling"] / SETTLE_TIME, 0.0, 1.0)
		node.quaternion = settle_from.slerp(_settled_rotation(settle_from), t)
		if t >= 1.0:
			item["settled"] = true
			item["velocity"] = Vector3.ZERO
			item["spin"] = Vector3.ZERO
	else:
		item["settling"] = 0.0


## Turn the item by its angular velocity for this frame. A rotation is not a
## vector, so this is a real quaternion step about the spin's own axis rather
## than Euler angles added together -- which is what keeps a fast tumble from
## wobbling as its angles pass each other.
func _turn(item: Dictionary, delta: float) -> void:
	if item["settled"]:
		return
	var spin: Vector3 = item["spin"]
	var node: Node3D = item["node"]
	if spin.length_squared() > 0.0001:
		var step := Quaternion(spin.normalized(), spin.length() * delta)
		node.quaternion = (step * node.quaternion).normalized()
	item["rotation"] = node.quaternion


## Where a tumbling item comes to rest: the axis-aligned orientation nearest the
## one it was left in, so the block sets down on a face instead of on a corner.
## Built by asking which of the six world directions the item's own axes are
## closest to, which gives a whole right-angled basis rather than three
## independently rounded angles that might not be.
func _settled_rotation(q: Quaternion) -> Quaternion:
	var basis := Basis(q)
	var x := _nearest_axis(basis.x)
	var y := _nearest_axis(basis.y)
	var z := _nearest_axis(basis.z)
	# The three are chosen independently and can repeat (a corner-on tumble can
	# put two axes nearest the same direction), which is not a rotation. Keep the
	# first two and take the third as their cross product: it is the one the other
	# two leave, and it is at right angles to both by construction.
	if absf(x.dot(y)) > 0.5:
		y = _nearest_axis(basis.y, x)
	if absf(x.dot(y)) > 0.5 or absf(x.dot(z)) > 0.5 or absf(y.dot(z)) > 0.5:
		z = x.cross(y).normalized()
	return Basis(x, y, z).get_rotation_quaternion()


## Which of the six world axes `v` points along most nearly.
func _nearest_axis(v: Vector3, avoid := Vector3.ZERO) -> Vector3:
	var best := Vector3.ZERO
	var best_dot := -2.0
	for axis in [Vector3.RIGHT, Vector3.LEFT, Vector3.UP, Vector3.DOWN, Vector3.FORWARD, Vector3.BACK]:
		if avoid != Vector3.ZERO and absf(axis.dot(avoid)) > 0.5:
			continue
		var d := v.dot(axis)
		if d > best_dot:
			best_dot = d
			best = axis
	return best


func _random_spin() -> Vector3:
	var axis := Vector3(randf() * 2.0 - 1.0, randf() * 2.0 - 1.0, randf() * 2.0 - 1.0)
	if axis.length_squared() < 0.01:
		axis = Vector3.RIGHT
	return axis.normalized() * randf_range(SPIN_MIN, SPIN_MAX)


## Whether the player is close enough to take the item back. The inventory's own
## answer decides: a full inventory leaves the item on the ground rather than
## destroying it, which is the same rule the mined-block collect uses.
##
## Asked with can_add_block() before give_block(), and never the other way round.
## give_block() fills what it can and only then reports whether ALL of it fitted,
## so a refused pickup would have kept the part that did -- and the item, left on
## the ground because it was refused, would hand that part over again on the next
## frame. The read-only question has to be the gate; the write only follows it.
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


## Put the node where the item's box is. The box's position is the resolver's --
## feet, footprint centred -- and the node's origin is the box's middle, so the
## two differ by half the height.
func _draw_item(item: Dictionary, delta: float) -> void:
	var node: Node3D = item["node"]
	if not is_instance_valid(node):
		return
	var at: Vector3 = item["position"]
	node.global_position = at + Vector3.UP * (item["size"].y * 0.5)
	var fade := 1.0
	var left: float = LIFETIME - item["age"]
	if left < FADE_TIME:
		fade = clampf(left / FADE_TIME, 0.0, 1.0)
	var mesh_instance := node.get_child(0) as MeshInstance3D
	if mesh_instance != null:
		var mat := mesh_instance.material_override as StandardMaterial3D
		if mat != null and not mat.transparency == BaseMaterial3D.TRANSPARENCY_DISABLED:
			mat.albedo_color.a = fade


## The mesh a block drops as: its own shape, built once per block id.
##
## A block is a box mesh of the shape's own boxes, so a slab drops as a slab and
## a stair as a stair. An item -- a stick, a torch -- has no boxes to speak of
## and is drawn as the flat sprite mesh the viewmodel uses for it.
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
			var full := PackedFloat32Array([0.0, 0.0, 0.0, 1.0, 1.0, 1.0])
			boxes.append(full)
		mesh = _mesh_from(ViewmodelMeshes.build_shaped_mesh(boxes))
	_meshes[block_id] = mesh
	return mesh


## The box an item is drawn and collided as: the mesh's own bounds, so a slab is
## collided as the half block it is rather than as a full one.
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
## same material the viewmodel gives a held block, minus the depth-test tricks
## that belong to a first-person hand.
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
	# An item's sprite mesh needs the alpha test for its transparent silhouette;
	# a block's textures are fully opaque and the scissor can only punch holes in
	# them where a UV rounds onto a texel boundary.
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


## Everything currently on the ground, for probes and tests: one entry per item
## with the block, the count and where it is.
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
			"settled": item["settled"],
			"age": item["age"],
		})
	return out
