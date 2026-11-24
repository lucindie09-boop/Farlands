extends Node
## Debug tool: the pause menu's Tools page has a "Spawn Goblin" button, and it
## puts `Assets/models/Goblin2.obj` on the block the player is looking at.
##
## Nothing is simulated: a goblin is one MeshInstance3D standing on the aimed
## block, so the model itself is what is being looked at. The obj is Blockbench's
## own export in blocks already (1.44 tall, feet on y = 0), so it needs no scale;
## only its body's own centre is shifted onto the cell, and it turns to face the
## player who spawned it.
##
## ToolLoader makes this tool on first use (registered there as "goblin"), so the
## mesh and its texture are not part of the launch.

const MODEL_PATH := "res://Assets/models/Goblin2.obj"
const TEXTURE_PATH := "res://Assets/models/Goblin2.png"
## The eye ray's reach, the same 10 blocks block placement uses.
const REACH := 10.0
## The obj's body (torso and legs) is centred on x -0.031, z -0.094, so the node
## is shifted by this much to put the body on the cell's centre instead of half a
## shoulder off it. Its feet are already at y = 0.
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)

var _mesh: Mesh = null
var _material: StandardMaterial3D = null


func _load_goblin() -> void:
	if _mesh != null:
		return
	_mesh = load(MODEL_PATH) as Mesh
	# The obj names an .mtl that is not in the tree, so the imported mesh carries
	# no material: one StandardMaterial3D with the model's own texture is the
	# whole look. Nearest filtering and no specular, because the texture is a
	# small voxel atlas and every UV island has to stay crisp.
	_material = StandardMaterial3D.new()
	_material.albedo_texture = load(TEXTURE_PATH) as Texture2D
	_material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_material.roughness = 1.0
	_material.metallic = 0.0
	_material.specular_mode = BaseMaterial3D.SPECULAR_DISABLED


## One goblin on the block being looked at. This is the whole entry point: the
## Tools page's button asks ToolLoader for the tool by name and then calls this,
## the same shape the dummy egg uses for `pose_clone`'s `spawn()`.
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
	if _mesh == null:
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
	print("Goblin spawned at %s (in the cell the crosshair would place a block in)" % goblin.global_position)


## The model's face looks down its own -Z, so this is the yaw that maps -Z onto
## the direction from the goblin's cell to the player.
func _yaw_facing_player(scene_root: Node, cell: Vector3) -> float:
	var player := scene_root.get_node_or_null("Player") as Node3D
	if player == null:
		return 0.0
	var dir: Vector3 = player.global_position - (cell + Vector3(0.5, 0.0, 0.5))
	return atan2(-dir.x, -dir.z)
