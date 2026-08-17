extends Node3D

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")

var outline_enabled: bool = true
var outline_color: Color = Color.BLACK
var outline_opacity: float = 1.0
var outline_thickness: float = 0.1  # 0.0 = outer edges only, 1.0 = inner edges at original position
var outline_pulse_enabled: bool = false
var outline_pulse_speed: float = 1.5
var outline_pulse_min_opacity: float = 0.75
var outline_pulse_max_opacity: float = 1.0
var reach_distance: float = 5.0

var fill_enabled: bool = false
var fill_color: Color = Color.WHITE
var fill_opacity: float = 0.1
var fill_pulse_enabled: bool = true
var fill_pulse_speed: float = 1.5
var fill_pulse_min_opacity: float = 0.05
var fill_pulse_max_opacity: float = 0.1

var outline_mesh: MeshInstance3D
var fill_mesh: MeshInstance3D

var _outline_material: StandardMaterial3D
var _fill_material: StandardMaterial3D

var _current_thickness: float = 0.5
var _pulse_time: float = 0.0
var _current_block_id: int = -1
var _current_boxes: Array = []

# Throttling: skip raycast when camera hasn't moved significantly
var _last_camera_position: Vector3 = Vector3.ZERO
var _last_camera_rotation: Vector3 = Vector3.ZERO
var _last_block_edit_counter: int = -1
var _position_threshold: float = 0.01
var _rotation_threshold: float = 0.001

func _ready():
	_create_fill()
	_create_materials()

func _create_wireframe_box(box_min_x: float, box_min_y: float, box_min_z: float,
							box_max_x: float, box_max_y: float, box_max_z: float) -> ArrayMesh:
	var verts = PackedVector3Array()
	var indices = PackedInt32Array()
	
	# 8 corners of the AABB
	var corners = [
		Vector3(box_min_x, box_min_y, box_min_z),
		Vector3(box_max_x, box_min_y, box_min_z),
		Vector3(box_max_x, box_max_y, box_min_z),
		Vector3(box_min_x, box_max_y, box_min_z),
		Vector3(box_min_x, box_min_y, box_max_z),
		Vector3(box_max_x, box_min_y, box_max_z),
		Vector3(box_max_x, box_max_y, box_max_z),
		Vector3(box_min_x, box_max_y, box_max_z)
	]
	
	# 12 edges of the cube (each edge = 2 vertices forming a thin quad)
	var edges = [
		[0, 1], [1, 2], [2, 3], [3, 0],  # bottom, right, top, left (Z- face)
		[4, 5], [5, 6], [6, 7], [7, 4],  # bottom, right, top, left (Z+ face)
		[0, 4], [1, 5], [2, 6], [3, 7]   # connecting edges
	]
	
	for edge in edges:
		var p0 = corners[edge[0]]
		var p1 = corners[edge[1]]
		var dir = (p1 - p0).normalized()
		var length = p0.distance_to(p1)
		if length < 0.001:
			continue
		
		# Compute perpendicular axes for the wireframe thickness
		var up = Vector3(0, 1, 0)
		if abs(dir.dot(up)) > 0.99:
			up = Vector3(1, 0, 0)
		var right = dir.cross(up).normalized() * outline_thickness * 0.02
		up = dir.cross(right).normalized() * outline_thickness * 0.02
		
		var base_idx = verts.size()
		verts.append(p0 - right - up)
		verts.append(p0 + right - up)
		verts.append(p1 + right - up)
		verts.append(p0 - right + up)
		verts.append(p1 - right + up)
		verts.append(p1 + right + up)
		verts.append(p0 - right + up)
		verts.append(p1 + right + up)
		verts.append(p1 + right - up)
		verts.append(p1 + right + up)
		verts.append(p1 - right + up)
		verts.append(p1 - right - up)
		
		for i in range(12):
			indices.append(base_idx + i)
	
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = indices
	
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func _rebuild_outline_mesh():
	if outline_mesh:
		outline_mesh.queue_free()
		outline_mesh = null
	
	if _current_boxes.is_empty():
		return
	
	var verts = PackedVector3Array()
	var indices = PackedInt32Array()
	
	for box in _current_boxes:
		var box_min_x = box[0]
		var box_min_y = box[1]
		var box_min_z = box[2]
		var box_max_x = box[3]
		var box_max_y = box[4]
		var box_max_z = box[5]
		
		var corners = [
			Vector3(box_min_x, box_min_y, box_min_z),
			Vector3(box_max_x, box_min_y, box_min_z),
			Vector3(box_max_x, box_max_y, box_min_z),
			Vector3(box_min_x, box_max_y, box_min_z),
			Vector3(box_min_x, box_min_y, box_max_z),
			Vector3(box_max_x, box_min_y, box_max_z),
			Vector3(box_max_x, box_max_y, box_max_z),
			Vector3(box_min_x, box_max_y, box_max_z)
		]
		
		var edges = [
			[0, 1], [1, 2], [2, 3], [3, 0],
			[4, 5], [5, 6], [6, 7], [7, 4],
			[0, 4], [1, 5], [2, 6], [3, 7]
		]
		
		for edge in edges:
			var p0 = corners[edge[0]]
			var p1 = corners[edge[1]]
			var dir = (p1 - p0).normalized()
			var length = p0.distance_to(p1)
			if length < 0.001:
				continue
			
			var up = Vector3(0, 1, 0)
			if abs(dir.dot(up)) > 0.99:
				up = Vector3(1, 0, 0)
			var right = dir.cross(up).normalized() * outline_thickness * 0.02
			up = dir.cross(right).normalized() * outline_thickness * 0.02
			
			var base_idx = verts.size()
			verts.append(p0 - right - up)
			verts.append(p0 + right - up)
			verts.append(p1 + right - up)
			verts.append(p0 - right + up)
			verts.append(p1 - right + up)
			verts.append(p1 + right + up)
			verts.append(p0 - right + up)
			verts.append(p1 + right + up)
			verts.append(p1 + right - up)
			verts.append(p1 + right + up)
			verts.append(p1 - right + up)
			verts.append(p1 - right - up)
			
			for i in range(12):
				indices.append(base_idx + i)
	
	if verts.size() == 0:
		return
	
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = indices
	
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	
	outline_mesh = MeshInstance3D.new()
	outline_mesh.mesh = mesh
	outline_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	outline_mesh.material_override = _outline_material
	add_child(outline_mesh)

func _create_fill():
	fill_mesh = MeshInstance3D.new()
	var mesh = BoxMesh.new()
	mesh.size = Vector3(1.001, 1.001, 1.001)
	fill_mesh.mesh = mesh
	fill_mesh.position = Vector3(0.5, 0.5, 0.5)
	fill_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	fill_mesh.visible = false
	add_child(fill_mesh)

func _create_materials():
	_outline_material = StandardMaterial3D.new()
	_outline_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_outline_material.albedo_color = outline_color
	_outline_material.albedo_color.a = outline_opacity
	_outline_material.no_depth_test = false
	_outline_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_outline_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_outline_material.render_priority = 10

	_fill_material = StandardMaterial3D.new()
	_fill_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_fill_material.no_depth_test = false
	_fill_material.cull_mode = BaseMaterial3D.CULL_BACK
	_fill_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_fill_material.albedo_color = fill_color
	_fill_material.albedo_color.a = fill_opacity
	_fill_material.render_priority = 9

	# Apply materials
	fill_mesh.mesh.surface_set_material(0, _fill_material)

func _process(delta):
	if not player_controller or not chunk_manager:
		if outline_mesh:
			outline_mesh.visible = false
		if fill_mesh:
			fill_mesh.visible = false
		return

	if player_controller.is_chat_open() or player_controller.is_inventory_open() or player_controller.is_settings_open():
		if outline_mesh:
			outline_mesh.visible = false
		if fill_mesh:
			fill_mesh.visible = false
		return

	# Get current camera state
	var camera = player_controller.get_node("Camera3D")
	if not camera:
		return

	var current_position = camera.global_position
	var current_rotation = camera.global_rotation
	var current_edit_counter = player_controller.get_block_edit_counter()

	# Skip raycast if camera hasn't moved significantly and no blocks were edited (throttling)
	var position_changed = current_position.distance_to(_last_camera_position) > _position_threshold
	var rotation_changed = abs(current_rotation.x - _last_camera_rotation.x) > _rotation_threshold or \
						  abs(current_rotation.y - _last_camera_rotation.y) > _rotation_threshold or \
						  abs(current_rotation.z - _last_camera_rotation.z) > _rotation_threshold
	var world_changed = current_edit_counter != _last_block_edit_counter

	var needs_raycast = position_changed or rotation_changed or world_changed or _last_camera_position == Vector3.ZERO

	if not needs_raycast:
		_pulse_time += delta
		_update_materials()
		return

	_last_camera_position = current_position
	_last_camera_rotation = current_rotation
	_last_block_edit_counter = current_edit_counter

	var result = chunk_manager.raycast_from_camera(reach_distance)
	if not result or not result.get("success", false):
		if outline_mesh:
			outline_mesh.visible = false
		if fill_mesh:
			fill_mesh.visible = false
		return

	var block_pos = result.get("position")
	var place_pos = result.get("place_position")

	if not block_pos or not place_pos:
		if outline_mesh:
			outline_mesh.visible = false
		if fill_mesh:
			fill_mesh.visible = false
		return

	var bx = int(floor(block_pos.x))
	var by = int(floor(block_pos.y))
	var bz = int(floor(block_pos.z))

	global_position = Vector3(bx, by, bz)

	# Get selection boxes for this block
	var block_id = result.get("block_id", 0)
	if block_id != _current_block_id:
		_current_block_id = block_id
		_current_boxes = chunk_manager.get_selection_boxes(block_id)
		_rebuild_outline_mesh()

	if outline_mesh:
		outline_mesh.visible = outline_enabled
	if fill_mesh:
		fill_mesh.visible = fill_enabled

	_pulse_time += delta
	_update_materials()

func _update_materials():
	var outline_pulse_factor = (sin(_pulse_time * outline_pulse_speed) + 1.0) / 2.0
	var fill_pulse_factor = (sin(_pulse_time * fill_pulse_speed) + 1.0) / 2.0
	
	var current_outline_opacity = outline_opacity
	if outline_pulse_enabled:
		current_outline_opacity = lerp(outline_pulse_min_opacity, outline_pulse_max_opacity, outline_pulse_factor)
	_outline_material.albedo_color = outline_color
	_outline_material.albedo_color.a = clampf(current_outline_opacity, 0.0, 1.0)

	var current_fill_opacity = fill_opacity
	if fill_pulse_enabled:
		current_fill_opacity = lerp(fill_pulse_min_opacity, fill_pulse_max_opacity, fill_pulse_factor)
	_fill_material.albedo_color = fill_color
	_fill_material.albedo_color.a = clampf(current_fill_opacity, 0.0, 1.0)
