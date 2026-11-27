extends Node3D
## The moving lights the world draws besides the player's glow: the torches a goblin carries,
## in EITHER hand -- the one it spawned with in its offhand, and the one in its main hand (an
## item a loot run is inspecting for three seconds, or a torch it decided to keep).
##
## It does not light anything itself. Every frame it picks the nearest torch-bearers to the
## camera and hands `ChunkManager.set_entity_lights` their DRAWN positions and item ids; the
## C++ side resolves each id through `ItemRegistry` (level/15 is the intensity, the item's
## `light` colour is the tint) and pushes the set to the terrain, the water and every item
## and body material. See docs/rendering-notes.md for the stack.
##
## It is a shader glow and NOT a block in the light grid, on purpose: a light that moves
## every frame cannot be marched through the chunk map, which is why the player's own light
## stopped doing that (src/world/player_light.hpp). Its known limit is that the glow is not
## occluded -- a torch behind a wall lights the near face of it -- and that is written down
## rather than solved with a raycast per light.
##
## `process_priority` is negative so this runs before the bodies' own `_process`: a body
## re-feeds its material from the lighting the C++ side has cached, so the cache has to be
## this frame's first.

## The cap is the shaders' own array length (`MAX_ENTITY_LIGHTS` in the two voxel shaders,
## the item include, and render/entity_lights.hpp): the nearest this many are drawn and the
## rest are not.
const MAX_ENTITY_LIGHTS := 4
## How far a light reaches, in blocks: the shaders' `entity_light_radius` default, which the
## C++ side pushes over (render/entity_lights.hpp) -- half the torch's level, the same rule the
## player's glow takes half of, so a torch lights the same pool in either hand. It is also the
## sphere the visibility test below uses, so the two agree by construction.
const ENTITY_LIGHT_RADIUS := 7.0
## The six points the sphere is tested at: a torch just off the frame can still light what
## is in it, which a point test alone would throw away.
const SPHERE_AXES := [Vector3.RIGHT, Vector3.LEFT, Vector3.UP, Vector3.DOWN,
	Vector3.FORWARD, Vector3.BACK]
## Before every goblin's (Node's own is 0).
const GATHER_PRIORITY := -10

var _chunk_manager: Node = null
## What was pushed last, so the empty set is pushed ONCE and then left alone. The lights
## themselves move every frame, so this only ever saves the no-torch case.
var _last_positions := PackedVector3Array()
var _last_ids := PackedInt32Array()
var _pushed := false


func _ready() -> void:
	process_priority = GATHER_PRIORITY
	_chunk_manager = get_node_or_null("/root/Main/ChunkManager")


func _process(_delta: float) -> void:
	if _chunk_manager == null or not _chunk_manager.has_method("set_entity_lights"):
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var found := []
	for node in get_tree().get_nodes_in_group("goblin"):
		if not (node is Node3D) or node.is_queued_for_deletion():
			continue
		# BOTH hands, and the main one first only because it is the rarer light: one goblin
		# contributing two is legal, and the cap trims the whole list nearest-first.
		_gather_hand(node, "get_main_hand_item", "get_main_hand_position", camera, found)
		_gather_hand(node, "get_offhand_item", "get_offhand_position", camera, found)
	if found.size() > 1:
		found.sort_custom(func(a, b): return float(a["away"]) < float(b["away"]))
	var positions := PackedVector3Array()
	var ids := PackedInt32Array()
	for i in mini(found.size(), MAX_ENTITY_LIGHTS):
		var at: Vector3 = found[i]["at"]
		positions.append(at)
		ids.append(int(found[i]["id"]))
	if _pushed and positions == _last_positions and ids == _last_ids:
		return
	_pushed = true
	_last_positions = positions
	_last_ids = ids
	_chunk_manager.call("set_entity_lights", positions, ids)


## One hand's light, if that hand holds a lit thing and the thing can be seen. A body that
## does not answer the pair (a stub, or a model under another host) holds nothing.
func _gather_hand(node: Node, item_method: String, at_method: String, camera: Camera3D,
		found: Array) -> void:
	if not node.has_method(item_method) or not node.has_method(at_method):
		return
	var id := int(node.call(item_method))
	if id <= 0:
		return
	# The DRAWN item, not the goblin's feet: the arm's swing carries the light with it, and an
	# inspected item's light is in the raised hand rather than at its side.
	var at: Vector3 = node.call(at_method)
	# Whether the light can be SEEN, not how far the camera is from it. The old test was a
	# distance from the camera, and that is what made a torch go out five blocks away: the
	# third-person camera sits four or five blocks behind the player, so a goblin five blocks
	# ahead of him was ten from the camera and dropped -- while its 16-block disc was lighting
	# the ground in the middle of the screen. A light's disc is visible from any distance, so
	# the test is the disc against the frustum.
	if not _light_on_screen(camera, at):
		return
	found.append({"away": at.distance_to(camera.global_position), "at": at, "id": id})


## True when any part of the light's sphere is inside the camera's frustum: the centre, or
## one of the six points on its own axes. Seven point tests on a handful of torch-bearers is
## nothing next to a raycast, and nothing here is occluded anyway -- the glow's own known
## limit, written down in docs/rendering-notes.md.
func _light_on_screen(camera: Camera3D, at: Vector3) -> bool:
	if camera.is_position_in_frustum(at):
		return true
	for axis in SPHERE_AXES:
		if camera.is_position_in_frustum(at + (axis as Vector3) * ENTITY_LIGHT_RADIUS):
			return true
	return false
