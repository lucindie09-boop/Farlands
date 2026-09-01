extends Node

# Renders isometric block icons for inventory/UI use.
# Caches rendered textures so each block is only rendered once.
# Uses orthographic camera at Minecraft's dimetric angle (45° yaw, ~30° pitch).

const ICON_SIZE := 300  # Output icon resolution
const BLOCK_SCALE := 1.0  # Scale of block in viewport

var _viewport: SubViewport
var _camera: Camera3D
var _block_mesh: MeshInstance3D
var _icon_cache: Dictionary = {}  # block_id -> ImageTexture
var _block_defs: Array = []
var _block_shapes: Dictionary = {}  # shape_name -> shape data

func _ready() -> void:
	_load_block_definitions()
	_load_block_shapes()
	_setup_viewport()
	# Start pre-rendering in background after a brief delay
	call_deferred("_pre_render_all_icons_async")

func _pre_render_all_icons_async() -> void:
	# Pre-render all block icons in the background
	for block_id in range(_block_defs.size()):
		var block_def = _block_defs[block_id]
		# Skip air blocks (but render transparent blocks like leaves)
		if block_def.get("name", "") == "air":
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

func _load_block_shapes() -> void:
	var file := FileAccess.open("res://data/block_shapes.json", FileAccess.READ)
	if file == null:
		push_error("Failed to load block_shapes.json")
		return
	
	var json_text := file.get_as_text()
	file.close()
	
	var json := JSON.new()
	var parse_err := json.parse(json_text)
	if parse_err != OK:
		push_error("Failed to parse block_shapes.json: " + json.get_error_message())
		return
	
	_block_shapes = json.data

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
	
	# Create orthographic camera for isometric view
	_camera = Camera3D.new()
	_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	_camera.size = 1.75
	_camera.near = 0.1
	_camera.far = 100.0
	
	# Position camera for dimetric isometric view (45° yaw, ~30° pitch)
	var distance = 3.0
	var yaw = deg_to_rad(45)
	var pitch = deg_to_rad(30)
	_camera.position = Vector3(
		distance * cos(pitch) * sin(yaw),
		distance * sin(pitch),
		distance * cos(pitch) * cos(yaw)
	)
	_camera.look_at_from_position(_camera.position, Vector3.ZERO, Vector3.UP)
	_camera.make_current()
	_viewport.add_child(_camera)

# Get or render an icon for a block by ID (synchronous for UI use)
func get_block_icon(block_id: int) -> ImageTexture:
	if block_id < 0 or block_id >= _block_defs.size():
		return null
	
	# Check cache first - this is the primary path
	if _icon_cache.has(block_id):
		return _icon_cache[block_id]
	
	# If not in cache, it means pre-rendering hasn't finished yet
	# Return null to let UI fall back to BlockTextures
	return null

# Render a single block icon (synchronous)
func _render_block_icon_sync(block_id: int) -> ImageTexture:
	if block_id < 0 or block_id >= _block_defs.size():
		return null
	
	var block_def = _block_defs[block_id]
	
	# Skip air blocks (but render transparent blocks like leaves for icons)
	if block_def.get("name", "") == "air":
		return null
	
	# Build block mesh
	var mesh := _build_block_mesh(block_def)
	if mesh == null:
		return null
	
	_block_mesh.mesh = mesh
	_block_mesh.material_override = null  # Clear any previous material override
	
	# Center the mesh based on its AABB so all blocks appear at the same distance
	var aabb = mesh.get_aabb()
	var center_offset = aabb.get_center() - Vector3(0.5, 0.5, 0.5)
	_block_mesh.position = -center_offset
	
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
	
	# Skip air blocks (but render transparent blocks like leaves for icons)
	if block_def.get("name", "") == "air":
		return null
	
	print("Rendering block " + str(block_id) + ": " + block_def.get("name", "unknown"))
	
	# Build block mesh
	var mesh := _build_block_mesh(block_def)
	if mesh == null:
		print("Failed to build mesh for block " + str(block_id))
		return null
	
	print("Mesh has " + str(mesh.get_surface_count()) + " surfaces")
	
	_block_mesh.mesh = mesh
	_block_mesh.material_override = null  # Clear any previous material override
	
	# Center the mesh based on its AABB so all blocks appear at the same distance
	var aabb = mesh.get_aabb()
	var center_offset = aabb.get_center() - Vector3(0.5, 0.5, 0.5)
	_block_mesh.position = -center_offset
	
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
	
	# Check if block has a custom shape
	var shape = block_def.get("shape", "")
	if not shape.is_empty():
		return _build_shaped_mesh(shape, block_def)
	
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

# Build a mesh from custom shape definition
func _build_shaped_mesh(shape: String, _block_def: Dictionary) -> ArrayMesh:
	var parts = shape.split("/")
	
	# Handle shapes without variants (like "pole")
	var shape_type = parts[0]
	var shape_variant = ""
	if parts.size() >= 2:
		shape_variant = parts[1]
	
	print("Building shaped mesh: " + shape_type + "/" + shape_variant)
	
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
			print("Shape variant not found: " + shape_variant)
			return null
		variant_data = shape_data[shape_variant]
	
	var selection_boxes = variant_data.get("selection_boxes", [])
	
	print("Selection boxes: " + str(selection_boxes))
	
	if selection_boxes.is_empty():
		print("No selection boxes for shape")
		return null
	
	var mesh = ArrayMesh.new()
	
	# Collect all faces from all boxes into single arrays
	var all_verts = PackedVector3Array()
	var all_uvs = PackedVector2Array()
	var all_normals = PackedVector3Array()
	var all_indices = PackedInt32Array()
	var vertex_offset := 0
	
	# Build a cube for each selection box
	for box in selection_boxes:
		if box.size() < 6:
			continue
		
		var min_x = box[0]
		var min_y = box[1]
		var min_z = box[2]
		var max_x = box[3]
		var max_y = box[4]
		var max_z = box[5]
		
		print("Building box: min(" + str(min_x) + "," + str(min_y) + "," + str(min_z) + ") max(" + str(max_x) + "," + str(max_y) + "," + str(max_z) + ")")
		
		# Build the 6 faces of this box with proper winding (counter-clockwise when viewed from outside)
		var box_faces = [
			# +X face (right) - viewed from +X looking toward origin
			{"normal": Vector3(1, 0, 0), "verts": [Vector3(max_x, min_y, max_z), Vector3(max_x, max_y, max_z), Vector3(max_x, max_y, min_z), Vector3(max_x, min_y, min_z)]},
			# -X face (left) - viewed from -X looking toward origin
			{"normal": Vector3(-1, 0, 0), "verts": [Vector3(min_x, min_y, max_z), Vector3(min_x, max_y, max_z), Vector3(min_x, max_y, min_z), Vector3(min_x, min_y, min_z)]},
			# +Y face (top) - viewed from +Y looking down
			{"normal": Vector3(0, 1, 0), "verts": [Vector3(min_x, max_y, min_z), Vector3(max_x, max_y, min_z), Vector3(max_x, max_y, max_z), Vector3(min_x, max_y, max_z)]},
			# -Y face (bottom) - viewed from -Y looking up
			{"normal": Vector3(0, -1, 0), "verts": [Vector3(min_x, min_y, min_z), Vector3(max_x, min_y, min_z), Vector3(max_x, min_y, max_z), Vector3(min_x, min_y, max_z)]},
			# +Z face (front) - viewed from +Z looking toward origin
			{"normal": Vector3(0, 0, 1), "verts": [Vector3(min_x, min_y, min_z), Vector3(min_x, max_y, min_z), Vector3(max_x, max_y, min_z), Vector3(max_x, min_y, min_z)]},
			# -Z face (back) - viewed from -Z looking toward origin
			{"normal": Vector3(0, 0, -1), "verts": [Vector3(min_x, min_y, max_z), Vector3(min_x, max_y, max_z), Vector3(max_x, max_y, max_z), Vector3(max_x, min_y, max_z)]},
		]
		
		for face in box_faces:
			for v in face.verts:
				all_verts.append(v)
				all_normals.append(face.normal)
			
			all_uvs.append(Vector2(0.0, 1.0))
			all_uvs.append(Vector2(0.0, 0.0))
			all_uvs.append(Vector2(1.0, 0.0))
			all_uvs.append(Vector2(1.0, 1.0))
			
			all_indices.append(vertex_offset + 0)
			all_indices.append(vertex_offset + 1)
			all_indices.append(vertex_offset + 2)
			all_indices.append(vertex_offset + 0)
			all_indices.append(vertex_offset + 2)
			all_indices.append(vertex_offset + 3)
			
			vertex_offset += 4
	
	print("Total vertices: " + str(all_verts.size()) + ", Total indices: " + str(all_indices.size()))
	
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = all_verts
	arrays[Mesh.ARRAY_TEX_UV] = all_uvs
	arrays[Mesh.ARRAY_NORMAL] = all_normals
	arrays[Mesh.ARRAY_INDEX] = all_indices
	
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

# Apply textures to the block mesh
func _apply_block_textures(block_def: Dictionary) -> void:
	var textures = block_def.get("textures", ["", "", "", "", "", ""])
	var visible_faces = block_def.get("visible_faces", [true, true, true, true, true, true])
	var shape = block_def.get("shape", "")
	
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
	
	# For shaped blocks, use the same texture for all faces
	# For full blocks, use per-face textures
	var surface_count = _block_mesh.mesh.get_surface_count()
	
	for surface_index in range(surface_count):
		var face_tex: Texture2D = fallback_tex
		
		# For full blocks (not shaped), try to use per-face textures
		if shape.is_empty() and surface_index < 6:
			var face_index = surface_index
			if visible_faces[face_index]:
				var face_tex_name = textures[face_index]
				if not face_tex_name.is_empty():
					var face_tex_path: String = "res://textures/blocks/" + face_tex_name + ".png"
					if FileAccess.file_exists(ProjectSettings.globalize_path(face_tex_path)):
						var tex = load(face_tex_path) as Texture2D
						if tex != null:
							face_tex = tex
		
		if face_tex == null:
			continue  # Skip if no texture available
		
		var mat := StandardMaterial3D.new()
		mat.albedo_texture = face_tex
		mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		mat.roughness = 1.0
		mat.metallic = 0.0
		
		_block_mesh.set_surface_override_material(surface_index, mat)

# Clear the icon cache (call this if textures change)
func clear_cache() -> void:
	_icon_cache.clear()

# Test function - renders a few block icons and saves them to disk
func test_render_icons() -> void:
	print("Testing block icon rendering...")
	
	# Test common blocks including shaped ones
	var test_blocks = [1, 2, 3, 4]  # stone, dirt, grass, sand
	
	# Add some shaped blocks to test
	for block_id in range(_block_defs.size()):
		var block_def = _block_defs[block_id]
		var shape = block_def.get("shape", "")
		if not shape.is_empty():
			test_blocks.append(block_id)
			if test_blocks.size() >= 10:  # Limit to 10 total
				break
	
	for block_id in test_blocks:
		var icon = get_block_icon(block_id)
		if icon != null:
			var img = icon.get_image()
			var block_name = _block_defs[block_id].get("name", "unknown")
			var path = "user://test_icon_" + str(block_id) + "_" + block_name + ".png"
			img.save_png(path)
			print("Saved icon for block " + str(block_id) + " (" + block_name + ") to " + path)
		else:
			print("Failed to render icon for block " + str(block_id))
	
	print("Icon rendering test complete.")
