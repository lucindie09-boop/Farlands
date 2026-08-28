extends SubViewportContainer

# Drag-to-orbit preview of the player model, used by the skin maker.
# The model sits in an isolated sub-viewport; the camera orbits it on drag.
# The viewport background is transparent so the page behind shows through.

const MODEL_SCENE: PackedScene = preload("res://player.glb")
const SKIN_SCRIPT: GDScript = preload("res://player_model.gd")

var _camera: Camera3D
var _target := Vector3(0, 16, 0)
var _yaw := -35.0
var _pitch := 14.0
var _dist := 34.0
var _rotating := false

func _ready():
	# The camera must NOT be a child of the rotating node: it would orbit along
	# with the model, freezing the view while the world-fixed lights play over
	# the faces. Instead the model stays put and the camera orbits its AABB
	# centre (the glb's origin is at the feet, not the visual middle).
	mouse_filter = Control.MOUSE_FILTER_STOP
	stretch = true

	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.transparent_bg = true
	vp.msaa_3d = Viewport.MSAA_4X
	add_child(vp)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1, 1, 1, 1)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1, 1)
	env.ambient_light_energy = 0.35
	var world := World3D.new()
	world.environment = env
	vp.world_3d = world

	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.4
	sun.rotation_degrees = Vector3(-45, -35, 0)
	vp.add_child(sun)

	var fill := DirectionalLight3D.new()
	fill.light_energy = 0.5
	fill.rotation_degrees = Vector3(70, 140, 0)
	vp.add_child(fill)

	var model := MODEL_SCENE.instantiate()
	model.scale = Vector3(0.9, 0.9, 0.9)
	model.set_script(SKIN_SCRIPT)
	vp.add_child(model)

	# Every glb mesh's vertex data starts at its own local origin (its segment's
	# bottom), and the node transform lifts each body part. `get_aabb()` reports
	# the raw geometry only, so compose the path transform manually or the merged
	# extent collapses to the legs. The authored model also sits at +z ~1.5.
	var total := AABB()
	var first := true
	for child in model.find_children("", "MeshInstance3D", true, false):
		var mi := child as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var a := _xform_aabb(mi.mesh.get_aabb(), model.transform * mi.transform)
		if first:
			total = a
			first = false
		else:
			total = total.merge(a)
	if not first:
		_target = total.get_center()

	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.make_current()
	vp.add_child(_camera)

	_update_camera()

func _xform_aabb(a: AABB, t: Transform3D) -> AABB:
	var mn := a.position
	var mx := a.position + a.size
	var out := AABB(t * mn, Vector3.ZERO)
	out = out.expand(t * Vector3(mx.x, mn.y, mn.z))
	out = out.expand(t * Vector3(mn.x, mx.y, mn.z))
	out = out.expand(t * Vector3(mn.x, mn.y, mx.z))
	out = out.expand(t * Vector3(mx.x, mx.y, mn.z))
	out = out.expand(t * Vector3(mx.x, mn.y, mx.z))
	out = out.expand(t * Vector3(mn.x, mx.y, mx.z))
	out = out.expand(t * Vector3(mx.x, mx.y, mx.z))
	return out

func _update_camera():
	var pitch_r := deg_to_rad(_pitch)
	var yaw_r := deg_to_rad(_yaw)
	var dir := Vector3(
		_dist * cos(pitch_r) * sin(yaw_r),
		_dist * sin(pitch_r),
		_dist * cos(pitch_r) * cos(yaw_r)
	)
	_camera.global_position = _target + dir
	_camera.look_at(_target, Vector3.UP)

func _gui_input(event):
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_rotating = true
		elif _rotating:
			_rotating = false
	elif event is InputEventMouseMotion and _rotating:
		_yaw -= event.relative.x * 0.35
		_pitch = clampf(_pitch - event.relative.y * 0.35, -85.0, 85.0)
		_update_camera()

# Catch a left-button release that happens outside this control while dragging.
func _input(event):
	if _rotating and event is InputEventMouseButton \
			and event.button_index == MOUSE_BUTTON_LEFT and not event.pressed:
		_rotating = false