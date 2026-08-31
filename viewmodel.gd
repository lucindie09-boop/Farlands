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
var _material: Material
var _cube_mesh: ArrayMesh
var _stick_mesh: BoxMesh
var _block_id := -2
var _prev_block_id := -2
var _equip := 0.0
var _is_swapping := false
var _swing := 0.0
var _item_meshes := {} # Cache for generated item meshes

# Real-time adjustment HUD
var _hud_panel: Control
var _hud_visible := false
var _adjustment_mode := "ARM" # "ARM" or "BLOCK"
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

	# The glb arm node carries a big baked-in offset (x -8.5/+3.5 px, y 12 px);
	# we re-parent its mesh under a shoulder pivot and ignore that lever arm for
	# placement. arm_pivot carries the punch rotation so the mesh keeps its scale
	# (setting .basis directly would nuke it).
	var mdl := PLAYER_MODEL.instantiate()
	var arm_node := mdl.get_child(arm_index) as Node3D
	mdl.remove_child(arm_node)
	mdl.free()
	_arm_pivot = Node3D.new()
	arm_root.add_child(_arm_pivot)
	_arm_pivot.add_child(arm_node)
	arm_node.position = Vector3(0, -12.0, 0) * MODEL_SCALE * _arm_scale
	arm_node.scale = Vector3.ONE * MODEL_SCALE * _arm_scale
	arm_node.rotation_degrees = Vector3.ZERO
	_arm_pivot.basis = MC_ARM_BASIS

	_flip_arm_mesh_uvs(arm_node)
	_apply_skin(arm_node)
	_arm = arm_node
	_update_arm_rotation()
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

	# Use StandardMaterial3D with per-pixel lighting instead of unshaded
	# This should give us dynamic lighting without the whitening issue
	var std_mat := StandardMaterial3D.new()
	std_mat.shading_mode = BaseMaterial3D.SHADING_MODE_PER_PIXEL
	std_mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	std_mat.cull_mode = BaseMaterial3D.CULL_BACK
	std_mat.no_depth_test = true
	std_mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
	std_mat.roughness = 1.0
	std_mat.metallic = 0.0
	_material = std_mat

	_cube_mesh = _build_cube_mesh()
	_stick_mesh = null # Will be generated dynamically from texture

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
	
	# Equip animation: both normal equip and swap take 0.25s
	var equip_speed = 0.25
	_equip = move_toward(_equip, 1.0, delta / equip_speed)
	
	_update_swing_hooks()
	_refresh_held_item()

# Kick a punch/swing (roadmap: punch animation). Progress 1..0 over 0.25s.
func punch() -> void:
	_swing = 1.0

func _update_swing_hooks() -> void:
	var s := _swing
	var f3 := sin(s * s * PI) # sin(swing^2 * pi)
	var f4 := sin(sqrt(s) * PI) # sin(sqrt(swing) * pi)
	
	# Equip animation logic
	var equip_offset: float
	if _is_swapping:
		# Swap animation: first half unequip (go down), second half equip (come up)
		if _equip < 0.5:
			# Unequip phase: old item goes from -0.25 down to below
			var unequip_progress = _equip * 2.0 # 0.0 to 1.0 during first half
			equip_offset = -0.25 - 0.5 * unequip_progress
		else:
			# Equip phase: new item comes from below up to -0.25
			var equip_progress = (_equip - 0.5) * 2.0 # 0.0 to 1.0 during second half
			equip_offset = -0.75 + 0.5 * equip_progress
	else:
		# Normal equip: item comes from below up to -0.25
		equip_offset = -0.75 + 0.5 * _equip
	
	# MC bob: (-0.4 sin(sqrt s pi), 0.2 sin(sqrt s pi*2), -0.2 sin(s pi))
	_hand_bob.position = Vector3(-0.4 * f4, 0.2 * sin(sqrt(s) * PI * 2.0) + equip_offset, -0.2 * sin(s * PI))
	
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
		
		_item_scale_node.scale = Vector3.ONE # already 0.4 from parent
		# Reset block adjustments for items
		_item_scale_node.position = Vector3.ZERO
		_item_scale_node.rotation_degrees = Vector3.ZERO
	else:
		_item.mesh = _cube_mesh
		_item_scale_node.scale = Vector3.ONE * _block_scale # 0.4 total scale from parent
		_item.rotation = Vector3.ZERO
		# Apply block adjustments
		_item_scale_node.position = Vector3(_block_position_x, _block_position_y, _block_position_z)
		_item_scale_node.rotation_degrees = Vector3(_block_rotation_x, _block_rotation_y, _block_rotation_z)

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
		std_mat.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
		std_mat.roughness = 1.0
		std_mat.metallic = 0.0
		if tex != null:
			std_mat.albedo_texture = tex
		
		for s in range(mi.mesh.get_surface_count()):
			mi.set_surface_override_material(s, std_mat)

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
	
	var label := Label.new()
	label.text = "Arm & Block Adjustments (F12 to toggle, B to switch mode)"
	label.position = Vector2(10, 10)
	label.add_theme_font_size_override("font_size", 16)
	_hud_panel.add_child(label)
	
	var instructions := Label.new()
	instructions.text = "R/F: X rot | A/D: Y rot | W/S: Z rot | T/G: Scale | I/K/J/L/U/O: Position | B: Switch Arm/Block"
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
			_adjustment_mode = "BLOCK" if _adjustment_mode == "ARM" else "ARM"
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
			else: # BLOCK mode
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
			
			if changed:
				if _adjustment_mode == "ARM":
					_update_arm_rotation()
				else:
					_update_block_transform()
				_update_hud_labels()

func _update_arm_rotation() -> void:
	if _arm_pivot != null:
		# The source pose is the exact MC_ARM_BASIS matrix (avoids Godot's
		# Euler-order ambiguity). The F12 HUD rotation keys apply a small euler
		# delta on top.
		_arm_pivot.basis = MC_ARM_BASIS.rotated(Vector3.RIGHT, deg_to_rad(_rotation_x))
		_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.UP, deg_to_rad(_rotation_y))
		_arm_pivot.basis = _arm_pivot.basis.rotated(Vector3.BACK, deg_to_rad(_rotation_z))
	if _arm != null:
		_arm.position = Vector3(0, -12.0, 0) * MODEL_SCALE * _arm_scale
		_arm.scale = Vector3.ONE * MODEL_SCALE * _arm_scale
		if _arm.get_parent() != null and _arm.get_parent().get_parent() != null:
			var arm_root = _arm.get_parent().get_parent() as Node3D
			if arm_root != null:
				arm_root.position = Vector3(_arm_position_x, _arm_position_y, _arm_position_z)

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
		else:
			labels[1].text = "X Rot : " + str(_block_rotation_x)
			labels[2].text = "Y Rot : " + str(_block_rotation_y)
			labels[3].text = "Z Rot : " + str(_block_rotation_z)
			labels[4].text = "Scale : " + str(_block_scale)
			labels[5].text = "Pos X : " + str(_block_position_x)
			labels[6].text = "Pos Y : " + str(_block_position_y)
			labels[7].text = "Pos Z : " + str(_block_position_z)

func _update_block_transform() -> void:
	if _item_scale_node != null and _block_id > 0 and not BlockTextures.is_item(_block_id):
		_item_scale_node.position = Vector3(_block_position_x, _block_position_y, _block_position_z)
		_item_scale_node.rotation_degrees = Vector3(_block_rotation_x, _block_rotation_y, _block_rotation_z)
		_item_scale_node.scale = Vector3.ONE * _block_scale

func _generate_item_mesh(texture: Texture2D) -> ArrayMesh:
	# Generate 3D item model from texture data by extruding painted pixels
	var mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()
	
	# Get texture data
	var image := texture.get_image()
	if image == null:
		return mesh
	
	# Convert to RGBA if needed
	if image.get_format() != Image.FORMAT_RGBA8:
		var new_image := Image.new()
		new_image.copy_from(image)
		new_image.convert(Image.FORMAT_RGBA8)
		image = new_image
	
	var width: int = image.get_width()
	var height: int = image.get_height()
	var pixels := image.get_data()
	
	# Analyze pixels to find painted (non-transparent) ones
	var painted_pixels := []
	for y: int in range(height):
		for x: int in range(width):
			var pixel_index := (y * width + x) * 4
			var alpha := pixels[pixel_index + 3]
			if alpha > 0: # Non-transparent pixel
				painted_pixels.append(Vector2i(x, y))
	
	if painted_pixels.is_empty():
		return mesh
	
	# Extrude painted pixels into 3D geometry
	var extrusion_depth := 0.08 # Thinner extrusion depth
	var texel_size := 1.0 / maxf(width, height)
	var vertex_index := 0
	
	for pixel in painted_pixels:
		var x: int = pixel.x
		var y: int = pixel.y
		
		# Convert pixel coordinates to normalized UV space
		var u = float(x) / width
		var v = float(y) / height
		
		# Convert to 3D space (centered)
		var px = (x - width / 2.0) * texel_size
		var py = (height / 2.0 - y) * texel_size # Flip Y for texture coordinates
		var pz_front = extrusion_depth / 2.0
		var pz_back = -extrusion_depth / 2.0
		
		# Create 6 faces for each pixel (like MC's extrusion)
		# Front face
		verts.append_array([
			Vector3(px - texel_size/2, py - texel_size/2, pz_front),
			Vector3(px - texel_size/2, py + texel_size/2, pz_front),
			Vector3(px + texel_size/2, py + texel_size/2, pz_front),
			Vector3(px + texel_size/2, py - texel_size/2, pz_front)
		])
		normals.append_array([Vector3.FORWARD, Vector3.FORWARD, Vector3.FORWARD, Vector3.FORWARD])
		uvs.append_array([
			Vector2(u, v + texel_size),
			Vector2(u, v),
			Vector2(u + texel_size, v),
			Vector2(u + texel_size, v + texel_size)
		])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
		
		# Back face
		verts.append_array([
			Vector3(px + texel_size/2, py - texel_size/2, pz_back),
			Vector3(px + texel_size/2, py + texel_size/2, pz_back),
			Vector3(px - texel_size/2, py + texel_size/2, pz_back),
			Vector3(px - texel_size/2, py - texel_size/2, pz_back)
		])
		normals.append_array([Vector3.BACK, Vector3.BACK, Vector3.BACK, Vector3.BACK])
		uvs.append_array([
			Vector2(u + texel_size, v + texel_size),
			Vector2(u + texel_size, v),
			Vector2(u, v),
			Vector2(u, v + texel_size)
		])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
		
		# Top face - use pixel's own center color
		verts.append_array([
			Vector3(px - texel_size/2, py + texel_size/2, pz_front),
			Vector3(px - texel_size/2, py + texel_size/2, pz_back),
			Vector3(px + texel_size/2, py + texel_size/2, pz_back),
			Vector3(px + texel_size/2, py + texel_size/2, pz_front)
		])
		normals.append_array([Vector3.UP, Vector3.UP, Vector3.UP, Vector3.UP])
		var center_uv := Vector2(u + texel_size/2, v + texel_size/2) # Sample from pixel center
		uvs.append_array([center_uv, center_uv, center_uv, center_uv])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
		
		# Bottom face - use pixel's own center color
		verts.append_array([
			Vector3(px - texel_size/2, py - texel_size/2, pz_back),
			Vector3(px - texel_size/2, py - texel_size/2, pz_front),
			Vector3(px + texel_size/2, py - texel_size/2, pz_front),
			Vector3(px + texel_size/2, py - texel_size/2, pz_back)
		])
		normals.append_array([Vector3.DOWN, Vector3.DOWN, Vector3.DOWN, Vector3.DOWN])
		uvs.append_array([center_uv, center_uv, center_uv, center_uv])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
		
		# Right face - use pixel's own center color
		verts.append_array([
			Vector3(px + texel_size/2, py - texel_size/2, pz_front),
			Vector3(px + texel_size/2, py + texel_size/2, pz_front),
			Vector3(px + texel_size/2, py + texel_size/2, pz_back),
			Vector3(px + texel_size/2, py - texel_size/2, pz_back)
		])
		normals.append_array([Vector3.RIGHT, Vector3.RIGHT, Vector3.RIGHT, Vector3.RIGHT])
		uvs.append_array([center_uv, center_uv, center_uv, center_uv])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
		
		# Left face - use pixel's own center color
		verts.append_array([
			Vector3(px - texel_size/2, py - texel_size/2, pz_back),
			Vector3(px - texel_size/2, py + texel_size/2, pz_back),
			Vector3(px - texel_size/2, py + texel_size/2, pz_front),
			Vector3(px - texel_size/2, py - texel_size/2, pz_front)
		])
		normals.append_array([Vector3.LEFT, Vector3.LEFT, Vector3.LEFT, Vector3.LEFT])
		uvs.append_array([center_uv, center_uv, center_uv, center_uv])
		indices.append_array([vertex_index, vertex_index + 1, vertex_index + 2, vertex_index, vertex_index + 2, vertex_index + 3])
		vertex_index += 4
	
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh
