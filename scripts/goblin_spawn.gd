extends Node
## Debug tool: the pause menu's Tools page has a "Spawn Goblin" button, and G does
## the same in the world -- it puts `Assets/models/Goblin2.obj` on the block the
## player is looking at.
##
## Nothing is simulated: a goblin is one MeshInstance3D standing on the aimed
## block, so the model itself is what is being looked at. The obj is Blockbench's
## own export in blocks already (1.44 tall, feet on y = 0), so it needs no scale;
## only its body's own centre is shifted onto the cell, and it turns to face the
## player who spawned it.
##
## Lighting is the world's own, not the engine's, and it is the same light the
## player's body and the dropped items are drawn with: `item_shader.gdshader` over
## `item_lighting.gdshaderinc`, fed `ChunkManager.apply_item_lighting` once a frame
## plus the light of the cell the goblin stands in, through the per-instance
## `item_light`. A goblin in a cave is dark, one under the open sky is lit and one
## beside a torch is warm -- and it can never disagree with the ground under it.
##
## ToolLoader makes this tool on first use (registered there as "goblin"), so the
## mesh and its texture are not part of the launch.

const MODEL_PATH := "res://Assets/models/Goblin2.obj"
const TEXTURE_PATH := "res://Assets/models/Goblin2.png"
## The world's own light model; the same shader the player's body wears. It is
## unshaded on purpose and samples its albedo with `filter_nearest`, so a voxel
## texture stays crisp without a StandardMaterial3D's filtering hints.
const ITEM_SHADER: Shader = preload("res://Assets/shaders/item_shader.gdshader")
## The eye ray's reach, the same 10 blocks block placement uses.
const REACH := 10.0
## The obj's body (torso and legs) is centred on x -0.031, z -0.094, so the node
## is shifted by this much to put the body on the cell's centre instead of half a
## shoulder off it. Its feet are already at y = 0.
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)
## A goblin stands still, but the light around it does not: a torch placed beside it
## is a step, so the cell light eases over the same short fade the player's body and
## the dropped items use (their LIGHT_EASE_TIME), rather than cutting.
const LIGHT_EASE_TIME := 0.15

var _mesh: Mesh = null
## One material for every goblin: the model is one surface and one texture, and the
## per-goblin half of the light is the instance parameter below.
var _material: ShaderMaterial = null
## How far above its feet the goblin's middle is: the cell its light is read from,
## the way the player's body reads it at its own middle.
var _light_height := 0.0
var _chunk_manager: Node = null
var _goblins: Array[MeshInstance3D] = []
## Each goblin's current cell light, so a step eases instead of cutting.
var _lights: Dictionary = {}


func _load_goblin() -> void:
	if _mesh != null:
		return
	_mesh = load(MODEL_PATH) as Mesh
	if _mesh == null:
		return
	var aabb: AABB = _mesh.get_aabb()
	_light_height = aabb.get_center().y
	# The obj names an .mtl that is not in the tree, so the imported mesh carries no
	# material: this one is the whole look, and it is the world's light model rather
	# than the engine's.
	_material = ShaderMaterial.new()
	_material.shader = ITEM_SHADER
	_material.set_shader_parameter("albedo_texture", load(TEXTURE_PATH) as Texture2D)
	# Closed opaque boxes: there is no sprite texel here to cut away.
	_material.set_shader_parameter("alpha_scissor", 0.0)


## One goblin on the block being looked at. This is the whole entry point: the
## Tools page's button and the G key both ask ToolLoader for the tool by name and
## then call this, the same shape the dummy egg uses for `pose_clone`'s `spawn()`.
func spawn() -> void:
	var scene_root := get_tree().current_scene
	if scene_root == null:
		return
	var chunk_manager := scene_root.get_node_or_null("ChunkManager")
	if chunk_manager == null or not chunk_manager.has_method("raycast_from_camera"):
		return
	# This is already the EYE ray along the look (it falls back to the camera only
	# where there is no player), so third person points where first person does.
	var hit: Dictionary = chunk_manager.raycast_from_camera(REACH)
	if not hit.get("success", false):
		print("Goblin: nothing within %d blocks to stand on" % int(REACH))
		return
	_load_goblin()
	if _mesh == null or _material == null:
		push_error("Goblin: %s did not load" % MODEL_PATH)
		return
	# place_position is the cell a block would go into, so the feet land on the
	# aimed face: on top when looking down at a block, beside it when looking at
	# a side.
	var cell: Vector3 = hit["place_position"]
	var goblin := MeshInstance3D.new()
	goblin.name = "Goblin"
	goblin.mesh = _mesh
	goblin.material_override = _material
	# A group rather than the node name: the second goblin in a scene gets an
	# engine-generated name (Godot does not number a duplicate one), so the group is
	# what identifies them.
	goblin.add_to_group("goblin")
	scene_root.add_child(goblin)
	goblin.global_position = Vector3(cell.x + BODY_OFFSET.x, cell.y, cell.z + BODY_OFFSET.z)
	goblin.rotation.y = _yaw_facing_player(scene_root, cell)
	_goblins.append(goblin)
	_chunk_manager = chunk_manager
	# Lit before the next frame draws it, instead of one frame of black; the world's
	# light uniforms follow in `_process` while a goblin is standing there.
	_update_light(goblin, 0.0)
	set_process(true)
	print("Goblin spawned at %s (in the cell the crosshair would place a block in)" % goblin.global_position)


## The world light half of the item model onto the shared material, then each
## goblin's own cell light onto its instance -- the same pair `player_model.gd`
## feeds the player's body every frame. The world half moves with the hour and the
## cell half moves when the world around the goblin does, so both are re-pushed
## while any goblin is standing.
func _process(delta: float) -> void:
	for i in range(_goblins.size() - 1, -1, -1):
		if not is_instance_valid(_goblins[i]):
			_lights.erase(_goblins[i])
			_goblins.remove_at(i)
	if _goblins.is_empty():
		set_process(false)
		return
	if _chunk_manager == null or not is_instance_valid(_chunk_manager):
		return
	_chunk_manager.apply_item_lighting(_material)
	for goblin in _goblins:
		_update_light(goblin, delta)


## The light of the cell the goblin's middle is in, eased toward over
## LIGHT_EASE_TIME. `delta` 0 takes it as it is: a goblin that has just appeared has
## nothing to fade from.
func _update_light(goblin: MeshInstance3D, delta: float) -> void:
	var at: Vector3 = goblin.global_position + Vector3(0.0, _light_height, 0.0)
	var cell: Vector4 = _chunk_manager.get_light_at(floori(at.x), floori(at.y), floori(at.z))
	var light := cell
	if delta > 0.0 and LIGHT_EASE_TIME > 0.0 and _lights.has(goblin):
		light = (_lights[goblin] as Vector4).lerp(cell, 1.0 - exp(-delta / LIGHT_EASE_TIME))
	_lights[goblin] = light
	goblin.set_instance_shader_parameter("item_light", light)


## The model's face looks down its own -Z, so this is the yaw that maps -Z onto
## the direction from the goblin's cell to the player.
func _yaw_facing_player(scene_root: Node, cell: Vector3) -> float:
	var player := scene_root.get_node_or_null("Player") as Node3D
	if player == null:
		return 0.0
	var dir: Vector3 = player.global_position - (cell + Vector3(0.5, 0.0, 0.5))
	return atan2(-dir.x, -dir.z)
