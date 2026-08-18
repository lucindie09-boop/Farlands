extends Node3D

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")

var outline_enabled: bool = true
var outline_color: Color = Color.BLACK
var outline_opacity: float = 1.0
var outline_thickness: float = 0.1
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

var _current_thickness: float = -1.0
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

func _get_edge_perpendiculars(dir: Vector3) -> Array:
	var up = Vector3(0, 1, 0)
	if abs(dir.dot(up)) > 0.99:
		up = Vector3(1, 0, 0)
	var right = dir.cross(up).normalized()
	up = dir.cross(right).normalized()
	return [right, up]

func _rebuild_outline_mesh():
	_current_thickness = outline_thickness
	if outline_mesh:
		outline_mesh.queue_free()
		outline_mesh = null

	if _current_boxes.is_empty():
		return

	# Process per-axis: for each face plane, faces from both directions
	# (positive and negative normals) are XORed so internal faces
	# (covered on both sides) are excluded.
	var edge_set := {}
	var verts := PackedVector3Array()
	var indices := PackedInt32Array()

	# axis -> [u_axis, v_axis]
	var axis_uv := [[1, 2], [0, 2], [0, 1]]

	for axis in 3:
		var u_axis: int = axis_uv[axis][0]
		var v_axis: int = axis_uv[axis][1]

		# Collect all plane positions from both min and max of each box
		var plane_positions: Array = []
		for box in _current_boxes:
			var neg_pos: float = box[axis]
			var pos_pos: float = box[3 + axis]
			var found_neg := false
			var found_pos := false
			for p in plane_positions:
				if absf(p - neg_pos) < 0.0001:
					found_neg = true
				if absf(p - pos_pos) < 0.0001:
					found_pos = true
			if not found_neg:
				plane_positions.append(neg_pos)
			if not found_pos:
				plane_positions.append(pos_pos)
		plane_positions.sort()

		# For each plane position, collect positive-dir and negative-dir rects
		for fp in plane_positions:
			var pos_rects: Array = []  # faces with normal in +axis direction
			var neg_rects: Array = []  # faces with normal in -axis direction

			for box in _current_boxes:
				# Positive-direction face (at box max on this axis)
				if absf(box[3 + axis] - fp) < 0.0001:
					pos_rects.append([box[u_axis], box[3 + u_axis], box[v_axis], box[3 + v_axis]])
				# Negative-direction face (at box min on this axis)
				if absf(box[axis] - fp) < 0.0001:
					neg_rects.append([box[u_axis], box[3 + u_axis], box[v_axis], box[3 + v_axis]])

			# XOR: surface exists where exactly one side is solid
			_emit_xor_perimeter(axis, u_axis, v_axis, fp, pos_rects, neg_rects, edge_set, verts, indices)

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

func _emit_xor_perimeter(axis: int, u_axis: int, v_axis: int, fp: float,
		pos_rects: Array, neg_rects: Array,
		edge_set: Dictionary, verts: PackedVector3Array, indices: PackedInt32Array) -> void:
	# Find exposed cells where solid is on one side but not the other.
	var all_rects: Array = pos_rects + neg_rects
	if all_rects.is_empty():
		return

	# Single rect in only one direction — emit its 4 edges directly
	if pos_rects.size() + neg_rects.size() == 1:
		var r: Array = all_rects[0]
		_emit_face_edges(axis, u_axis, v_axis, fp, r[0], r[1], r[2], r[3], edge_set, verts, indices)
		return

	# Collect unique u and v coordinates from all rectangles
	var u_vals: Array = []
	var v_vals: Array = []
	for r in all_rects:
		u_vals.append(r[0])
		u_vals.append(r[1])
		v_vals.append(r[2])
		v_vals.append(r[3])
	u_vals.sort()
	v_vals.sort()

	# Deduplicate near-equal values
	var u_unique: Array = [u_vals[0]]
	var v_unique: Array = [v_vals[0]]
	for i in range(1, u_vals.size()):
		if absf(u_vals[i] - u_unique[-1]) > 0.0001:
			u_unique.append(u_vals[i])
	for i in range(1, v_vals.size()):
		if absf(v_vals[i] - v_unique[-1]) > 0.0001:
			v_unique.append(v_vals[i])

	var cols: int = u_unique.size() - 1
	var rows: int = v_unique.size() - 1
	if cols < 1 or rows < 1:
		return

	# Build pos_mask and neg_mask grids
	var pos_mask: Array = []
	var neg_mask: Array = []
	pos_mask.resize(cols * rows)
	neg_mask.resize(cols * rows)
	pos_mask.fill(false)
	neg_mask.fill(false)

	for r in pos_rects:
		for ci in cols:
			var cu: float = (u_unique[ci] + u_unique[ci + 1]) * 0.5
			if cu < r[0] or cu > r[1]:
				continue
			for ri in rows:
				var cv: float = (v_unique[ri] + v_unique[ri + 1]) * 0.5
				if cv >= r[2] and cv <= r[3]:
					pos_mask[ci + ri * cols] = true

	for r in neg_rects:
		for ci in cols:
			var cu: float = (u_unique[ci] + u_unique[ci + 1]) * 0.5
			if cu < r[0] or cu > r[1]:
				continue
			for ri in rows:
				var cv: float = (v_unique[ri] + v_unique[ri + 1]) * 0.5
				if cv >= r[2] and cv <= r[3]:
					neg_mask[ci + ri * cols] = true

	# XOR: surface exists where exactly one side is solid
	var filled: Array = []
	filled.resize(cols * rows)
	for i in cols * rows:
		filled[i] = pos_mask[i] != neg_mask[i]

	# Emit perimeter edges
	# Horizontal edges (along u, between rows)
	for ci in cols:
		for ri in range(0, rows + 1):
			var above: bool = (ri > 0 and filled[ci + (ri - 1) * cols])
			var below: bool = (ri < rows and filled[ci + ri * cols])
			if above != below:
				var p0 := Vector3.ZERO
				var p1 := Vector3.ZERO
				p0[axis] = fp
				p0[u_axis] = u_unique[ci]
				p0[v_axis] = v_unique[ri]
				p1[axis] = fp
				p1[u_axis] = u_unique[ci + 1]
				p1[v_axis] = v_unique[ri]
				_add_outline_edge(p0, p1, edge_set, verts, indices)

	# Vertical edges (along v, between columns)
	for ri in rows:
		for ci in range(0, cols + 1):
			var left: bool = (ci > 0 and filled[(ci - 1) + ri * cols])
			var right: bool = (ci < cols and filled[ci + ri * cols])
			if left != right:
				var p0 := Vector3.ZERO
				var p1 := Vector3.ZERO
				p0[axis] = fp
				p0[u_axis] = u_unique[ci]
				p0[v_axis] = v_unique[ri]
				p1[axis] = fp
				p1[u_axis] = u_unique[ci]
				p1[v_axis] = v_unique[ri + 1]
				_add_outline_edge(p0, p1, edge_set, verts, indices)

func _emit_face_edges(axis: int, u_axis: int, v_axis: int,
		fp: float, u_min: float, u_max: float, v_min: float, v_max: float,
		edge_set: Dictionary, verts: PackedVector3Array, indices: PackedInt32Array) -> void:
	var c := []
	for ci in 4:
		var v := Vector3.ZERO
		v[axis] = fp
		v[u_axis] = u_min if ci < 2 else u_max
		v[v_axis] = v_min if ci == 0 or ci == 3 else v_max
		c.append(v)
	for ci in 4:
		_add_outline_edge(c[ci], c[(ci + 1) % 4], edge_set, verts, indices)

func _add_outline_edge(p0: Vector3, p1: Vector3,
		edge_set: Dictionary, verts: PackedVector3Array, indices: PackedInt32Array) -> void:
	if p0.distance_to(p1) < 0.001:
		return
	var key: String
	if p0.x < p1.x or (is_equal_approx(p0.x, p1.x) and (p0.y < p1.y or (is_equal_approx(p0.y, p1.y) and p0.z < p1.z))):
		key = "%f_%f_%f_%f_%f_%f" % [p0.x, p0.y, p0.z, p1.x, p1.y, p1.z]
	else:
		key = "%f_%f_%f_%f_%f_%f" % [p1.x, p1.y, p1.z, p0.x, p0.y, p0.z]
	if edge_set.has(key):
		return
	edge_set[key] = true

	var dir = (p1 - p0).normalized()
	var perps = _get_edge_perpendiculars(dir)
	var half = outline_thickness * 0.01
	var right = perps[0] * half
	var up = perps[1] * half

	var base = verts.size()
	verts.append(p0 - right - up)
	verts.append(p0 + right - up)
	verts.append(p0 + right + up)
	verts.append(p0 - right + up)
	verts.append(p1 - right - up)
	verts.append(p1 + right - up)
	verts.append(p1 + right + up)
	verts.append(p1 - right + up)

	indices.append_array([base+0, base+4, base+7, base+0, base+7, base+3])
	indices.append_array([base+1, base+2, base+6, base+1, base+6, base+5])
	indices.append_array([base+0, base+5, base+4, base+0, base+1, base+5])
	indices.append_array([base+3, base+7, base+6, base+3, base+6, base+2])

func _create_fill():
	fill_mesh = MeshInstance3D.new()
	var mesh = BoxMesh.new()
	mesh.size = Vector3(1.001, 1.001, 1.001)
	fill_mesh.mesh = mesh
	fill_mesh.position = Vector3(0.5, 0.5, 0.5)
	fill_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	fill_mesh.visible = false
	add_child(fill_mesh)

func _update_fill_for_boxes():
	if _current_boxes.is_empty():
		return
	var bmin = Vector3(INF, INF, INF)
	var bmax = Vector3(-INF, -INF, -INF)
	for box in _current_boxes:
		bmin.x = minf(bmin.x, box[0])
		bmin.y = minf(bmin.y, box[1])
		bmin.z = minf(bmin.z, box[2])
		bmax.x = maxf(bmax.x, box[3])
		bmax.y = maxf(bmax.y, box[4])
		bmax.z = maxf(bmax.z, box[5])
	var size = bmax - bmin
	var pad = 0.001
	fill_mesh.mesh.size = size + Vector3(pad, pad, pad)
	fill_mesh.position = (bmin + bmax) * 0.5

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
		_update_fill_for_boxes()
	elif outline_thickness != _current_thickness:
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
