extends SceneTree
## Does an item's face shading snap as the body turns?
##
## The item light model's face shade is the terrain's own table -- top 1.0, bottom
## 0.5, +/-X 0.6, +/-Z 0.8 (voxel_shader.gdshader) -- read off the WORLD normal. A
## terrain face never moves, so a table is exact there; an item's normal turns with
## its body, so a table with thresholds in it (top above |y| 0.9, then x against z)
## steps a whole shade the moment a face crosses one: a flicker on a tumbling block
## or a swinging hand that no light value explains. This probe holds the rule twice:
##
##   * the include's arithmetic, re-run in GDScript (as probe_bend_geo.gd does for
##     the bend include): it has to be the terrain's table on the six axes, and no
##     0.25-degree turn of the normal may move it by more than a percent -- while
##     the thresholded version it replaced is shown stepping 20% at once, so the
##     check is known to have teeth;
##   * the rendered frame: one block turning in front of a still camera, half a
##     degree a frame, and no frame may change more than a quarter of the sampled
##     picture. A shade step on one face changes most of it in a single frame;
##     texels sliding under the rotation change only a thin band.
##
##   probes/run_probe_shot.sh probes/probe_item_face_shade.gd 400
##
## WINDOWED: half of it reads the rendered frame, and the dummy renderer has none.

const STONE := 1
const CROP := 160            # the sampled square, in pixels, at the middle of the frame
const SAMPLE := 64           # ...resized to this before comparing (blocks, not texels)
const PIXEL_DELTA := 0.05    # a sample counts as changed above this much luma
const MAX_CHANGED_SHARE := 0.25
const VISIBLE_SHARE := 0.30  # how much of the crop the fixture item must cover
const MIN_LUMA := 0.05       # ...and how lit it must be to measure at all
const YAW_FROM := 15.0
const YAW_STEP := 0.5
const YAW_FRAMES := 120

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - t0) / 1_000_000.0 < seconds:
		await process_frame


# --- The rule, as arithmetic ---------------------------------------------------

## shaders/item_lighting.gdshaderinc's face_shade. Keep the two in step: this is
## the include's own arithmetic, re-run, so a change there that this does not copy
## is a change nothing here is holding to.
func _shade(n: Vector3) -> float:
	return 0.60 * n.x * n.x + 0.80 * n.z * n.z + (0.75 + 0.25 * n.y) * n.y * n.y


## The thresholded table it replaced, kept so the sweep can show what a step is.
func _shade_thresholded(n: Vector3) -> float:
	if absf(n.y) > 0.9:
		return 1.0 if n.y > 0.0 else 0.5
	return 0.6 if absf(n.x) > absf(n.z) else 0.8


func _axes_check() -> void:
	var axes := {
		"top +Y": [Vector3.UP, 1.0],
		"bottom -Y": [Vector3.DOWN, 0.5],
		"+X": [Vector3.RIGHT, 0.6],
		"-X": [Vector3.LEFT, 0.6],
		"+Z": [Vector3.BACK, 0.8],
		"-Z": [Vector3.FORWARD, 0.8],
	}
	var worst := 0.0
	var worst_name := ""
	for name in axes:
		var axis: Vector3 = axes[name][0]
		var want: float = axes[name][1]
		var got := _shade(axis)
		if absf(got - want) > worst:
			worst = absf(got - want)
			worst_name = "%s: %.4f, want %.1f" % [name, got, want]
	_ok("face shade: the terrain's table on the six axes", worst <= 0.001, worst_name if worst > 0.001 else "worst %.5f" % worst)


func _sweep_check() -> void:
	var worst_new := 0.0
	var worst_old := 0.0
	# Two sweeps, each through one of the thresholds the table used: yaw moves the
	# normal between +X and +Z (where x against z flipped), pitch between +Y and
	# +Z (where the top/bottom test flipped).
	for kind in range(2):
		var previous_new := 0.0
		var previous_old := 0.0
		for i in range(361):
			var a := deg_to_rad(float(i) * 0.25)
			var n := Vector3(cos(a), 0.0, sin(a)) if kind == 0 else Vector3(0.0, cos(a), sin(a))
			var now_new := _shade(n)
			var now_old := _shade_thresholded(n)
			if i > 0:
				worst_new = maxf(worst_new, absf(now_new - previous_new))
				worst_old = maxf(worst_old, absf(now_old - previous_old))
			previous_new = now_new
			previous_old = now_old
	_ok("face shade: a 0.25-degree turn moves it less than 1%", worst_new <= 0.01, "worst %.4f over both sweeps" % worst_new)
	_ok("face shade: the table it replaced does step (teeth)", worst_old >= 0.15, "worst %.4f over both sweeps" % worst_old)


# --- The rule, as pixels -------------------------------------------------------

## The middle of the frame, shrunk to SAMPLE blocks so a comparison is about
## shading and not about which side of a texel a pixel fell on.
func _crop() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	var w := img.get_width()
	var h := img.get_height()
	var region := Rect2i(w / 2 - CROP / 2, h / 2 - CROP / 2, CROP, CROP)
	var crop := img.get_region(region)
	crop.resize(SAMPLE, SAMPLE, Image.INTERPOLATE_BILINEAR)
	if crop.get_format() != Image.FORMAT_RGBA8:
		crop.convert(Image.FORMAT_RGBA8)
	return crop


func _share_changed(a: Image, b: Image, threshold: float, lumas: Array = []) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	var n := a.get_width() * a.get_height()
	var changed := 0
	var total := 0.0
	for i in range(n):
		var o := i * 4
		var la := (0.299 * float(pa[o]) + 0.587 * float(pa[o + 1]) + 0.114 * float(pa[o + 2])) / 255.0
		var lb := (0.299 * float(pb[o]) + 0.587 * float(pb[o + 1]) + 0.114 * float(pb[o + 2])) / 255.0
		total += lb
		if absf(la - lb) > threshold:
			changed += 1
	if not lumas.is_empty():
		lumas[0] = total / float(n)
	return float(changed) / float(n)


func _run() -> void:
	_sweep_check()
	_axes_check()

	DisplayServer.window_set_size(Vector2i(1280, 720))
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)

	var player: Node3D = main.get_node_or_null("Player")
	var dropped: Node = main.get_node_or_null("DroppedItems")
	var camera: Camera3D = main.get_node_or_null("Player/Camera3D")
	if player == null or dropped == null or camera == null:
		print("PROBE FAIL: main.tscn is missing Player / DroppedItems / Camera3D")
		quit(1)
		return

	for i in range(1800):
		await process_frame
		if player.has_method("is_on_floor") and player.is_on_floor():
			break
	await _seconds(1.0)

	# The fixture: one block on the view axis, a metre and a half out, where the
	# crop can be a square of it. Everything about it is then the probe's: the body
	# is parked (an asleep item is left by the solver alone), so the node's
	# rotation is set by hand -- which is what a tumbling item does for real.
	var eye := camera.global_position
	var forward := -camera.global_transform.basis.z
	var at := eye + forward * 1.5
	var clean := _crop()
	dropped.call("spawn", STONE, 1, at, forward)
	var items: Array = dropped.get("_items")
	if items.is_empty():
		_ok("face shade: fixture", false, "spawn() added no item")
		print("PROBE item face shade: %d failure(s)" % _failures)
		quit(1)
		return
	for other in items:
		other["asleep"] = true
		other["velocity"] = Vector3.ZERO
		other["spin"] = Vector3.ZERO
	var item: Dictionary = items[items.size() - 1]
	item["position"] = at
	var node: Node3D = item["node"]
	node.quaternion = Quaternion(Vector3.UP, deg_to_rad(YAW_FROM))
	await _seconds(0.5)

	var lumas: Array = [0.0]
	var shown := _crop()
	var covered := _share_changed(clean, shown, PIXEL_DELTA, lumas)
	var luma: float = lumas[0]
	if covered < VISIBLE_SHARE or luma < MIN_LUMA:
		_ok("face shade: fixture", false,
			"the item covers %.0f%% of the crop at luma %.3f: nothing to measure (is the view blocked?)" % [100.0 * covered, luma])
	else:
		var worst := 0.0
		var worst_yaw := 0.0
		var previous := shown
		for step in range(YAW_FRAMES):
			var yaw := YAW_FROM + YAW_STEP * float(step)
			node.quaternion = Quaternion(Vector3.UP, deg_to_rad(yaw))
			await process_frame
			var now := _crop()
			var changed := _share_changed(previous, now, PIXEL_DELTA)
			if changed > worst:
				worst = changed
				worst_yaw = yaw
			previous = now
		_ok("face shade: no frame changes a whole face", worst <= MAX_CHANGED_SHARE,
			"worst frame %.0f%% of the crop, at yaw %.1f deg (item luma %.3f)" % [100.0 * worst, worst_yaw, luma])

	print("PROBE item face shade: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
