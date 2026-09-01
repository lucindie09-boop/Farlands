extends Node

# Renders isometric block icons for inventory/UI use.
# Caches rendered textures so each block is only rendered once.
# Uses orthographic camera at Minecraft's dimetric angle (45° yaw, ~30° pitch).

const ICON_SIZE := 64  # Output icon resolution
const BLOCK_SCALE := 1.0  # Scale of block in viewport

var _viewport: SubViewport
var _camera: Camera3D
var _block_mesh: MeshInstance3D
var _icon_cache: Dictionary = {}  # block_id -> ImageTexture
var _block_defs: Array = []

func _ready() -> void:
	_load_block_definitions()
	_setup_viewport()
	# Start pre-rendering in background after a brief delay
	call_deferred("_pre_render_all_icons_async")

func _pre_render_all_icons_async() -> void:
	# Pre-render all block icons in the background
	for block_id in range(_block_defs.size()):
		var block_def = _block_defs[block_id]
		# Skip air and transparent blocks
		if "Transparent" in block_def.get("properties", []):
			continue
		# Render and cache
		if not _icon_cache.has(block_id):
			var icon := await _render_block_icon(block_id)
			if icon != null:
				_icon_cache[block_id] = icon

func _load_block_definitions() -> void:
	var file := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if file == null:
		push_error("Failed to load block_definitions.json")
		return
	
	var json_text := file.get_as_text()
	file.close()
	
	var json := JSON.new()
	var parse_err := json.parse(json_text)
	if parse_err != OK:
		push_error("Failed to parse block_definitions.json: " + json.get_error_message())
		return
	
	_block_defs = json.data

func _setup_viewport() -> void:
	# Create sub-viewport for icon rendering
	_viewport = SubViewport.new()
	_viewport.name = "IconViewport"
	_viewport.transparent_bg = true
	_viewport.size = Vector2(ICON_SIZE, ICON_SIZE)
	_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	_viewport.msaa_3d = Viewport.MSAA_4X
	add_child(_viewport)
	
	# Set up environment
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1, 1, 1, 1)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1, 1)
	env.ambient_light_energy = 0.35
	var world := World3D.new()
	world.environment = env
	_viewport.world_3d = world
	
	# Add lighting - matches skin_preview/block_preview setup
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.4
	sun.rotation_degrees = Vector3(-45, -35, 0)
	_viewport.add_child(sun)
	
	var fill := DirectionalLight3D.new()
	fill.light_energy = 0.5
	fill.rotation_degrees = Vector3(70, 140, 0)
	_viewport.add_child(fill)
	
	# Create block mesh placeholder
	_block_mesh = MeshInstance3D.new()
	_block_mesh.scale = Vector3.ONE * BLOCK_SCALE
	_viewport.add_child(_block_mesh)
	
	# Create perspective camera for now (easier to debug)
	_camera = Camera3D.new()
	_camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	_camera.fov = 40.0
	_camera.near = 0.1
	_camera.far = 100.0
	
	# Position camera to look at the block from an angle
	_camera.position = Vector3(2.0, 1.5, 2.0)
	# Use look_at_from_position since we're not in tree yet
	_camera.look_at_from_position(_camera.position, Vector3.ZERO, Vector3.UP)
	_camera.make_current()
	_viewport.add_child(_camera)

# Get or render an icon for a block by ID (synchronous for UI use)
func get_block_icon(block_id: int) -> ImageTexture:
	if block_id < 0 or block_id >= _block_defs.size():
		return null
	
	# Check cache
	if _icon_cache.has(block_id):
		return _icon_cache[block_id]
	
	# Render new icon synchronously
	var icon := _render_block_icon_sync(block_id)
	if icon != null:
		_icon_cache[block_id] = icon
	
	return icon

# Render a single block icon (synchronous)
func _render_block_icon_sync(block_id: int) -> ImageTexture:
	if block_id < 0 or block_id >= _block_defs.size():
		return null
	
	var block_def = _block_defs[block_id]
	
	# Skip air and invisible blocks
	if "Transparent" in block_def.get("properties", []):
		return null
	
	# Build block mesh
	var mesh := _build_block_mesh(block_def)
	if mesh == null:
		return null
	
	_block_mesh.mesh = mesh
	
	# Apply textures
	_apply_block_textures(block_def)
	
	# Force viewport render always for synchronous capture
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	
	# Capture viewport to texture immediately
	var tex := _viewport.get_texture()
	if tex == null:
		return null
	
	var img := tex.get_image()
	if img == null:
		return null
	
	# Reset to disabled to save performance
	_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	
	return ImageTexture.create_from_image(img)

# Render a single block icon (async, for test function)
func _render_block_icon(block_id: int) -> ImageTexture:
	if block_id < 0 or block_id >= _block_defs.size():
		return null
	
	var block_def = _block_defs[block_id]
	
	# Skip air and invisible blocks
	if "Transparent" in block_def.get("properties", []):
		return null
	
	print("Rendering block " + str(block_id) + ": " + block_def.get("name", "unknown"))
	
	# Build block mesh
	var mesh := _build_block_mesh(block_def)
	if mesh == null:
		print("Failed to build mesh for block " + str(block_id))
		return null
	
	print("Mesh has " + str(mesh.get_surface_count()) + " surfaces")
	
	_block_mesh.mesh = mesh
	
	# Apply textures
	_apply_block_textures(block_def)
	
	# Force viewport render once
	_viewport.render_target_update_mode = SubViewport.UPDATE_ONCE
	
	# Wait for multiple frames to ensure render completes
	for i in range(5):
		await get_tree().process_frame
	
	# Capture viewport to texture
	var tex := _viewport.get_texture()
	if tex == null:
		print("Failed to get viewport texture for block " + str(block_id))
		return null
	
	var img := tex.get_image()
	if img == null:
		print("Failed to get image from texture for block " + str(block_id))
		return null
	
	# Check if image is blank
	var is_blank := true
	for y in range(img.get_height()):
		for x in range(img.get_width()):
			var pixel = img.get_pixel(x, y)
			if pixel.a > 0.01:
				is_blank = false
				break
		if not is_blank:
			break
	
	if is_blank:
		print("Warning: Rendered image appears blank for block " + str(block_id))
	
	print("Successfully rendered icon for block " + str(block_id) + ", size: " + str(img.get_width()) + "x" + str(img.get_height()))
	return ImageTexture.create_from_image(img)

# Build a block mesh from block definition
func _build_block_mesh(block_def: Dictionary) -> ArrayMesh:
	var visible_faces = block_def.get("visible_faces", [true, true, true, true, true, true])
	
	# Skip if no faces visible
	var has_visible := false
	for v in visible_faces:
		if v:
			has_visible = true
			break
	if not has_visible:
		return null
	
	var mesh = ArrayMesh.new()
	
	# Face order: +X, -X, +Y, -Y, +Z, -Z
	var face_defs = [
		{"normal": Vector3(1, 0, 0), "verts": [Vector3(0.5, -0.5, 0.5), Vector3(0.5, 0.5, 0.5), Vector3(0.5, 0.5, -0.5), Vector3(0.5, -0.5, -0.5)]},
		{"normal": Vector3(-1, 0, 0), "verts": [Vector3(-0.5, -0.5, -0.5), Vector3(-0.5, 0.5, -0.5), Vector3(-0.5, 0.5, 0.5), Vector3(-0.5, -0.5, 0.5)]},
		{"normal": Vector3(0, 1, 0), "verts": [Vector3(-0.5, 0.5, -0.5), Vector3(0.5, 0.5, -0.5), Vector3(0.5, 0.5, 0.5), Vector3(-0.5, 0.5, 0.5)]},
		{"normal": Vector3(0, -1, 0), "verts": [Vector3(-0.5, -0.5, 0.5), Vector3(0.5, -0.5, 0.5), Vector3(0.5, -0.5, -0.5), Vector3(-0.5, -0.5, -0.5)]},
		{"normal": Vector3(0, 0, 1), "verts": [Vector3(-0.5, -0.5, 0.5), Vector3(-0.5, 0.5, 0.5), Vector3(0.5, 0.5, 0.5), Vector3(0.5, -0.5, 0.5)]},
		{"normal": Vector3(0, 0, -1), "verts": [Vector3(0.5, -0.5, -0.5), Vector3(0.5, 0.5, -0.5), Vector3(-0.5, 0.5, -0.5), Vector3(-0.5, -0.5, -0.5)]},
	]
	
	# Create a separate surface for each face so we can apply different textures
	for i in range(6):
		if not visible_faces[i]:
			continue
		
		var face = face_defs[i]
		var face_verts = face.verts
		
		var arrays = []
		arrays.resize(Mesh.ARRAY_MAX)
		
		var verts = PackedVector3Array()
		var uvs = PackedVector2Array()
		var normals = PackedVector3Array()
		var indices = PackedInt32Array()
		
		# Add vertices
		for v in face_verts:
			verts.append(v)
			normals.append(face.normal)
		
		# UVs for this face
		uvs.append(Vector2(0.0, 1.0))
		uvs.append(Vector2(0.0, 0.0))
		uvs.append(Vector2(1.0, 0.0))
		uvs.append(Vector2(1.0, 1.0))
		
		# Add indices (two triangles per face)
		indices.append(0)
		indices.append(1)
		indices.append(2)
		indices.append(0)
		indices.append(2)
		indices.append(3)
		
		arrays[Mesh.ARRAY_VERTEX] = verts
		arrays[Mesh.ARRAY_TEX_UV] = uvs
		arrays[Mesh.ARRAY_NORMAL] = normals
		arrays[Mesh.ARRAY_INDEX] = indices
		
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	
	return mesh

# Apply textures to the block mesh
func _apply_block_textures(block_def: Dictionary) -> void:
	var textures = block_def.get("textures", ["", "", "", "", "", ""])
	var visible_faces = block_def.get("visible_faces", [true, true, true, true, true, true])
	
	# Find first valid texture for fallback
	var fallback_tex: Texture2D = null
	for tex_name in textures:
		if not tex_name.is_empty():
			var tex_path: String = "res://textures/blocks/" + tex_name + ".png"
			if FileAccess.file_exists(ProjectSettings.globalize_path(tex_path)):
				var tex := load(tex_path) as Texture2D
				if tex != null:
					fallback_tex = tex
					break
	
	# Create a separate material for each visible face
	# Face order: +X, -X, +Y, -Y, +Z, -Z
	var surface_index := 0
	for i in range(6):
		if not visible_faces[i]:
			continue  # Skip invisible faces (like grass bottom)
		
		var face_tex_name = textures[i]
		var face_tex: Texture2D = null
		
		if not face_tex_name.is_empty():
			var face_tex_path: String = "res://textures/blocks/" + face_tex_name + ".png"
			if FileAccess.file_exists(ProjectSettings.globalize_path(face_tex_path)):
				face_tex = load(face_tex_path) as Texture2D
		
		# Use fallback if specific texture doesn't exist
		if face_tex == null:
			face_tex = fallback_tex
		
		if face_tex == null:
			continue  # Skip if no texture available at all
		
		var mat := StandardMaterial3D.new()
		mat.albedo_texture = face_tex
		mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		mat.roughness = 1.0
		mat.metallic = 0.0
		
		_block_mesh.set_surface_override_material(surface_index, mat)
		surface_index += 1

# Clear the icon cache (call this if textures change)
func clear_cache() -> void:
	_icon_cache.clear()

# Test function - renders a few block icons and saves them to disk
func test_render_icons() -> void:
	print("Testing block icon rendering...")
	
	# Test a few common blocks
	var test_blocks = [1, 2, 3, 4]  # stone, dirt, grass, sand
	
	for block_id in test_blocks:
		var icon = get_block_icon(block_id)
		if icon != null:
			var img = icon.get_image()
			var path = "user://test_icon_" + str(block_id) + ".png"
			img.save_png(path)
			print("Saved icon for block " + str(block_id) + " to " + path)
		else:
			print("Failed to render icon for block " + str(block_id))
	
	print("Icon rendering test complete.")
