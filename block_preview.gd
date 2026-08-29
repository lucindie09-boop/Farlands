extends SubViewportContainer

# Drag-to-orbit preview of a cube for the block maker.
# Similar to skin_preview but for a simple cube instead of player model.

signal paint_history_changed(has_undo: bool)

const ZOOM_SPEED := 0.25
const ZOOM_MIN := 8.0
const ZOOM_MAX := 70.0
const TEXEL := 1.0 / 16.0  # Block textures are 16x16
const ATLAS_WIDTH := 16  # Single 16x16 texture for all faces
const ATLAS_HEIGHT := 16

var _camera: Camera3D
var _cube: MeshInstance3D
var _target := Vector3(0, 0, 0)
var _yaw := -35.0
var _pitch := 14.0
var _dist := 34.0
var _rotating := false
var _zooming := false
var _painting := false
var _last_paint_uv := Vector2.INF
var _tool := "DRAW"  # DRAW, FILL, BOX
var _color := Color.WHITE
var _box_start := Vector2.INF
var _box_end := Vector2.INF
var _box_active := false

var _undo_stack := []
var _stroke := []
var _uv_overlay_enabled := false

func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	stretch = true
	_ensure_viewport()
	_setup_scene()
	set_color(_color)
	set_tool(_tool)

func _ensure_viewport() -> void:
	if get_child_count() == 0:
		var viewport := SubViewport.new()
		viewport.name = "Viewport"
		viewport.transparent_bg = true
		viewport.size = Vector2(400, 400)
		viewport.msaa_3d = Viewport.MSAA_4X
		add_child(viewport)
		
		# Set up world and environment - copied from skin_preview
		var env := Environment.new()
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(1, 1, 1, 1)
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(1, 1, 1, 1)
		env.ambient_light_energy = 0.35
		var world := World3D.new()
		world.environment = env
		viewport.world_3d = world

func _setup_scene() -> void:
	var _viewport := get_child(0) as SubViewport
	_viewport.transparent_bg = true
	
	# Add lighting exactly like skin_preview
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.4
	sun.rotation_degrees = Vector3(-45, -35, 0)
	_viewport.add_child(sun)
	
	var fill := DirectionalLight3D.new()
	fill.light_energy = 0.5
	fill.rotation_degrees = Vector3(70, 140, 0)
	_viewport.add_child(fill)
	
	_cube = MeshInstance3D.new()
	_cube.mesh = _build_cube_mesh()
	_cube.scale = Vector3(4.0, 4.0, 4.0)  # Much larger scale like skin maker
	
	_viewport.add_child(_cube)
	
	# Set target to cube center
	var aabb: AABB = _cube.mesh.get_aabb()
	_target = aabb.get_center()
	
	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.make_current()
	_viewport.add_child(_camera)
	
	_apply_block_texture()
	_update_camera()

func _build_cube_mesh() -> ArrayMesh:
	# Create a cube mesh with explicit UVs spanning 0-1 for each face
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	
	var verts = PackedVector3Array()
	var uvs = PackedVector2Array()
	var normals = PackedVector3Array()
	var indices = PackedInt32Array()
	
	# +X face (right)
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	var base := 0
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	# -X face (left)
	verts.append(Vector3(-0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, -0.5, 0.5))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 4
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	# +Y face (top)
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 8
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	# -Y face (bottom)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 12
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	# +Z face (front)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 16
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	# -Z face (back)
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 20
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)
	
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	
	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func _apply_block_texture() -> void:
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	var tex: ImageTexture = block_manager.get_texture()
	if tex == null:
		return
	
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = tex
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.albedo_color = Color.WHITE  # Ensure no black overlay
	mat.roughness = 1.0  # Ensure proper lighting
	
	# Add UV overlay as next pass if enabled
	if _uv_overlay_enabled:
		var overlay_shader := ShaderMaterial.new()
		overlay_shader.shader = load("res://shaders/block_uv_overlay.gdshader") as Shader
		overlay_shader.set_shader_parameter("grid_color", Color(0.0, 0.0, 0.0, 0.9))
		overlay_shader.set_shader_parameter("cells", 16.0)
		overlay_shader.set_shader_parameter("line_thickness", 1.5)
		mat.next_pass = overlay_shader
	
	_cube.set_surface_override_material(0, mat)

func _update_camera() -> void:
	if _camera == null:
		return
	_camera.position = _target + Vector3(
		_dist * cos(_pitch) * sin(_yaw),
		_dist * sin(_pitch),
		_dist * cos(_pitch) * cos(_yaw))
	_camera.look_at(_target, Vector3.UP)

func _input(event: InputEvent) -> void:
	if not is_visible_in_tree():
		return
	
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MouseButton.MOUSE_BUTTON_LEFT:
			if mb.pressed:
				_handle_left_click()
			else:
				_handle_left_release()
		elif mb.button_index == MouseButton.MOUSE_BUTTON_RIGHT:
			if mb.pressed:
				_zooming = true
			else:
				_zooming = false
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _rotating:
			_yaw += mm.relative.x * 0.15
			_pitch = clampf(_pitch + mm.relative.y * 0.15, -89.0, 89.0)
			_update_camera()
		elif _zooming:
			_dist += mm.relative.y * ZOOM_SPEED
			_dist = clampf(_dist, ZOOM_MIN, ZOOM_MAX)
			_update_camera()
		elif _painting:
			_handle_paint_motion(mm.position)
		
		# Update last paint UV for DRAW tool
		if _painting and _tool == "DRAW":
			var local_pos := _get_local_uv_from_mouse(mm.position)
			if local_pos != Vector2.INF:
				_last_paint_uv = local_pos

func _handle_left_click() -> void:
	var mouse_pos := get_viewport().get_mouse_position()
	var local_pos := _get_local_uv_from_mouse(mouse_pos)
	
	if local_pos == Vector2.INF:
		_rotating = true
		return
	
	_painting = true
	
	if _tool == "DRAW":
		_paint_texel(local_pos.x, local_pos.y)
	elif _tool == "FILL":
		_flood_fill(local_pos.x, local_pos.y)
	elif _tool == "BOX":
		_box_start = local_pos
		_box_end = local_pos
		_box_active = true

func _handle_left_release() -> void:
	_rotating = false
	_painting = false
	
	if _box_active:
		_draw_box()
		_box_active = false
		_box_start = Vector2.INF
		_box_end = Vector2.INF
	
	_commit_stroke()

func _handle_paint_motion(mouse_pos: Vector2) -> void:
	if _tool == "DRAW":
		var local_pos := _get_local_uv_from_mouse(mouse_pos)
		if local_pos != Vector2.INF:
			# Check UV break distance to prevent painting across different faces
			if _last_paint_uv != Vector2.INF:
				var uv_dist := local_pos.distance_to(_last_paint_uv)
				if uv_dist > 0.1:  # Simple threshold for face separation
					_last_paint_uv = local_pos
					return
			_paint_texel(local_pos.x, local_pos.y)
			_last_paint_uv = local_pos
	elif _tool == "BOX" and _box_active:
		var local_pos := _get_local_uv_from_mouse(mouse_pos)
		if local_pos != Vector2.INF:
			_box_end = local_pos

func _get_local_uv_from_mouse(mouse_pos: Vector2) -> Vector2:
	# Direct triangle raycast like skin_preview - MeshInstance3D has no physics
	var origin := _camera.project_ray_origin(get_local_mouse_position())
	var dir := _camera.project_ray_normal(get_local_mouse_position())
	var best := INF
	var best_uv := Vector2.INF
	
	if _cube == null or _cube.mesh == null:
		return Vector2.INF
	
	var inv := _cube.global_transform.affine_inverse()
	var lo := inv * origin
	var ld := inv.basis * dir
	
	for surface_idx in range(_cube.mesh.get_surface_count()):
		var arrays := _cube.mesh.surface_get_arrays(surface_idx)
		if arrays.is_empty():
			continue
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
		var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		
		var tri_count: int
		if idx.size() >= 3:
			tri_count = int(idx.size() / 3.0)
		else:
			tri_count = int(verts.size() / 3.0)
		
		for t in range(tri_count):
			var i0: int
			var i1: int
			var i2: int
			if idx.size() >= 3:
				i0 = idx[t * 3]
				i1 = idx[t * 3 + 1]
				i2 = idx[t * 3 + 2]
			else:
				i0 = t * 3
				i1 = t * 3 + 1
				i2 = t * 3 + 2
			
			var v0 := verts[i0]
			var v1 := verts[i1]
			var v2 := verts[i2]
			
			var edge1 := v1 - v0
			var edge2 := v2 - v0
			var h := ld.cross(edge2)
			var a := edge1.dot(h)
			
			if absf(a) < 0.00001:
				continue
			
			var f := 1.0 / a
			var ray_s := lo - v0
			var u := f * ray_s.dot(h)
			if u < 0.0 or u > 1.0:
				continue
			
			var q := ray_s.cross(edge1)
			var v := f * ld.dot(q)
			if v < 0.0 or u + v > 1.0:
				continue
			
			var t_param := f * edge2.dot(q)
			if t_param < 0.0 or t_param >= best:
				continue
			
			best = t_param
			var uv0 := uvs[i0]
			var uv1 := uvs[i1]
			var uv2 := uvs[i2]
			best_uv = uv0 + (uv1 - uv0) * u + (uv2 - uv0) * v
	
	return best_uv

func _paint_texel(u: float, v: float) -> void:
	# Map UV from 0-1 atlas space to pixel coordinates (0-95 x 0-15)
	var px := int(u * ATLAS_WIDTH)
	var py := int(v * ATLAS_HEIGHT)
	
	# Clamp to valid range
	px = clampi(px, 0, ATLAS_WIDTH - 1)
	py = clampi(py, 0, ATLAS_HEIGHT - 1)
	
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	var old_color: Color = block_manager.get_image().get_pixel(px, py)
	if block_manager.set_pixel(px, py, _color):
		_record_stroke_texel(px, py, old_color, _color)
		_apply_block_texture()

func _flood_fill(start_u: float, start_v: float) -> void:
	# Map UV from 0-1 atlas space to pixel coordinates
	var start_x := int(start_u * ATLAS_WIDTH)
	var start_y := int(start_v * ATLAS_HEIGHT)
	
	start_x = clampi(start_x, 0, ATLAS_WIDTH - 1)
	start_y = clampi(start_y, 0, ATLAS_HEIGHT - 1)
	
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	var img: Image = block_manager.get_image()
	var target_color: Color = img.get_pixel(start_x, start_y)
	
	if target_color.is_equal_approx(_color):
		return
	
	var queue: Array = [[start_x, start_y]]
	var visited: Dictionary = {}
	visited[start_y * ATLAS_WIDTH + start_x] = true
	
	while not queue.is_empty():
		var current: Array = queue.pop_front()
		var x: int = current[0]
		var y: int = current[1]
		
		if img.get_pixel(x, y).is_equal_approx(target_color):
			var old_color: Color = img.get_pixel(x, y)
			if block_manager.set_pixel(x, y, _color):
				_record_stroke_texel(x, y, old_color, _color)
			
			for neighbor in [[x+1, y], [x-1, y], [x, y+1], [x, y-1]]:
				var nx: int = neighbor[0]
				var ny: int = neighbor[1]
				if nx >= 0 and nx < ATLAS_WIDTH and ny >= 0 and ny < ATLAS_HEIGHT:
					var key: int = ny * ATLAS_WIDTH + nx
					if not visited.has(key):
						visited[key] = true
						queue.append([nx, ny])
	
	_apply_block_texture()

func _draw_box() -> void:
	if _box_start == Vector2.INF or _box_end == Vector2.INF:
		return
	
	# Map UV from 0-1 atlas space to pixel coordinates
	var x0 := int(minf(_box_start.x, _box_end.x) * ATLAS_WIDTH)
	var y0 := int(minf(_box_start.y, _box_end.y) * ATLAS_HEIGHT)
	var x1 := int(maxf(_box_start.x, _box_end.x) * ATLAS_WIDTH)
	var y1 := int(maxf(_box_start.y, _box_end.y) * ATLAS_HEIGHT)
	
	x0 = clampi(x0, 0, ATLAS_WIDTH - 1)
	y0 = clampi(y0, 0, ATLAS_HEIGHT - 1)
	x1 = clampi(x1, 0, ATLAS_WIDTH - 1)
	y1 = clampi(y1, 0, ATLAS_HEIGHT - 1)
	
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	block_manager.fill_rect(x0, y0, x1, y1, _color)
	_apply_block_texture()

func _record_stroke_texel(px: int, py: int, old_color: Color, new_color: Color) -> void:
	_stroke.append({"x": px, "y": py, "old": old_color, "new": new_color})

func _commit_stroke() -> void:
	if _stroke.is_empty():
		return
	_undo_stack.append(_stroke.duplicate())
	_stroke.clear()
	paint_history_changed.emit(not _undo_stack.is_empty())

func undo() -> void:
	if _undo_stack.is_empty():
		return
	
	var stroke: Array = _undo_stack.pop_back()
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	for entry in stroke:
		var px: int = entry["x"]
		var py: int = entry["y"]
		var old_color: Color = entry["old"]
		block_manager.set_pixel(px, py, old_color)
	
	paint_history_changed.emit(not _undo_stack.is_empty())
	_apply_block_texture()

func set_tool(tool_name: String) -> void:
	_tool = tool_name

func set_color(color: Color) -> void:
	_color = color
	# Update the BlockManager color too
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager != null:
		pass  # BlockManager doesn't need color state, it's per-paint operation

func set_uv_overlay(enabled: bool) -> void:
	_uv_overlay_enabled = enabled
	_apply_block_texture()

func save_block(path: String) -> bool:
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return false
	var img: Image = block_manager.get_image()
	if img == null:
		return false
	return img.save_png(path) == OK

func load_block(path: String) -> bool:
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return false
	var img: Image = Image.load_from_file(path)
	if img == null:
		return false
	block_manager.set_from_image(img)
	_apply_block_texture()
	_undo_stack.clear()
	paint_history_changed.emit(false)
	return true
