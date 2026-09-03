extends Node3D

# First-person viewmodel.
#
# The held item pose is Minecraft 1.8.8's transformFirstPersonItem (base offset,
# 45-degree yaw, 0.4 scale doubled for blocks, plus the exact swing/bob math), so
# punch() reproduces the MC swing. The arm is an explicit shoulder -> grip limb
# aligned to reach the held item, because MC's own arm chain (func_178095_a)
# keeps the shoulder behind the camera and invisible on our field of view.
#
# Everything worth tuning is an export so positions can be nudged in-game/editor.

const PLAYER_MODEL: PackedScene = preload("res://player.glb")
const MODEL_SCALE := 0.05625 # player.glb px -> meters (0.9 / 16)

@export var arm_index := 3 # glb child holding the right arm (raw scene vs import mirror)
@export var arm_side := 1.0 # mirror the arm's placement (x-axis): 1 or -1
@export var arm_from := Vector3(0.30, 0.25, -0.42) # shoulder, eye space
@export var arm_grip := Vector3(0.20, 0.10, -0.60) # hand target
@export var arm_size := 0.72 # arm thickness/length multiplier
@export var arm_roll := 90.0 # roll about the arm axis so it reads as 3D, not a flat sliver
@export var hold_from := Vector3(0.42, 0.10, -0.55) # held item base, eye space

var _player: Node
var _hand_bob: Node3D
var _arm_root: Node3D
var _arm_pivot: Node3D
var _arm: Node3D
var _swing_node: Node3D
var _item_scale_node: Node3D
var _item: MeshInstance3D
var _material: Material
var _cube_mesh: ArrayMesh
var _stick_mesh: BoxMesh
var _block_id := -2
var _prev_block_id := -2
var _equip := 0.0
var _is_swapping := false
var _swing := 0.0          # punch/break swing progress (1 -> 0)
var _swing_place := 0.0    # place swing progress (1 -> 0)
var _swing_strength := 1.0 # 1.0 = full punch reach, 0.75 = weaker place reach
var _item_meshes := {} # Cache for generated item meshes
var _shaped_block_meshes := {} # Cache for shaped block meshes
var _block_defs: Array = []  # Block definitions for shape lookup
# Removed unused variable _last_cam_rot to clear the warning error
var _item_sway_offset: Vector3 = Vector3.ZERO
var _mouse_delta: Vector2 = Vector2.ZERO
var _block_shapes: Dictionary = {}  # shape_name -> shape data

# Walk bobbing (vanilla bobView): _walk_dist accumulates distance walked,
# _bob is the amplitude envelope that ramps up with movement, _last_pos tracks
# the previous frame's player position to measure horizontal speed.
var _walk_dist := 0.0
var _bob := 0.0
var _last_pos := Vector3.ZERO
var _has_last_pos := false
var _bob_offset := Vector3.ZERO
var _bob_rotation := Vector3.ZERO

# Real-time adjustment HUD
var _hud_panel: Control
var _hud_visible := false
var _adjustment_mode := "ARM" # "ARM", "BLOCK", or "ITEM"
var _rotation_x: float = 5.0
var _rotation_y: float = -13.0
var _rotation_z: float = 5.0
var _arm_scale: float = 1.0
var _arm_position_x: float = 0.67
var _arm_position_y: float = -0.01
var _arm_position_z: float = -0.75

# Block model adjustment
var _block_rotation_x: float = 0.0
var _block_rotation_y: float = -7.0
var _block_rotation_z: float = 0.0
var _block_scale: float = 0.83
var _block_position_x: float = 0.15
var _block_position_y: float = -0.36
var _block_position_z: float = 0.23

# Item model adjustment
var _item_rotation_x: float = 0.0
var _item_rotation_y: float = -692.0
var _item_rotation_z: float = 422.0
var _item_scale: float = 1.17
var _item_position_x: float = 0.0
var _item_position_y: float = 0.09
var _item_position_z: float = 0.46

# Broadcast swing progress/angle (computed in _update_swing_hooks) so
# _update_item_transform can swing the held item toward its own peak pose.
var _swing_s: float = 0.0
var _swing_angle: float = 0.0

# Minecraft 1.8.8 decompiled ItemRenderer.java + ModelPlayer.java, empty-hand
# arm path:
#   func_178095_a() rotation calls       (GL post-multiply, vertex-first):
#       rotate(45, Y)  ->  RotY45
#       rotate(120, Z) ->  RotZ120
#       rotate(200, X) ->  RotX200
#       rotate(-135, Y)->  RotY-135
#   -> R = RotY45 * RotZ120 * RotX200 * RotY-135
#   renderRightArm() (ModelPlayer.func_178725_a) renders bipedRightArm with
#   setRotationAngles all-zero -> the arm hangs straight down (rotation ~0).
# So the whole pose is R. On a down-hanging right arm R sends the axis to
# (-0.334,-0.470,+0.817) = down / left / strongly TOWARD THE VIEWER - the
# foreshortened, angled Minecraft held-arm (NOT straight down; earlier attempts
# only applied a 45-yaw, which kept it hanging straight down - the bug).
#
# We apply R as a Basis matrix, not Euler (Godot's Euler-order is ambiguous and
# an off-by-one made the arm spin the wrong way). Columns = image of world axes
# under R (Basis(xcol,ycol,zcol) * v == R * v). MC and Godot are both
# right-handed, so no mirroring is needed.
const MC_ARM_BASIS := Basis(
	Vector3(-0.3679, -0.7333, -0.5717),
	Vector3(0.3336,  0.4698, -0.8173),
	Vector3(0.8679, -0.4915,  0.0717)
)

# Shoulder pivot, eye-space metres (camera child: +x=right, +y=up, +z=toward
# viewer). The right arm's shoulder sits to the right of center; the arm then
# extends down-left-toward the camera (per the matrix above), reading as the
# diagonal MC held-arm in the lower-right of the screen.
const MC_SHOULDER := Vector3(0.67, -0.01, -0.75)

# Peak-punch pose, manually verified with the F12 HUD. The animation sweeps the
# hand along a semicircular arc from the resting pose to this peak pose.
const PEAK_ROT := Vector3(0.0, 58.0, -10.0)
const PEAK_POS := Vector3(0.19, 0.26, -0.75)

# Peak-punch pose for the HELD ITEM, manually calculated with the F12 HUD.
# Kept as the RAW HUD readouts (not normalized) because they're close to the
# item's resting rotations (_item_rotation_* = 0, -692, 422), so interpolating
# to these raw values gives a small, natural swing. It hangs from a different
# base than the hand, so it has its own end point on the same curve.
const PEAK_ROT_ITEM := Vector3(-20.0, -660.0, 372.0)
const PEAK_POS_ITEM := Vector3(-0.66, -0.03, -0.16)

# Peak-punch pose for the HELD BLOCK, read from the F12 BLOCK-mode HUD
# (_block_rotation_* / _block_position_*, rest = 0, -7, 0 / 0.15, -0.36, 0.23).
# Fill in with the values found via the F12 HUD, same as PEAK_ROT_ITEM/PEAK_POS_ITEM.
const PEAK_ROT_BLOCK := Vector3(-70.0, 30.0, 45.0)
const PEAK_POS_BLOCK := Vector3(-0.65, -0.02, -0.16)

func _ready() -> void:
	_player = get_node_or_null("/root/Main/Player")
	if _player != null and _player.has_signal("block_placed"):
		_player.block_placed.connect(place)
	
	_load_block_definitions()
	_load_block_shapes()

	# Shared container that gets the swing bob and equip nudge.
	_hand_bob = Node3D.new()
	add_child(_hand_bob)

	# --- Arm: a limb aligned from shoulder to grip. ---
	var arm_root := Node3D.new()
	_hand_bob.add_child(arm_root)
	arm_root.name = "ArmRoot"
	arm_root.position = Vector3(_arm_position_x, _arm_position_y, _arm_position_z)
	_arm_root = arm_root

	var mdl := PLAYER_MODEL.instantiate()
	var arm_node := mdl.get_child(arm_index) as Node3D
	mdl.remove_child(arm_node)
	mdl.free()
	_arm_pivot = Node3D.new()
	arm_root.add_child(_arm_pivot)
	_arm_pivot.add_child(arm_node)
	# Scale only the mesh, not the node position, to preserve pivot alignment
	arm_node.position = Vector3(0, -12.0, 0) * MODEL_SCALE
	arm_node.scale = Vector3.ONE * MODEL_SCALE
	arm_node.rotation_degrees = Vector3.ZERO
	# Apply scale to mesh instances instead of the node
	for mesh_instance in arm_node.find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi != null:
			mi.scale = Vector3.ONE * _arm_scale
	_arm_pivot.basis = MC_ARM_BASIS

	_flip_arm_mesh_uvs(arm_node)
	_apply_skin(arm_node)
	_arm = arm_node
	_update_arm_rotation()

	# --- Held item (transformFirstPersonItem). ---
	# The punch/swing must pivot about the SHOULDER (matching the arm's pivot),
	# not about the hand -- otherwise the held block spins about its own center
	# instead of arcing the way a real arm held at the shoulder would.
	var arm_shoulder := Vector3(_arm_position_x, _arm_position_y, _arm_position_z)

	var swing_pivot := Node3D.new()
	_hand_bob.add_child(swing_pivot)
	swing_pivot.name = "SwingPivot"
	swing_pivot.position = arm_shoulder
	_swing_node = swing_pivot

	var item_root := Node3D.new()
	swing_pivot.add_child(item_root)
	item_root.name = "ItemRoot"
	item_root.position = Vector3(
		hold_from.x - arm_shoulder.x,
		hold_from.y - arm_shoulder.y - 0.15,
		hold_from.z - arm_shoulder.z
	)

	var var45 := Node3D.new()
	item_root.add_child(var45)
	var45.rotation_degrees.y = 45.0

	var scale04 := Node3D.new()
	var45.add_child(scale04)
	scale04.scale = Vector3.ONE * 0.4 

	_item_scale_node = Node3D.new()
	scale04.add_child(_item_scale_node)

# Fixed Material Setup with Alpha Scissor and proper depth testing
	var std_mat := StandardMaterial3D.new()
	std_mat.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
	std_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	std_mat.cull_mode = BaseMaterial3D.CULL_BACK
	std_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	std_mat.alpha_scissor_threshold = 0.5
	std_mat.no_depth_test = false  # Enable depth testing to prevent see-through
	std_mat.render_priority = 5
	std_mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	std_mat.roughness = 1.0
	std_mat.metallic = 0.0
	_material = std_mat

	_cube_mesh = _build_cube_mesh()
	_stick_mesh = null 

	_item = MeshInstance3D.new()
	_item.material_override = _material
	_item.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_item_scale_node.add_child(_item)
	
	_create_hud()

func _align_to_grip(root: Node3D) -> Basis:
	# Point the glb arm's +Y long axis from shoulder toward grip. The roll is
	# anchored to the camera's forward, not world up: the arm is kept in the view
	# plane, so its flat face points at the viewer like MC's first-person arm
	# (anchoring to world up turns the flat face sideways once the limb reaches
	# forward).
	var shoulder := arm_from * Vector3(arm_side, 1.0, 1.0)
	var grip := arm_grip * Vector3(arm_side, 1.0, 1.0)
	var dir := (grip - shoulder)
	if dir.length() < 1e-4:
		dir = Vector3.DOWN
	dir = dir.normalized()
	var ax := dir.cross(Vector3.FORWARD)
	if ax.length() < 1e-4:
		ax = Vector3.RIGHT
	ax = ax.normalized()
	var b := Basis(ax, dir, ax.cross(dir))
	if arm_roll != 0.0:
		b = b.rotated(dir, deg_to_rad(arm_roll))
	root.transform.basis = b
	return b

func _process(delta: float) -> void:
	if _player == null:
		return
	visible = not _player.get_third_person() and not _player.is_dead()
	if not visible:
		return
	if _swing > 0.0:
		_swing = maxf(_swing - delta / 0.225, 0.0)
	# Loop the punch while the player holds to break a block. Checked even when
	# the swing has already settled at 0, so holding LMB on empty air first (which
	# lets it expire) then hovering over a block still starts the loop.
	if _swing <= 0.0 and _is_breaking():
		_swing = 1.0
	if _swing_place > 0.0:
		_swing_place = maxf(_swing_place - delta / 0.225, 0.0)
	# The active swing is whichever timer is further along; the place stroke
	# reaches a weaker endpoint (0.75x) than a full punch.
	if _swing_place > _swing:
		_swing_strength = 0.75
	else:
		_swing_strength = 1.0
	
	# Equip animation: both normal equip and swap take 0.25s
	var equip_speed = 0.25
	_equip = move_toward(_equip, 1.0, delta / equip_speed)
	
	# --- FIXED ARM VISIBILITY LOGIC ---
	if _arm != null:
		var active_display_id = _block_id
		if _is_swapping:
			active_display_id = _prev_block_id if (_equip < 0.5) else _block_id
		_arm.visible = (active_display_id <= 0)
	# ----------------------------------
	
# --- SCALED MINECRAFT-STYLE VIEWMODEL LAG (~1.5x Stronger) ---
	# Sway math (clamp + exponential lerp) lives in C++ (ViewmodelPose.step_sway).
	_item_sway_offset = ViewmodelPose.step_sway(delta, _mouse_delta, _item_sway_offset)

	# Clear out mouse delta frame-by-frame
	_mouse_delta = Vector2.ZERO

	# Apply sway rotation plus the walk-bob rotation to the hand container
	if _hand_bob != null:
		_hand_bob.rotation = _item_sway_offset + _bob_rotation * PI / 180.0
	# -------------------------------------------------------

	# --- Walk bobbing (vanilla bobView) ---
	# Track horizontal distance walked and a bob amplitude that ramps up with
	# movement and dies down when idle, then offset the hand container by a
	# single-frequency sin/cos bob.
	# The bob math (distance accumulation, envelope lerp, sin/cos offsets) lives
	# in C++ (ViewmodelPose.step_walk_bob).
	var p: Vector3 = _player.global_position
	var bob_data := ViewmodelPose.step_walk_bob(delta, p, _last_pos, _has_last_pos, _player.is_on_floor(), _walk_dist, _bob)
	_walk_dist = bob_data["walk_dist"]
	_bob = bob_data["bob"]
	_bob_offset = bob_data["bob_offset"]
	_bob_rotation = bob_data["bob_rotation"]
	_last_pos = bob_data["last_pos"]
	_has_last_pos = bob_data["has_last_pos"]
	# ----------------------------------------------

	_update_swing_hooks()
	_refresh_held_item()

# Kick a punch/swing (roadmap: punch animation). Progress 1..0 over 0.25s.
func punch() -> void:
	_swing = 1.0

# Kick a place motion: same swing as the punch but with a weaker (0.5x) endpoint.
func place() -> void:
	_swing_place = 1.0

# True while the player is actively holding to break a block (LMB held on a
# breakable target), so the punch animation loops instead of playing once.
func _is_breaking() -> bool:
	if _player == null:
		return false
	var state = _player.get_break_state()
	return state is Dictionary and state.get("active", false)

func _update_swing_hooks() -> void:
	# The active swing is the closer-to-rest (larger) punch/place timer, so a
	# quick place right after a punch doesn't discard the punch's remaining motion.
	# All the swing/equip math (depth curve, arm rotation + shoulder arc, equip
	# offset, swing_s/angle broadcast) lives in C++ (ViewmodelPose.compute_swing_pose).
	var pose := ViewmodelPose.compute_swing_pose(
		_swing, _swing_place, _swing_strength, _equip, _is_swapping,
		_rotation_x, _rotation_y, _rotation_z,
		_arm_position_x, _arm_position_y, _arm_position_z,
		_bob_offset, PEAK_ROT, PEAK_POS
	)

	# Equip-only vertical bob (shoulder position handles the swing motion), plus
	# the walk bob offset.
	_hand_bob.position = pose["hand_bob_pos"]

	# Apply to arm (rotation + position): MC arm basis + HUD rotation delta.
	_update_arm_animation(pose["arm_rot"], pose["arm_pos"])

	# Broadcast the swing state so _update_item_transform (which runs every frame
	# and owns _item_scale_node in F12 ITEM space) can drive the held item toward
	# its own tuned peak pose exactly.
	_swing_s = pose["swing_s"]
	_swing_angle = pose["swing_angle"]

	# The item swing is applied on _item_scale_node inside _update_item_transform;
	# leave the _swing_node pivot at its resting shoulder so it doesn't stack.
	if _swing_node != null:
		_swing_node.position = pose["swing_node_pos"]
		_swing_node.rotation_degrees = Vector3.ZERO

func _update_arm_animation(current_rot: Vector3, current_pos: Vector3) -> void:
	if _arm_pivot == null:
		return

	# Build pose: MC arm basis + interpolated HUD rotation values
	_arm_pivot.basis = MC_ARM_BASIS.rotated(Vector3.RIGHT, deg_to_rad(-current_rot.x))
	_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.UP, deg_to_rad(current_rot.y))
	_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.BACK, deg_to_rad(current_rot.z))

	# Shoulder position follows the animation
	if _arm_root != null:
		_arm_root.position = current_pos

func _refresh_held_item() -> void:
	if _player == null:
		return
	var slot := int(_player.get_selected_hotbar_slot())
	var id := int(_player.get_hotbar_slot_block_id(slot))
	
	# Detect item change
	if id != _block_id:
		_prev_block_id = _block_id
		_block_id = id
		_equip = 0.0 # start equip animation
		_is_swapping = (_prev_block_id != -2) # only swap if we had a previous item
	
	# Reset swap flag when animation completes
	if _equip >= 1.0:
		_is_swapping = false
	
	# Determine which item to show based on equip progress
	var current_display_id: int = id
	if _is_swapping and _equip < 0.5:
		current_display_id = _prev_block_id
	
	if current_display_id <= 0:
		_item.visible = false
		return
	
	var tex := BlockTextures.get_texture(current_display_id)
	if tex == null:
		_item.visible = false
		return
	
	var std_mat := _material as StandardMaterial3D
	if std_mat != null:
		std_mat.albedo_texture = tex
	
	_item.visible = true
	if BlockTextures.is_item(current_display_id):
		# Generate or get cached item mesh
		if not _item_meshes.has(current_display_id):
			var item_tex := BlockTextures.get_texture(current_display_id)
			if item_tex != null:
				_item_meshes[current_display_id] = _generate_item_mesh(item_tex)
		
		if _item_meshes.has(current_display_id):
			_item.mesh = _item_meshes[current_display_id]
		else:
			_item.mesh = _cube_mesh # Fallback
		
		_item_scale_node.scale = Vector3.ONE * _item_scale
		# Apply item adjustments
		_update_item_transform()
	else:
		# Check if block has a custom shape
		var block_def = _block_defs[current_display_id] if current_display_id >= 0 and current_display_id < _block_defs.size() else {}
		var shape = block_def.get("shape", "") if block_def else ""
		
		if not shape.is_empty():
			# Remap shape variant to face towards camera for viewmodel
			var viewmodel_shape = _remap_shape_for_viewmodel(shape)
			
			# Generate or get cached shaped block mesh
			if not _shaped_block_meshes.has(viewmodel_shape):
				var shape_mesh = _build_shaped_block_mesh(viewmodel_shape)
				if shape_mesh != null:
					_shaped_block_meshes[viewmodel_shape] = shape_mesh
			
			if _shaped_block_meshes.has(viewmodel_shape):
				_item.mesh = _shaped_block_meshes[viewmodel_shape]
				_item_scale_node.scale = Vector3.ONE * _block_scale
				_item.rotation = Vector3.ZERO
				_update_block_transform()
			else:
				_item.mesh = _cube_mesh
				_item_scale_node.scale = Vector3.ONE * _block_scale
				_item.rotation = Vector3.ZERO
				_update_block_transform()
		else:
			_item.mesh = _cube_mesh
			_item_scale_node.scale = Vector3.ONE * _block_scale # 0.4 total scale from parent
			_item.rotation = Vector3.ZERO
			# Apply block adjustments
			_update_block_transform()

func _flip_arm_mesh_uvs(arm: Node3D) -> void:
	var list: Array[Node3D] = [arm]
	for child in arm.find_children("", "MeshInstance3D", true, false):
		list.append(child)
	for n in list:
		var mi := n as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var old_mesh := mi.mesh
		var new_mesh := ArrayMesh.new()
		for s in range(old_mesh.get_surface_count()):
			var arrays := old_mesh.surface_get_arrays(s)
			var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV].duplicate()
			var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
			var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			
			var top_indices: Array[int] = []
			var bot_indices: Array[int] = []
			
			for i in range(uvs.size()):
				var norm := normals[i]
				if norm.y > 0.5:
					top_indices.append(i)
				elif norm.y < -0.5:
					bot_indices.append(i)
				else:
					# Side faces: vertically invert V along sleeve (span 20..32 px)
					uvs[i].y = (52.0 / 64.0) - uvs[i].y
			
			# Swap top face and bottom face UVs by matching XZ positions
			for ti in top_indices:
				var tp := positions[ti]
				for bi in bot_indices:
					var bp := positions[bi]
					if is_equal_approx(tp.x, bp.x) and is_equal_approx(tp.z, bp.z):
						var temp := uvs[ti]
						uvs[ti] = uvs[bi]
						uvs[bi] = temp
						break
			
			arrays[Mesh.ARRAY_TEX_UV] = uvs
			new_mesh.add_surface_from_arrays(old_mesh.surface_get_primitive_type(s), arrays)
		mi.mesh = new_mesh

func _apply_skin(arm: Node3D) -> void:
	var mgr := get_node_or_null("/root/SkinManager")
	var tex: Texture2D = mgr.get_texture() if mgr != null else null
	var list: Array[Node3D] = [arm]
	for child in arm.find_children("", "MeshInstance3D", true, false):
		list.append(child)
	for n in list:
		var mi := n as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		
		# Use StandardMaterial3D with per-pixel lighting
		var std_mat := StandardMaterial3D.new()
		std_mat.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
		std_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		std_mat.no_depth_test = true
		std_mat.render_priority = 5
		std_mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
		std_mat.roughness = 1.0
		std_mat.metallic = 0.0
		if tex != null:
			std_mat.albedo_texture = tex
		
		for s in range(mi.mesh.get_surface_count()):
			mi.set_surface_override_material(s, std_mat)

func _build_cube_mesh() -> ArrayMesh:
	# Same layout as the Block Maker / block-break overlay cube: texture-top =
	# world-top on every face. Geometry lives in C++ (ViewmodelMeshes) so the
	# three copied builders can't drift.
	var data := ViewmodelMeshes.build_cube_mesh()
	if data.is_empty():
		return ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func _create_hud() -> void:
	_hud_panel = Control.new()
	_hud_panel.name = "ArmAdjustmentHUD"
	_hud_panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_hud_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	_hud_panel.visible = false
	add_child(_hud_panel)
	
	var label := Label.new()
	label.text = "Arm/Block/Item Adjustments (F12 to toggle, B to cycle mode)"
	label.position = Vector2(10, 10)
	label.add_theme_font_size_override("font_size", 16)
	_hud_panel.add_child(label)
	
	var instructions := Label.new()
	instructions.text = "R/F: X rot | A/D: Y rot | W/S: Z rot | T/G: Scale | I/K/J/L/U/O: Position | B: Cycle Arm/Block/Item"
	instructions.position = Vector2(10, 30)
	instructions.add_theme_font_size_override("font_size", 12)
	_hud_panel.add_child(instructions)
	
	var offset_y := 60
	var mode_label := Label.new()
	mode_label.text = "Mode: ARM"
	mode_label.position = Vector2(10, offset_y)
	mode_label.name = "ModeLabel"
	_hud_panel.add_child(mode_label)
	offset_y += 25
	
	var axes = ["X Rot", "Y Rot", "Z Rot", "Scale", "Pos X", "Pos Y", "Pos Z"]
	var values = [_rotation_x, _rotation_y, _rotation_z, _arm_scale, _arm_position_x, _arm_position_y, _arm_position_z]
	
	for i in range(7):
		var axis_label := Label.new()
		axis_label.text = axes[i] + " : " + str(values[i])
		axis_label.position = Vector2(10, offset_y)
		axis_label.name = "AxisLabel" + str(i)
		_hud_panel.add_child(axis_label)
		offset_y += 25

func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		if event.keycode == KEY_F12 and event.pressed:
			_hud_visible = !_hud_visible
			_hud_panel.visible = _hud_visible
		elif event.keycode == KEY_B and event.pressed and _hud_visible:
			# Cycle through ARM -> BLOCK -> ITEM -> ARM
			if _adjustment_mode == "ARM":
				_adjustment_mode = "BLOCK"
			elif _adjustment_mode == "BLOCK":
				_adjustment_mode = "ITEM"
			else:
				_adjustment_mode = "ARM"
			_update_hud_labels()
		elif _hud_visible:
			var changed := false
			
			if _adjustment_mode == "ARM":
				if event.keycode == KEY_R and event.pressed:
					_rotation_x -= 5.0
					changed = true
				elif event.keycode == KEY_F and event.pressed:
					_rotation_x += 5.0
					changed = true
				elif event.keycode == KEY_A and event.pressed:
					_rotation_y -= 1.0
					changed = true
				elif event.keycode == KEY_D and event.pressed:
					_rotation_y += 1.0
					changed = true
				elif event.keycode == KEY_W and event.pressed:
					_rotation_z -= 5.0
					changed = true
				elif event.keycode == KEY_S and event.pressed:
					_rotation_z += 5.0
					changed = true
				elif event.keycode == KEY_T and event.pressed:
					_arm_scale -= 0.01
					changed = true
				elif event.keycode == KEY_G and event.pressed:
					_arm_scale += 0.01
					changed = true
				elif event.keycode == KEY_I and event.pressed:
					_arm_position_x -= 0.01
					changed = true
				elif event.keycode == KEY_K and event.pressed:
					_arm_position_x += 0.01
					changed = true
				elif event.keycode == KEY_J and event.pressed:
					_arm_position_y -= 0.01
					changed = true
				elif event.keycode == KEY_L and event.pressed:
					_arm_position_y += 0.01
					changed = true
				elif event.keycode == KEY_U and event.pressed:
					_arm_position_z -= 0.01
					changed = true
				elif event.keycode == KEY_O and event.pressed:
					_arm_position_z += 0.01
					changed = true
			elif _adjustment_mode == "BLOCK":
				if event.keycode == KEY_R and event.pressed:
					_block_rotation_x -= 5.0
					changed = true
				elif event.keycode == KEY_F and event.pressed:
					_block_rotation_x += 5.0
					changed = true
				elif event.keycode == KEY_A and event.pressed:
					_block_rotation_y -= 1.0
					changed = true
				elif event.keycode == KEY_D and event.pressed:
					_block_rotation_y += 1.0
					changed = true
				elif event.keycode == KEY_W and event.pressed:
					_block_rotation_z -= 5.0
					changed = true
				elif event.keycode == KEY_S and event.pressed:
					_block_rotation_z += 5.0
					changed = true
				elif event.keycode == KEY_T and event.pressed:
					_block_scale -= 0.01
					changed = true
				elif event.keycode == KEY_G and event.pressed:
					_block_scale += 0.01
					changed = true
				elif event.keycode == KEY_I and event.pressed:
					_block_position_x -= 0.01
					changed = true
				elif event.keycode == KEY_K and event.pressed:
					_block_position_x += 0.01
					changed = true
				elif event.keycode == KEY_J and event.pressed:
					_block_position_y -= 0.01
					changed = true
				elif event.keycode == KEY_L and event.pressed:
					_block_position_y += 0.01
					changed = true
				elif event.keycode == KEY_U and event.pressed:
					_block_position_z -= 0.01
					changed = true
				elif event.keycode == KEY_O and event.pressed:
					_block_position_z += 0.01
					changed = true
			else: # ITEM mode
				if event.keycode == KEY_R and event.pressed:
					_item_rotation_x -= 5.0
					changed = true
				elif event.keycode == KEY_F and event.pressed:
					_item_rotation_x += 5.0
					changed = true
				elif event.keycode == KEY_A and event.pressed:
					_item_rotation_y -= 1.0
					changed = true
				elif event.keycode == KEY_D and event.pressed:
					_item_rotation_y += 1.0
					changed = true
				elif event.keycode == KEY_W and event.pressed:
					_item_rotation_z -= 5.0
					changed = true
				elif event.keycode == KEY_S and event.pressed:
					_item_rotation_z += 5.0
					changed = true
				elif event.keycode == KEY_T and event.pressed:
					_item_scale -= 0.01
					changed = true
				elif event.keycode == KEY_G and event.pressed:
					_item_scale += 0.01
					changed = true
				elif event.keycode == KEY_I and event.pressed:
					_item_position_x -= 0.01
					changed = true
				elif event.keycode == KEY_K and event.pressed:
					_item_position_x += 0.01
					changed = true
				elif event.keycode == KEY_J and event.pressed:
					_item_position_y -= 0.01
					changed = true
				elif event.keycode == KEY_L and event.pressed:
					_item_position_y += 0.01
					changed = true
				elif event.keycode == KEY_U and event.pressed:
					_item_position_z -= 0.01
					changed = true
				elif event.keycode == KEY_O and event.pressed:
					_item_position_z += 0.01
					changed = true
			
			if changed:
				if _adjustment_mode == "ARM":
					_update_arm_rotation()
				elif _adjustment_mode == "BLOCK":
					_update_block_transform()
				else:
					_update_item_transform()
				_update_hud_labels()
				
	# Capture raw mouse motion for the viewmodel sway effect
	if event is InputEventMouseMotion:
		_mouse_delta = event.relative
	
	# Trigger/spam the punch immediately on every left-click press, but never
	# while a UI is open (mouse released = inventory/chat/settings/crafting) --
	# clicking slots must not make the hand punch.
	if (
		event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT and event.pressed
		and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED
	):
		punch()

func _update_arm_rotation() -> void:
	if _arm_pivot != null:
		# The source pose is the exact MC_ARM_BASIS matrix (avoids Godot's
		# Euler-order ambiguity). The F12 HUD rotation keys apply a small euler
		# delta on top.
		_arm_pivot.basis = MC_ARM_BASIS.rotated(Vector3.RIGHT, deg_to_rad(-_rotation_x))
		_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.UP, deg_to_rad(_rotation_y))
		_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.BACK, deg_to_rad(_rotation_z))
	if _arm != null:
		_arm.position = Vector3(0, -12.0, 0) * MODEL_SCALE
		_arm.scale = Vector3.ONE * MODEL_SCALE
		# Update mesh instance scales
		for mesh_instance in _arm.find_children("", "MeshInstance3D", true, false):
			var mi := mesh_instance as MeshInstance3D
			if mi != null:
				mi.scale = Vector3.ONE * _arm_scale
	if _arm_root != null:
		_arm_root.position = Vector3(_arm_position_x, _arm_position_y, _arm_position_z)

func _update_hud_labels() -> void:
	var labels = _hud_panel.find_children("", "Label", false, false)
	
	# Update mode label
	if labels.size() >= 1:
		labels[0].text = "Mode: " + _adjustment_mode
	
	if labels.size() >= 8:
		if _adjustment_mode == "ARM":
			labels[1].text = "X Rot : " + str(_rotation_x)
			labels[2].text = "Y Rot : " + str(_rotation_y)
			labels[3].text = "Z Rot : " + str(_rotation_z)
			labels[4].text = "Scale : " + str(_arm_scale)
			labels[5].text = "Pos X : " + str(_arm_position_x)
			labels[6].text = "Pos Y : " + str(_arm_position_y)
			labels[7].text = "Pos Z : " + str(_arm_position_z)
		elif _adjustment_mode == "BLOCK":
			labels[1].text = "X Rot : " + str(_block_rotation_x)
			labels[2].text = "Y Rot : " + str(_block_rotation_y)
			labels[3].text = "Z Rot : " + str(_block_rotation_z)
			labels[4].text = "Scale : " + str(_block_scale)
			labels[5].text = "Pos X : " + str(_block_position_x)
			labels[6].text = "Pos Y : " + str(_block_position_y)
			labels[7].text = "Pos Z : " + str(_block_position_z)
		else: # ITEM
			labels[1].text = "X Rot : " + str(_item_rotation_x)
			labels[2].text = "Y Rot : " + str(_item_rotation_y)
			labels[3].text = "Z Rot : " + str(_item_rotation_z)
			labels[4].text = "Scale : " + str(_item_scale)
			labels[5].text = "Pos X : " + str(_item_position_x)
			labels[6].text = "Pos Y : " + str(_item_position_y)
			labels[7].text = "Pos Z : " + str(_item_position_z)

func _update_block_transform() -> void:
	if _item_scale_node != null and _block_id > 0 and not BlockTextures.is_item(_block_id):
		# Mirror the held-item swing: interpolate the block's resting pose toward
		# its tuned peak (F12 BLOCK mode) on the same 's' curve, plus the same
		# two-sided perpendicular arc. The swing interpolation math lives in C++
		# (ViewmodelPose.compute_swing_transform).
		var tf := ViewmodelPose.compute_swing_transform(
			_swing_s, _swing_angle,
			_block_position_x, _block_position_y, _block_position_z,
			_block_rotation_x, _block_rotation_y, _block_rotation_z,
			PEAK_POS_BLOCK, PEAK_ROT_BLOCK
		)

		_item_scale_node.position = tf["position"]
		_item_scale_node.rotation_degrees = tf["rotation_degrees"]
		_item_scale_node.scale = Vector3.ONE * _block_scale

func _update_item_transform() -> void:
	if _item_scale_node != null and _block_id > 0 and BlockTextures.is_item(_block_id):
		# Interpolate the item's resting pose toward its tuned peak on the same
		# 's' curve as the arm, plus a perpendicular arc (sin(angle) two-sided).
		# This is applied in F12 ITEM space, so at s=1 the item reaches
		# PEAK_POS_ITEM / PEAK_ROT_ITEM exactly. The swing math lives in C++
		# (ViewmodelPose.compute_swing_transform).
		var tf := ViewmodelPose.compute_swing_transform(
			_swing_s, _swing_angle,
			_item_position_x, _item_position_y, _item_position_z,
			_item_rotation_x, _item_rotation_y, _item_rotation_z,
			PEAK_POS_ITEM, PEAK_ROT_ITEM
		)

		_item_scale_node.position = tf["position"]
		_item_scale_node.rotation_degrees = tf["rotation_degrees"]
		_item_scale_node.scale = Vector3.ONE * _item_scale

func _generate_item_mesh(texture: Texture2D) -> ArrayMesh:
	# Extruded-sprite geometry (front/back faces + silhouette rims) is built
	# in C++ from the texture's alpha field.
	var data := ViewmodelMeshes.build_item_mesh(texture)
	if data.is_empty():
		return ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

# Load block definitions from block_definitions.json
func _load_block_definitions() -> void:
	var file := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if file == null:
		print("Failed to load block_definitions.json")
		return
	
	var json_text := file.get_as_text()
	file.close()
	
	var json := JSON.new()
	var parse_result := json.parse(json_text)
	if parse_result != OK:
		print("Failed to parse block_definitions.json: " + json.get_error_message())
		return
	
	_block_defs = json.data

# Remap shape variant to face towards camera for viewmodel
func _remap_shape_for_viewmodel(shape: String) -> String:
	var parts = shape.split("/")
	if parts.size() < 2:
		return shape
	
	var shape_type = parts[0]
	var variant = parts[1]
	
	# Map stairs to face east (towards camera)
	if shape_type == "stair":
		return "stair/e"
	
	# Slabs, walls, and poles don't need remapping
	return shape

# Load block shapes from block_shapes.json
func _load_block_shapes() -> void:
	var file := FileAccess.open("res://data/block_shapes.json", FileAccess.READ)
	if file == null:
		print("Failed to load block_shapes.json")
		return
	
	var json_text := file.get_as_text()
	file.close()
	
	var json := JSON.new()
	var parse_result := json.parse(json_text)
	if parse_result != OK:
		print("Failed to parse block_shapes.json: " + json.get_error_message())
		return
	
	_block_shapes = json.data
	print("Loaded " + str(_block_shapes.size()) + " block shapes")

# Build a shaped block mesh for the viewmodel
func _build_shaped_block_mesh(shape_key: String) -> ArrayMesh:
	# Parse shape type and variant (e.g., "slab/bottom" -> type="slab", variant="bottom")
	var parts = shape_key.split("/")
	var shape_type = parts[0]
	var shape_variant = parts[1] if parts.size() > 1 else ""
	
	if not _block_shapes.has(shape_type):
		print("Shape type not found: " + shape_type)
		return null
	
	var shape_data = _block_shapes[shape_type]
	var variant_data
	
	# If shape has no variant, use the shape data directly
	if shape_variant.is_empty():
		variant_data = shape_data
	else:
		if not shape_data.has(shape_variant):
			print("Shape variant not found: " + shape_variant + " for type: " + shape_type)
			return null
		variant_data = shape_data[shape_variant]
	
	var selection_boxes = variant_data.get("selection_boxes", [])
	
	if selection_boxes.is_empty():
		print("No selection boxes for shape: " + shape_type)
		return null
	
	# Payload matches the C++ binding exactly: one PackedFloat32Array per box,
	# six 0..1 floats each (min x/y/z then max x/y/z).
	var boxes := []
	for box in selection_boxes:
		if box.size() < 6:
			continue
		var b := PackedFloat32Array()
		for i in range(6):
			b.append(float(box[i]))
		boxes.append(b)
	
	var data := ViewmodelMeshes.build_shaped_mesh(boxes)
	if data.is_empty():
		return null
	
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]
	
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh
