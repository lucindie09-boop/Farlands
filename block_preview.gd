extends SubViewportContainer

# Drag-to-orbit preview of a cube for the block maker.
# Similar to skin_preview but for a simple cube instead of player model.

signal paint_history_changed(has_undo: bool)

const ZOOM_SPEED := 0.25
const ZOOM_MIN := 8.0
const ZOOM_MAX := 70.0
const TEXEL := 1.0 / 16.0  # Block textures are 16x16

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
	_cube.mesh = BoxMesh.new()
	_cube.mesh.size = Vector3(1, 1, 1)
	_cube.scale = Vector3(4.0, 4.0, 4.0)  # Much larger scale like skin maker
	
	# Add collision shape for raycasting
	var collision := StaticBody3D.new()
	collision.collision_layer = 1
	collision.collision_mask = 1
	var shape := CollisionShape3D.new()
	shape.shape = BoxShape3D.new()
	shape.shape.size = Vector3(1, 1, 1)
	collision.add_child(shape)
	_cube.add_child(collision)
	
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
			_yaw += mm.relative.x * 0.3
			_pitch = clampf(_pitch + mm.relative.y * 0.3, -89.0, 89.0)
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
	var _viewport := get_child(0) as SubViewport
	var ray_origin: Vector3 = _camera.project_ray_origin(mouse_pos)
	var ray_dir: Vector3 = _camera.project_ray_normal(mouse_pos)
	
	var physics_query := PhysicsRayQueryParameters3D.create(ray_origin, ray_origin + ray_dir * 100)
	var result: Dictionary = _viewport.world_3d.direct_space_state.intersect_ray(physics_query)
	
	if result.is_empty():
		return Vector2.INF
	
	var collider: Node = result.get("collider")
	if collider != _cube:
		return Vector2.INF
	
	var uv: Vector2 = result.get("uv")
	if uv == null:
		return Vector2.INF
	
	return uv

func _paint_texel(u: float, v: float) -> void:
	var px := int(u * 16.0)
	var py := int(v * 16.0)
	
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	var old_color: Color = block_manager.get_image().get_pixel(px, py)
	if block_manager.set_pixel(px, py, _color):
		_record_stroke_texel(px, py, old_color, _color)
		_apply_block_texture()

func _flood_fill(start_u: float, start_v: float) -> void:
	var start_x := int(start_u * 16.0)
	var start_y := int(start_v * 16.0)
	
	var block_manager := get_node_or_null("/root/BlockManager")
	if block_manager == null:
		return
	
	var img: Image = block_manager.get_image()
	var target_color: Color = img.get_pixel(start_x, start_y)
	
	if target_color.is_equal_approx(_color):
		return
	
	var queue: Array = [[start_x, start_y]]
	var visited: Dictionary = {}
	visited[start_y * 16 + start_x] = true
	
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
				if nx >= 0 and nx < 16 and ny >= 0 and ny < 16:
					var key: int = ny * 16 + nx
					if not visited.has(key):
						visited[key] = true
						queue.append([nx, ny])
	
	_apply_block_texture()

func _draw_box() -> void:
	if _box_start == Vector2.INF or _box_end == Vector2.INF:
		return
	
	var x0 := int(minf(_box_start.x, _box_end.x) * 16.0)
	var y0 := int(minf(_box_start.y, _box_end.y) * 16.0)
	var x1 := int(maxf(_box_start.x, _box_end.x) * 16.0)
	var y1 := int(maxf(_box_start.y, _box_end.y) * 16.0)
	
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
	# UV overlay could be implemented to show the 16x16 grid
	# For now, just a placeholder
	pass

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
