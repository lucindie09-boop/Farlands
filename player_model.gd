extends Node3D

@export var skin_texture: Texture2D = preload("res://skin.png")

signal texel_painted(px: int, py: int, old_color: Color, new_color: Color)

const UV_OVERLAY_SHADER: Shader = preload("res://shaders/skin_uv_overlay.gdshader")
const ATLAS_DIM := 64

var uv_overlay_enabled := false
var paint_color := Color.WHITE

var _uv_overlay_mat: ShaderMaterial
var _paint_image: Image
var _paint_texture: ImageTexture
var _anim_player: AnimationPlayer

func _manager():
	return get_node_or_null("/root/SkinManager")

func _ready():
	apply_skin_texture()
	_anim_player = $AnimationPlayer
	_setup_animations()

func _setup_animations():
	if _anim_player == null:
		return
	
	# Create an animation library
	var anim_library = AnimationLibrary.new()
	
	# Create idle animation - subtle breathing/bobbing
	var idle_anim = Animation.new()
	idle_anim.loop = true
	idle_anim.length = 2.0
	
	# Add track for root position (subtle bobbing)
	var root_track = idle_anim.add_track(Animation.TYPE_POSITION_3D)
	idle_anim.track_set_path(root_track, ".")
	idle_anim.track_insert_key(root_track, 0.0, Vector3.ZERO)
	idle_anim.track_insert_key(root_track, 1.0, Vector3(0, 0.02, 0))
	idle_anim.track_insert_key(root_track, 2.0, Vector3.ZERO)
	
	anim_library.add_animation("idle", idle_anim)
	
	# Create walk animation - more pronounced bobbing
	var walk_anim = Animation.new()
	walk_anim.loop = true
	walk_anim.length = 0.6
	
	# Add track for root position (walking bobbing)
	var walk_track = walk_anim.add_track(Animation.TYPE_POSITION_3D)
	walk_anim.track_set_path(walk_track, ".")
	walk_anim.track_insert_key(walk_track, 0.0, Vector3.ZERO)
	walk_anim.track_insert_key(walk_track, 0.15, Vector3(0, 0.05, 0))
	walk_anim.track_insert_key(walk_track, 0.3, Vector3.ZERO)
	walk_anim.track_insert_key(walk_track, 0.45, Vector3(0, 0.05, 0))
	walk_anim.track_insert_key(walk_track, 0.6, Vector3.ZERO)
	
	# Add track for root rotation (subtle sway)
	var rot_track = walk_anim.add_track(Animation.TYPE_ROTATION_3D)
	walk_anim.track_set_path(rot_track, ".")
	walk_anim.track_insert_key(rot_track, 0.0, Quaternion.IDENTITY)
	walk_anim.track_insert_key(rot_track, 0.15, Quaternion.from_euler(Vector3(0, 0.05, 0)))
	walk_anim.track_insert_key(rot_track, 0.3, Quaternion.IDENTITY)
	walk_anim.track_insert_key(rot_track, 0.45, Quaternion.from_euler(Vector3(0, -0.05, 0)))
	walk_anim.track_insert_key(rot_track, 0.6, Quaternion.IDENTITY)
	
	anim_library.add_animation("walk", walk_anim)
	
	# Add the library to the AnimationPlayer with a name
	_anim_player.add_animation_library("player_animations", anim_library)
	_anim_player.play("player_animations/idle")

func set_animation_state(is_walking: bool) -> void:
	if _anim_player == null:
		return
	
	var current_anim = _anim_player.current_animation
	var target_anim = "player_animations/walk" if is_walking else "player_animations/idle"
	
	if current_anim != target_anim:
		_anim_player.play(target_anim)

func set_uv_overlay(enabled: bool) -> void:
	uv_overlay_enabled = enabled
	if enabled and _uv_overlay_mat == null:
		_uv_overlay_mat = ShaderMaterial.new()
		_uv_overlay_mat.shader = UV_OVERLAY_SHADER
		_uv_overlay_mat.set_shader_parameter("cells", 64.0)
	for mesh_instance in find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for surface_index in range(mi.mesh.get_surface_count()):
			var material := mi.get_surface_override_material(surface_index)
			if material == null:
				material = mi.mesh.surface_get_material(surface_index)
			if material == null:
				continue
			material.next_pass = _uv_overlay_mat if enabled else null

func apply_skin_texture():
	# In-game the skin lives in the shared SkinManager (autoload), so the
	# in-game model and the skin-maker preview show the same editable texture.
	# Without the manager, fall back to the base skin texture.
	var mgr = _manager()
	var albedo = mgr.get_texture() if mgr != null else skin_texture
	var mesh_instances = find_children("", "MeshInstance3D", true, false)
	
	for mesh_instance in mesh_instances:
		# Get the mesh to check surface count
		var mesh = mesh_instance.mesh
		if mesh == null:
			continue
			
		# Iterate through all surfaces
		for surface_index in range(mesh.get_surface_count()):
			# Get the current material
			var material = mesh_instance.get_surface_override_material(surface_index)
			
			if material == null:
				# If no override material, try to get the surface material from the mesh
				material = mesh.surface_get_material(surface_index)
			
			if material != null and material is StandardMaterial3D:
				# Apply the skin texture to the albedo texture
				material.albedo_texture = albedo
				# The skin is a tightly-packed 64x64 pixel-art atlas (like the
				# block textures), so use nearest filtering with no mipmaps.
				# Otherwise linear/mipmap filtering blends texels across
				# neighboring UV islands, causing the smeared/aliased look on
				# angled or minified (side) faces.
				material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
				# Create an override material if one doesn't exist
				if mesh_instance.get_surface_override_material(surface_index) == null:
					mesh_instance.set_surface_override_material(surface_index, material)

func set_paint_color(color: Color) -> void:
	paint_color = color

func paint_texel(uv: Vector2, color: Color) -> void:
	# Nearest sampling: the texel whose span contains the sample point.
	var px := clampi(int(uv.x * ATLAS_DIM), 0, ATLAS_DIM - 1)
	var py := clampi(int(uv.y * ATLAS_DIM), 0, ATLAS_DIM - 1)
	var mgr = _manager()
	if mgr != null:
		var old: Color = mgr.get_image().get_pixel(px, py)
		if mgr.set_pixel(px, py, color):
			texel_painted.emit(px, py, old, color)
		return
	_ensure_paint_texture()
	var old_color := _paint_image.get_pixel(px, py)
	if old_color.is_equal_approx(color):
		return
	_paint_image.set_pixel(px, py, color)
	_paint_texture.update(_paint_image)
	texel_painted.emit(px, py, old_color, color)

func undo_texel(px: int, py: int, color: Color) -> void:
	var mgr = _manager()
	if mgr != null:
		mgr.set_pixel(px, py, color)
		return
	_ensure_paint_texture()
	px = clampi(px, 0, ATLAS_DIM - 1)
	py = clampi(py, 0, ATLAS_DIM - 1)
	if _paint_image.get_pixel(px, py).is_equal_approx(color):
		return
	_paint_image.set_pixel(px, py, color)
	_paint_texture.update(_paint_image)

func _ensure_paint_texture() -> void:
	if _paint_texture != null:
		return
	var img := skin_texture.get_image()
	if img == null:
		img = Image.load_from_file("res://skin.png")
	if img == null:
		return
	_paint_image = img.duplicate()
	_paint_texture = ImageTexture.create_from_image(_paint_image)
	# Swap every surface's albedo to the editable copy so the painted texels
	# show up across all UV islands at once.
	_swap_albedo_texture(_paint_texture)

# The currently painted image (guaranteed to be initialised from the base skin
# even if nothing has been painted yet) — the source of truth for saving.
func get_paint_image() -> Image:
	var mgr = _manager()
	if mgr != null:
		return mgr.get_image()
	_ensure_paint_texture()
	if _paint_image == null:
		return skin_texture.get_image()
	return _paint_image

# Replace the whole skin with a new image (e.g. loaded from a save). The shared
# texture is pointed at the new image so every model updates. Callers should
# reset undo history, which no longer matches the new pixels.
# Since saved skins are now clean (noise-free), this also resets the noise base
# so noise can be re-applied from the sidecar value after loading.
func load_skin_image(img: Image) -> void:
	if img == null:
		return
	var mgr = _manager()
	if mgr != null:
		mgr.set_from_image(img)
		# Reset noise base since the loaded image is clean (noise-free)
		if mgr.has_method("reset_noise_base"):
			mgr.reset_noise_base()
		_swap_albedo_texture(mgr.get_texture())
		return
	_paint_image = img.duplicate()
	_paint_texture = ImageTexture.create_from_image(_paint_image)
	_swap_albedo_texture(_paint_texture)

func apply_gray_noise(base: Image, noise_map: Image, amount: float) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("apply_gray_noise"):
		mgr.apply_gray_noise(base, noise_map, amount)

func set_noise(severity: float) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("set_noise"):
		mgr.set_noise(severity)

func fill_uv_rect(lo: Vector2, hi: Vector2, color: Color) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("fill_uv_rect"):
		mgr.fill_uv_rect(lo, hi, color)
		return
	fill_box_local(lo, hi, color)

func fill_box_local(lo: Vector2, hi: Vector2, color: Color) -> void:
	_ensure_paint_texture()
	if _paint_image == null:
		return
	var x0 := clampi(int(ceil(lo.x * ATLAS_DIM - 0.5)), 0, ATLAS_DIM - 1)
	var x1 := clampi(int(ceil(hi.x * ATLAS_DIM - 0.5)) - 1, 0, ATLAS_DIM - 1)
	var y0 := clampi(int(ceil(lo.y * ATLAS_DIM - 0.5)), 0, ATLAS_DIM - 1)
	var y1 := clampi(int(ceil(hi.y * ATLAS_DIM - 0.5)) - 1, 0, ATLAS_DIM - 1)
	if x1 < x0 or y1 < y0:
		return
	var changed := false
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			if not _paint_image.get_pixel(x, y).is_equal_approx(color):
				_paint_image.set_pixel(x, y, color)
				changed = true
	if changed and _paint_texture != null:
		_paint_texture.update(_paint_image)

func _swap_albedo_texture(tex: Texture2D) -> void:
	for mesh_instance in find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for surface_index in range(mi.mesh.get_surface_count()):
			var material := mi.get_surface_override_material(surface_index)
			if material == null:
				material = mi.mesh.surface_get_material(surface_index)
			if material is StandardMaterial3D:
				material.albedo_texture = tex
