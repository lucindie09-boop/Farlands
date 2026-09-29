extends SceneTree
## Slider behaviour, measured in the rendered frame:
##
##  1. the thumb travels the track's width minus its own, so its leftmost pixel
##     sits on the track's leftmost at the minimum and its rightmost on the
##     track's rightmost at the maximum (with the icon centred on the value
##     instead, half of it hangs off each end);
##  2. the state a pointer changes is the HANDLE: a hovered/focused slider
##     brightens the thumb and leaves the track's pixels exactly as they were,
##     and an uneditable slider draws our own dimmed thumb rather than the
##     engine's default disabled icon.
##
## The checks read rendered pixels, so this runs WINDOWED through
## .freebuff/run_probe_shot.sh; the shots land in user://menu_shots/.

const SHOT_DIR := "user://menu_shots"
const THUMB_TEX := "res://textures/gui/slider.png"
## The row to measure, by the label the row carries.
const ROW_LABEL := "FPS Cap"
## How close a pixel has to be to a reference colour to count as it.
const TOLERANCE := 0.015

var menu: Control = null
var failures := 0
## The thumb's middle rows are narrower than the whole icon: its rounded corners
## are background, so the detected centre run is inset by this much (in texture
## pixels) from the icon's own left and right edges.
var _inset := 0
var _centre := Color.BLACK
var _scale := 1.0


func _initialize() -> void:
	_run()


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _hex(c: Color) -> String:
	return "%02x%02x%02x" % [int(round(c.r * 255.0)), int(round(c.g * 255.0)), int(round(c.b * 255.0))]


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	if menu == null:
		print("PROBE FAIL: no SettingsMenu node under HUD")
		quit(1)
		return
	for i in range(60):
		await process_frame
	menu.call("_open")
	menu.call("_show_page", "settings")
	for i in range(6):
		await process_frame

	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	_read_thumb_shape()
	_scale = float(menu.call("_ui_scale"))
	print("probe: ui scale %.0f, thumb icon %d px wide, centre inset %d texture px"
		% [_scale, 8, _inset])

	var page: Control = menu.get("_pages")["settings"]
	var slider := _find_slider(page, ROW_LABEL)
	if slider == null:
		_fail("no \"%s\" slider on the settings page" % ROW_LABEL)
		print("probe: %d failures" % failures)
		quit(1)
		return

	await _measure(slider, slider.min_value, "slider_min", true)
	await _measure(slider, slider.max_value, "slider_max", false)
	await _check_states(slider)

	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)


## Put the slider at one end and measure where its thumb landed.
func _measure(slider: HSlider, value: float, shot: String, at_min: bool) -> void:
	slider.value = value
	var image := await _frame(shot)
	if image == null:
		return
	var rect := slider.get_global_rect()
	var cols := _thumb_columns(image, rect, _centre)
	if cols.is_empty():
		_fail("%s: no thumb pixels found between x=%d and x=%d"
			% [shot, int(rect.position.x), int(rect.end.x)])
		return

	var thumb_left := float(cols[0]) - _inset * _scale
	var thumb_right := float(cols[cols.size() - 1]) + 1.0 + _inset * _scale
	var width := thumb_right - thumb_left
	print("probe: %-14s track x %d..%d   thumb x %d..%d (%.0f px)"
		% [shot, int(round(rect.position.x)), int(round(rect.end.x)) - 1,
		   int(round(thumb_left)), int(round(thumb_right)) - 1, width])
	if absf(width - 8.0 * _scale) > 1.0:
		_fail("%s: thumb measures %.0f px wide, want %.0f (colour detection?)"
			% [shot, width, 8.0 * _scale])
	if at_min:
		if absf(thumb_left - rect.position.x) > 1.0:
			_fail("%s: the thumb's left edge is %.0f px from the track's, want 0"
				% [shot, thumb_left - rect.position.x])
	elif absf(thumb_right - rect.end.x) > 1.0:
		_fail("%s: the thumb's right edge is %.0f px from the track's, want 0"
			% [shot, thumb_right - rect.end.x])


## Which surface the pointer's state reaches: the handle, not the track.
func _check_states(slider: HSlider) -> void:
	slider.value = lerpf(slider.min_value, slider.max_value, 0.5)
	# Whatever the OS pointer is doing, start from the un-highlighted state; the
	# engine sends this same notification when the pointer really leaves, and
	# sends the enter one below when it really arrives.
	slider.notification(Control.NOTIFICATION_MOUSE_EXIT)
	var idle := await _frame("slider_idle")
	if idle == null:
		return
	var rect := slider.get_global_rect()
	var cols := _thumb_columns(idle, rect, _centre)
	if cols.size() < 4:
		_fail("slider_idle: the idle thumb's centre colour is not on screen (%d columns)" % cols.size())
		return
	# The thumb does not move between the states, so one column in the middle of
	# the idle run samples every one of them. The row is above the value label's
	# glyphs, which are drawn over the slider's middle.
	var mid := int((cols[0] + cols[cols.size() - 1]) / 2)
	var row := int(round(rect.position.y)) + 6
	var track_x := int(round(rect.position.x)) + 6
	var idle_thumb: Color = idle.get_pixel(mid, row)
	var idle_track: Color = idle.get_pixel(track_x, row)
	print("probe: slider_idle     thumb %s   track %s" % [_hex(idle_thumb), _hex(idle_track)])

	slider.notification(Control.NOTIFICATION_MOUSE_ENTER)
	var hover := await _frame("slider_hover")
	slider.notification(Control.NOTIFICATION_MOUSE_EXIT)
	if hover != null:
		var hover_thumb: Color = hover.get_pixel(mid, row)
		var hover_track: Color = hover.get_pixel(track_x, row)
		print("probe: slider_hover    thumb %s   track %s" % [_hex(hover_thumb), _hex(hover_track)])
		if hover_thumb.v <= idle_thumb.v + 0.05:
			_fail("hover: the thumb did not brighten (%s -> %s)" % [_hex(idle_thumb), _hex(hover_thumb)])
		if hover_track != idle_track:
			_fail("hover: the track changed too (%s -> %s)" % [_hex(idle_track), _hex(hover_track)])
		if _thumb_columns(hover, rect, hover_thumb).size() < cols.size() - 2:
			_fail("hover: the highlighted handle is not the full-height thumb any more")

	# Mipmap bias with mipmaps off is the menu's uneditable slider: it must draw
	# our own dimmed thumb, not the engine's default disabled icon (which is a
	# different size and colour entirely).
	slider.editable = false
	var off := await _frame("slider_disabled")
	slider.editable = true
	if off == null:
		return
	var off_thumb: Color = off.get_pixel(mid, row)
	var off_track: Color = off.get_pixel(track_x, row)
	print("probe: slider_disabled thumb %s   track %s" % [_hex(off_thumb), _hex(off_track)])
	if off_thumb.v >= idle_thumb.v - 0.05:
		_fail("disabled: the thumb was not dimmed (%s -> %s)" % [_hex(idle_thumb), _hex(off_thumb)])
	if off_track != idle_track:
		_fail("disabled: the track changed too (%s -> %s)" % [_hex(idle_track), _hex(off_track)])
	if _thumb_columns(off, rect, off_thumb).size() < cols.size() - 2:
		_fail("disabled: the handle is not our thumb (wrong icon size or colour)")


## The columns of the rendered frame that carry the thumb, found by colour: a
## column counts only when most of the band inside the slider wears it, which a
## smaller or differently shaped icon cannot fake.
func _thumb_columns(image: Image, rect: Rect2, colour: Color) -> Array:
	var top := int(round(rect.position.y)) + 4
	var bottom := int(round(rect.end.y)) - 4
	var need := int(float(bottom - top) * 0.75)
	var cols: Array = []
	for x in range(maxi(0, int(rect.position.x) - 32), mini(image.get_width(), int(rect.end.x) + 32)):
		var hits := 0
		for y in range(top, bottom):
			if _is(image.get_pixel(x, y), colour):
				hits += 1
		if hits >= need:
			cols.append(x)
	return cols


func _is(c: Color, colour: Color) -> bool:
	return absf(c.r - colour.r) < TOLERANCE and absf(c.g - colour.g) < TOLERANCE \
		and absf(c.b - colour.b) < TOLERANCE


## Let the frame settle and hand back what it drew.
func _frame(shot: String) -> Image:
	for i in range(4):
		await process_frame
	var image: Image = root.get_texture().get_image()
	if image == null:
		_fail("%s: no viewport image (running headless?)" % shot)
		return null
	image.save_png("%s/%s_%dx%d.png" % [SHOT_DIR, shot, image.get_width(), image.get_height()])
	return image


## Read the icon itself, so the measurement knows which pixels of a detected run
## belong to the thumb and which to its transparent corners.
func _read_thumb_shape() -> void:
	var tex := (load(THUMB_TEX) as Texture2D).get_image()
	var row := tex.get_height() / 2
	_centre = tex.get_pixel(tex.get_width() / 2, row)
	var first := -1
	var last := -1
	for x in range(tex.get_width()):
		if _is(tex.get_pixel(x, row), _centre):
			if first < 0:
				first = x
			last = x
	_inset = first if first >= 0 else 0
	print("probe: thumb icon %s, centre colour %s, centre run %d..%d of %d"
		% [tex.get_size(), _hex(_centre), first, last, tex.get_width()])


## The row whose label is `label`, as the column of the row builder: the option
## control carries its label in metadata and its slider beside it.
func _find_slider(node: Node, label: String) -> HSlider:
	for child in node.get_children():
		if child is Control and String(child.get_meta("row_label", "")) == label \
				and child.has_meta("slider"):
			return child.get_meta("slider")
		var found := _find_slider(child, label)
		if found != null:
			return found
	return null
