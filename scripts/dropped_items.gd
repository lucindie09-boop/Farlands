extends Node3D

# Items that have been thrown out of the inventory, in the world.
#
# The inventory half of a drop is not here: the hotbar takes the item out of the
# slot it was dropped from (hotbar.gd, the Q binding) and the shatter that plays
# as the stack drains is the hotbar's own. This node is only the part that leaves
# the HUD -- a stack that has left the player's hands and is now falling, lying
# somewhere, and waiting to be walked over.
#
# Nothing here is a physics body. An item is a billboard sprite and a small box,
# and it moves through the voxel world the same way the player does: the frame's
# motion is handed to ChunkManager.resolve_voxel_collision(), which returns where
# the box actually ended up and whether it is standing on something. That
# resolver takes Minecraft-style positions -- X/Z at the CENTRE of the footprint,
# Y at the feet -- so an item keeps its `position` that way too, and only
# converts to the sprite's own centre when drawing.
#
# One Sprite3D is the whole of a dropped item: the icon renderer's isometric
# texture, billboarded, at a fixed world size. A drop costs a sprite and a
# dictionary, not a scene instance or a physics body, so a pile of them is cheap
# and none of it has to be registered with the engine.

const BlockIconArt := preload("res://scripts/block_icon_art.gd")

## The node under Main that owns the world. Set in Main.tscn.
@export var chunk_manager_path: NodePath = NodePath("../ChunkManager")

## How far along the aim ray the item starts, and how far below the eye. The
## offset is what makes the throw read as leaving the hand rather than the face.
const SPAWN_FORWARD = 0.45
const SPAWN_DOWN = 0.12

## How wide the item is, in blocks: the box it is swept as, and the size it is
## drawn at. Small enough to sit inside one cell, so an item never straddles a
## block boundary and refuses to settle.
const SIZE = 0.25

const THROW_SPEED = 4.0        # blocks/s along the aim, at full strength
const THROW_VARY = 0.20        # fraction of that, per drop, so two drops differ
const THROW_UP = 0.35          # fraction of the throw spent lifting the item
const THROW_SPIN = 2.5         # radians/s the sprite turns while it is in the air
const THROW_TURN = 1.0         # how far the item tips, in radians, as it is thrown

const GRAVITY = 7.0            # blocks/s^2, a little under the player's, so a throw arcs
const MAX_FALL = 60.0          # terminal speed, so a long fall cannot tunnel a thin floor
const SETTLE_SPEED = 0.05      # below this a grounded item stops being simulated

## The item is picked up when the player's feet are within this of it.
const PICKUP_RADIUS = 1.2
## A freshly thrown item cannot be picked up for this long: without it the throw
## that put it on the ground is also the step that collects it again.
const PICKUP_DELAY = 0.6

## How long an item lies there before it is gone. Not persisted across a save in
## this version: an item on the ground does not survive a quit.
const LIFETIME = 300.0
const FADE_TIME = 8.0          # the last stretch of that, spent fading out

# One entry per item: the sprite it is drawn with, where its box is, how fast it
# is going, and how long it has left.
var _items: Array = []

var _chunk_manager: Node = null
var _player: Node3D = null


func _ready() -> void:
	_chunk_manager = get_node_or_null(chunk_manager_path)
	_player = get_node_or_null("../Player")


## Throw `count` of `block_id` into the world from the camera's position, along
## the aim. The caller has already taken them out of the inventory.
func spawn(block_id: int, count: int, from: Vector3, direction: Vector3) -> void:
	if block_id <= 0 or count <= 0:
		return
	var tex := BlockIconArt.texture(block_id)
	if tex == null:
		return
	var dir := direction.normalized()
	if dir.length_squared() < 0.5:
		dir = Vector3.FORWARD
	var sprite := Sprite3D.new()
	sprite.texture = tex
	sprite.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	sprite.shaded = false
	sprite.no_depth_test = false
	sprite.pixel_size = SIZE / float(maxi(tex.get_width(), 1))
	# Drawn on top of the terrain it lands on rather than inside it: an item is
	# 0.25 blocks tall and a block is a whole one, so an item resting on the
	# ground is still inside the cell it landed in.
	sprite.render_priority = 1
	add_child(sprite)
	# A short toss out of the hand: along the aim, with a lift, and a little
	# spread so two items of one drop do not travel as one.
	var speed := THROW_SPEED * randf_range(1.0 - THROW_VARY, 1.0 + THROW_VARY)
	var vel := dir * speed + Vector3.UP * (speed * THROW_UP)
	_items.append({
		"block_id": block_id,
		"count": count,
		"sprite": sprite,
		"position": from + dir * SPAWN_FORWARD + Vector3.DOWN * SPAWN_DOWN,
		"velocity": vel,
		"age": 0.0,
		"grounded": false,
		"spin": randf() * TAU,
		"turn": 0.0,
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
		_place_sprite(item, delta)


## One frame of an item's fall: gravity, then the world's answer to where that
## motion actually puts it. The answer is what makes an item land on terrain
## instead of through it, and what stops it against a wall.
func _step(item: Dictionary, delta: float) -> void:
	if item["grounded"]:
		# A settled item is not falling any more. Re-asking the world every frame
		# would be a collision query per item for an answer that has not changed.
		return
	var vel: Vector3 = item["velocity"]
	vel.y = maxf(vel.y - GRAVITY * delta, -MAX_FALL)
	var size := Vector3(SIZE, SIZE, SIZE)
	var from: Vector3 = item["position"]
	if _chunk_manager == null:
		# No world to collide with (a scene without a ChunkManager): the item
		# still falls, so the throw is visible, it just has no ground.
		item["position"] = from + vel * delta
		item["velocity"] = vel
		return
	var result: Dictionary = _chunk_manager.resolve_voxel_collision(from, vel * delta, size)
	item["position"] = result.get("position", from + vel * delta)
	item["velocity"] = vel
	# Landed, or as good as: stop asking. `on_floor` is the resolver's own
	# answer, and the speed check is for the frame it comes to rest on.
	if result.get("on_floor", false) and absf(vel.y) < SETTLE_SPEED:
		item["grounded"] = true
		item["velocity"] = Vector3.ZERO


## Whether the player is close enough to take the item back. The inventory's own
## answer decides: a full inventory leaves the item on the ground rather than
## destroying it, which is the same rule the mined-block collect uses.
func _try_pickup(item: Dictionary) -> bool:
	if item["age"] < PICKUP_DELAY or _player == null:
		return false
	var at: Vector3 = item["position"]
	var feet := _player.global_position
	var flat := Vector2(at.x - feet.x, at.z - feet.z).length()
	if flat > PICKUP_RADIUS or absf(at.y - feet.y) > PICKUP_RADIUS + 1.0:
		return false
	if not _player.has_method("give_block"):
		return false
	if not _player.give_block(item["block_id"], item["count"]):
		return false
	return true


## Put the sprite where the item's box is: the box's centre, and turning while it
## is still in the air.
func _place_sprite(item: Dictionary, delta: float) -> void:
	var sprite: Sprite3D = item["sprite"]
	var at: Vector3 = item["position"]
	sprite.global_position = at + Vector3.UP * (SIZE * 0.5)
	var fade := 1.0
	var left: float = LIFETIME - item["age"]
	if left < FADE_TIME:
		fade = clampf(left / FADE_TIME, 0.0, 1.0)
	sprite.modulate.a = fade
	if item["grounded"]:
		sprite.rotation.z = 0.0
		return
	item["spin"] = item["spin"] + THROW_SPIN * delta
	# Tipped a little by the throw and spinning about its own axis, which is all
	# the motion a flat billboard can show.
	item["turn"] = minf(item["turn"] + delta * 2.0, THROW_TURN)
	sprite.rotation.z = sin(item["spin"]) * item["turn"]


func _remove(index: int) -> void:
	var item: Dictionary = _items[index]
	var sprite: Sprite3D = item["sprite"]
	if is_instance_valid(sprite):
		sprite.queue_free()
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
			"grounded": item["grounded"],
			"age": item["age"],
		})
	return out
