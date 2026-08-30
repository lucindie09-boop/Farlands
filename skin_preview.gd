extends SubViewportContainer

# Drag-to-orbit preview of the player model, used by the skin maker.
# The model sits in an isolated sub-viewport; the camera orbits it on drag.
# The viewport background is transparent so the page behind shows through.
#
# Modes (left button, cycled by the tool button): DRAW paints the texel under
# the cursor, FILL floods the clicked face, BOX drags a paint rectangle.
# Right-drag always zooms. Pressing empty space always orbits the camera.

const MODEL_SCENE: PackedScene = preload("res://player.glb")
const SKIN_SCRIPT: GDScript = preload("res://player_model.gd")

signal paint_history_changed(has_undo: bool)

const ZOOM_SPEED := 0.25
const ZOOM_MIN := 8.0
const ZOOM_MAX := 70.0
const TEXEL := 1.0 / 64.0
# UV gap beyond which two samples are considered different atlas islands / body
# parts, so interpolation must not bridge across them (mega pixel streaks).
const UV_BREAK_DIST := 2.0 / 64.0

var _camera: Camera3D
var _model: Node3D
var _target := Vector3(0, 16, 0)
var _yaw := -35.0
var _pitch := 14.0
var _dist := 34.0
var _rotating := false
var _zooming := false
var _painting := false
var _paint_color := Color.WHITE
var _last_paint_uv := Vector2.INF
var _last_hit_mesh: MeshInstance3D
var _prev_hit_mesh: MeshInstance3D
var _last_hit_uvs := PackedVector2Array()
var _tool := TOOL_DRAW
var _boxing := false
var _box_anchor_uv := Vector2.ZERO
var _box_last_uv := Vector2.INF
var _box_base: Image
var _box_touched := {}
var _box_face_min := Vector2.ZERO
var _box_face_max := Vector2.ZERO

const TOOL_DRAW := 0
const TOOL_FILL := 1
const TOOL_BOX := 2
var _stroke: Array = []
var _undo_stack: Array = []

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

	var model := MODEL_SCENE.instantiate() as Node3D
	model.scale = Vector3(0.9, 0.9, 0.9)
	model.set_script(SKIN_SCRIPT)
	vp.add_child(model)
	_model = model
	_model.set_paint_color(_paint_color)
	_model.texel_painted.connect(_record_stroke_texel)

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

func _can_start_drag() -> bool:
	# Don't steal presses that landed on interactive controls (the colour
	# wheel, its internal rows, or the page's buttons). Empty page space and
	# the preview window itself are fair game for orbiting/zooming/painting.
	var hovered := get_viewport().gui_get_hovered_control()
	if hovered == null or hovered == self or hovered is ColorRect:
		return true
	# Gui hover can lag one event right after a button press: the last-pressed
	# button is still "hovered" even though the pointer already moved onto the
	# preview, which would refuse the very first drag. Geometry is the source
	# of truth - if the pointer is inside the preview rect and the claimed
	# control isn't actually under it, the press belongs to us.
	var pos := get_global_mouse_position()
	if get_global_rect().has_point(pos) and not hovered.get_global_rect().has_point(pos):
		return true
	return false

# All pointer handling lives here (not _gui_input) so dragging works anywhere
# on the skin page, not just inside the preview window. Handled in _input so a
# released button outside the window still ends the drag. The menu keeps pages
# in the tree when hidden, so everything is gated on visibility.
func _input(event):
	if not is_visible_in_tree():
		return
	if event is InputEventKey and event.pressed and event.keycode == KEY_Z \
			and (event.ctrl_pressed or event.command_pressed):
		undo_last()
		return
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT and event.pressed:
			if not _can_start_drag():
				return
			get_viewport().set_input_as_handled()
			# Left button behaves per tool: DRAW strokes texels as you drag,
			# FILL floods the clicked face, BOX drags a rectangle. Pressing
			# empty space always orbits the camera instead, whatever the tool.
			var uv := _raycast_uv()
			if uv != Vector2.INF:
				match _tool:
					TOOL_FILL:
						_fill_face(uv)
					TOOL_BOX:
						# Simple drag box: the first clicked pixel is one corner,
						# the pixel under the cursor on release is the other,
						# and everything between gets filled. Live-fills as you
						# drag for feedback; release trims to the final rect.
						_boxing = true
						_box_anchor_uv = uv
						_box_last_uv = uv
						var face := _hit_face_rect()
						_box_face_min = face.position
						_box_face_max = face.end
						_box_base = _model.get_paint_image().duplicate()
						_box_touched.clear()
						_fill_box_live(uv)
					_:
						_painting = true
						_stroke = []
						# A new stroke must not interpolate from the previous
						# one's last UV, or the first paint draws a line.
						_last_paint_uv = Vector2.INF
						_prev_hit_mesh = null
						_paint_at_cursor()
			else:
				_rotating = true
		elif event.button_index == MOUSE_BUTTON_LEFT and not event.pressed:
			if _painting or _rotating or _boxing:
				get_viewport().set_input_as_handled()
			_rotating = false
			_painting = false
			if _boxing:
				_finish_box()
			_commit_stroke()
		elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed:
			if not _can_start_drag():
				return
			get_viewport().set_input_as_handled()
			_zooming = true
		elif event.button_index == MOUSE_BUTTON_RIGHT and not event.pressed:
			if _zooming:
				get_viewport().set_input_as_handled()
			_zooming = false
	elif event is InputEventMouseMotion:
		if _rotating:
			_yaw -= event.relative.x * 0.35
			_pitch = clampf(_pitch - event.relative.y * 0.35, -85.0, 85.0)
			_update_camera()
			get_viewport().set_input_as_handled()
		elif _zooming:
			_dist = clampf(_dist - event.relative.y * ZOOM_SPEED, ZOOM_MIN, ZOOM_MAX)
			_update_camera()
			get_viewport().set_input_as_handled()
		elif _painting:
			_paint_at_cursor()
			get_viewport().set_input_as_handled()
		elif _boxing:
			var buv := _raycast_uv()
			if buv != Vector2.INF:
				_fill_box_live(buv)
			get_viewport().set_input_as_handled()

func set_uv_overlay(enabled: bool) -> void:
	if _model and _model.has_method("set_uv_overlay"):
		_model.set_uv_overlay(enabled)

func set_paint_color(color: Color) -> void:
	_paint_color = color
	if _model and _model.has_method("set_paint_color"):
		_model.set_paint_color(color)

# Cycle the left-button tool: DRAW (texel strokes), FILL (flood the clicked
# face), BOX (drag a rectangle). Any gesture in progress is finished first so
# nothing already painted is lost.
func set_tool(tool: int) -> void:
	if _boxing:
		_finish_box()
	_painting = false
	_commit_stroke()
	_tool = tool

# Flood the face under the cursor with the current colour. The raycast records
# the hit triangle's UV corners; for a square cuboid face those corners span
# the whole face on the atlas, so the bounding rectangle IS the face.
func _fill_face(_uv: Vector2) -> void:
	var rect := _hit_face_rect()
	# Texel centres with a half-open range keep shared island borders on the
	# correct side; neighbours that share the exact boundary don't get painted.
	_stroke = []
	for py in range(64):
		for px in range(64):
			var c := Vector2((px + 0.5) * TEXEL, (py + 0.5) * TEXEL)
			if c.x >= rect.position.x and c.x < rect.end.x \
					and c.y >= rect.position.y and c.y < rect.end.y:
				_model.paint_texel(c, _paint_color)
	_commit_stroke()

func _hit_face_rect() -> Rect2:
	# Rect2.end is DERIVED from position+size, so you can't accumulate corners
	# through it (end shrinks whenever position shrinks). Track explicit
	# min/max vectors and derive the final rectangle once.
	var mn := Vector2(INF, INF)
	var mx := Vector2(-INF, -INF)
	for p in _last_hit_uvs:
		mn.x = minf(mn.x, p.x)
		mn.y = minf(mn.y, p.y)
		mx.x = maxf(mx.x, p.x)
		mx.y = maxf(mx.y, p.y)
	return Rect2(mn, mx - mn)

# True when `uv` lies on (a hair around) the first-clicked face's own UV rect.
# The box must only ever be computed from positions on that face: a cursor on
# another body part has a UV from a different atlas island, and clamping that
# in would collapse or re-anchor the box to a random corner.
func _uv_on_box_face(uv: Vector2) -> bool:
	var tol := TEXEL * 0.5
	return uv.x >= _box_face_min.x - tol and uv.x <= _box_face_max.x + tol \
			and uv.y >= _box_face_min.y - tol and uv.y <= _box_face_max.y + tol

# Live paint the rectangle between the box anchor and the cursor, CLAMPED to the
# face that was originally clicked. Every touched texel is remembered so release
# can trim stray cells (from pulling back) and build one undo step. The box is
# defined in WHOLE texels: every texel between (and including) the pressed texel
# and the cursor's texel gets filled, so the pixel you started on is always part
# of the box.
func _fill_box_live(uv: Vector2) -> void:
	# Left the face (another body part or off the island): keep the LAST good
	# rectangle instead of collapsing the box to the anchor pixel.
	if not _uv_on_box_face(uv):
		return
	_box_last_uv = uv
	var ax := clampi(int(_box_anchor_uv.x * 64.0), 0, 63)
	var ay := clampi(int(_box_anchor_uv.y * 64.0), 0, 63)
	var cx := clampi(int(uv.x * 64.0), 0, 63)
	var cy := clampi(int(uv.y * 64.0), 0, 63)
	var x0 := mini(ax, cx)
	var x1 := maxi(ax, cx)
	var y0 := mini(ay, cy)
	var y1 := maxi(ay, cy)
	# Fill exactly these cells: lo/hi are the texel spans' edges, which the
	# manager's centre-based fill maps back to the same inclusive range.
	_model.fill_uv_rect(
		Vector2(float(x0) / 64.0, float(y0) / 64.0),
		Vector2(float(x1 + 1) / 64.0, float(y1 + 1) / 64.0),
		_paint_color)
	for yy in range(y0, y1 + 1):
		for xx in range(x0, x1 + 1):
			_box_touched[yy * 64 + xx] = true

# The texel cells of the box between the anchor and `uv` (inclusive, always
# includes the pixel that was pressed), used on release to compute the exact
# final rectangle. Uses the same whole-texel convention as _fill_box_live.
func _box_cell_set(uv: Vector2) -> Dictionary:
	var ax := clampi(int(_box_anchor_uv.x * 64.0), 0, 63)
	var ay := clampi(int(_box_anchor_uv.y * 64.0), 0, 63)
	var cx := clampi(int(uv.x * 64.0), 0, 63)
	var cy := clampi(int(uv.y * 64.0), 0, 63)
	var cells := {}
	for yy in range(mini(ay, cy), maxi(ay, cy) + 1):
		for xx in range(mini(ax, cx), maxi(ax, cx) + 1):
			cells[yy * 64 + xx] = true
	return cells

# Release the box: the final rectangle is exactly anchor .. release-point texel.
# Cells that were live-painted but fall outside it (the cursor was pulled back)
# are restored to their pre-drag colour, then one undo step records the result.
func _finish_box() -> void:
	if _box_base == null:
		_box_touched.clear()
		_boxing = false
		_box_last_uv = Vector2.INF
		return
	var last := _box_last_uv if _box_last_uv != Vector2.INF else _box_anchor_uv
	var cells := _box_cell_set(last)
	for key in _box_touched:
		if not cells.has(key):
			var k: int = int(key)
			var px := k % 64
			var py := int(k / 64.0)
			_model.undo_texel(px, py, _box_base.get_pixel(px, py))
	_box_touched = cells
	_commit_box_stroke()
	_boxing = false
	_box_last_uv = Vector2.INF

# Release the box: record a single undo stroke whose "old" values come from the
# pre-drag snapshot, so live-painted texels undo to the original colour even
# though they were already changed mid-drag.
func _commit_box_stroke() -> void:
	if _box_base == null:
		_box_touched.clear()
		return
	_stroke = []
	for key in _box_touched:
		var k: int = int(key)
		var px := k % 64
		var py := int(k / 64.0)
		_stroke.append({"x": px, "y": py, "old": _box_base.get_pixel(px, py), "new": _paint_color})
	_box_touched.clear()
	_box_base = null
	_commit_stroke()

# Save the current skin (base + all paint edits) to a PNG at `path`.
# Always saves the CLEAN (noise-free) image so noise remains reversible.
func save_skin(path: String) -> bool:
	if _model == null or not _model.has_method("get_paint_image"):
		return false
	var skin_manager := get_node_or_null("/root/SkinManager")
	if skin_manager == null:
		return false
	# Get the clean image (noise_base if noise is active, otherwise current image)
	var img: Image
	if skin_manager.has_method("get_clean_image"):
		img = skin_manager.get_clean_image()
	if img == null:
		img = _model.get_paint_image()
	if img == null:
		return false
	return img.save_png(path) == OK

# Replace the preview's skin from a saved PNG at `path`. The undo history is
# cleared because its entries refer to the previous image's pixels.
func load_skin(path: String) -> bool:
	if _model == null or not _model.has_method("load_skin_image"):
		return false
	var img := Image.load_from_file(path)
	if img == null:
		return false
	_rotating = false
	_painting = false
	_boxing = false
	_box_last_uv = Vector2.INF
	_box_touched.clear()
	_box_base = null
	_commit_stroke()
	_stroke = []
	_undo_stack = []
	_last_paint_uv = Vector2.INF
	_prev_hit_mesh = null
	# Do NOT reset noise here - the loaded image is clean (noise-free), and the
	# settings menu will re-apply the noise value from the sidecar JSON after loading.
	_model.load_skin_image(img)
	paint_history_changed.emit(false)
	return true

# Live noise severity (0..100). The effect and its reversibility base live in
# SkinManager (which survives the skin page being rebuilt on every menu open),
# so closing and reopening the maker leaves the slider value and the noise state
# intact.
func set_noise(severity: float) -> void:
	if _model == null or not _model.has_method("set_noise"):
		return
	_model.set_noise(severity)

func _record_stroke_texel(px: int, py: int, old_color: Color, new_color: Color) -> void:
	_stroke.append({"x": px, "y": py, "old": old_color, "new": new_color})

func _commit_stroke() -> void:
	if _stroke.is_empty():
		return
	_undo_stack.append(_stroke)
	_stroke = []
	paint_history_changed.emit(true)

func undo_last() -> void:
	_commit_stroke()
	if _undo_stack.is_empty():
		return
	# Revert in reverse paint order so a texel painted twice in one stroke
	# winds up at its pre-stroke colour (forward order would leave the stale
	# "old" value from the earlier entry).
	var stroke: Array = _undo_stack.pop_back()
	for i in range(stroke.size() - 1, -1, -1):
		var entry: Dictionary = stroke[i]
		var x: int = entry["x"]
		var y: int = entry["y"]
		var old: Color = entry["old"]
		_model.undo_texel(x, y, old)
	paint_history_changed.emit(not _undo_stack.is_empty())

func _paint_at_cursor() -> void:
	if _model == null or not _model.has_method("paint_texel"):
		return
	var uv := _raycast_uv()
	if uv == Vector2.INF:
		_last_paint_uv = Vector2.INF
		_prev_hit_mesh = null
		return
	# Only bridge gaps between samples on the SAME atlas island: the skin map
	# packs every body part into its own UV island, so a drag that crosses a
	# part boundary jumps across atlas space. Interpolating then paints a long
	# streak over whatever islands the atlas line happens to pass through.
	# A different mesh or a UV jump beyond UV_BREAK_DIST means we crossed a
	# seam, so bail to a single-pixel paint and continue from that point.
	if _last_paint_uv != Vector2.INF and _last_hit_mesh == _prev_hit_mesh \
			and _last_paint_uv.distance_to(uv) < UV_BREAK_DIST:
		var step_count := int(ceil(_last_paint_uv.distance_to(uv) / (TEXEL * 0.5)))
		for i in range(step_count + 1):
			var t := float(i) / float(step_count)
			_model.paint_texel(_last_paint_uv.lerp(uv, t), _paint_color)
	else:
		_model.paint_texel(uv, _paint_color)
	_last_paint_uv = uv
	_prev_hit_mesh = _last_hit_mesh

# Cast a ray from the camera through the cursor and return the interpolated UV
# of the closest hit. MeshInstance3D surfaces have no physics geometry, so the
# mesh triangles are tested directly (the voxel model is tiny).
func _raycast_uv() -> Vector2:
	var origin := _camera.project_ray_origin(get_local_mouse_position())
	var dir := _camera.project_ray_normal(get_local_mouse_position())
	var best := INF
	var best_uv := Vector2.INF
	_last_hit_mesh = null
	_last_hit_uvs = PackedVector2Array()
	for child in _model.find_children("", "MeshInstance3D", true, false):
		var mi := child as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var inv := mi.global_transform.affine_inverse()
		var lo := inv * origin
		var ld := inv.basis * dir
		for s in range(mi.mesh.get_surface_count()):
			var arrays := mi.mesh.surface_get_arrays(s)
			if arrays.is_empty():
				continue
			var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
			if uvs.size() != verts.size():
				var uv2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
				if uv2.size() == verts.size():
					uvs = uv2
			var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
			var tri_count: int
			if idx.size() >= 3:
				tri_count = int(idx.size() / 3.0)
			else:
				tri_count = int(verts.size() / 3.0)
			for t in range(tri_count):
				var i0: int
				var i1: int
				var i2: int
				if idx.size() >= 3:
					var ti := t * 3
					i0 = idx[ti]
					i1 = idx[ti + 1]
					i2 = idx[ti + 2]
				else:
					i0 = t * 3
					i1 = t * 3 + 1
					i2 = t * 3 + 2
				var hit := _ray_triangle(lo, ld, verts[i0], verts[i1], verts[i2])
				if not hit.is_empty() and hit.t < best:
					best = hit.t
					_last_hit_mesh = mi
					var u: float = hit.u
					var v: float = hit.v
					best_uv = uvs[i0] * (1.0 - u - v) + uvs[i1] * u + uvs[i2] * v
					_last_hit_uvs = PackedVector2Array([uvs[i0], uvs[i1], uvs[i2]])
	return best_uv

func _ray_triangle(origin: Vector3, dir: Vector3, a: Vector3, b: Vector3, c: Vector3) -> Dictionary:
	var e1 := b - a
	var e2 := c - a
	var p := dir.cross(e2)
	var det := e1.dot(p)
	if absf(det) < 1e-9:
		return {}
	var inv_det := 1.0 / det
	var s1 := origin - a
	var u := s1.dot(p) * inv_det
	if u < 0.0 or u > 1.0:
		return {}
	var q := s1.cross(e1)
	var v := dir.dot(q) * inv_det
	if v < 0.0 or u + v > 1.0:
		return {}
	var t := e2.dot(q) * inv_det
	if t <= 0.0:
		return {}
	return {"t": t, "u": u, "v": v}