extends SceneTree
## Do the world-effect includes survive contact with the compiler?
##
## Both geometry effects rest on one thing Godot has to actually allow: a
## .gdshaderinc pulled in with #include, whose uniforms and function then exist
## inside both consumers. Nothing else in either effect works if that does not, and
## --headless cannot answer it - the dummy renderer never compiles a shader - so
## this runs windowed and reads what the engine prints.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_bend_compile.gd [timeout]
##
## It also pins the *shape* of both effects' interfaces: which uniforms each of the
## four shaders ends up with (the terrain, the water, and a marker that wires both
## includes the way those two do), that the bend's four knobs reach every one of
## them while the water's ripple reaches only the water, that the curve's two reach
## them too, and that every param of both registry entries lands on a uniform of
## one of the two world materials.

const TERRAIN := "res://shaders/voxel_shader.gdshader"
const WATER := "res://shaders/voxel_shader_water.gdshader"
const MARKER := "res://probes/bend_marker.gdshader"
const HORIZON_MARKER := "res://probes/horizon_marker.gdshader"
const REGISTRY := "res://data/shaders.json"

# Every shader that draws the world's geometry, and which effects it has to end up
# carrying. The world's own two must have both - a terrain drawn through one of
# them and not the other is a shoreline that opens up as the far field sinks - and
# the markers are the probes' own: the bend's wires the bend alone, so the include
# is still exercised on its own and with no sibling compiled beside it, and the
# horizon's wires both exactly as the world does.
const SHADERS := [
	["res://shaders/voxel_shader.gdshader", ["bend", "horizon"]],
	["res://shaders/voxel_shader_water.gdshader", ["bend", "horizon"]],
	["res://probes/bend_marker.gdshader", ["bend"]],
	["res://probes/horizon_marker.gdshader", ["bend", "horizon"]],
]

# The bend's four knobs, and the ripple the include takes as an argument rather
# than reading itself: the terrain never declares it.
const BEND_UNIFORMS := ["world_bend_on", "world_bend", "world_bend_radius", "world_bend_rise"]
# The curve's two, the switch and the planet it is bent onto.
const HORIZON_UNIFORMS := ["world_horizon_on", "world_horizon_radius"]
# What each world effect's entry in the registry has to be called, and the
# uniforms of its own that all of the materials driving it have to end up with.
const WORLD_EFFECTS := {
	"bend": BEND_UNIFORMS,
	"horizon": HORIZON_UNIFORMS,
}
# The kind those two are, which is the kind the registry has to give them: the
# menu groups its rows by it, and a 'world' effect is a different thing that this
# file has nothing to say about.
const VERTEX_KIND := "vertex"

var failures := 0
var _compiled := {}


func _initialize() -> void:
	_run()


func _run() -> void:
	var size := Vector2i(640, 360)
	DisplayServer.window_set_size(size)
	await process_frame

	var scene := Node3D.new()
	root.add_child(scene)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 4.0, 0.0)
	camera.current = true
	scene.add_child(camera)

	# Each shader gets a real mesh in a real scene: a shader that is never drawn
	# is a shader the renderer is free not to compile, and "it loaded" would then
	# mean nothing.
	for entry in SHADERS:
		var path: String = entry[0]
		var shader: Shader = load(path)
		if shader == null:
			_fail("no shader at %s" % path)
			continue
		var material := ShaderMaterial.new()
		material.shader = shader
		var mesh := MeshInstance3D.new()
		mesh.mesh = BoxMesh.new()
		mesh.material_override = material
		mesh.position = Vector3(0.0, 0.0, -4.0)
		mesh.set_meta("path", path)
		scene.add_child(mesh)
		_compiled[path] = _uniforms(shader)
	for i in range(8):
		await process_frame

	var terrain: Array = _compiled.get(TERRAIN, [])
	var water: Array = _compiled.get(WATER, [])
	print("probe: terrain shader %d uniforms, water %d, bend marker %d, horizon marker %d"
		% [terrain.size(), water.size(), Array(_compiled.get(MARKER, [])).size(),
			Array(_compiled.get(HORIZON_MARKER, [])).size()])

	for entry in SHADERS:
		var path: String = entry[0]
		var names: Array = _compiled.get(path, [])
		if names.is_empty():
			continue
		var wanted: Array = []
		for effect in entry[1]:
			wanted.append_array(WORLD_EFFECTS[effect])
		var missing: Array = []
		for name in wanted:
			if not names.has(name):
				missing.append(name)
		if not missing.is_empty():
			_fail("%s is missing %s" % [path.get_file(), missing])
		var sway := names.has("world_bend_sway")
		var want_sway: bool = path == WATER
		if sway != want_sway:
			_fail("%s %s the ripple uniform" % [path.get_file(), "declares" if sway else "lost"])
		print("probe: %s has %s' knobs%s"
			% [path.get_file(), " and ".join(PackedStringArray(entry[1])), ", plus the ripple" if sway else ""])

	_check_registry()
	_finish()


## Every vertex effect the registry declares has to be wired: its materials the two
## the world is drawn with, its `enable_key` a uniform those materials really have,
## and every one of its params, in its own list, a uniform of one of them - or the
## row the menu draws for it is a slider that drives nothing, which is exactly the
## mistake the overlay warns about. And every effect's own named uniforms have to
## be there, because a switch or a knob renamed in one place and not the other is
## an effect that draws nothing when it is turned on.
func _check_registry() -> void:
	var text := FileAccess.get_file_as_string(REGISTRY)
	var data: Variant = JSON.parse_string(text)
	if typeof(data) != TYPE_DICTIONARY:
		_fail("the registry is not an object")
		return
		# Every entry's kind has to be one the file's own `kinds` list declares, and
	# that list is what the Shaders page groups its rows by: an effect whose kind is
	# not declared there is an effect with no category to sit in, which the menu
	# would silently leave out rather than show in the wrong place.
	var declared: Array = []
	for kind in data.get("kinds", []):
		declared.append(String(kind.get("kind", "")))
	for entry in data.get("shaders", []):
		var entry_kind := String(entry.get("kind", "screen"))
		if not declared.has(entry_kind):
			_fail("'%s' is a '%s', which the kinds list does not declare (%s)"
				% [entry.get("id"), entry_kind, declared])
	var found: Array = []
	for entry in data.get("shaders", []):
		if entry.get("kind", "screen") == VERTEX_KIND:
			found.append(entry)
	if found.size() != WORLD_EFFECTS.size():
		_fail("the registry has %d vertex effects, not %d" % [found.size(), WORLD_EFFECTS.size()])

	for entry in found:
		var id := String(entry.get("id", ""))
		print("probe: registry world effect '%s' (%s)" % [entry.get("name"), id])
		if not WORLD_EFFECTS.has(id):
			_fail("the registry's world effects are %s, and this probe knows %s"
				% [found.map(func(e): return e.get("id")), WORLD_EFFECTS.keys()])
			continue

		var materials: Array = entry.get("materials", [])
		if materials.size() != 2:
			_fail("%s names %d materials, not 2" % [id, materials.size()])
		var uniforms := {}
		for path in materials:
			var material: ShaderMaterial = load(String(path))
			if material == null or material.shader == null:
				_fail("%s is not a ShaderMaterial with a shader" % path)
				continue
			for name in _uniforms(material.shader):
				uniforms[name] = true
			print("probe: %s -> %s, %d uniforms"
				% [String(path).get_file(), String(path).get_file(), _uniforms(material.shader).size()])

		var enable_key := String(entry.get("enable_key", ""))
		if not uniforms.has(enable_key):
			_fail("%s's enable_key '%s' is not a uniform of any of its materials" % [id, enable_key])
		for name in WORLD_EFFECTS[id]:
			if not uniforms.has(name):
				_fail("%s's own uniform '%s' is on none of its materials" % [id, name])

		for param in entry.get("params", []):
			var key := String(param.get("key", ""))
			if key.is_empty():
				_fail("%s has a param with no key" % id)
				continue
			if not uniforms.has(key):
				_fail("%s's param '%s' is on neither world material" % [id, key])
			else:
				print("probe: param %-22s -> a uniform of %s's materials" % [key, id])
			if param.has("default") and not param.has("min") and param.get("type") == "float":
				_fail("float param '%s' has no range" % key)


func _uniforms(shader: Shader) -> Array:
	var names: Array = []
	for uniform in shader.get_shader_uniform_list():
		names.append(String(uniform["name"]))
	return names


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _finish() -> void:
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)
