extends SceneTree
## Does our plane test agree with Godot's frustum convention?
##
## `Frustum::is_aabb_visible` rejects a box when `plane.distance_to(center) < -r`
## for any of the six planes, i.e. it assumes the normals point INWARD (a point
## inside the frustum is on the positive side of all six). Godot's
## `Camera3D::get_frustum()` does not document which way its normals point, so
## rather than trust a reading of the docs this asks the engine: `is_position_in_frustum`
## is the ground truth for the same points, and the box in FRONT of the camera must
## come out visible while the one BEHIND must not.
##
##   .freebuff/run_probe.sh .freebuff/probe_frustum.gd 120

const UNIT := 32.0

var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

# The predicate from src/core/frustum.hpp, evaluated here so the C++ does not
# have to change for the test to run.
func _ours(planes: Array, aabb_center: Vector3, half: Vector3) -> bool:
	for p in planes:
		var n: Vector3 = p.normal
		var r := half.x * absf(n.x) + half.y * absf(n.y) + half.z * absf(n.z)
		if (n.dot(aabb_center) - p.d) < -r:
			return false
	return true

func _initialize() -> void:
	_run()

func _run() -> void:
	var cam := Camera3D.new()
	cam.fov = 70.0
	cam.near = 0.05
	cam.far = 2048.0
	root.add_child(cam)
	# At (500, 100, 500) looking along -Z.
	cam.global_transform = Transform3D(Basis(), Vector3(500.0, 100.0, 500.0))
	cam.make_current()
	await process_frame
	await process_frame

	var planes: Array = cam.get_frustum()
	print("probe: planes=%d current=%s pos=%s" % [planes.size(), str(root.get_camera_3d() == cam), str(cam.global_position)])
	if planes.size() < 6:
		_fail("get_frustum returned fewer than 6 planes")
		quit(1)
		return
	for i in range(planes.size()):
		var p: Plane = planes[i]
		print("probe: plane %d normal=(%.2f,%.2f,%.2f) d=%.2f" % [i, p.normal.x, p.normal.y, p.normal.z, p.d])

	# A chunk box 3 chunks in front of the eye (Camera3D looks down -Z), the eye
	# itself, and one 3 chunks behind. Half extents are the chunk half-size.
	var half := Vector3(UNIT * 0.5, UNIT * 0.5, UNIT * 0.5)
	var cases := [
		["in front", Vector3(500.0, 100.0, 500.0 - 3.0 * UNIT)],
		["at the eye", Vector3(500.0, 100.0, 500.0)],
		["behind", Vector3(500.0, 100.0, 500.0 + 3.0 * UNIT)],
		["far in front", Vector3(500.0, 100.0, 500.0 - 40.0 * UNIT)],
	]
	var mismatches := 0
	for c in cases:
		var label: String = c[0]
		var center: Vector3 = c[1]
		var truth: bool = cam.is_position_in_frustum(center)
		var ours: bool = _ours(planes, center, half)
		print("probe: %-12s engine=%s ours=%s %s" % [label, str(truth), str(ours), "AGREE" if truth == ours else "MISMATCH"])
		if truth != ours:
			mismatches += 1

	if mismatches > 0:
		_fail("%d of %d boxes disagreed with the engine - the plane test uses the wrong normal convention" % [mismatches, cases.size()])

	# Same question the sweep asks, for a chunk the camera is standing in.
	var eye_chunk := Vector3i(floori(500.0 / UNIT), floori(100.0 / UNIT), floori(500.0 / UNIT))
	var eye_center := Vector3(eye_chunk.x * UNIT + UNIT * 0.5, eye_chunk.y * UNIT + UNIT * 0.5, eye_chunk.z * UNIT + UNIT * 0.5)
	print("probe: own-chunk centre=%s engine=%s ours=%s" % [str(eye_center), str(cam.is_position_in_frustum(eye_center)), str(_ours(planes, eye_center, half))])

	print("PROBE %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)
