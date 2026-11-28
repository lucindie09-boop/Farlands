extends Node
## Debug tool: the pause menu's Tools page has a "Spawn Goblin" button, and G does
## the same in the world -- it puts `assets/models/Goblin.obj` on the block the
## player is looking at.
##
## A goblin is an ENTITY, not scenery: the body is a Node3D carrying `goblin.gd` --
## moved by the PLAYER's own movement simulation (`ChunkManager.make_mob_body`), plus
## six hearts of its own health -- standing in the `pose_clone` group, which is the
## group PlayerController punches. Left-clicking one knocks it back with the base 0.4
## and the sprint bonus through the sim's own hurt window, and takes a half-heart, so
## it tumbles down a hill or off a ledge and dies on the twelfth hit instead of
## standing there. Nothing here builds the movement: the entity hands the world its
## wish direction and the world moves it.
##
## The model is a child of that body, and it is a BODY of its own: `goblin_model.gd` is
## the player's own body -- the walk blend tree, the idle added over it, the lit
## per-cell material (scripts/player_model.gd) -- wearing the goblin's mesh. The obj is
## Blockbench's own export in blocks already (1.44 tall, feet on y = 0), so nothing is
## scaled: only its body's own centre is shifted onto the body's origin, and it is
## turned to face the player who spawned it.
##
## The lighting is that body's, not this tool's, so a goblin in a cave is dark, one
## under the open sky is lit and one beside a torch is warm -- the same light the
## player's body and the dropped items are drawn with, re-read every frame so a
## knocked-back goblin is lit where it lands.
##
## ToolLoader makes this tool on first use (registered there as "goblin"), so the
## mesh and its texture are not part of the launch.

const MODEL_PATH := "res://assets/models/Goblin.obj"
## The entity's own script: the player's movement sim for the body, `apply_knockback`
## for the punch, and the goblin's six hearts (held as state; it draws no bar over its
## head).
const BODY_SCRIPT_PATH := "res://scripts/goblin.gd"
## The body the mesh is DRAWN as: the player's animation and world light wearing the
## goblin's own picture, with the obj's eleven surfaces split into the six parts it was
## authored as, each hung from its joint.
const MODEL_SCRIPT_PATH := "res://scripts/goblin_model.gd"
## The eye ray's reach, the same 10 blocks block placement uses.
const REACH := 10.0
## The obj's body (torso and legs) is centred on x -0.031, z -0.094, so the model is
## shifted by this much to put the body on its own origin instead of half a shoulder
## off the box the click is tested against. Its feet are already at y = 0.
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)

var _mesh: Mesh = null
var _body_script: Script = null
var _model_script: Script = null


func _load_goblin() -> void:
	if _mesh != null:
		return
	_mesh = load(MODEL_PATH) as Mesh
	if _mesh == null:
		return
	# The obj names an .mtl that is not in the tree, so the imported mesh carries no
	# material: the drawn body's own material is the whole look (goblin_model.gd).
	_body_script = load(BODY_SCRIPT_PATH) as Script
	_model_script = load(MODEL_SCRIPT_PATH) as Script


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
	if _mesh == null or _body_script == null or _model_script == null:
		push_error("Goblin: %s or %s did not load" % [MODEL_PATH, BODY_SCRIPT_PATH])
		return
	# place_position is the cell a block would go into, so the feet land on the
	# aimed face: on top when looking down at a block, beside it when looking at
	# a side.
	var cell: Vector3 = hit["place_position"]
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(_body_script)
	# The K dummy's group: PlayerController punches the body in it that is nearest
	# along the aim ray, and calls `apply_knockback` on it. `goblin` is ours, so the
	# ones this tool made can be found (a second one gets an engine-generated name).
	body.add_to_group("pose_clone")
	body.add_to_group("goblin")
	# The cell's CENTRE, not its corner: `place_position` is a block's integer cell, and
	# a body dropped on the corner stands on the seam between four cells with its box
	# half inside a wall (it would be pinned there, unable to be knocked anywhere).
	# This is the point a block would occupy, and where the dummy egg puts the clone.
	# Positioned BEFORE add_child: dummy.gd's _ready snapshots this as its tick
	# position, and its interpolation then renders around it every frame.
	body.position = Vector3(cell.x + 0.5, cell.y, cell.z + 0.5)
	scene_root.add_child(body)

	# The model rides the body: the child's own offset centres the mesh on the box
	# the punch is tested against, and the yaw only ever turns the picture, never the
	# body, so the physics stays axis-aligned.
	var model := Node3D.new()
	model.name = "Model"
	model.set_script(_model_script)
	# Handed over before the body enters the tree: goblin_model.gd builds its rig from
	# this mesh at `_ready`, and captures each part's rest rotation there.
	model.set("source_mesh", _mesh)
	model.position = BODY_OFFSET
	model.rotation.y = _yaw_facing_player(scene_root, cell)
	body.add_child(model)

	print("Goblin spawned at %s (in the cell the crosshair would place a block in)" % body.global_position)


## The model's face looks down its own -Z, so this is the yaw that maps -Z onto
## the direction from the goblin's cell to the player.
func _yaw_facing_player(scene_root: Node, cell: Vector3) -> float:
	var player := scene_root.get_node_or_null("Player") as Node3D
	if player == null:
		return 0.0
	var dir: Vector3 = player.global_position - (cell + Vector3(0.5, 0.0, 0.5))
	return atan2(-dir.x, -dir.z)
