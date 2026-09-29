extends SceneTree
## Headless check for the chunk border overlay (chunk_borders.gd).
##
## What it pins, and why each one is worth a probe rather than a look:
##
##  * the SHAPE of the grid — every line on a 32-block multiple, 5x5 chunks
##    around the player, verticals spanning the player's slice +/- 64. A grid
##    drawn one block off is still "a grid" to the eye;
##  * that the two boundary planes are exactly the chunk's floor and ceiling,
##    because those are the y boundaries the mesher and the fluid writes are
##    keyed on;
##  * that the overlay lives in the scenario (added to the loaded scene, no
##    shadow casting, unshaded, no depth test) — a Node3D parented somewhere
##    else renders nothing at all;
##  * that the toggle actually hides it, and that re-enabling redraws.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_chunk_borders.gd

const CHUNK_SIZE := 32
const SCRIPT_PATH := "res://chunk_borders.gd"
const TOGGLE_ACTION := "toggle_chunk_borders"

var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _ok(msg: String) -> void:
	print("PROBE OK: " + msg)

func _run() -> void:
	# 1. Wiring: the action exists and is bound to B, and the script is an
	#    autoload. A missing action means the key silently does nothing.
	if not InputMap.has_action(TOGGLE_ACTION):
		_fail("input action '%s' is not defined" % TOGGLE_ACTION)
		quit(1)
		return
	var key := ""
	for event in InputMap.action_get_events(TOGGLE_ACTION):
		if event is InputEventKey:
			key = OS.get_keycode_string((event as InputEventKey).keycode)
	if key != "B":
		_fail("action '%s' is bound to '%s', expected B" % [TOGGLE_ACTION, key])
	else:
		_ok("action '%s' is bound to B" % TOGGLE_ACTION)
	if not ProjectSettings.has_setting("autoload/ChunkBorders"):
		_fail("chunk_borders.gd is not registered as an autoload")
	else:
		_ok("chunk_borders.gd is an autoload")

	# 2. The scene, so the overlay has a current_scene to attach to.
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	current_scene = main
	await process_frame
	await process_frame

	var player := main.get_node_or_null("Player") as Node3D
	if player == null:
		_fail("Player node missing")
		quit(1)
		return
	# Wherever the engine puts the player (it drops it on the surface itself), not
	# wherever this probe would like it: the overlay follows the player, so the
	# expectation has to come from the player's own position.
	await process_frame

	var overlay: Node = (load(SCRIPT_PATH) as GDScript).new()
	overlay.name = "ChunkBordersProbe"
	root.add_child(overlay)
	await process_frame

	overlay.set_enabled(true)
	await process_frame
	print("PROBE: player at %s" % str(player.global_position))
	await process_frame

	var mesh_instance: MeshInstance3D = overlay.get_mesh_instance()
	if mesh_instance == null or not is_instance_valid(mesh_instance):
		_fail("enabling the overlay created no MeshInstance3D")
		quit(1)
		return
	if mesh_instance.get_parent() != main:
		_fail("overlay mesh is not parented to the loaded scene")
	else:
		_ok("overlay mesh is in the scene, visible = %s" % str(mesh_instance.visible))
	if mesh_instance.cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF:
		_fail("overlay casts shadows")
	var material := mesh_instance.material_override as StandardMaterial3D
	if material == null:
		_fail("overlay has no StandardMaterial3D")
	elif not material.vertex_color_use_as_albedo or not material.no_depth_test or \
			material.shading_mode != BaseMaterial3D.SHADING_MODE_UNSHADED:
		_fail("overlay material is not unshaded vertex-colour X-ray")
	else:
		_ok("overlay material is unshaded vertex-colour, depth test off")

	# 3. The geometry.
	var mesh := mesh_instance.mesh as ImmediateMesh
	if mesh == null or mesh.get_surface_count() != 1:
		_fail("expected one ImmediateMesh surface")
		quit(1)
		return
	var arrays := mesh.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	if verts.size() == 0:
		_fail("overlay drew no lines")
		quit(1)
		return
	_ok("overlay drew %d line vertices" % verts.size())

	# Read both at the same moment: the player is idling/falling on its own clock,
	# and the overlay only redraws when its chunk changes.
	var live := player.global_position
	var chunk := Vector3i(floori(live.x / CHUNK_SIZE), floori(live.y / CHUNK_SIZE), floori(live.z / CHUNK_SIZE))
	if overlay.get_built_chunk() != chunk:
		_fail("overlay centred on chunk %s, player is in %s (%s)" %
			[str(overlay.get_built_chunk()), str(chunk), str(live)])
	else:
		_ok("overlay centred on the player's chunk %s" % str(chunk))

	var lo_x := INF
	var hi_x := -INF
	var lo_y := INF
	var hi_y := -INF
	var lo_z := INF
	var hi_z := -INF
	var off_grid := 0
	for v in verts:
		lo_x = minf(lo_x, v.x); hi_x = maxf(hi_x, v.x)
		lo_y = minf(lo_y, v.y); hi_y = maxf(hi_y, v.y)
		lo_z = minf(lo_z, v.z); hi_z = maxf(hi_z, v.z)
		if fmod(absf(v.x), CHUNK_SIZE) > 0.001 or fmod(absf(v.z), CHUNK_SIZE) > 0.001:
			off_grid += 1
	if off_grid != 0:
		_fail("%d vertices are off the 32-block grid" % off_grid)
	else:
		_ok("every vertex sits on a chunk boundary (x/z multiple of 32)")

	# 5 chunks wide/deep (2 each way around the player's own) and the slice's two
	# planes +/- 64 for the verticals.
	var want_x := float((chunk.x - 2) * CHUNK_SIZE)
	var want_hi_x := float((chunk.x + 3) * CHUNK_SIZE)
	if absf(lo_x - want_x) > 0.001 or absf(hi_x - want_hi_x) > 0.001:
		_fail("x span is %f..%f, expected %f..%f" % [lo_x, hi_x, want_x, want_hi_x])
	else:
		_ok("x span covers 5 chunks: %f..%f" % [lo_x, hi_x])
	var want_z := float((chunk.z - 2) * CHUNK_SIZE)
	if absf(lo_z - want_z) > 0.001:
		_fail("z span starts at %f, expected %f" % [lo_z, want_z])
	else:
		_ok("z span covers 5 chunks: %f..%f" % [lo_z, hi_z])
	var want_lo_y := float(chunk.y * CHUNK_SIZE) - 64.0
	var want_hi_y := float((chunk.y + 1) * CHUNK_SIZE) + 64.0
	if absf(lo_y - want_lo_y) > 0.001 or absf(hi_y - want_hi_y) > 0.001:
		_fail("y span is %f..%f, expected %f..%f" % [lo_y, hi_y, want_lo_y, want_hi_y])
	else:
		_ok("verticals span the player's slice +/- 64: %f..%f" % [lo_y, hi_y])

	# Both boundary planes must be drawn, exactly at the chunk's floor/ceiling.
	var planes := {float(chunk.y * CHUNK_SIZE): false, float((chunk.y + 1) * CHUNK_SIZE): false}
	for v in verts:
		if planes.has(v.y):
			planes[v.y] = true
	if not planes[float(chunk.y * CHUNK_SIZE)] or not planes[float((chunk.y + 1) * CHUNK_SIZE)]:
		_fail("the chunk's floor and ceiling planes are not both drawn")
	else:
		_ok("floor (%d) and ceiling (%d) planes are both drawn" % [chunk.y * CHUNK_SIZE, (chunk.y + 1) * CHUNK_SIZE])

	# 4. Toggling off hides it; on again redraws the same geometry.
	overlay.set_enabled(false)
	await process_frame
	if mesh_instance.visible:
		_fail("disabling the overlay left it visible")
	else:
		_ok("disabling hides the overlay")
	overlay.set_enabled(true)
	await process_frame
	await process_frame
	if not mesh_instance.visible:
		_fail("re-enabling left the overlay hidden")
	else:
		var again := (mesh_instance.mesh as ImmediateMesh).surface_get_arrays(0)
		if (again[Mesh.ARRAY_VERTEX] as PackedVector3Array).size() != verts.size():
			_fail("re-enabling drew a different number of lines")
		else:
			_ok("re-enabling redraws the same grid")

	print("PROBE %s" % ("OK" if ok else "FAILED"))
	quit(0 if ok else 1)
