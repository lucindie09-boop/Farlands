extends SceneTree
## Does every reading pass carry its own copy, and is it ordered where it claims?
##
## A shader reads the frame through a copy of it, and Godot makes ONE copy per
## frame - taken at the first node in the layer that reads the screen, wherever
## that node is. So a second reading pass is handed the frame as it stood BEFORE
## the first one drew, and since a pass writes its picture back over the whole
## screen it paints that older frame over the screen: the pass before it is
## deleted, and so is every HUD item drawn between the two. That is not a subtle
## failure, it is a missing hotbar.
##
## The fix is one BackBufferCopy per pass, at the pass's own depth, added
## immediately before the pass's rect so tree order breaks the z_index tie. The
## registry holds two reading passes today - `anime` (world, z -1) and `crt`
## (screen, z 0) - so the stack is not hypothetical: without this the CRT deleted
## the anime pass and the hotbar.
##
## What is checked here is the structure, headless and in a second, because the
## structure is the whole claim: the copy exists, it is at the rect's depth, it
## comes before its own rect, and the NEXT pass's copy comes after this pass's
## rect (so the next pass reads a frame that already contains this one). Plus
## that a vertex effect - which is geometry, not a pass - carries neither, and
## that switching a pass off takes its copy with it rather than leaving a live
## copy standing between two passes that are on.
##
## probe_shaders_shot.gd measures what the passes DO to the picture; this
## measures that the frame each one is handed is the one it should be, which is
## the half that survives a shader edit and fails under a layout edit.
##
##   Godot --headless --path . --script res://.freebuff/probe_shader_stack_copy.gd

const OVERLAY_PATH := "HUD/ShaderOverlay"
const REGISTRY := "res://data/shaders.json"

var failures := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_fail("Main.tscn missing")
		_finish()
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	# The overlay's build is deferred, and its siblings' _ready may still be
	# running on the first frame.
	for i in range(6):
		await process_frame

	var overlay: Node = main.get_node_or_null(OVERLAY_PATH)
	if overlay == null:
		_fail("%s missing" % OVERLAY_PATH)
		_finish()
		return

	var registry := load(REGISTRY) as JSON
	var definitions: Array = [] if registry == null else registry.data.get("shaders", [])
	if definitions.is_empty():
		_fail("the registry holds no effects")
		_finish()
		return

	# Registry order is draw order (the file says so), so walking it is walking
	# the stack: whatever each pass writes, the next one samples it.
	var children: Array = overlay.get_children()
	var passes: Array = []
	var vertices: Array = []
	var cameras: Array = []
	for definition in definitions:
		var kind := String(definition.get("kind", "screen"))
		if kind == "vertex":
			vertices.append(definition)
		elif kind == "camera":
			cameras.append(definition)
		else:
			passes.append({"definition": definition, "kind": kind})

	if passes.size() < 2:
		_fail("only %d reading pass(es): nothing stacks, so this proves nothing"
			% passes.size())
		_finish()
		return

	# --- one copy per pass, at its own rect's depth, before its own rect ------
	var rect_indexes: Array = []
	for entry in passes:
		var definition: Dictionary = entry["definition"]
		var kind: String = entry["kind"]
		var id := String(definition["id"])

		var rect: CanvasItem = _find_child(children, id)
		if rect == null:
			_fail("%s: no layer rect named %s" % [id, id])
			continue
		var rect_index := children.find(rect)
		rect_indexes.append(rect_index)

		var expected_z := -1 if kind == "world" else 0
		if rect.z_index != expected_z:
			_fail("%s is a %s effect drawn at z_index %d, wanted %d"
				% [id, kind, rect.z_index, expected_z])

		var copy: Node = _find_child(children, id + "BackBuffer")
		if copy == null:
			# The bug this probe exists for, stated as it presents: a pass with
			# no copy of its own falls back on whatever copy the engine already
			# took this frame, which is the pass before it.
			_fail("%s has no BackBufferCopy: with %d passes in the layer the "
				% [id, passes.size()]
				+ "engine's one copy is the other pass's, so this one reads a "
				+ "frame from before the other drew and deletes it on the way out")
			continue
		if not (copy is BackBufferCopy):
			_fail("%s's copy is a %s, not a BackBufferCopy" % [id, copy.get_class()])
			continue
		if copy.z_index != rect.z_index:
			_fail("%s: its copy is at z_index %d and its rect at %d, so it "
				% [id, copy.z_index, rect.z_index]
				+ "snapshots the frame at a different depth than the pass draws at")
		# The copy's mode has to agree with the switch, whichever way that switch
		# happens to be set: a pass that is on must be sampling, and a pass that is
		# off must not be taking the frame - an off effect's live copy is what
		# steals the frame a pass further down the stack needs. Stated relative to
		# the switch rather than as "on" outright, because every effect ships off
		# and a probe that insisted on VIEWPORT would fail on a correct build.
		var on := bool(overlay.call("is_enabled", id))
		if on and copy.copy_mode == BackBufferCopy.COPY_MODE_DISABLED:
			_fail("%s is on but its copy is disabled, so it samples nothing" % id)
		if not on and copy.copy_mode != BackBufferCopy.COPY_MODE_DISABLED:
			_fail("%s is off but its copy is still taking the frame, which is the "
				% id + "one a pass below it in the stack needs")
		var copy_index := children.find(copy)
		if copy_index > rect_index:
			_fail("%s: its copy is at index %d and its rect at %d, so the pass "
				% [id, copy_index, rect_index]
				+ "draws before the frame it reads was taken")

	# --- and the chain: pass N+1's copy is taken after pass N drew ------------
	for i in range(1, mini(rect_indexes.size(), passes.size())):
		var id := String(passes[i]["definition"]["id"])
		var next_copy: Node = _find_child(children, id + "BackBuffer")
		if next_copy == null:
			continue
		var next_copy_index: int = children.find(next_copy)
		var prev_rect_index: int = rect_indexes[i - 1]
		if next_copy_index < prev_rect_index:
			_fail("the copy for %s sits at index %d, before %s drew at %d, so "
				% [id, next_copy_index, passes[i - 1]["definition"]["id"],
					prev_rect_index]
				+ "%s never sees %s at all" % [id, passes[i - 1]["definition"]["id"]])

	# --- the previous-frame history, for the passes that asked for one ---------
	# A reading pass can only see its own frame, so an after-image needs a pair of
	# viewports trading places: one captures the frame being drawn while the other
	# stands still holding the one before. What is checked is that a pass gets a
	# pair exactly when its shader declares `previous_frame` (a pass that did not
	# ask must not be paying for two viewports), that the pair swaps, that exactly
	# one of them captures at a time, and that switching the pass off stops both -
	# a stopped pass still holding a live viewport is two blits a frame for
	# nothing, and the one that matters is the one that would steal a frame.
	var histories := 0
	for entry in passes:
		var definition: Dictionary = entry["definition"]
		var id := String(definition["id"])
		var shader: Shader = load(String(definition["shader"])) as Shader
		if shader == null:
			_fail("%s: its shader would not load" % id)
			continue
		var wants := false
		for uniform in shader.get_shader_uniform_list():
			if String(uniform["name"]) == "previous_frame":
				wants = true
		var views: Array = []
		for suffix in ["A", "B"]:
			var view := _find_child(children, id + "History" + suffix)
			if view != null:
				views.append(view)

		if not wants:
			if not views.is_empty():
				_fail("%s does not declare previous_frame but has %d history "
					% [id, views.size()] + "viewport(s) built for it")
			continue

		histories += 1
		if views.size() != 2:
			_fail("%s declares previous_frame and has %d history viewport(s), "
				% [id, views.size()]
				+ "so it cannot hold the frame before and capture this one at once")
			continue
		for view in views:
			var port := view as SubViewport
			if port == null:
				_fail("%s: a history node is a %s, not a SubViewport"
					% [id, view.get_class()])
				continue
			var frame := port.get_node_or_null("Frame") as TextureRect
			if frame == null:
				_fail("%s: its history viewport %s holds no Frame rect, so it "
					% [id, port.name] + "shows nothing to age")
			elif frame.texture == null:
				_fail("%s: the Frame rect in %s has no texture" % [id, port.name])
			if port.size.x <= 0 or port.size.y <= 0:
				_fail("%s: its history viewport %s is %dx%d, so a ghost measured "
					% [id, port.name, port.size.x, port.size.y]
					+ "in it is the wrong shape for the frame")

		# On, and the pair has to actually be running: one capturing, one holding,
		# and which is which has to change from frame to frame or the pass is
		# handed the same picture twice and the trail never ages.
		overlay.call("set_enabled", id, true)
		await process_frame
		await process_frame
		var first := _capture_state(views)
		await process_frame
		var second := _capture_state(views)
		if first.count(true) != 1:
			_fail("%s: %d of its 2 history viewports are capturing, so the frame "
				% [id, first.count(true)]
				+ "before is being rewritten rather than held")
		if first == second:
			_fail("%s: its history viewports are capturing the same one twice "
				% id + "over (%s), so the trail is fed the frame it just drew"
				% str(first))
		overlay.call("set_enabled", id, false)
		await process_frame
		var off := _capture_state(views)
		if off.count(true) != 0:
			_fail("%s is off but %d of its history viewports are still "
				% [id, off.count(true)]
				+ "capturing, so an effect that is switched off still costs two "
				+ "blits a frame")
		print("probe: %-8s history pair swapping, %s then %s, off = %s"
			% [id, str(first), str(second), str(off)])

	if histories == 0:
		_fail("no effect declares previous_frame, so the history machinery is "
			+ "unexercised and this proves nothing about it")

	# --- a vertex effect is geometry, not a pass ------------------------------
	for definition in vertices:
		var id := String(definition["id"])
		if _find_child(children, id) != null:
			_fail("%s is a vertex effect but has a layer rect: it is geometry "
				% id + "and has nothing to copy")
		if _find_child(children, id + "BackBuffer") != null:
			_fail("%s is a vertex effect but has a BackBufferCopy" % id)

	# --- a camera effect is the eye, not a picture ---------------------------
	# It draws nothing at all, and that is the whole of what distinguishes it: a
	# rect here would be a pass over the frame that grades nothing, and a copy
	# would be a live copy nobody reads. Both are silent - an invisible rect
	# costs one quad - so "it draws nothing" is asserted rather than assumed.
	for definition in cameras:
		var id := String(definition["id"])
		if _find_child(children, id) != null:
			_fail("%s is a camera effect but has a layer rect: it moves the eye, "
				% id + "it does not draw a picture over the frame")
		if _find_child(children, id + "BackBuffer") != null:
			_fail("%s is a camera effect but has a BackBufferCopy: it has no "
				% id + "frame to read")
		var layers: Dictionary = overlay.get("_layers") as Dictionary
		var layer: Dictionary = layers.get(id, {}) as Dictionary
		if layer.is_empty():
			_fail("%s is a camera effect but built no layer to drive" % id)
			continue
		if layer.get("kind", "") != "camera":
			_fail("%s is a camera effect but its layer is a %s"
				% [id, layer.get("kind", "")])
		# Its params have to be the driver's own, and the driver has to actually
		# read them: an effect whose sliders are written somewhere the driver
		# never looks is an effect whose knobs do nothing.
		var uniforms: Dictionary = layer.get("uniforms", {})
		for param in definition.get("params", []):
			var key := String(param.get("key", ""))
			if not uniforms.has(key):
				_fail("%s: the camera driver has no key for the %s row" % [id, key])
		printerr("probe: %-8s camera rect/copy absent, driver keys %s"
			% [id, str(uniforms.keys())])

		# The claim this whole effect rests on is that it moves the eye, and that
		# switching it off puts the eye back exactly where it was - a write to a
		# live camera that is not undone is a camera a player cannot aim with.
		# The steps are held between rolls, so the first frames are legitimately
		# still, and this waits for a roll rather than sampling frame two.
		var camera := overlay.get_viewport().get_camera_3d()
		if camera == null:
			printerr("probe: no current Camera3D here, so %s's driver and its "
				% id + "restore are unexercised")
			continue
		# The baseline is whatever the camera is at with the effect OFF and settled,
		# not whatever it happened to be carrying: the effect can arrive already on
		# from saved settings, and sampling the eye mid-jitter and then asking to be
		# returned to it is a test that fails on a correct restore.
		overlay.call("set_enabled", id, false)
		await process_frame
		await process_frame
		var before := Vector2(camera.h_offset, camera.v_offset)
		overlay.call("set_enabled", id, true)
		var moved := Vector2.ZERO
		for _attempt in 90:
			await process_frame
			moved = Vector2(camera.h_offset, camera.v_offset)
			if moved != Vector2.ZERO:
				break
		if moved == Vector2.ZERO:
			_fail("%s is on but never moved the camera, so its rows drive nothing"
				% id)
		overlay.call("set_enabled", id, false)
		await process_frame
		var after := Vector2(camera.h_offset, camera.v_offset)
		if after != before:
			_fail("%s is off but left the camera at %s having found it at %s, so "
				% [id, str(after), str(before)]
				+ "its own write was never taken back off")
		printerr("probe: %-8s moved the eye to %s, restored to %s on switch-off"
			% [id, str(moved), str(after)])

	if cameras.is_empty():
		printerr("probe: no camera effect, so nothing is exercised against a "
			+ "kind that must draw nothing")

	# --- switching a pass off takes its copy with it --------------------------
	for entry in passes:
		var definition: Dictionary = entry["definition"]
		var id := String(definition["id"])
		var rect: CanvasItem = _find_child(children, id)
		var copy: BackBufferCopy = _find_child(children, id + "BackBuffer")
		if rect == null or copy == null:
			continue

		overlay.set_enabled(id, false)
		await process_frame
		if rect.visible:
			_fail("%s is off but its rect is still visible" % id)
		if copy.visible:
			_fail("%s is off but its copy still stands: a live copy between two "
				% id + "passes that are on takes the frame the next one needs")
		if copy.copy_mode != BackBufferCopy.COPY_MODE_DISABLED:
			_fail("%s is off but its copy mode is %d, not disabled"
				% [id, copy.copy_mode])

		overlay.set_enabled(id, true)
		await process_frame
		if not rect.visible:
			_fail("%s is on but its rect is hidden" % id)
		if not copy.visible:
			_fail("%s is on but its copy is hidden, so it samples nothing" % id)
		if copy.copy_mode != BackBufferCopy.COPY_MODE_VIEWPORT:
			_fail("%s is on but its copy mode is %d, not viewport"
				% [id, copy.copy_mode])

		print("probe: %-8s %-6s rect z=%d copy z=%d  order %d,%d"
			% [id, entry["kind"], rect.z_index, copy.z_index,
				children.find(copy), children.find(rect)])

	_finish()


func _find_child(children: Array, wanted: String) -> Node:
	for child in children:
		if String(child.name) == wanted:
			return child
	return null


# Which of a history pair is capturing this frame. Capturing is what makes a
# viewport hold the frame being drawn; not capturing is what leaves it standing
# still on the frame before, and the two are how "this frame" becomes "the one
# before" - so the pair has to be one and one, and the one has to change.
func _capture_state(views: Array) -> Array:
	var state: Array = []
	for view in views:
		var port := view as SubViewport
		state.append(port != null
			and port.render_target_update_mode == SubViewport.UPDATE_ALWAYS)
	return state


func _fail(message: String) -> void:
	failures += 1
	printerr("FAIL: %s" % message)


func _finish() -> void:
	if failures == 0:
		print("probe: every reading pass carries its own copy, at its own depth")
	quit(1 if failures > 0 else 0)
