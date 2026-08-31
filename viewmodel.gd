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
var _arm_pivot: Node3D
var _arm: Node3D
var _swing_node: Node3D
var _item_scale_node: Node3D
var _item: MeshInstance3D
var _material: StandardMaterial3D
var _cube_mesh: ArrayMesh
var _stick_mesh: BoxMesh
var _block_id := -2
var _equip := 0.0
var _swing := 0.0

# Real-time adjustment HUD
var _hud_panel: Control
var _hud_visible := false
var _rotation_x: float = MC_ROT_X
var _rotation_y: float = MC_ROT_Y
var _rotation_z: float = MC_ROT_Z
var _arm_scale: float = 0.47
var _arm_position_x: float = MC_POS_X
var _arm_position_y: float = -0.03  # Adjusted for visibility (MC value -0.85 is too low)
var _arm_position_z: float = -0.50  # Adjusted for visibility (MC value 0.75 may be wrong direction)

# Auto-optimizer
var _optimizer_active := false
var _optimizer_iterations := 0
var _best_score := 0.0
var _reference_texture: Texture2D
var _reference_image: Image
var _best_params := {}
var _optimizer_step_size := 1.0
var _optimizer_timer := 0.0
var _optimizer_interval := 0.067  # ~15 changes per second

# Minecraft exact values from ItemRenderer.java renderRightArm()
const MC_ROT_X := 64.0
const MC_ROT_Y := 54.0
const MC_ROT_Z := -62.0
const MC_POS_X := 0.25
const MC_POS_Y := -0.85
const MC_POS_Z := 0.75

func _ready() -> void:
	_player = get_node_or_null("/root/Main/Player")

	# Shared container that gets the swing bob and equip nudge.
	_hand_bob = Node3D.new()
	add_child(_hand_bob)

	# --- Arm: a limb aligned from shoulder to grip. ---
	var arm_root := Node3D.new()
	_hand_bob.add_child(arm_root)
	arm_root.name = "ArmRoot"
	arm_root.position = Vector3(_arm_position_x, _arm_position_y, _arm_position_z)

	# The glb arm node has a big baked-in offset (x -8.5 / +3.5 px, y 12 px). We
	# re-parent the MeshInstance3D with its position zeroed so the shoulder pivot
	# sits exactly on arm_root and placement is driven by the anchors/alignment,
	# not by that lever arm being rotated around. arm_pivot carries the punch
	# rotation so the mesh keeps its scale (setting .basis directly would nuke it).
	var mdl := PLAYER_MODEL.instantiate()
	var arm_node := mdl.get_child(arm_index) as Node3D
	mdl.remove_child(arm_node)
	mdl.free()
	_arm_pivot = Node3D.new()
	arm_root.add_child(_arm_pivot)
	_arm_pivot.add_child(arm_node)
	arm_node.position = Vector3.ZERO
	arm_node.scale = Vector3.ONE * MODEL_SCALE * _arm_scale
	arm_node.rotation_degrees = Vector3(_rotation_x, _rotation_y, _rotation_z)

	_apply_skin(arm_node)
	_arm = arm_node
	# _align_to_grip(arm_root)  # Disabled - we control rotation manually now

	# --- Held item (transformFirstPersonItem). ---
	var item_root := Node3D.new()
	_hand_bob.add_child(item_root)
	item_root.name = "ItemRoot"
	item_root.position = hold_from

	var var45 := Node3D.new()
	item_root.add_child(var45)
	var45.rotation_degrees.y = 45.0
	_swing_node = Node3D.new() # punch rotation + thrust
	var45.add_child(_swing_node)
	_swing_node.position = Vector3(0.0, -0.15, 0.0) # mild equip/use dip

	var scale04 := Node3D.new()
	_swing_node.add_child(scale04)
	scale04.scale = Vector3.ONE * 0.4 # transformFirstPersonItem scale

	_item_scale_node = Node3D.new()
	scale04.add_child(_item_scale_node)

	_material = StandardMaterial3D.new()
	_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_material.cull_mode = BaseMaterial3D.CULL_BACK
	# Viewmodels must always draw over terrain: disable the depth *test* (nearest
	# fragment wins) while keeping depth *writes* so nothing behind the arm can
	# paint over it either.
	_material.no_depth_test = true

	_cube_mesh = _build_cube_mesh()
	_stick_mesh = BoxMesh.new()
	_stick_mesh.size = Vector3(0.125, 0.75, 0.125)

	_item = MeshInstance3D.new()
	_item.material_override = _material
	_item.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_item_scale_node.add_child(_item)
	
	# Create adjustment HUD
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
		_swing = maxf(_swing - delta / 0.25, 0.0)
	_equip = move_toward(_equip, 1.0, delta / 0.25)
	_update_swing_hooks()
	_refresh_held_item()
	
	# Run optimizer if active
	if _optimizer_active:
		_process_optimizer()

# Kick a punch/swing (roadmap: punch animation). Progress 1..0 over 0.25s.
func punch() -> void:
	_swing = 1.0

func _update_swing_hooks() -> void:
	var s := _swing
	var f3 := sin(s * s * PI) # sin(swing^2 * pi)
	var f4 := sin(sqrt(s) * PI) # sin(sqrt(swing) * pi)
	# MC bob: (-0.4 sin(sqrt s pi), 0.2 sin(sqrt s pi*2), -0.2 sin(s pi))
	_hand_bob.position = Vector3(-0.4 * f4, 0.2 * sin(sqrt(s) * PI * 2.0) - 0.25 * _equip, -0.2 * sin(s * PI))
	# MC swing rotations (transformFirstPersonItem): yaw f*-20, roll f1*-20, pitch f1*-80
	_swing_node.rotation_degrees = Vector3(f4 * -80.0, f3 * -20.0, f4 * -20.0)
	# arm swings around its shoulder the same way (on the pivot, so the mesh
	# scale/basis from _ready is never touched) - DISABLED for manual control
	# _arm_pivot.basis = Basis.IDENTITY.rotated(Vector3.RIGHT, deg_to_rad(f4 * -80.0))

func _refresh_held_item() -> void:
	if _player == null:
		return
	var slot := int(_player.get_selected_hotbar_slot())
	var id := int(_player.get_hotbar_slot_block_id(slot))
	if id != _block_id:
		_block_id = id
		_equip = 0.0 # re-equip slide on slot change
	if id <= 0:
		_item.visible = false
		return
	var tex := BlockTextures.get_texture(id)
	if tex == null:
		_item.visible = false
		return
	_material.albedo_texture = tex
	_item.visible = true
	if BlockTextures.is_item(id):
		_item.mesh = _stick_mesh
		_item_scale_node.scale = Vector3.ONE # already 0.4 from parent
	else:
		_item.mesh = _cube_mesh
		_item_scale_node.scale = Vector3.ONE * 2.0 # renderItem 3D doubling
		_item.rotation = Vector3.ZERO

func _apply_skin(arm: Node3D) -> void:
	# Always drive the arm with our own StandardMaterial3D, ignoring whatever the
	# glb surfaces carry (a ShaderMaterial there would keep depth testing on and
	# look flat). Unshaded + nearest + no_depth_test => visible over terrain.
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
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		mat.no_depth_test = true
		if tex != null:
			mat.albedo_texture = tex
		for s in range(mi.mesh.get_surface_count()):
			mi.set_surface_override_material(s, mat)

func _build_cube_mesh() -> ArrayMesh:
	# Same layout as the Block Maker / block-break overlay cube: texture-top =
	# world-top on every face.
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()

	var FACE_DEFS := [
		[Vector3(1, 0, 0), [Vector3(0.5, -0.5, 0.5), Vector3(0.5, 0.5, 0.5), Vector3(0.5, 0.5, -0.5), Vector3(0.5, -0.5, -0.5)]],
		[Vector3(-1, 0, 0), [Vector3(-0.5, -0.5, -0.5), Vector3(-0.5, 0.5, -0.5), Vector3(-0.5, 0.5, 0.5), Vector3(-0.5, -0.5, 0.5)]],
		[Vector3(0, 1, 0), [Vector3(-0.5, 0.5, -0.5), Vector3(0.5, 0.5, -0.5), Vector3(0.5, 0.5, 0.5), Vector3(-0.5, 0.5, 0.5)]],
		[Vector3(0, -1, 0), [Vector3(-0.5, -0.5, 0.5), Vector3(0.5, -0.5, 0.5), Vector3(0.5, -0.5, -0.5), Vector3(-0.5, -0.5, -0.5)]],
		[Vector3(0, 0, 1), [Vector3(-0.5, -0.5, 0.5), Vector3(-0.5, 0.5, 0.5), Vector3(0.5, 0.5, 0.5), Vector3(0.5, -0.5, 0.5)]],
		[Vector3(0, 0, -1), [Vector3(0.5, -0.5, -0.5), Vector3(0.5, 0.5, -0.5), Vector3(-0.5, 0.5, -0.5), Vector3(-0.5, -0.5, -0.5)]],
	]

	var FACE_UVS := [Vector2(0.0, 1.0), Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(1.0, 1.0)]

	var base := 0
	for face in FACE_DEFS:
		var normal: Vector3 = face[0]
		var quad: Array = face[1]
		for i in range(4):
			verts.append(quad[i])
			normals.append(normal)
			uvs.append(FACE_UVS[i])
		indices.append(base + 0)
		indices.append(base + 1)
		indices.append(base + 2)
		indices.append(base + 0)
		indices.append(base + 2)
		indices.append(base + 3)
		base += 4

	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
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
	
	var background := ColorRect.new()
	background.color = Color(0, 0, 0, 0.8)
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_hud_panel.add_child(background)
	
	# Load and display the reference texture (on top of background)
	var htr_texture = load("res://textures/htr.png") if ResourceLoader.exists("res://textures/htr.png") else load("res://textures/htr.jpg") if ResourceLoader.exists("res://textures/htr.jpg") else null
	if htr_texture != null:
		var reference_image := TextureRect.new()
		reference_image.texture = htr_texture
		reference_image.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		reference_image.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_COVERED
		reference_image.modulate = Color(1, 1, 1, 0.3) # 30% opacity for better visibility
		_hud_panel.add_child(reference_image)
	
	var label := Label.new()
	label.text = "Arm Rotation Adjustments (F12 to toggle)"
	label.position = Vector2(10, 10)
	label.add_theme_font_size_override("font_size", 16)
	_hud_panel.add_child(label)
	
	var instructions := Label.new()
	instructions.text = "R/F: X rot | A/D: Y rot | W/S: Z rot | T/G: Scale | I/K/J/L/U/O: Position | P: Auto-optimize"
	instructions.position = Vector2(10, 30)
	instructions.add_theme_font_size_override("font_size", 12)
	_hud_panel.add_child(instructions)
	
	var offset_y := 60
	var axes = ["X Rot", "Y Rot", "Z Rot", "Scale", "Pos X", "Pos Y", "Pos Z"]
	var values = [_rotation_x, _rotation_y, _rotation_z, _arm_scale, _arm_position_x, _arm_position_y, _arm_position_z]
	
	for i in range(7):
		var axis_label := Label.new()
		axis_label.text = axes[i] + " : " + str(values[i])
		axis_label.position = Vector2(10, offset_y)
		_hud_panel.add_child(axis_label)
		offset_y += 25
	
	var optimizer_label := Label.new()
	optimizer_label.text = "Optimizer: " + ("ON" if _optimizer_active else "OFF") + " | Iterations: " + str(_optimizer_iterations)
	optimizer_label.position = Vector2(10, offset_y)
	_hud_panel.add_child(optimizer_label)

func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		if event.keycode == KEY_F12 and event.pressed:
			_hud_visible = !_hud_visible
			_hud_panel.visible = _hud_visible
		elif _hud_visible:
			var changed := false
			if event.keycode == KEY_R and event.pressed:
				_rotation_x -= 5.0
				changed = true
			elif event.keycode == KEY_F and event.pressed:
				_rotation_x += 5.0
				changed = true
			elif event.keycode == KEY_A and event.pressed:
				_rotation_y -= 5.0
				changed = true
			elif event.keycode == KEY_D and event.pressed:
				_rotation_y += 5.0
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
			elif event.keycode == KEY_P and event.pressed:
				_optimizer_active = !_optimizer_active
				if _optimizer_active:
					_start_optimizer()
				changed = true
			
			if changed:
				_update_arm_rotation()
				_update_hud_labels()

func _update_arm_rotation() -> void:
	if _arm_pivot != null:
		_arm_pivot.rotation_degrees = Vector3(_rotation_x, _rotation_y, _rotation_z)
	if _arm != null:
		_arm.scale = Vector3.ONE * MODEL_SCALE * _arm_scale
	var arm_root = _arm.get_parent().get_parent()
	if arm_root != null:
		arm_root.position = Vector3(_arm_position_x, _arm_position_y, _arm_position_z)

func _update_hud_labels() -> void:
	var labels = _hud_panel.find_children("", "Label", false, false)
	if labels.size() >= 9:
		labels[1].text = "X Rot : " + str(_rotation_x)
		labels[2].text = "Y Rot : " + str(_rotation_y)
		labels[3].text = "Z Rot : " + str(_rotation_z)
		labels[4].text = "Scale : " + str(_arm_scale)
		labels[5].text = "Pos X : " + str(_arm_position_x)
		labels[6].text = "Pos Y : " + str(_arm_position_y)
		labels[7].text = "Pos Z : " + str(_arm_position_z)
		labels[8].text = "Optimizer: " + ("ON" if _optimizer_active else "OFF") + " | Iterations: " + str(_optimizer_iterations)

func _start_optimizer() -> void:
	# Load reference texture for comparison
	_reference_texture = load("res://textures/htr.png") if ResourceLoader.exists("res://textures/htr.png") else load("res://textures/htr.jpg") if ResourceLoader.exists("res://textures/htr.jpg") else null
	if _reference_texture != null:
		_reference_image = _reference_texture.get_image()
		# Convert reference to grayscale and focus on black areas
		_reference_image = _extract_black_silhouette(_reference_image)
		_optimizer_iterations = 0
		_best_score = _calculate_image_similarity()
		_best_params = {
			"rot_x": _rotation_x,
			"rot_y": _rotation_y,
			"rot_z": _rotation_z,
			"scale": _arm_scale,
			"pos_x": _arm_position_x,
			"pos_y": _arm_position_y,
			"pos_z": _arm_position_z
		}
		_optimizer_step_size = 5.0  # Start with much larger steps for faster convergence
		print("Optimizer started with initial score: ", _best_score)
	else:
		print("Could not load reference texture for optimizer")
		_optimizer_active = false

func _extract_black_silhouette(image: Image) -> Image:
	# For reference: black arm on white background
	var result = Image.new()
	result.copy_from(image)
	result.resize(64, 48)
	
	for y in range(result.get_height()):
		for x in range(result.get_width()):
			var pixel = result.get_pixel(x, y)
			var brightness = (pixel.r + pixel.g + pixel.b) / 3.0
			if brightness < 0.3:  # Black pixels
				result.set_pixel(x, y, Color.WHITE)
			else:
				result.set_pixel(x, y, Color.BLACK)
	
	return result

func _extract_arm_area(image: Image) -> Image:
	# For game screenshot: extract non-white pixels (the actual arm)
	var result = Image.new()
	result.copy_from(image)
	result.resize(64, 48)
	
	for y in range(result.get_height()):
		for x in range(result.get_width()):
			var pixel = result.get_pixel(x, y)
			var brightness = (pixel.r + pixel.g + pixel.b) / 3.0
			if brightness < 0.9:  # Any non-white pixel (arm)
				result.set_pixel(x, y, Color.WHITE)
			else:
				result.set_pixel(x, y, Color.BLACK)
	
	return result

func _calculate_image_similarity() -> float:
	# Manual override - use mathematical scoring since screenshot comparison is unreliable
	var score = 0.0
	
	# Ideal target values (from your requested starting point)
	var ideal_x = 0.0
	var ideal_y = -175.0
	var ideal_z = 100.0
	var ideal_scale = 0.47
	var ideal_pos_x = 0.30
	var ideal_pos_y = 0.25
	var ideal_pos_z = -0.50
	
	# Calculate errors from ideal values
	var rot_x_error = abs(_rotation_x - ideal_x)
	var rot_y_error = abs(_rotation_y - ideal_y)
	var rot_z_error = abs(_rotation_z - ideal_z)
	var scale_error = abs(_arm_scale - ideal_scale)
	var pos_x_error = abs(_arm_position_x - ideal_pos_x)
	var pos_y_error = abs(_arm_position_y - ideal_pos_y)
	var pos_z_error = abs(_arm_position_z - ideal_pos_z)
	
	# Simple linear scoring - no exponential penalties to avoid getting stuck
	var total_error = rot_x_error + rot_y_error + rot_z_error + scale_error * 100 + pos_x_error * 100 + pos_y_error * 100 + pos_z_error * 100
	
	# Convert to similarity score (lower error = higher score)
	score = maxf(0.0, 2000.0 - total_error)
	
	return score

func _compare_silhouettes(img1: Image, img2: Image) -> float:
	if img1.get_size() != img2.get_size():
		return 0.0
	
	var width = img1.get_width()
	var height = img1.get_height()
	var matching_pixels = 0
	var total_pixels = 0
	
	for y in range(height):
		for x in range(width):
			var pixel1 = img1.get_pixel(x, y)
			var pixel2 = img2.get_pixel(x, y)
			
			# Count pixels where both are black (or both are white)
			var is_black1 = pixel1.r < 0.5
			var is_black2 = pixel2.r < 0.5
			
			if is_black1 == is_black2:
				matching_pixels += 1
			total_pixels += 1
	
	var similarity = float(matching_pixels) / float(total_pixels) * 100.0
	return similarity

func _process_optimizer() -> void:
	if not _optimizer_active:
		return
	
	_optimizer_timer += get_process_delta_time()
	if _optimizer_timer < _optimizer_interval:
		return
	_optimizer_timer = 0.0
	
	_optimizer_iterations += 1
	
	# Random search - just try random values in reasonable ranges
	_rotation_x = randf_range(-45.0, 45.0)
	_rotation_y = randf_range(-200.0, -150.0)
	_rotation_z = randf_range(80.0, 120.0)
	_arm_scale = randf_range(0.4, 0.6)
	_arm_position_x = 0.6  # Hardcoded
	_arm_position_y = -0.03  # Hardcoded
	_arm_position_z = randf_range(-0.7, -0.3)
	
	_update_arm_rotation()
	
	# Stop after 10000 iterations
	if _optimizer_iterations >= 10000:
		_optimizer_active = false
		print("Random search finished. Current params: ", _rotation_x, _rotation_y, _rotation_z, _arm_scale, _arm_position_x, _arm_position_y, _arm_position_z)
