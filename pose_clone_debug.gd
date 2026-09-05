extends Node
## Debug helper: press the bound action (pose_clone_toggle, default K) while
## aiming at a block to spawn a standing clone of the player model on top of
## that block with its animation playing, plus a small bright cube at the
## pivot point of each of the model's meshes. Press again to remove the clone.
##
## The clone is built exactly like Main.tscn builds Player/PlayerModel — a
## fresh player.glb instance carrying player_model.gd with an AnimationPlayer
## child — so the Idle animation loads and plays through the same code path as
## the real model. Pivot markers are parented to each MeshInstance3D, so they
## sit at the node origin and follow the animation if a part moves. The glb
## was re-baked by tools/rebake_player_pivots.py so those origins sit on the
## true Blockbench pivots (arm/arm2 tops at y=24, leg/leg2 tops at y=12,
## torso y=18, head at the neck y=24 in glb units).

const PLAYER_SCENE: PackedScene = preload("res://player.glb")
const PLAYER_MODEL_SCRIPT: Script = preload("res://player_model.gd")
const PIVOT_SHADER: Shader = preload("res://shaders/pose_pivot_marker.gdshader")

# Matches the transform Main.tscn applies to Player/PlayerModel: the glb is
# 0.05625-scaled (1 glb unit = 1/17.78 blocks) with a 180-degree yaw flip and
# a slight sink so the model's feet sit on the stand point.
const MODEL_SCALE := 0.05625
const MODEL_Y_OFFSET := -0.0844
# Marker size in glb units (world size = this * MODEL_SCALE).
const PIVOT_MARKER_GLB := 2.0

var _clone: Node3D = null

func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("pose_clone_toggle") and not event.is_echo():
		# Gameplay only: releasing the mouse (chat/inventory/menu open, dead)
		# also disables the toggle so typing doesn't spawn clones.
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			return
		if _clone != null:
			_despawn()
		else:
			_spawn()

func _spawn() -> void:
	var scene_root := get_tree().current_scene
	if scene_root == null:
		return
	var chunk_manager := scene_root.get_node_or_null("ChunkManager")
	if chunk_manager == null or not chunk_manager.has_method("raycast_from_camera"):
		return

	var hit: Dictionary = chunk_manager.raycast_from_camera(10.0)
	if not hit.get("success", false):
		return

	# Aimed block cell (integers); the clone's feet rest on the highest solid
	# block at that x/z so it stands on top of whatever column was aimed at.
	var bx := int(hit["position"].x)
	var by := int(hit["position"].y)
	var bz := int(hit["position"].z)
	var feet_y := by + 1
	while feet_y < 1024 and chunk_manager.get_block(bx, feet_y, bz) != 0:
		feet_y += 1

	var host := Node3D.new()
	host.name = "PoseClone"
	scene_root.add_child(host)
	host.global_position = Vector3(bx + 0.5, feet_y, bz + 0.5)
	_clone = host

	var model := PLAYER_SCENE.instantiate()
	model.set_script(PLAYER_MODEL_SCRIPT)
	model.transform = _clone_model_transform()
	var anim_player := AnimationPlayer.new()
	anim_player.name = "AnimationPlayer"
	model.add_child(anim_player)
	host.add_child(model)

	_add_pivot_markers(model)
	print("Pose clone spawned at %s (top of block %d,%d,%d)" % [host.global_position, bx, by, bz])
	print_pivots(model)

func _despawn() -> void:
	if _clone == null:
		return
	_clone.queue_free()
	_clone = null
	print("Pose clone removed")

# Same world orientation as the live PlayerModel (player yaw + the glb flip),
# with the feet placed exactly on the stand point.
func _clone_model_transform() -> Transform3D:
	var scene_root := get_tree().current_scene
	# The live model now lives under PlayerController's ModelPivot wrapper
	# (body-yaw lag); fall back to the old path just in case.
	var src_model: Node3D = null
	if scene_root:
		src_model = scene_root.get_node_or_null("Player/ModelPivot/PlayerModel")
		if src_model == null:
			src_model = scene_root.get_node_or_null("Player/PlayerModel")
	if src_model != null:
		return Transform3D(src_model.global_transform.basis, Vector3(0, MODEL_Y_OFFSET, 0))
	var basis := Basis(Vector3(-MODEL_SCALE, 0, 0), Vector3(0, MODEL_SCALE, 0), Vector3(0, 0, -MODEL_SCALE))
	return Transform3D(basis, Vector3(0, MODEL_Y_OFFSET, 0))

# Where Godot thinks each part's pivot is: the MeshInstance3D's origin — the
# point Godot rotates that node around. mi.position is the glb node translation
# (model-local, unaffected by the model's yaw/scale), so these numbers can be
# checked directly against the markers and against the model in an editor.
func print_pivots(model: Node3D) -> void:
	for mesh_instance in model.find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null or mi.name == "PivotMarker":
			continue
		var local: Vector3 = mi.position
		var world: Vector3 = mi.global_position
		print("pivot %-8s model-glb (%7.1f, %6.1f, %6.1f)  world (%6.2f, %6.2f, %6.2f)"
			% [mi.name, local.x, local.y, local.z, world.x, world.y, world.z])

# One bright cube at every MeshInstance3D's origin (== its pivot). Parented to
# the mesh node so markers inherit the pivot position and any animation motion,
# and rendered with depth testing off so they stay visible inside the body.
func _add_pivot_markers(model: Node3D) -> void:
	var marker_mesh := BoxMesh.new()
	marker_mesh.size = Vector3.ONE * PIVOT_MARKER_GLB
	var marker_material := ShaderMaterial.new()
	marker_material.shader = PIVOT_SHADER
	marker_material.set_shader_parameter("marker_color", Color(1.0, 0.15, 0.15))
	for mesh_instance in model.find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null:
			continue
		var marker := MeshInstance3D.new()
		marker.name = "PivotMarker"
		marker.mesh = marker_mesh
		marker.material_override = marker_material
		mi.add_child(marker)
