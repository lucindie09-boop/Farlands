extends Node
## Debug helper: press the bound action (pose_clone_toggle, default K) while
## aiming at a block to spawn a standing clone of the player model on top of
## that block with NO animation (no AnimationPlayer child, so player_model.gd
## never loads or plays Idle.anim — a frozen dummy), plus a small bright cube
## at the pivot point of each of the model's meshes. Press again to remove the
## clone.
##
## The clone is a physics dummy (dummy.gd): it falls with vanilla gravity/drag
## and gets knocked back with the vanilla combat knockback when you left-click
## it within punch reach (see PlayerController::try_punch_dummy).
##
## The clone is built like Main.tscn builds Player/PlayerModel — a fresh
## player.glb instance carrying player_model.gd — but without the
## AnimationPlayer, so the Idle animation never loads or plays. Pivot markers
## are parented to each MeshInstance3D, so they sit at the node origin and
## follow the animation if a part moves. The glb was re-baked by
## tools/rebake_player_pivots.py so those origins sit on the true Blockbench
## pivots (arm/arm2 tops at y=24, leg/leg2 tops at y=12, torso y=18, head at
## the neck y=24 in glb units).

const PLAYER_SCENE: PackedScene = preload("res://player.glb")
const PLAYER_MODEL_SCRIPT: Script = preload("res://player_model.gd")
const PIVOT_SHADER: Shader = preload("res://shaders/pose_pivot_marker.gdshader")
# Vanilla-accurate physics (gravity/drag/knockback) — see dummy.gd.
const DUMMY_SCRIPT: Script = preload("res://dummy.gd")

# --- path debug -------------------------------------------------------------
# P asks the engine's planner (worker thread) for a ground route from the clone
# to the player and draws it as translucent red cubes on the blocks the route
# stands on. L toggles between the raw A* grid path and the string-pulled
# waypoints; P again clears. A route the planner could not finish inside its
# budget (a partial, best-effort run toward the goal) is drawn amber instead of
# red, so a truncated route is never mistaken for a complete one. The clone is
# not moved — this only visualises the route the planner found. Results come
# back through ChunkManager.poll_paths(), matched by job id.
const PATH_ACTION := "pose_clone_path"
const PATH_TOGGLE_ACTION := "pose_clone_path_toggle"
const PATH_COLOR_RAW := Color(1.0, 0.22, 0.12, 0.40)
const PATH_COLOR_WAYPOINTS := Color(0.25, 1.0, 0.4, 0.5)
const PATH_COLOR_PARTIAL := Color(1.0, 0.72, 0.15, 0.5)

# Matches the transform Main.tscn applies to Player/PlayerModel: the glb is
# 0.05625-scaled (1 glb unit = 1/17.78 blocks) with a 180-degree yaw flip and
# a slight sink so the model's feet sit on the stand point.
const MODEL_SCALE := 0.05625
const MODEL_Y_OFFSET := -0.0844
# The glb's geometry (and every part's pivot axis) is centered on glb z=1.5
# while the node origin sits at z=0, so the model lies 1.5 glb units toward
# its face from the origin. Offsetting the origin +0.0844 (1.5 * scale) puts
# the rotation axis exactly on the host's x/z — same centering Main.tscn now
# applies to the live PlayerModel — so the eye line, the crosshair and the
# head's rotation axis all coincide like the vanilla head rig.
const MODEL_Z_OFFSET := 0.0844
# Marker size in glb units (world size = this * MODEL_SCALE).
const PIVOT_MARKER_GLB := 2.0

var _clone: Node3D = null

# Path debug state: the job being awaited (0 = idle), the two point sets the
# engine returned, and the overlay instance built from the selected set.
var _path_job := 0
var _overlay: MultiMeshInstance3D = null
var _overlay_mat: StandardMaterial3D = null
var _path_nodes: Array = []
var _path_waypoints: Array = []
var _path_truncated := false
var _show_waypoints := false

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
		return
	if event.is_action_pressed(PATH_ACTION) and not event.is_echo():
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			return
		_plan_path()
		return
	if event.is_action_pressed(PATH_TOGGLE_ACTION) and not event.is_echo():
		if _path_nodes.is_empty() and _path_waypoints.is_empty():
			return
		_show_waypoints = not _show_waypoints
		_rebuild_overlay()

# The planner runs on a worker thread, so results are polled each frame and
# matched by job id.
func _process(_delta: float) -> void:
	if _path_job == 0:
		return
	var chunk_manager := _chunk_manager()
	if chunk_manager == null:
		_path_job = 0
		return
	for result in chunk_manager.poll_paths():
		if int(result.get("id", 0)) != _path_job:
			continue
		_path_job = 0
		_path_nodes = result.get("nodes", PackedVector3Array())
		_path_waypoints = result.get("waypoints", PackedVector3Array())
		_path_truncated = bool(result.get("truncated", false))
		_show_waypoints = false
		_rebuild_overlay()
		var detail := ""
		if not bool(result.get("found", false)):
			detail = "  error=\"%s\"" % result.get("error", "")
		# cells vs locks is the in-game read cost: one shard lock per chunk
		# visited, not per block classified.
		print("Path: found=%s truncated=%s timed_out=%s grid=%d waypoints=%d expansions=%d columns=%d cells=%d locks=%d %.2f ms%s"
			% [result.get("found", false), result.get("truncated", false),
			   result.get("time_exhausted", false),
			   _path_nodes.size(), _path_waypoints.size(),
			   int(result.get("expansions", 0)), int(result.get("columns", 0)),
			   int(result.get("cells", 0)), int(result.get("locks", 0)),
			   float(result.get("ms", 0.0)), detail])

func _chunk_manager() -> Node:
	var scene_root := get_tree().current_scene
	if scene_root == null:
		return null
	return scene_root.get_node_or_null("ChunkManager")

func _plan_path() -> void:
	var chunk_manager := _chunk_manager()
	if chunk_manager == null:
		return
	# Second press clears the route.
	if _path_job != 0 or _overlay != null:
		_clear_path()
		return
	if _clone == null:
		print("Path: spawn the dummy first (K)")
		return
	if not chunk_manager.has_method("request_path"):
		print("Path: planner binding not available")
		return
	var player := get_tree().current_scene.get_node_or_null("Player") as Node3D
	if player == null:
		return
	_path_job = int(chunk_manager.request_path(_clone.global_position, player.global_position))
	print("Path: planning %s -> %s (job %d)"
		% [_clone.global_position, player.global_position, _path_job])

func _clear_path() -> void:
	_path_job = 0
	_path_nodes = []
	_path_waypoints = []
	_path_truncated = false
	if _overlay != null:
		_overlay.queue_free()
		_overlay = null
		_overlay_mat = null

# One MultiMeshInstance3D of centred cubes, one per route node.
func _rebuild_overlay() -> void:
	var points: Array = _path_waypoints if _show_waypoints else _path_nodes
	if points.is_empty():
		_clear_path()
		return
	var scene_root := get_tree().current_scene
	if scene_root == null:
		return
	if _overlay == null:
		_overlay = MultiMeshInstance3D.new()
		_overlay.name = "PathOverlay"
		var multimesh := MultiMesh.new()
		multimesh.transform_format = MultiMesh.TRANSFORM_3D
		var cube := BoxMesh.new()
		cube.size = Vector3.ONE * 1.02
		_overlay_mat = StandardMaterial3D.new()
		_overlay_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_overlay_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_overlay_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
		cube.material = _overlay_mat
		multimesh.mesh = cube
		_overlay.multimesh = multimesh
		scene_root.add_child(_overlay)
	if _path_truncated:
		_overlay_mat.albedo_color = PATH_COLOR_PARTIAL
	else:
		_overlay_mat.albedo_color = PATH_COLOR_WAYPOINTS if _show_waypoints else PATH_COLOR_RAW
	var multimesh: MultiMesh = _overlay.multimesh
	multimesh.instance_count = points.size()
	for i in points.size():
		# Nodes are block cells; the cube is centred on the cell.
		var cell: Vector3 = points[i]
		multimesh.set_instance_transform(i, Transform3D(Basis(), cell + Vector3(0.5, 0.5, 0.5)))
	print("Path overlay: %d %s cubes%s"
		% [points.size(), "waypoint" if _show_waypoints else "grid",
		   " (partial — budget ran out)" if _path_truncated else ""])


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
	# The dummy is a physics body (dummy.gd: vanilla gravity/drag + knockback), so
	# left-clicking it in game punches it instead of mining the block behind.
	# PlayerController finds it through the pose_clone group.
	host.set_script(DUMMY_SCRIPT)
	host.add_to_group("pose_clone")
	# Position BEFORE add_child: dummy.gd's _ready snapshots this as its tick
	# position, and the interpolation then renders around it every frame.
	host.position = Vector3(bx + 0.5, feet_y, bz + 0.5)
	scene_root.add_child(host)
	_clone = host

	var model := PLAYER_SCENE.instantiate()
	model.set_script(PLAYER_MODEL_SCRIPT)
	model.transform = _clone_model_transform()
	# No AnimationPlayer child on purpose: player_model.gd only loads and plays
	# Idle.anim when an AnimationPlayer exists, so the clone stays frozen.
	# Skip head tracking too so the dummy is completely rigid.
	model.skip_head_look = true
	host.add_child(model)

	_add_pivot_markers(model)
	print("Pose clone spawned at %s (top of block %d,%d,%d)" % [host.global_position, bx, by, bz])
	print_pivots(model)

func _despawn() -> void:
	if _clone == null:
		return
	_clear_path()
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
		return Transform3D(src_model.global_transform.basis, Vector3(0, MODEL_Y_OFFSET, MODEL_Z_OFFSET))
	var basis := Basis(Vector3(-MODEL_SCALE, 0, 0), Vector3(0, MODEL_SCALE, 0), Vector3(0, 0, -MODEL_SCALE))
	return Transform3D(basis, Vector3(0, MODEL_Y_OFFSET, MODEL_Z_OFFSET))

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
