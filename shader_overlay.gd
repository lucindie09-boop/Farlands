extends Control

# The shader effects' live state, and the layers that draw them.
#
# data/shaders.json lists the effects, in that file's order, and this node brings
# each one up:
#
#   - a 'screen' effect gets a child ColorRect and a ShaderMaterial of its own,
#     and passes the frame already drawn beneath it (the world, plus whichever
#     HUD art this node sits above in Main.tscn) through its shader. One that is
#     off is simply hidden, so an off stack costs nothing but a hidden Control.
#   - a 'world' effect is the same layer and the same shader, drawn somewhere
#     else: at a negative z_index, which is over the 3D world and under every
#     other CanvasItem in the HUD. So it reads the world with no HUD in it and
#     what it writes is put back under the HUD, while a screen effect grades the
#     finished screenful. That is the whole difference between the two kinds on
#     this side - an outline belongs on the rock and not on the hotbar - and it is
#     a difference the shaders themselves cannot express, because a shader is
#     handed one frame and has no idea where in the stack it was drawn.
#   - a 'vertex' effect gets no rect, because it is not a picture of the world but
#     the world: it names the materials the world is drawn with (the registry's
#     `materials`), and this node moves *their* uniforms. Those materials are
#     loaded rather than instantiated, so they are the very resources the engine
#     draws with. Such an effect has nothing to hide, so its switch is pushed as
#     a uniform as well (`enable_key`), and the shaders multiply the effect by
#     nothing when it is off.
#   - a 'camera' effect is not a picture either and gets no layer: it moves the
#     camera itself, which no shader can do - a shader is handed a frame after the
#     eye has seen it, and moving the picture after the fact is not the same as
#     moving the eye (it would not move what the frustum chose, only smear it).
#     There is no shader to load and nothing to draw, so the overlay runs the
#     effect itself in _process, writing the movement where the engine reads it -
#     the camera's own offset fields - and undoing the previous frame's write
#     before each new one, so an effect switched off leaves the camera exactly as
#     it found it. Its params are plain state, checked against the registry's
#     keys rather than against shader uniforms, because there are none.
#
# A pass may also ask for the frame *before* the one it is drawing into: a shader
# that declares the `previous_frame` uniform is handed the frame as it stood where
# the pass sits, one frame ago, out of a pair of viewports this node keeps for it.
# That is the one thing a reading pass cannot get for itself - the engine's screen
# copy is of the frame being drawn - and it is what an after-image, a trail or any
# accumulate-and-fade effect is made of. See _build_history and _advance_histories.
#
# The registry also lists the kinds themselves (`kinds`), in the order the
# Shaders page shows them and with the name each category is given there, which
# is what `get_kinds` hands the menu: the page is grouped by the kind of effect,
# and a kind with nothing in it is not shown at all, so the next kind is an entry
# in that list and a shader rather than a page of menu code.
#
# Every pass also carries its own BackBufferCopy, and that is what lets a stack of
# them be a stack. A shader reads the frame through a copy of it, and Godot makes
# one copy per frame - at the first node that reads the screen, wherever that node
# is. A second reading pass is therefore handed the frame as it was *before* the
# first one drew, and since a pass writes its picture back over the whole screen,
# it paints that older frame back: the pass before it disappears, and so does
# every HUD item drawn between the two. That is the one thing a stack of these
# cannot be assembled out of without saying so, so each pass puts a copy node
# immediately before itself and reads the frame as drawn up to where it sits. See
# _build_pass, and WORLD_PASS_Z_INDEX below for where a world pass sits.
#
# This node owns the state but not the file: settings_menu.gd is the one that
# reads and writes user://settings.cfg, so the Shaders page calls in here for
# every value it shows and every value it changes.
#
# The uniform a param drives is the param's own `key`, so a shader's uniforms,
# data/shaders.json and the saved config keys all carry the same names.

const DEFINITIONS_PATH := "res://data/shaders.json"

# Where the world's engine lives, for the one thing a vertex effect needs from it
# that a shader cannot do itself: the chunks are culled against boxes that say
# where the world *was*, because a vertex shader runs after the engine has
# decided what to draw. A bent or curved world would therefore have its far field
# culled away. Each effect's own knobs go to the engine as well as to its
# materials, and the engine grows those boxes to where the shaders will put the
# geometry - see src/core/world_cull.hpp and src/mesh/mesh_manager_cull.cpp.

# The knobs each vertex effect hands the engine, in the order that effect's own
# method takes them: an effect's `id` names the method it is told through
# (`bend` -> set_world_bend, `horizon` -> set_world_horizon), and this list is
# what the culling arithmetic needs of it, which is not the same list the menu
# offers. The water's ripple is a knob the menu offers and the culling has no use
# for: it moves the surface sideways, and the slack the box already carries is
# wider than its whole slider.
const VERTEX_ENGINE_KNOBS := {
	"bend": ["world_bend", "world_bend_radius", "world_bend_rise"],
	"horizon": ["world_horizon_radius"],
}

# Where a world effect's layer is drawn: under every other CanvasItem in the
# layer, and therefore over the 3D world, which is drawn before the HUD's canvas
# at all. A screen effect is drawn wherever its rect lands in this node, which is
# above the HUD art the node sits over and under the menu, the crosshair and the
# compass above it - so the two kinds see different frames, and that is the whole
# of what the registry's third kind means here.
const WORLD_PASS_Z_INDEX := -1

# The z a screen effect's rect gets. Spelled out because its copy node has to be
# ordered with it rather than left at the engine's default, and because the world
# pass's index above is the only other one there is.
const SCREEN_PASS_Z_INDEX := 0

# The uniform a pass declares to be handed the frame before the one it draws into.
# Declaring it is the whole contract: the name is what this node looks for, and a
# shader that declares it is given a viewport holding that frame.
const PREVIOUS_FRAME_UNIFORM := "previous_frame"


var _definitions: Array = []
# The registry's own list of kinds, in the menu's order: the Shaders page groups
# its rows by them and takes each category's name from here.
var _kinds: Array = []
# id -> { kind, rect, materials, uniforms, enable_key }. A screen or world
# effect has a rect of its own to hide; a vertex effect has neither, and its
# switch is a uniform on the materials it drives.
var _layers: Dictionary = {}
var _enabled: Dictionary = {}  # id -> bool
var _values: Dictionary = {}   # id -> { key: value }
# The world's engine, if there is one in this scene. Null in a scene without one
# (a probe that builds its own world), which is why nothing here is required to
# be present.
var _world: Node = null
# A single black pixel, made once and handed to any pass that has no frame behind it
# yet. See _blank_frame.
var _blank: Texture2D = null


func _ready() -> void:
	# The layers are decoration: the pointer belongs to whatever HUD art is under
	# them, exactly like the god-rays overlay.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	# The anchors alone are not enough: this node's parent is a CanvasLayer, not a
	# Control, and a full-rect preset on it has been seen to leave the node at zero
	# size - which is a whole stack of zero-sized rects, drawing nothing, with no
	# error to say so. So the frame is set here as well, and kept in step below.
	size = get_viewport().get_visible_rect().size
	_load_definitions()
	for definition in _definitions:
		_build_layer(definition)
	_push_frame_size()
	# The patterns a shader measures are counted in frame pixels, so a resized
	# window has to tell them how many there are now.
	get_viewport().size_changed.connect(_push_frame_size)
	# Deferred, because a sibling's `_ready` may not have run yet and the engine
	# is a sibling. Pushed once here even though nothing has changed: the engine
	# starts with both vertex effects off and has to be told that they exist at
	# all.
	_push_vertex_effects.call_deferred()


# --- The registry -------------------------------------------------------------

func get_definitions() -> Array:
	return _definitions


func get_shader_ids() -> Array:
	var ids: Array = []
	for definition in _definitions:
		ids.append(String(definition.get("id", "")))
	return ids


## The kinds of effect the registry knows, in the order the Shaders page lists
## them: [{ kind, name }, ...]. The menu is the registry's own shape, which is
## what makes a third kind an entry there rather than a page of code here.
func get_kinds() -> Array:
	return _kinds


func get_shader_name(id: String) -> String:
	for definition in _definitions:
		if String(definition.get("id", "")) == id:
			return String(definition.get("name", id))
	return id


# --- Turned on, turned off ----------------------------------------------------

func is_enabled(id: String) -> bool:
	return bool(_enabled.get(id, false))


func set_enabled(id: String, enabled: bool) -> void:
	if not _layers.has(id):
		return
	_enabled[id] = enabled
	var layer: Dictionary = _layers[id]
	var rect: ColorRect = layer.get("rect", null)
	if rect != null:
		rect.visible = enabled
		var history: Dictionary = layer.get("history", {})
		if not history.is_empty():
			# Switched on, the pass spends one frame with nothing behind it, so that
			# the trail starts from the frame it was switched on in rather than from
			# whatever was on screen the last time it was on; switched off, what it
			# was fed is blanked. Both are done in _advance_histories, which is where
			# the two viewports a pass reads and writes swap over.
			history["priming"] = 1 if enabled else 0
			if not enabled:
				_set_previous_frame(layer, _blank_frame())
		# An off pass makes no copy, so it costs nothing and does not stand
		# between an on pass unchanged from the pass before it. Hidden is
		# enough - a hidden CanvasItem is not processed at all - and the mode is
		# cleared too, so a copy that is somehow still reached copies nothing.
		var copy: BackBufferCopy = layer.get("copy", null)
		if copy != null:
			copy.visible = enabled
			copy.copy_mode = BackBufferCopy.COPY_MODE_VIEWPORT if enabled \
				else BackBufferCopy.COPY_MODE_DISABLED
	else:
		# A vertex effect is not something that can be hidden: it is the geometry
		# itself, and what the switch means there is the effect's own strength
		# multiplied by nothing. A camera effect is the same in kind - the switch
		# is nothing to draw or hide, only state the driver reads in _process.
		_push(layer, String(layer.get("enable_key", "")), 1.0 if enabled else 0.0)
		if String(layer.get("kind", "screen")) == "camera":
			# Off, the write of the frame before is undone now rather than waiting
			# for a _process that will not reach it again; on, the offsets are
			# already whatever the driver left them, which is zero.
			if not enabled:
				_camera_restore(layer)
		_push_vertex_effects.call_deferred()


# --- Param values -------------------------------------------------------------

func get_value(id: String, key: String) -> Variant:
	var values: Dictionary = _values.get(id, {})
	return values.get(key, null)


func set_value(id: String, key: String, value: Variant) -> void:
	if not _values.has(id):
		return
	_values[id][key] = value
	_push(_layers.get(id, {}), key, value)
	_push_vertex_effects.call_deferred()


# Every vertex effect's own state, handed to the engine as well as to its
# materials, because the engine culls the chunks against boxes that describe where
# the world was: a shader cannot widen them and the engine cannot read a shader.
# One walk over the registry tells each effect's method what its own knobs are;
# a vertex effect whose knobs the engine has no method for is skipped here rather
# than pushed into a method that does not exist. Deferred by the callers rather
# than pushed straight away, because this is reached once per row while the menu
# is being built and taking it costs the engine a walk over every resident chunk.
func _push_vertex_effects() -> void:
	if not _can_take_vertex_effects(_world):
		_world = _find_world()
		if _world == null:
			return
	for definition in _definitions:
		if String(definition.get("kind", "screen")) != "vertex":
			continue
		var id := String(definition.get("id", ""))
		var method := "set_world_" + id
		if not _world.has_method(method):
			push_error("shader_overlay: the vertex effect '%s' needs %s and the engine has no such method" % [id, method])
			continue
		var values: Dictionary = _values.get(id, {})
		var args: Array = [bool(_enabled.get(id, false))]
		for key in VERTEX_ENGINE_KNOBS.get(id, []):
			args.append(float(values.get(key, 0.0)))
		_world.callv(method, args)


# The methods the engine has to have: the registry's vertex effects, each named by
# its own id. A node is the world's engine for this purpose only when it can take
# all of them, because requiring every one rather than any one is what makes a
# half-wired engine a node that is not found - rather than a node that silently
# stops growing boxes for the effect it forgot.
func _vertex_methods() -> Array:
	var methods: Array = []
	for definition in _definitions:
		if String(definition.get("kind", "screen")) == "vertex":
			methods.append("set_world_" + String(definition.get("id", "")))
	return methods


func _can_take_vertex_effects(node: Node) -> bool:
	if node == null or not is_instance_valid(node):
		return false
	var methods := _vertex_methods()
	if methods.is_empty():
		return false
	for method in methods:
		if not node.has_method(method):
			return false
	return true


# The engine is a node in the same scene, but not at a fixed depth: this is a
# child of the HUD, so a sibling of the HUD is an uncle of this, and a path that
# was right yesterday is a path that silently stops working today. So it is found
# by asking: walk outwards from here and take the first node that can take the
# vertex effects at all. Cached once found (the outer test re-checks it every
# push, so a freed engine is re-found rather than quietly dropped).
func _find_world() -> Node:
	var node: Node = self
	while node != null:
		if _can_take_vertex_effects(node):
			return node
		for child in node.get_children():
			if _can_take_vertex_effects(child):
				return child
		node = node.get_parent()
	return null


# Only push what the target actually declares, so a mistyped key in the registry
# is reported once at load (below) instead of erroring every frame. A vertex
# effect's params can be declared by any of its materials - the water's ripple
# is on the water and not on the terrain - so each one is pushed to whichever of
# them has it.
func _push(layer: Dictionary, key: String, value: Variant) -> void:
	if layer.is_empty() or key == "":
		return
	var uniforms: Dictionary = layer["uniforms"]
	if not uniforms.has(key):
		return
	# `target` and not `material`: a local by that name shadows a property of this
	# node's own class, which the engine warns about at load.
	for target in layer["materials"]:
		(target as ShaderMaterial).set_shader_parameter(key, value)


# --- Building the stack -------------------------------------------------------

func _load_definitions() -> void:
	var file := FileAccess.open(DEFINITIONS_PATH, FileAccess.READ)
	if file == null:
		push_error("Failed to load shaders.json")
		return
	var json_text := file.get_as_text()
	file.close()

	var json := JSON.new()
	var parse_err := json.parse(json_text)
	if parse_err != OK:
		push_error("Failed to parse shaders.json: " + json.get_error_message())
		return

	_definitions = json.data.get("shaders", []) if json.data is Dictionary else []
	_kinds = json.data.get("kinds", []) if json.data is Dictionary else []


func _build_layer(definition: Dictionary) -> void:
	var id := String(definition.get("id", ""))
	if id == "":
		push_error("shaders.json: an effect has no id")
		return
	var kind := String(definition.get("kind", "screen"))
	var built: Dictionary
	if kind == "vertex":
		built = _build_vertex_effect(definition)
	elif kind == "camera":
		built = _build_camera_effect(definition)
	else:
		built = _build_pass(definition, kind, id)
	if built.is_empty():
		return

	_enabled[id] = bool(definition.get("enabled", false))
	_values[id] = {}
	for param in definition.get("params", []):
		var key := String(param.get("key", ""))
		if key == "":
			continue
		if not (built["uniforms"] as Dictionary).has(key):
			push_warning("shaders.json: %s has no uniform %s" % [id, key])
		var value: Variant = param.get("default", 0.0)
		_values[id][key] = value
		_push(built, key, value)

	_layers[id] = built
	set_enabled(id, _enabled[id])


# A pass over a frame the node can see: a full-screen rect of its own with the
# entry's shader on it, plus the copy node that hands it that frame. Whatever the
# shader writes is the picture, alpha and all, so the rect's own colour only has
# to not be drawn over it before the shader runs. A screen effect and a world
# effect are the same layer built the same way and differ by the one line at the
# bottom - the world's rect (and its copy) is put at the world's depth, so it sees
# the frame without the HUD and is covered by it.
func _build_pass(definition: Dictionary, kind: String, id: String) -> Dictionary:
	var path := String(definition.get("shader", ""))
	if path == "" or not ResourceLoader.exists(path):
		push_error("shaders.json: %s has no shader at %s" % [id, path])
		return {}
	var shader: Shader = load(path)
	if shader == null:
		push_error("shaders.json: %s failed to load %s" % [id, path])
		return {}

	# Where this pass and everything that belongs to it is drawn: a world pass at the
	# world's depth, a screen pass above the HUD art this node covers.
	var pass_z := WORLD_PASS_Z_INDEX if kind == "world" else SCREEN_PASS_Z_INDEX

	# The copy this pass reads, added first so it is processed first: same
	# z_index as the rect, and tree order breaks the tie. It is the whole of what
	# a pass needs to see the frame as it stands where the pass is - the world
	# alone for a world pass, the world plus the passes and HUD art under a screen
	# pass - and it is why two passes no longer fight over one shared copy.
	var copy := BackBufferCopy.new()
	copy.name = id + "BackBuffer"
	copy.copy_mode = BackBufferCopy.COPY_MODE_VIEWPORT
	copy.z_index = pass_z
	add_child(copy)

	var rect := ColorRect.new()
	rect.name = id
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	rect.color = Color(0, 0, 0, 0)
	rect.z_index = pass_z
	var shader_material := ShaderMaterial.new()
	shader_material.shader = shader
	rect.material = shader_material
	add_child(rect)

	# A pass that declares the previous frame is given two of them, and a blank to
	# start from: the pair is what turns "this frame" into "the frame before it", and
	# the blank is what makes switching an after-image on start the trail from the
	# frame it was switched on in. See _build_history and _advance_histories.
	var uniforms := _uniform_names(shader)
	var history: Dictionary = {}
	if uniforms.has(PREVIOUS_FRAME_UNIFORM):
		history = _build_history(id)
		shader_material.set_shader_parameter(PREVIOUS_FRAME_UNIFORM, _blank_frame())

	return {
		"kind": kind,
		"rect": rect,
		"copy": copy,
		"history": history,
		"materials": [shader_material],
		"uniforms": uniforms,
		"enable_key": "",
	}


# The frames behind a pass that asked for them. A shader cannot see these for
# itself: the screen copy the engine makes for a reading pass is of the frame *being
# drawn*, taken where the pass sits, so the only frame a pass can look at is its own.
# What this node can do instead is keep viewports whose whole content is the main
# viewport's own texture, and hand one over as an ordinary sampler.
#
# There are two of them, and they swap over every frame, because a viewport holding
# the screen captures it as that screen is *at that moment*, which is this frame -
# and a pass needs the one before. So while one captures the frame being drawn, the
# other is stopped and handed over as it stands, holding the frame before; the next
# frame they trade places. One frame behind is the whole of the trick, and it is
# also why the capture sits after the pass in the tree rather than before it: what a
# pass has to age is the picture it drew a moment ago, this pass's own after-image
# included. Taken before the pass, the frame handed over would have no trail in it,
# and the trail would be two frames long instead of building up.
#
# It is a blit of a frame that already exists, so it is cheap; it is stopped with its
# effect, so an after-image that is off costs nothing at all; and there is a pair per
# pass, so two passes wanting the previous frame each age their own ghost instead of
# sharing one. What the frame handed over holds is the frame as drawn where the pass
# sits - the screen, this node's own passes and the HUD under it, and for a world
# pass the world alone, since the HUD over it is drawn later.
func _build_history(id: String) -> Dictionary:
	var views: Array = []
	for suffix in ["A", "B"]:
		var view := SubViewport.new()
		view.name = id + "History" + suffix
		view.size = Vector2i(get_viewport().get_visible_rect().size)
		view.render_target_update_mode = SubViewport.UPDATE_DISABLED
		add_child(view)
		# A viewport is not a Control, so the rect inside it is placed and sized by
		# hand rather than anchored: filling the viewport is the whole of its job.
		var copied := TextureRect.new()
		copied.name = "Frame"
		copied.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		copied.stretch_mode = TextureRect.STRETCH_SCALE
		copied.size = Vector2(view.size)
		copied.texture = get_viewport().get_texture()
		view.add_child(copied)
		views.append(view)
	return {"views": views, "writing": 0, "priming": 0}


# The half of the history pair that a pass is not reading, advanced every frame: the
# one that was handed over last frame captures this one, and the other one freezes
# with what it captured, which is what makes it the frame before this one. Runs in
# _process, so the modes and the uniform are in place before the frame is drawn.
func _advance_histories() -> void:
	for id in _layers:
		var layer: Dictionary = _layers[id]
		var history: Dictionary = layer.get("history", {})
		if history.is_empty():
			continue
		var views: Array = history["views"]
		if not bool(_enabled.get(id, false)):
			for view in views:
				(view as SubViewport).render_target_update_mode = SubViewport.UPDATE_DISABLED
			_set_previous_frame(layer, _blank_frame())
			continue
		if int(history["priming"]) > 0:
			# One frame with a blank behind it, both viewports filling at once: the
			# trail starts from the frame the effect was switched on in.
			history["priming"] = int(history["priming"]) - 1
			for view in views:
				(view as SubViewport).render_target_update_mode = SubViewport.UPDATE_ALWAYS
			_set_previous_frame(layer, _blank_frame())
			continue
		var writing := int(history["writing"])
		var holding := views[1 - writing] as SubViewport
		holding.render_target_update_mode = SubViewport.UPDATE_DISABLED
		(views[writing] as SubViewport).render_target_update_mode = SubViewport.UPDATE_ALWAYS
		_set_previous_frame(layer, holding.get_texture())
		history["writing"] = 1 - writing


func _process(_delta: float) -> void:
	_advance_histories()
	# Camera effects are movements of the eye rather than pictures, so their
	# driver runs here, where the engine is between frames and the write of the
	# frame before can be undone before the next one is made.
	for id in _layers:
		var layer: Dictionary = _layers[id]
		if String(layer.get("kind", "screen")) != "camera":
			continue
		if bool(_enabled.get(id, false)):
			_process_camera_effect(layer, id, _delta)
		else:
			_camera_restore(layer)


# The one uniform a history pass is handed every frame rather than once: which of
# its two viewports holds the frame before this one changes with the frame.
func _set_previous_frame(layer: Dictionary, frame: Texture2D) -> void:
	for target in layer.get("materials", []):
		(target as ShaderMaterial).set_shader_parameter(PREVIOUS_FRAME_UNIFORM, frame)


# A single black pixel, for a pass with nothing behind it yet. A picture that takes
# the brighter of two is the picture itself against black, so a pass with this behind
# it draws exactly what it was handed - which is what makes switching an after-image
# on start clean instead of replaying a frame from minutes ago.
func _blank_frame() -> Texture2D:
	if _blank == null:
		var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
		image.fill(Color(0, 0, 0, 1))
		_blank = ImageTexture.create_from_image(image)
	return _blank

# A camera effect: no rect, no shader, no materials - a movement of the camera
# itself, which this node runs in _process and writes where the engine reads it:
# the camera's own h_offset/v_offset. Those two fields displace the eye in view
# space without touching where it looks, so nothing that steers the camera (the
# player's mouse look, the head-bob, the frustum) needs to know this is here, and
# nothing of theirs is ever overwritten: only what this effect itself wrote is
# ever taken back off.
func _build_camera_effect(_definition: Dictionary) -> Dictionary:
	return {
		"kind": "camera",
		"rect": null,
		"materials": [],
		# The keys this effect is allowed to hold, spelled out because there is no
		# shader to ask: the params of a camera effect drive nothing, they are the
		# state the driver reads. One list for every camera effect, because the
		# builder is shared and the driver picks by id which of the keys are its
		# own.
		"uniforms": {"camera_jitter_strength": true, "camera_jitter_rate": true},
		"enable_key": "",
		# The driver's own state: what was written last frame, so it can come back
		# off; and where the jitter's step stands, so the steps are held and re-
		# rolled rather than re-rolled every frame, which would be noise, not a
		# jitter.
		"applied": Vector2.ZERO,
		"clock": 0.0,
		"offset": Vector2.ZERO,
	}


# The camera effects' driver, run from _process for every enabled one. Camera
# Jitter is a rattle: a fresh pair of offsets in -1..1 rolled every
# `camera_jitter_rate` of a second and HELD until the next roll, so the eye
# lands somewhere and stays there a moment - a re-roll every frame would be
# noise, not a jitter. Steps are eased into over two or three frames rather than
# jumped, so the rattle does not tick like a metronome. The motion is read out
# of its own function and written additively onto the camera's own
# h_offset/v_offset, with the write of the frame before taken back off first, so
# a second camera effect is a motion function, the line that dispatches it and
# its keys in the shared list: the write, the restore and the easing are the
# same for all of them. The sizes
# are the strengths themselves - metres of eye displacement, with the default
# well under a block's apparent size at arm's length - and a strength of zero
# writes nothing, so an enabled but zeroed row is free.
func _process_camera_effect(layer: Dictionary, id: String, delta: float) -> void:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		_camera_restore(layer)
		return
	var values: Dictionary = _values.get(id, {})
	var motion := Vector2.ZERO
	if id == "camera_jitter":
		motion = _camera_jitter_motion(layer, values, delta)
	if motion == Vector2.ZERO:
		# Nothing this frame (and, with a zero strength, ever): the restore below
		# still runs, which is what keeps an enabled row with no motion free.
		_camera_restore(layer)
		return
	# The write of the frame before is taken back off FIRST, and before the new
	# one is stored - a restore that read the layer after it was updated would
	# subtract the step it is about to write rather than the one it wrote, which
	# leaves the camera holding it with nothing left to undo. `previous` is kept
	# across the restore because the restore clears the field, and the easing is
	# from the step before, not from nothing.
	var previous: Vector2 = layer["applied"]
	_camera_restore(layer)
	var applied := previous.lerp(motion, 0.5)
	layer["applied"] = applied
	# Added to the camera, not assigned over it: the camera is back on whatever
	# baseline its own owners left it, and this is one more thing moving the eye
	# rather than the whole of what moves it.
	camera.h_offset += applied.x
	camera.v_offset += applied.y


# The jitter's motion: held steps of offset, re-rolled at the rate. The roll is a
# fresh pair of values in -1..1, so the movement is a rattle, not a drift.
func _camera_jitter_motion(layer: Dictionary, values: Dictionary, delta: float) -> Vector2:
	var strength := float(values.get("camera_jitter_strength", 0.02))
	# maxf and not max: the latter is a generic over Variant, which the := below
	# would then infer the variable from.
	var rate := maxf(float(values.get("camera_jitter_rate", 24.0)), 0.01)
	var clock: float = layer["clock"] + delta
	if clock >= 1.0 / rate:
		clock = 0.0
		layer["offset"] = Vector2(randf_range(-1.0, 1.0), randf_range(-1.0, 1.0))
	layer["clock"] = clock
	return strength * (layer["offset"] as Vector2)


# The write of the last frame, taken back off before the next one: only what this
# effect itself wrote is ever undone, so a value the player's own controls wrote
# between frames survives.
func _camera_restore(layer: Dictionary) -> void:
	var applied: Vector2 = layer["applied"]
	if applied == Vector2.ZERO:
		return
	var camera := get_viewport().get_camera_3d()
	if camera != null:
		camera.h_offset -= applied.x
		camera.v_offset -= applied.y
	layer["applied"] = Vector2.ZERO


# A vertex effect: no rect and no shader of its own. It names the materials the
# world is drawn with and moves their uniforms, which is what a bend of the
# world's own geometry is - the shaders that do it are the world's, not this
# node's. The materials are loaded, not instantiated: these are the very
# resources the engine draws with, so a value set here is set on the world.
func _build_vertex_effect(definition: Dictionary) -> Dictionary:
	var id := String(definition.get("id", ""))
	var materials: Array = []
	var uniforms := {}
	for path in definition.get("materials", []):
		var resource_path := String(path)
		if resource_path == "" or not ResourceLoader.exists(resource_path):
			push_error("shaders.json: %s has no material at %s" % [id, resource_path])
			continue
		var loaded: ShaderMaterial = load(resource_path)
		if loaded == null or loaded.shader == null:
			push_error("shaders.json: %s could not load a material at %s" % [id, resource_path])
			continue
		materials.append(loaded)
		for uniform in loaded.shader.get_shader_uniform_list():
			uniforms[String(uniform["name"])] = true
	if materials.is_empty():
		push_error("shaders.json: %s has no materials to drive" % id)
		return {}
	var enable_key := String(definition.get("enable_key", ""))
	if enable_key == "" or not uniforms.has(enable_key):
		push_error("shaders.json: %s has no enable_key uniform on its materials" % id)
		return {}
	return {
		"kind": "vertex",
		"rect": null,
		"materials": materials,
		"uniforms": uniforms,
		"enable_key": enable_key,
	}


func _uniform_names(shader: Shader) -> Dictionary:
	var names := {}
	for uniform in shader.get_shader_uniform_list():
		names[String(uniform["name"])] = true
	return names


# The frame the shaders make their patterns out of: the viewport they are drawn
# over, in pixels, which is the size of the screen texture they sample.
func _push_frame_size() -> void:
	var frame := Vector2(get_viewport().get_visible_rect().size)
	# A pass is a full-rect child of this node, so a node without a size is a stack of
	# passes without one; see _ready.
	size = frame
	for id in _layers:
		_push(_layers[id], "frame_size", frame)
		var history: Dictionary = _layers[id].get("history", {})
		if not history.is_empty():
			_resize_history(history, frame)


# A history viewport holds a copy of the frame, so it is the frame's size: a ghost
# measured in a texture of another size is a different shape on every window.
func _resize_history(history: Dictionary, frame: Vector2) -> void:
	for view in history.get("views", []):
		var port := view as SubViewport
		port.size = Vector2i(frame)
		var copied := port.get_node_or_null("Frame") as TextureRect
		if copied != null:
			copied.size = frame
