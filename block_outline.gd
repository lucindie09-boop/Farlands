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
var _original_vertices: PackedVector3Array
var _inner_vertex_indices: Array[int] = []
var _vertex_face_center: Dictionary = {}  # Map vertex index to its face center
var _pulse_time: float = 0.0

# Throttling: skip raycast when camera hasn't moved significantly
var _last_camera_position: Vector3 = Vector3.ZERO
var _last_camera_rotation: Vector3 = Vector3.ZERO
var _position_threshold: float = 0.01
var _rotation_threshold: float = 0.001

func _ready():
	_create_outline()
	_create_fill()
	_create_materials()

func _create_outline():
	var mesh = _load_and_build_mesh()
	
	outline_mesh = MeshInstance3D.new()
	outline_mesh.mesh = mesh
	outline_mesh.position = Vector3(0.5, 0.5, 0.5)
	outline_mesh.scale = Vector3(0.501, 0.501, 0.501)
	outline_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(outline_mesh)

func _load_and_build_mesh() -> ArrayMesh:
	var file = FileAccess.open("res://Untitled.obj", FileAccess.READ)
	if not file:
		return null
	
	var obj_vertices = PackedVector3Array()
	var obj_uvs = PackedVector2Array()
	var obj_normals = PackedVector3Array()
	
	while not file.eof_reached():
		var line = file.get_line().strip_edges()
		if line.is_empty() or line.begins_with("#"):
			continue
		
		if line.begins_with("v "):
			var parts = line.split(" ", false)
			var v = Vector3(float(parts[1]), float(parts[2]), float(parts[3]))
			obj_vertices.append(v)
		
		elif line.begins_with("vt "):
			var parts = line.split(" ", false)
			var uv = Vector2(float(parts[1]), float(parts[2]))
			obj_uvs.append(uv)
		
		elif line.begins_with("vn "):
			var parts = line.split(" ", false)
			var n = Vector3(float(parts[1]), float(parts[2]), float(parts[3]))
			obj_normals.append(n)
	
	file.close()
	
	# Parse faces and build mesh with proper vertex attributes
	var vertex_map = {}  # Maps (v_idx, vt_idx, vn_idx) to mesh vertex index
	var mesh_vertices = PackedVector3Array()
	var mesh_uvs = PackedVector2Array()
	var mesh_normals = PackedVector3Array()
	var mesh_indices = PackedInt32Array()
	var mesh_vertex_count = 0
	
	file = FileAccess.open("res://Untitled.obj", FileAccess.READ)
	while not file.eof_reached():
		var line = file.get_line().strip_edges()
		if line.begins_with("f "):
			var parts = line.split(" ", false)
			var face_vertices = []
			
			for i in range(1, parts.size()):
				var face_part = parts[i].split("/")
				var v_idx = int(face_part[0]) - 1
				var vt_idx = int(face_part[1]) - 1 if face_part.size() > 1 and face_part[1] != "" else 0
				var vn_idx = int(face_part[2]) - 1 if face_part.size() > 2 and face_part[2] != "" else 0
				
				face_vertices.append([v_idx, vt_idx, vn_idx])
			
			# Triangulate face (assuming quads)
			var triangles = [[0, 1, 2], [0, 2, 3]]
			for tri in triangles:
				for vtx_idx in tri:
					var v_data = face_vertices[vtx_idx]
					var key = str(v_data[0]) + "_" + str(v_data[1]) + "_" + str(v_data[2])
					
					if not vertex_map.has(key):
						mesh_vertices.append(obj_vertices[v_data[0]])
						mesh_uvs.append(obj_uvs[v_data[1]] if v_data[1] < obj_uvs.size() else Vector2(0, 0))
						mesh_normals.append(obj_normals[v_data[2]] if v_data[2] < obj_normals.size() else Vector3(0, 1, 0))
						vertex_map[key] = mesh_vertex_count
						mesh_vertex_count += 1
					
					mesh_indices.append(vertex_map[key])
	
	file.close()
	
	# Store original vertices and identify inner vertices with their face centers
	_original_vertices = mesh_vertices.duplicate()
	
	# Determine face centers for each inner vertex
	for i in range(mesh_vertices.size()):
		var v = mesh_vertices[i]
		# Inner vertices are those not at the outer cube corners (±1.0, ±1.0, ±1.0)
		# The outer cube has 8 corner vertices at exact (±1, ±1, ±1)
		var is_corner = abs(abs(v.x) - 1.0) < 0.01 and abs(abs(v.y) - 1.0) < 0.01 and abs(abs(v.z) - 1.0) < 0.01
		
		if not is_corner:
			_inner_vertex_indices.append(i)
			# Determine which face this vertex belongs to based on which coordinate is near ±1.0
			# The face center keeps the coordinate at ±1.0 and sets the others to 0
			var face_center = Vector3(0, 0, 0)
			if abs(v.x) > 0.5:
				face_center.x = sign(v.x) * 1.0
			if abs(v.y) > 0.5:
				face_center.y = sign(v.y) * 1.0
			if abs(v.z) > 0.5:
				face_center.z = sign(v.z) * 1.0
			_vertex_face_center[i] = face_center
	
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = mesh_vertices
	arrays[Mesh.ARRAY_TEX_UV] = mesh_uvs
	arrays[Mesh.ARRAY_NORMAL] = mesh_normals
	arrays[Mesh.ARRAY_INDEX] = mesh_indices
	
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

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
	outline_mesh.material_override = _outline_material
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

	# Skip raycast if camera hasn't moved significantly (throttling)
	var position_changed = current_position.distance_to(_last_camera_position) > _position_threshold
	var rotation_changed = abs(current_rotation.x - _last_camera_rotation.x) > _rotation_threshold or \
						  abs(current_rotation.y - _last_camera_rotation.y) > _rotation_threshold or \
						  abs(current_rotation.z - _last_camera_rotation.z) > _rotation_threshold

	var needs_raycast = position_changed or rotation_changed or _last_camera_position == Vector3.ZERO

	if not needs_raycast:
		# Still update pulse animations even if raycast is skipped
		_pulse_time += delta
		_update_materials()
		return

	# Update tracked camera state
	_last_camera_position = current_position
	_last_camera_rotation = current_rotation

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

	# Convert to block coordinates
	var bx = int(floor(block_pos.x))
	var by = int(floor(block_pos.y))
	var bz = int(floor(block_pos.z))

	# Position at block location
	global_position = Vector3(bx, by, bz)

	if outline_mesh:
		outline_mesh.visible = outline_enabled
	if fill_mesh:
		fill_mesh.visible = fill_enabled

	# Update pulse time
	_pulse_time += delta

	# Update materials and thickness with pulse
	_update_materials()

func _update_materials():
	# Regenerate mesh if thickness changed
	if abs(outline_thickness - _current_thickness) > 0.01:
		_current_thickness = outline_thickness
		call_deferred("_rebuild_mesh_with_thickness")
	
	# Calculate pulse factors (0.0 to 1.0 oscillating)
	var outline_pulse_factor = (sin(_pulse_time * outline_pulse_speed) + 1.0) / 2.0
	var fill_pulse_factor = (sin(_pulse_time * fill_pulse_speed) + 1.0) / 2.0
	
	# Update outline material with pulse
	var current_outline_opacity = outline_opacity
	if outline_pulse_enabled:
		current_outline_opacity = lerp(outline_pulse_min_opacity, outline_pulse_max_opacity, outline_pulse_factor)
	_outline_material.albedo_color = outline_color
	_outline_material.albedo_color.a = clampf(current_outline_opacity, 0.0, 1.0)

	# Update fill material with pulse
	var current_fill_opacity = fill_opacity
	if fill_pulse_enabled:
		current_fill_opacity = lerp(fill_pulse_min_opacity, fill_pulse_max_opacity, fill_pulse_factor)
	_fill_material.albedo_color = fill_color
	_fill_material.albedo_color.a = clampf(current_fill_opacity, 0.0, 1.0)

func _rebuild_mesh_with_thickness():
	if not outline_mesh or not outline_mesh.mesh:
		return
	
	var arrays = outline_mesh.mesh.surface_get_arrays(0)
	if arrays.is_empty():
		return
	
	var new_vertices = _original_vertices.duplicate()
	
	# Adjust inner vertices based on thickness
	for idx in _inner_vertex_indices:
		if idx >= new_vertices.size() or idx >= _original_vertices.size():
			continue
		var original = _original_vertices[idx]
		var face_center = _vertex_face_center.get(idx, Vector3.ZERO)
		
		# Skip if face center is zero (invalid)
		if face_center == Vector3.ZERO:
			continue
		
		# Move vertices toward the center of their face
		# thickness 0.0 = vertices at original position (thinnest outline)
		# thickness 1.0 = vertices at face center (thickest outline)
		var direction = original - face_center
		var thickness_factor = clampf(1.0 - outline_thickness, 0.0, 1.0)
		var target = original - direction * thickness_factor
		new_vertices[idx] = target
	
	arrays[Mesh.ARRAY_VERTEX] = new_vertices
	
	var new_mesh = ArrayMesh.new()
	new_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	
	if outline_mesh.mesh:
		outline_mesh.mesh.call_deferred("queue_free")
	outline_mesh.mesh = new_mesh
	outline_mesh.material_override = _outline_material
