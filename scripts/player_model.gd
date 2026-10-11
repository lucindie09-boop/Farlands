extends Node3D

@export var skin_texture: Texture2D = preload("res://textures/skin.png")

signal texel_painted(px: int, py: int, old_color: Color, new_color: Color)

const UV_OVERLAY_SHADER: Shader = preload("res://shaders/skin_uv_overlay.gdshader")
const ATLAS_DIM := 64
# The join between a clip's rig and this body's: scripts/clip_retarget.gd.
const ClipRetarget := preload("res://scripts/clip_retarget.gd")

# The world's own light model, the one the terrain, the dropped items and the
# first-person arm already run (shaders/item_lighting.gdshaderinc): the light of
# the CELL this body is in, rather than the engine's sky and ambient, which know
# nothing about caves, torches or the hour. Only for a body standing in the world
# -- see _init_world_lighting for why a preview does not get it.
const ITEM_SHADER: Shader = preload("res://shaders/item_shader.gdshader")
# The cell light is one value per block, so a body crossing a boundary -- or a
# cell being relit under it -- steps a whole cell's worth on a single frame. This
# is the time constant of the fade that turns that step into a move. The same
# rule, and the same number, as the other two consumers;
# probes/probe_item_light_smooth.gd holds all of them to it.
const LIGHT_EASE_TIME := 0.15

# The body's two clips, and the rig they are re-homed from. They are
# Animations/Idle_new.anim and Animations/Walk.anim, lifted out of
# models/minecraft-idle-and-walking-animation/source/model.gltf by
# tools/extract_model_animations.gd, and their tracks name THAT rig's nodes
# (`Node/Head2`, `Node/RightArm`, ...) in ITS units -- so they are re-homed onto
# this body's own parts on the way in (scripts/clip_retarget.gd). The mapping is
# by limb, and the sides are the two rigs' own: right is +x on both.
const CLIP_SOURCES := {
	"Idle": "res://Animations/Idle_new.anim",
	"Walk": "res://Animations/Walk.anim",
}
## ...and the sides are the CHARACTER's, which the x sign does not tell you. A rig
## that looks down -Z has its own right hand at +X (that is where the pack's
## `RightArm` sits), and a rig that looks down +Z -- this body -- has its right at
## -X. So turning a clip from the first onto the second inverts the sides as well:
## `RightArm` belongs on `arm`, not on `arm2`. Pairing by x sign instead is a
## MIRROR rather than a flip -- the same limbs, the other way round on the body,
## which is a walk that reads as a mirrored character rather than a turned one.
const CLIP_TRACK_MAP := {
	"Node/Head2": "head",
	"Node/Body2": "torso",
	"Node/RightArm": "arm",
	"Node/LeftArm": "arm2",
	"Node/RightLeg": "leg",
	"Node/LeftLeg": "leg2",
}
# How many of this body's units one of the clip rig's is: the body measures in
# Blockbench pixels (16 to the block, head pivot y=24) and model.gltf in blocks
# (head pivot y=1.5). Only a position track ever needs it -- a rotation is
# joint-relative and means the same thing on either rig.
const CLIP_UNIT_SCALE := 16.0
# The half turn between the clip rig's axes and this body's, and it is not
# optional: the pack's own model faces -Z while this body faces +Z. Measured off
# each rig's own skin rather than assumed -- a 64x64 skin's head has its face
# texels (the eyes) in the u,v [8,16) band, and that band sits on the -Z face of
# the head box in model.gltf and on the +Z face here. So the clips are lifted out
# of a rig looking the other way, and WITHOUT this they are a faithful copy of
# that rig's numbers seen from behind: the body walks backwards, feet leading
# away from its own face.
const CLIP_FRAME := Quaternion(Vector3.UP, PI)
# ...and the one limb a clip must not turn: the head tracks the player's AIM every
# frame (_track_head_look), so a clip that also wrote head.quaternion would win or
# lose by node order, one frame at a time. Its position track is still taken --
# that is the head's own bob, which nothing else writes.
const CLIP_CODE_OWNED_ROTATION := ["head"]
# Seconds a change of state takes to cross from one clip into the other.
const CLIP_BLEND := 0.15
# The clip the body played before it could walk: an arm-swing idle for an older
# rig, still what plays when the two above are missing, so a body is never left
# with no clip at all. (This is the file the extractor deliberately does not
# touch.)
const LEGACY_IDLE := "res://Animations/Idle.anim"

var uv_overlay_enabled := false
var paint_color := Color.WHITE
# When true the head never rotates (set by the K-key pose clone so the dummy
# is fully frozen instead of tracking the player's aim).
var skip_head_look := false

var _uv_overlay_mat: ShaderMaterial
var _paint_image: Image
var _paint_texture: ImageTexture
var _anim_player: AnimationPlayer
var _head: Node3D = null
# Which clip the body is in, so that being told the same state again costs
# nothing (see _play_state).
var _clip_state := ""

# The world light model, when this body is in the world: one material for every
# surface (the whole model is one skin), the cell's light eased between frames.
var _body_material: ShaderMaterial = null
var _body_meshes: Array[MeshInstance3D] = []
var _chunk_manager: Node = null
var _body_height := 0.0
var _body_light := Vector4.ZERO
var _body_light_ready := false

func _manager():
	return get_node_or_null("/root/SkinManager")

func _ready():
	# The light model first: it decides what the body is actually drawn WITH, and
	# apply_skin_texture has to know that before it picks where the skin goes.
	_init_world_lighting()
	apply_skin_texture()
	_anim_player = get_node_or_null("AnimationPlayer")
	if _anim_player != null:
		_load_clips()
	_head = find_child("head", true, false)


## The body's clips, re-homed onto its own parts and handed to its player as one
## library.
##
## Only ever called when there IS an AnimationPlayer: the K-key pose clone has
## none on purpose and stays frozen.
##
## Either clip missing falls back to the legacy Idle.anim rather than to no clip
## at all, and says which: a body with no clip is a body held in its rest pose,
## which reads as a broken model rather than as a missing file.
func _load_clips() -> void:
	var library := AnimationLibrary.new()
	for state in CLIP_SOURCES:
		var source: Animation = load(CLIP_SOURCES[state])
		if source == null:
			print("player_model: no clip at %s -- falling back to %s"
				% [CLIP_SOURCES[state], LEGACY_IDLE])
			_load_legacy_idle()
			return
		var clip := ClipRetarget.retarget(source, CLIP_TRACK_MAP, self, CLIP_UNIT_SCALE,
			CLIP_FRAME, CLIP_CODE_OWNED_ROTATION)
		var missing := ClipRetarget.unresolved(clip, self)
		if not missing.is_empty():
			print("player_model: %s turns nothing -- this body has no %s"
				% [CLIP_SOURCES[state], ", ".join(missing)])
		library.add_animation(state, clip)
	_anim_player.add_animation_library("default", library)
	_play_state("Idle")


## The pre-Walk body: one clip, written for an older rig, whose tracks are renamed
## by hand on the way in (arm_001/leg_001 are what that file calls this body's
## arm2/leg2). Still the fallback, so the body idles with no new clips on disk.
func _load_legacy_idle() -> void:
	var idle: Animation = load(LEGACY_IDLE)
	if idle == null:
		print("player_model: no body clip at all -- %s is missing too" % LEGACY_IDLE)
		return
	var fixed: Animation = idle.duplicate()
	for t in range(fixed.get_track_count()):
		var path := str(fixed.track_get_path(t))
		path = path.replace("arm_001", "arm2")
		path = path.replace("leg_001", "leg2")
		fixed.track_set_path(t, NodePath(path))
	var library := AnimationLibrary.new()
	library.add_animation("Idle", fixed)
	_anim_player.add_animation_library("default", library)
	_play_state("Idle")


## Move the body into one of its clips, fading across from whatever it was in.
##
## The controller tells the body the state it can see EVERY frame, so this has to
## be free to call and free to call again: a state already playing is left alone,
## because re-playing it restarts the clip -- and a walk restarted every frame is
## a walk frozen on its first pose.
func _play_state(state: String) -> void:
	var name := "default/" + state
	if not _anim_player.has_animation(name):
		return
	if _clip_state == state and _anim_player.is_playing():
		return
	_clip_state = state
	_anim_player.play(name, CLIP_BLEND)


func _process(delta: float) -> void:
	# Before the head-look guard: the pose clone sets skip_head_look, and it is
	# standing in the world like any other body, so it is lit like one.
	_push_world_lighting(delta)
	if skip_head_look:
		return
	_track_head_look()


## Give the body the world's own light model, when it is standing IN the world.
##
## A preview does not get it: the skin maker and the settings gallery put the
## model in a SubViewport with a world of their own, an orbiting camera and their
## own sun and fill (skin_preview.gd), where there is no world cell under the
## body to read a light from. Those keep the engine's lighting, which is what
## they are lit for. Everything else -- the player's own body, and the K-key pose
## clone -- is in the real world, and is lit by it.
##
## All of the model is one skin, so all of it gets ONE material: one albedo, one
## set of world uniforms, and one light value for the body it draws.
func _init_world_lighting() -> void:
	if get_viewport() is SubViewport:
		return
	_chunk_manager = _world_chunk_manager()
	if _chunk_manager == null:
		return
	for child in find_children("", "MeshInstance3D", true, false):
		var mi := child as MeshInstance3D
		if mi != null and mi.mesh != null:
			_body_meshes.append(mi)
	if _body_meshes.is_empty():
		_chunk_manager = null
		return

	_body_material = ShaderMaterial.new()
	_body_material.shader = ITEM_SHADER
	_body_material.set_shader_parameter("albedo_texture", _skin_albedo())
	# Closed opaque boxes: there is no sprite texel here to cut away.
	_body_material.set_shader_parameter("alpha_scissor", 0.0)
	for mi in _body_meshes:
		for surface in range((mi.mesh as Mesh).get_surface_count()):
			mi.set_surface_override_material(surface, _body_material)
	# The middle of the body, from its own geometry rather than a constant: the
	# light is read at that height (see _push_world_lighting).
	_body_height = _body_centre_height()


## The ChunkManager of the world this body is in, if any. The player's body is a
## child of the Player node, so the manager is found by the same route the
## controller and the dropped items use.
func _world_chunk_manager() -> Node:
	var cm := get_node_or_null("/root/Main/ChunkManager")
	if cm != null:
		return cm
	var scene := get_tree().current_scene
	if scene != null:
		return scene.get_node_or_null("ChunkManager")
	return null


## How far above the model's origin -- the feet, for both the player's body and
## the pose clone -- the middle of the body is, in the model's own units. The
## union of the parts' own boxes, so a model changed in Blockbench moves this
## with it instead of leaving a stale number behind.
func _body_centre_height() -> float:
	var total := AABB()
	var any := false
	for mi in _body_meshes:
		var box: AABB = mi.transform * (mi.mesh as Mesh).get_aabb()
		total = box if not any else total.merge(box)
		any = true
	return total.get_center().y if any else 0.0


## The world light half of the item model, onto the body's material, plus the
## light of the cell the body is in per mesh instance. Every frame: the day/night
## values move continuously, and a walking body crosses cells it must be re-read
## from.
##
## The cell light is the one part of that which does not move continuously, so
## `delta` is what turns its step into a short fade (LIGHT_EASE_TIME) -- the same
## model, applied the same way, as the dropped items and the first-person arm.
##
## The light is read at the body's own middle, one cell for the whole body: a
## body is not shaded limb by limb, and its middle is the cell it is most in (a
## body standing on a slab has its feet in the slab's own cell and its middle in
## the air above it, which is where the light is).
func _push_world_lighting(delta: float) -> void:
	if _body_material == null or _chunk_manager == null:
		return
	_chunk_manager.apply_item_lighting(_body_material)
	# ...but NOT the world's midday tint. The `sky_light_warmth` the line above
	# just wrote is a cream even at noon (the world wants it: it is baked into the
	# sky, the fog and the terrain alike), and this model is the one sky-lit
	# surface in the world with nothing between the light and its albedo to hide a
	# cast in -- a cream that reads as nothing on grass reads as orange on a pale
	# skin. The body takes the sun's own colour instead, which is WHITE overhead
	# and still warm at the horizon, so a sunset lights it exactly as it lights the
	# ground (the two curves land on the same colour there). Written after the
	# world's value, because that value is rewritten every frame.
	# probes/probe_body_light.gd reads both materials.
	var warmth: Color = _chunk_manager.get_body_sky_warmth()
	_body_material.set_shader_parameter("sky_light_warmth", warmth)
	var at := to_global(Vector3(0.0, _body_height, 0.0))
	var cell: Vector4 = _chunk_manager.get_light_at(floori(at.x), floori(at.y), floori(at.z))
	if not _body_light_ready:
		# The first frame has nothing to ease FROM: land on the cell's own value
		# instead of fading in from a black that was never on screen.
		_body_light = cell
		_body_light_ready = true
	else:
		_body_light = _ease_light(_body_light, cell, delta)
	for mi in _body_meshes:
		mi.set_instance_shader_parameter("item_light", _body_light)


## Framerate-independent exponential ease toward `target`: LIGHT_EASE_TIME is the
## time constant, so a step takes the same time to play out at any frame rate.
## probes/probe_item_light_smooth.gd holds this rule, for all of its consumers, to
## the value the shader is actually handed.
func _ease_light(current: Vector4, target: Vector4, delta: float) -> Vector4:
	if LIGHT_EASE_TIME <= 0.0:
		return target
	return current.lerp(target, 1.0 - exp(-delta / LIGHT_EASE_TIME))


## The skin in use: the shared SkinManager's texture in game, so the body and the
## skin maker show the same editable pixels, and the exported fallback otherwise.
func _skin_albedo() -> Texture2D:
	var mgr = _manager()
	return mgr.get_texture() if mgr != null else skin_texture

# Minecraft-style head look: the head follows the player's LOOK direction
# (mouse yaw+pitch), never the camera. The camera is placed on the look ray in
# every view, so in the back view (mode 1) its forward coincides with the look
# ray and copying it was harmless — but the front view (mode 2) camera is
# yaw-flipped 180 degrees to look back at the face, so copying IT spun the
# head a full 180 and showed the back of the head. The vanilla model drives the head
# from yaw/pitch state instead, so we rebuild the look basis
# from the controller's yaw (its world basis) + pitch (the aim direction) and
# fold in the 180-degree offset that maps the model's +Z face onto the look
# ray. Unwinding the parent's global transform makes it work regardless of the
# model's scale, export flip, body-yaw lag or the player's yaw. Runs on the
# real (third-person) model and the pose clone alike. Nothing fights it: the
# clips' own head ROTATION tracks are dropped on the way in (CLIP_CODE_OWNED_
# ROTATION), and only the head's position track -- its bob -- is left to them.
func _look_controller() -> Node:
	# The live model lives under PlayerController's ModelPivot, so its ancestor
	# chain has the controller. The pose clone is a detached copy under a plain
	# Node3D — it has no controller of its own, so fall back to the scene's
	# live Player node (whose look all these heads mirror).
	var n: Node = self
	while n != null:
		if n.has_method("get_aim_direction"):
			return n
		n = n.get_parent()
	var scene_root := get_tree().current_scene
	if scene_root != null:
		var player := scene_root.get_node_or_null("Player")
		if player != null and player.has_method("get_aim_direction"):
			return player
	return null

func _track_head_look() -> void:
	if _head == null:
		return
	var ctrl := _look_controller()
	var parent_q: Quaternion = _head.get_parent().global_transform.basis.get_rotation_quaternion()
	if ctrl != null:
		# Pitch from the aim direction expressed in the controller's own frame:
		# aim = ctrl_basis * rotX(pitch) * (0,0,-1), so local.y == sin(pitch) and
		# look_q = ctrl_q * rotX(pitch) reproduces the exact basis the camera gets
		# in modes 0/1 — view-independent by construction.
		var ctrl_q: Quaternion = (ctrl as Node3D).global_transform.basis.get_rotation_quaternion()
		var dir: Vector3 = ctrl.get_aim_direction()
		var local: Vector3 = (ctrl_q.inverse() * dir).normalized()
		var pitch := asin(clampf(local.y, -1.0, 1.0))
		var look_q: Quaternion = ctrl_q * Quaternion(Vector3.RIGHT, pitch)
		_head.quaternion = parent_q.inverse() * look_q * Quaternion(Vector3.UP, PI)
	else:
		# No gameplay controller (skin-maker preview, menu screens): follow the
		# viewport camera as before — those cameras orbit the model and never
		# get the yaw-flipped front-view treatment, so copying is correct here.
		var cam := get_viewport().get_camera_3d()
		if cam == null:
			return
		var cam_q: Quaternion = cam.global_transform.basis.get_rotation_quaternion()
		_head.quaternion = parent_q.inverse() * cam_q * Quaternion(Vector3.UP, PI)

## The player's body in its two states, called every frame by the controller
## (PlayerController::update_player_animation) with whether it is moving on the
## ground. Walk where there is a walk to play, Idle otherwise -- so a body on the
## legacy clip idles instead of asking for an animation it does not have.
func set_animation_state(is_walking: bool) -> void:
	if _anim_player == null:
		return
	var state := "Walk" if is_walking else "Idle"
	if not _anim_player.has_animation("default/" + state):
		state = "Idle"
	_play_state(state)

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
	var albedo := _skin_albedo()
	if _body_material != null:
		# In the world the body is one material of the world's own light model
		# (see _init_world_lighting), and the skin is a uniform of it: there is
		# no StandardMaterial3D here, and nothing of the engine's lighting for
		# one to be the albedo of.
		_body_material.set_shader_parameter("albedo_texture", albedo)
		return
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
		img = Image.load_from_file("res://textures/skin.png")
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
	if _body_material != null:
		# Same reason as apply_skin_texture: in the world the albedo is a uniform
		# of the one material every surface of the body is drawn with.
		_body_material.set_shader_parameter("albedo_texture", tex)
		return
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
