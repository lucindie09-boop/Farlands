extends SceneTree
## The items' light must not change in a single frame.
##
## A dropped block -- of any shape -- a dropped sprite, and the held item and arm
## are lit by the light of the CELL they are in (ChunkManager.get_light_at, through
## shaders/item_lighting.gdshaderinc). That value is a step function of position:
## crossing one block boundary is a whole cell's worth of light, and read raw it is
## applied by the next frame, so every step of a walk is a visible pop. This probe
## boots Main.tscn and holds both consumers to the rule that the value they PUSH
## must move toward the new cell's light over several frames, and then settle on it.
##
## The step is staged rather than walked: a light block placed above the subject's
## cell changes that cell's light in one frame, exactly as moving into another cell
## does, so the fixture needs no particular terrain -- it carves its own air pocket
## above the subject (and a floor for the item) first. It takes the light away again
## and measures the step back too.
##
## Both the carving and the block are world edits, so run this under
## probes/run_probe.sh, which snapshots user://chunks and restores it:
##
##   probes/run_probe.sh probes/probe_item_light_smooth.gd 400
##
## Nothing here reads the screen: the values checked are the per-instance shader
## parameters the two consumers set, so --headless is enough.

const AIR := 0
const STONE := 1
const LIGHT_BLOCK := 14  # data/block_definitions.json index 14: emissive 15,15,15

## How long a step is watched for, in seconds: enough for any ease to play out.
const STEP_SECONDS := 1.0
## Quiet time between fixtures, so every step starts from a settled value.
const SETTLE_SECONDS := 0.75
## The ramp to 90% of a step has to take at least this long: a step that is
## applied in one frame takes no time at all, which is exactly what the raw cell
## light does and what this probe exists to catch. (The eased ramp takes about
## 2.3 time constants: ~0.35 s at the consumers' 0.15 s.)
const MIN_RAMP_SECONDS := 0.15
## No single frame may carry more than this share of a step (a 0.15 s ease leaves
## ~10% of it to the frame at 60 fps, and less at higher rates).
const MAX_FRAME_SHARE := 0.40
## ...nor slow: it has to be all but done by the end of the window.
const MAX_FINAL_GAP_SHARE := 0.05

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-46s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - t0) / 1_000_000.0 < seconds:
		await process_frame


func _max_diff(a: Vector4, b: Vector4) -> float:
	return maxf(maxf(absf(a.x - b.x), absf(a.y - b.y)), maxf(absf(a.z - b.z), absf(a.w - b.w)))


## The per-instance value the item shader is actually handed, or null when nothing
## has reached the mesh yet.
func _pushed(node: Object) -> Variant:
	if node == null:
		return null
	return node.get_instance_shader_parameter("item_light")


func _pushed_vec(node: Object) -> Vector4:
	var v: Variant = _pushed(node)
	return v if v is Vector4 else Vector4.ZERO


## One world write, read back: a probe that trusts a queued write would measure a
## change that never happened.
func _write(cm: Node, cell: Vector3i, id: int) -> bool:
	if cm.get_block(cell.x, cell.y, cell.z) == id:
		return true
	cm.set_block(cell.x, cell.y, cell.z, id)
	return cm.get_block(cell.x, cell.y, cell.z) == id


## The pocket a fixture lights: `base` itself air, `base + 1 .. +5` air (the shaft
## the light comes down), and a stone floor under `base` when it wants one to rest
## on. Returns the cell the light block goes in, or null if a write did not land.
func _pocket(cm: Node, base: Vector3i, floor: bool) -> Variant:
	if floor and not _write(cm, base + Vector3i(0, -1, 0), STONE):
		return null
	if not _write(cm, base, AIR):
		return null
	for dy in range(1, 6):
		if not _write(cm, base + Vector3i(0, dy, 0), AIR):
			return null
	return base + Vector3i(0, 2, 0)


## Stage one step of the light the subject samples: write `id` into `cell` (a light
## block, or AIR to take one away), then hold the value the consumer pushes against
## the light of the cell the subject is in, frame by frame.
func _staged_step(label: String, cm: Node, cell: Vector3i, id: int,
		sample_target: Callable, sample_pushed: Callable) -> void:
	var before: Vector4 = sample_pushed.call()
	if not _write(cm, cell, id):
		_ok(label, false, "the write to (%d,%d,%d) did not land (chunk not loaded?)" % [cell.x, cell.y, cell.z])
		return

	var max_frame := 0.0
	var previous := before
	var values: Array[Vector4] = []
	var targets: Array[Vector4] = []
	var times: Array[float] = []
	var t0 := Time.get_ticks_usec()
	while true:
		await process_frame
		var now := float(Time.get_ticks_usec() - t0) / 1_000_000.0
		var pushed: Vector4 = sample_pushed.call()
		var target: Vector4 = sample_target.call()
		values.append(pushed)
		targets.append(target)
		times.append(now)
		max_frame = maxf(max_frame, _max_diff(pushed, previous))
		previous = pushed
		if now >= STEP_SECONDS:
			break

	var settled: Vector4 = targets[targets.size() - 1]
	var change := _max_diff(before, settled)
	if change < 0.1:
		_ok(label, false, "the staged step moved the cell's light by %.4f only: nothing to smooth" % change)
		return

	# The world's own relight is immediate, so the target has to arrive within a
	# frame or two; that is what makes the frames after it the consumer's own.
	var target_frames := -1
	for i in range(targets.size()):
		if _max_diff(targets[i], settled) <= 0.02:
			target_frames = i + 1
			break
	var ninety_seconds := -1.0
	for i in range(values.size()):
		if _max_diff(values[i], settled) <= 0.10 * change:
			ninety_seconds = times[i]
			break
	var gap := _max_diff(values[values.size() - 1], settled)
	var ok := target_frames > 0 and target_frames <= 3 \
		and max_frame <= MAX_FRAME_SHARE * change \
		and ninety_seconds >= MIN_RAMP_SECONDS \
		and gap <= MAX_FINAL_GAP_SHARE * change
	_ok(label, ok, "step %.4f; worst frame %.4f (%d%% of it); cell relit in %d fr; 90%% in %.2f s; ends %.4f off; first frames %s" % [
		change, max_frame, int(round(100.0 * max_frame / maxf(change, 0.0001))),
		target_frames, ninety_seconds, gap, str(values.slice(0, 5))])


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	var dropped: Node = main.get_node_or_null("DroppedItems")
	var viewmodel: Node = main.get_node_or_null("Player/Camera3D/Viewmodel")
	if cm == null or player == null or dropped == null or viewmodel == null:
		print("PROBE FAIL: main.tscn is missing ChunkManager / Player / DroppedItems / Viewmodel")
		quit(1)
		return

	# Land first: the pockets below are cut relative to where the player came to
	# rest, and the eye's own cell must be the loaded world's, not a spawn point
	# still in the air.
	for i in range(1800):
		await process_frame
		if player.has_method("is_on_floor") and player.is_on_floor():
			break
	await _seconds(1.0)

	var item_mesh: MeshInstance3D = viewmodel.get("_item")
	if item_mesh == null:
		print("PROBE FAIL: the viewmodel has no held-item mesh")
		quit(1)
		return
	if not (_pushed(item_mesh) is Vector4):
		print("PROBE FAIL: no item_light ever reached the viewmodel mesh (is a renderer running?)")
		quit(1)
		return

	# --- The held item and the arm: the light of the eye's cell. -------------
	var eye: Vector3 = viewmodel.call("_eye_position")
	var eye_cell := Vector3i(floori(eye.x), floori(eye.y), floori(eye.z))
	var eye_target := func() -> Vector4:
		var at: Vector3 = viewmodel.call("_eye_position")
		return cm.get_light_at(floori(at.x), floori(at.y), floori(at.z))
	var eye_pushed := func() -> Vector4:
		return _pushed_vec(item_mesh)

	var vm_light_cell: Variant = _pocket(cm, eye_cell, false)
	if vm_light_cell == null:
		_ok("held item: fixture", false, "could not cut an air pocket at the eye (%d,%d,%d)" % [eye_cell.x, eye_cell.y, eye_cell.z])
	else:
		await _seconds(SETTLE_SECONDS)
		await _staged_step("held item: light arrives", cm, vm_light_cell, LIGHT_BLOCK, eye_target, eye_pushed)
		await _seconds(SETTLE_SECONDS)
		await _staged_step("held item: light leaves", cm, vm_light_cell, AIR, eye_target, eye_pushed)
	await _seconds(SETTLE_SECONDS)

	# --- A dropped item: the light of the cell its own centre is in. ---------
	var feet := Vector3i(floori(player.global_position.x), floori(player.global_position.y), floori(player.global_position.z))
	# Three cells out, far enough that the pickup rule cannot collect the fixture
	# mid-measurement, on a floor the probe lays itself so the item rests in the
	# pocket wherever the player happens to have landed.
	var spot := feet + Vector3i(3, 0, 0)
	var item_light_cell: Variant = _pocket(cm, spot, true)
	if item_light_cell == null:
		_ok("dropped item: fixture", false, "could not cut an air pocket at (%d,%d,%d)" % [spot.x, spot.y, spot.z])
	else:
		var at := Vector3(float(spot.x) + 0.5, float(spot.y) + 0.5, float(spot.z) + 0.5)
		dropped.call("spawn", STONE, 1, at, Vector3.FORWARD)
		var items: Array = dropped.get("_items")
		if items.is_empty():
			_ok("dropped item: fixture", false, "spawn() added no item")
		else:
			var item: Dictionary = items[items.size() - 1]
			item["position"] = at
			item["velocity"] = Vector3.ZERO
			item["spin"] = Vector3.ZERO
			item["asleep"] = false
			var rested := false
			for i in range(420):
				await process_frame
				if item["asleep"]:
					rested = true
					break
			var mesh: MeshInstance3D = item["node"].get_child(0)
			var item_target := func() -> Vector4:
				var p: Vector3 = item["position"]
				return cm.get_light_at(floori(p.x), floori(p.y), floori(p.z))
			var item_pushed := func() -> Vector4:
				return _pushed_vec(mesh)
			if not rested:
				_ok("dropped item: fixture", false, "the item was still moving 420 frames after it was placed")
			else:
				await _seconds(SETTLE_SECONDS)
				await _staged_step("dropped item: light arrives", cm, item_light_cell, LIGHT_BLOCK, item_target, item_pushed)
				await _seconds(SETTLE_SECONDS)
				await _staged_step("dropped item: light leaves", cm, item_light_cell, AIR, item_target, item_pushed)

	print("PROBE item light smoothing: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
