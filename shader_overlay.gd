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
#   - a 'world' effect gets no rect, because it is not a picture of the world but
#     the world: it names the materials the world is drawn with (the registry's
#     `materials`), and this node moves *their* uniforms. Those materials are
#     loaded rather than instantiated, so they are the very resources the engine
#     draws with. Such an effect has nothing to hide, so its switch is pushed as
#     a uniform as well (`enable_key`), and the shaders multiply the effect by
#     nothing when it is off.
#
# This node owns the state but not the file: settings_menu.gd is the one that
# reads and writes user://settings.cfg, so the Shaders page calls in here for
# every value it shows and every value it changes.
#
# The uniform a param drives is the param's own `key`, so a shader's uniforms,
# data/shaders.json and the saved config keys all carry the same names.

const DEFINITIONS_PATH := "res://data/shaders.json"

# Where the world's engine lives, for the one thing a world effect needs from it
# that a shader cannot do itself: the chunks are culled against boxes that say
# where the world *was*, because a vertex shader runs after the engine has
# decided what to draw. A bent or curved world would therefore have its far field
# culled away. Each effect's own knobs go to the engine as well as to its
# materials, and the engine grows those boxes to where the shaders will put the
# geometry - see src/core/world_cull.hpp and src/mesh/mesh_manager_cull.cpp.

# The knobs each world effect hands the engine, in the order that effect's own
# method takes them: an effect's `id` names the method it is told through
# (`bend` -> set_world_bend, `horizon` -> set_world_horizon), and this list is
# what the culling arithmetic needs of it, which is not the same list the menu
# offers. The water's ripple is a knob the menu offers and the culling has no use
# for: it moves the surface sideways, and the slack the box already carries is
# wider than its whole slider.
const WORLD_ENGINE_KNOBS := {
	"bend": ["world_bend", "world_bend_radius", "world_bend_rise"],
	"horizon": ["world_horizon_radius"],
}


var _definitions: Array = []
# id -> { kind, rect, materials, uniforms, enable_key }. A screen effect has a
# rect of its own to hide; a world effect has neither, and its switch is a
# uniform on the materials it drives.
var _layers: Dictionary = {}
var _enabled: Dictionary = {}  # id -> bool
var _values: Dictionary = {}   # id -> { key: value }
# The world's engine, if there is one in this scene. Null in a scene without one
# (a probe that builds its own world), which is why nothing here is required to
# be present.
var _world: Node = null


func _ready() -> void:
	# The layers are decoration: the pointer belongs to whatever HUD art is under
	# them, exactly like the god-rays overlay.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_load_definitions()
	for definition in _definitions:
		_build_layer(definition)
	_push_frame_size()
	# The patterns a shader measures are counted in frame pixels, so a resized
	# window has to tell them how many there are now.
	get_viewport().size_changed.connect(_push_frame_size)
	# Deferred, because a sibling's `_ready` may not have run yet and the engine
	# is a sibling. Pushed once here even though nothing has changed: the engine
	# starts with both world effects off and has to be told that they exist at
	# all.
	_push_world_effects.call_deferred()


# --- The registry -------------------------------------------------------------

func get_definitions() -> Array:
	return _definitions


func get_shader_ids() -> Array:
	var ids: Array = []
	for definition in _definitions:
		ids.append(String(definition.get("id", "")))
	return ids


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
	else:
		# A world effect is not something that can be hidden: it is the geometry
		# itself, and what the switch means there is the effect's own strength
		# multiplied by nothing.
		_push(layer, String(layer.get("enable_key", "")), 1.0 if enabled else 0.0)
		_push_world_effects.call_deferred()


# --- Param values -------------------------------------------------------------

func get_value(id: String, key: String) -> Variant:
	var values: Dictionary = _values.get(id, {})
	return values.get(key, null)


func set_value(id: String, key: String, value: Variant) -> void:
	if not _values.has(id):
		return
	_values[id][key] = value
	_push(_layers.get(id, {}), key, value)
	_push_world_effects.call_deferred()


# Every world effect's own state, handed to the engine as well as to its
# materials, because the engine culls the chunks against boxes that describe where
# the world was: a shader cannot widen them and the engine cannot read a shader.
# One walk over the registry tells each effect's method what its own knobs are;
# a world effect whose knobs the engine has no method for is skipped here rather
# than pushed into a method that does not exist. Deferred by the callers rather
# than pushed straight away, because this is reached once per row while the menu
# is being built and taking it costs the engine a walk over every resident chunk.
func _push_world_effects() -> void:
	if not _can_take_world_effects(_world):
		_world = _find_world()
		if _world == null:
			return
	for definition in _definitions:
		if String(definition.get("kind", "screen")) != "world":
			continue
		var id := String(definition.get("id", ""))
		var method := "set_world_" + id
		if not _world.has_method(method):
			push_error("shader_overlay: the world effect '%s' needs %s and the engine has no such method" % [id, method])
			continue
		var values: Dictionary = _values.get(id, {})
		var args: Array = [bool(_enabled.get(id, false))]
		for key in WORLD_ENGINE_KNOBS.get(id, []):
			args.append(float(values.get(key, 0.0)))
		_world.callv(method, args)


# The methods the engine has to have: the registry's world effects, each named by
# its own id. A node is the world's engine for this purpose only when it can take
# all of them, because requiring every one rather than any one is what makes a
# half-wired engine a node that is not found - rather than a node that silently
# stops growing boxes for the effect it forgot.
func _world_methods() -> Array:
	var methods: Array = []
	for definition in _definitions:
		if String(definition.get("kind", "screen")) == "world":
			methods.append("set_world_" + String(definition.get("id", "")))
	return methods


func _can_take_world_effects(node: Node) -> bool:
	if node == null or not is_instance_valid(node):
		return false
	var methods := _world_methods()
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
# world's effects at all. Cached once found (the outer test re-checks it every
# push, so a freed engine is re-found rather than quietly dropped).
func _find_world() -> Node:
	var node: Node = self
	while node != null:
		if _can_take_world_effects(node):
			return node
		for child in node.get_children():
			if _can_take_world_effects(child):
				return child
		node = node.get_parent()
	return null


# Only push what the target actually declares, so a mistyped key in the registry
# is reported once at load (below) instead of erroring every frame. A world
# effect's params can be declared by any of its materials - the water's ripple
# is on the water and not on the terrain - so each one is pushed to whichever of
# them has it.
func _push(layer: Dictionary, key: String, value: Variant) -> void:
	if layer.is_empty() or key == "":
		return
	var uniforms: Dictionary = layer["uniforms"]
	if not uniforms.has(key):
		return
	for material in layer["materials"]:
		(material as ShaderMaterial).set_shader_parameter(key, value)


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


func _build_layer(definition: Dictionary) -> void:
	var id := String(definition.get("id", ""))
	if id == "":
		push_error("shaders.json: an effect has no id")
		return
	var built := _build_world_effect(definition) if String(definition.get("kind", "screen")) == "world" \
		else _build_screen_effect(definition)
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


# A screen effect: a full-screen rect of its own, drawn over the frame beneath
# it. Whatever the shader writes is the picture, alpha and all, so the rect's own
# colour only has to not be drawn over it before the shader runs.
func _build_screen_effect(definition: Dictionary) -> Dictionary:
	var id := String(definition.get("id", ""))
	var path := String(definition.get("shader", ""))
	if path == "" or not ResourceLoader.exists(path):
		push_error("shaders.json: %s has no shader at %s" % [id, path])
		return {}
	var shader: Shader = load(path)
	if shader == null:
		push_error("shaders.json: %s failed to load %s" % [id, path])
		return {}

	var rect := ColorRect.new()
	rect.name = id
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	rect.color = Color(0, 0, 0, 0)
	var material := ShaderMaterial.new()
	material.shader = shader
	rect.material = material
	add_child(rect)

	return {
		"kind": "screen",
		"rect": rect,
		"materials": [material],
		"uniforms": _uniform_names(shader),
		"enable_key": "",
	}


# A world effect: no rect and no shader of its own. It names the materials the
# world is drawn with and moves their uniforms, which is what a bend of the
# world's own geometry is - the shaders that do it are the world's, not this
# node's. The materials are loaded, not instantiated: these are the very
# resources the engine draws with, so a value set here is set on the world.
func _build_world_effect(definition: Dictionary) -> Dictionary:
	var id := String(definition.get("id", ""))
	var materials: Array = []
	var uniforms := {}
	for path in definition.get("materials", []):
		var resource_path := String(path)
		if resource_path == "" or not ResourceLoader.exists(resource_path):
			push_error("shaders.json: %s has no material at %s" % [id, resource_path])
			continue
		var material: ShaderMaterial = load(resource_path)
		if material == null or material.shader == null:
			push_error("shaders.json: %s could not load a material at %s" % [id, resource_path])
			continue
		materials.append(material)
		for uniform in material.shader.get_shader_uniform_list():
			uniforms[String(uniform["name"])] = true
	if materials.is_empty():
		push_error("shaders.json: %s has no materials to drive" % id)
		return {}
	var enable_key := String(definition.get("enable_key", ""))
	if enable_key == "" or not uniforms.has(enable_key):
		push_error("shaders.json: %s has no enable_key uniform on its materials" % id)
		return {}
	return {
		"kind": "world",
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
	var size := Vector2(get_viewport().get_visible_rect().size)
	for id in _layers:
		_push(_layers[id], "frame_size", size)
