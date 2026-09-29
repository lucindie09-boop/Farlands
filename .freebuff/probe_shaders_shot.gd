extends SceneTree
## The Shaders page and the passes it drives.
##
## Runs WINDOWED: every check below is about pixels that were actually rendered,
## and the dummy renderer of --headless has no frame to read. Run it through
## .freebuff/run_probe_shot.sh.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_shaders_shot.gd [timeout]
##
## What it pins:
##   - the pause menu offers Shaders, and the page it leads to is the registry:
##     one switch row per effect, then one row per uniform under the effect's
##     own heading, and every uniform the registry names reaching the material
##   - the two kinds of effect are two kinds of thing: a screen pass gets a rect
##     of its own to draw the frame through, and a vertex effect drives the very
##     materials the world is drawn with, so the switch it has reaches them
##   - the overlay draws UNDER the menu pages in the HUD, so tuning an effect
##     never paints the menu you are tuning it from
##   - the frame changes when an effect is switched on, and each uniform does the
##     one thing its name says: the tube's corners go dark, its beam ripples, its
##     guns separate, glow lifts its bright pixels, vignette drops its edges and
##     its tone controls move brightness and saturation; the bend moves the far
##     ground and leaves the ground underfoot where it was
##   - the stack survives the round trip through settings.cfg and through its own
##     `FS-` share code
##
## Where a vertex effect's *geometry* is pinned is probe_bend_geo.gd: this file
## runs against the live world, where what can be measured is that the switch
## reaches the world and where in the frame the change lands, not how many blocks
## it moved.
##
## The user's own settings are read at the start and written back at the end, so
## nothing here leaves the game looking different than it was found.

const SHOT_DIR := "user://shader_shots"
const SETTINGS_PATH := "user://settings.cfg"
const OVERLAY_PATH := "HUD/ShaderOverlay"
# The effects are registry entries, so a check about one of them names it rather
# than counting rows: World Bend was added above the CRT, and "the first effect"
# would have quietly become a different effect that day.
const CRT_ID := "crt"
const BEND_ID := "bend"

var overlay: Control = null
var menu: Control = null
var ids: Array = []
var failures := 0
var shots := 0
var _settings_backup := PackedByteArray()
var _had_settings := false


func _initialize() -> void:
	_run()


func _run() -> void:
	var size := Vector2i(1280, 720)
	if OS.get_environment("PROBE_W").is_valid_int():
		size.x = int(OS.get_environment("PROBE_W"))
	if OS.get_environment("PROBE_H").is_valid_int():
		size.y = int(OS.get_environment("PROBE_H"))
	DisplayServer.window_set_size(size)
	_backup_settings()
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)

	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		_finish()
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	overlay = main.get_node_or_null(OVERLAY_PATH)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	if overlay == null or menu == null:
		_fail("no %s or HUD/SettingsMenu in main.tscn" % OVERLAY_PATH)
		_finish()
		return

	for i in range(60):
		await process_frame

	check_registry()
	if failures == 0:
		await check_menu()
		await check_rendering(size)
		check_persistence()

	_finish()


# --- The registry, the layers and where they sit ------------------------------

func check_registry() -> void:
	var definitions: Array = overlay.call("get_definitions")
	if definitions.is_empty():
		_fail("the overlay has no shader definitions (data/shaders.json)")
		return
	ids = overlay.call("get_shader_ids")
	print("probe: %d shader effect(s) registered: %s" % [definitions.size(), ids])

	# The kinds bring up different things, and the count of rects is the count of
	# the two *pass* kinds: a vertex effect with a rect would be a picture of a bend
	# rather than a bend, and a pass without one would draw nothing at all.
	#
	# What separates the two pass kinds is where the rect is drawn, and that is a
	# property of the layer rather than of the shader - a world entry quietly built
	# as a screen effect would grade the hotbar along with the rock, and the shader
	# itself could not tell. So the z_index each layer ends up with is checked
	# against its kind, and it is the whole of the difference the registry's third
	# kind means here.
	var passes: Array = []
	var vertices: Array = []
	for definition in definitions:
		if String(definition.get("kind", "screen")) == "vertex":
			vertices.append(definition)
		else:
			passes.append(definition)

	var layers: Array = []
	for child in overlay.get_children():
		if child is ColorRect:
			layers.append(child)
	if layers.size() != passes.size():
		_fail("%d layer(s) for %d pass effect(s)" % [layers.size(), passes.size()])
	if vertices.is_empty():
		_fail("the registry has no vertex effect (the bend is its geometry)")
	for definition in vertices:
		_check_vertex_effect(definition)

	for i in range(mini(layers.size(), passes.size())):
		var definition: Dictionary = passes[i]
		var layer := layers[i] as ColorRect
		if layer.name != String(definition["id"]):
			_fail("layer %d is %s, not %s" % [i, layer.name, definition["id"]])
		var material := layer.material as ShaderMaterial
		if material == null or material.shader == null:
			_fail("%s has no ShaderMaterial" % definition["id"])
			continue
		if material.shader.resource_path != String(definition["shader"]):
			_fail("%s draws %s, not %s" % [definition["id"], material.shader.resource_path, definition["shader"]])
		if not is_equal_approx(layer.anchor_right, 1.0) or not is_equal_approx(layer.anchor_bottom, 1.0):
			_fail("%s does not cover the viewport" % definition["id"])
		if layer.mouse_filter != Control.MOUSE_FILTER_IGNORE:
			_fail("%s eats the pointer" % definition["id"])
		var world := String(definition.get("kind", "screen")) == "world"
		var expected_z := -1 if world else 0
		if layer.z_index != expected_z:
			_fail("%s is drawn at z_index %d, wanted %d for a %s effect"
				% [definition["id"], layer.z_index, expected_z, definition.get("kind", "screen")])
		# Every uniform the registry calls a param has to exist in the shader, or
		# the row is a slider wired to nothing.
		var uniforms := {}
		for uniform in material.shader.get_shader_uniform_list():
			uniforms[String(uniform["name"])] = true
		for param in definition["params"]:
			if not uniforms.has(String(param["key"])):
				_fail("%s: no uniform %s" % [definition["id"], param["key"]])
		# And a pass that measures anything in frame pixels has to be told the
		# frame it is drawing into, which is the one thing it cannot read out of
		# the texture it samples. A pass that works in screen coordinates
		# throughout wants the number nowhere, and being told it anyway would be a
		# uniform the overlay pushes and nothing reads.
		if uniforms.has("frame_size") and material.get_shader_parameter("frame_size") != Vector2(root.get_visible_rect().size):
			_fail("%s was told the frame is %s, not %s"
				% [definition["id"], material.get_shader_parameter("frame_size"), root.get_visible_rect().size])
		print("probe: %s is a %s effect at z_index %d -> %s, %d params, %d uniforms, frame_size %s"
			% [definition["id"], definition.get("kind", "screen"), layer.z_index, layer.name,
				definition["params"].size(), uniforms.size(),
				"yes" if uniforms.has("frame_size") else "no"])

	# The stack draws over the world and under the menus: tuning a shader with the
	# Shaders page open must not paint the page you are tuning it from.
	var hud := overlay.get_parent()
	var overlay_index := overlay.get_index()
	var menu_index := (menu as Node).get_index()
	if menu_index < overlay_index:
		_fail("the menu (%d) is below the shader overlay (%d)" % [menu_index, overlay_index])
	print("probe: HUD child order: overlay %d, menu %d (of %d)"
		% [overlay_index, menu_index, hud.get_child_count()])


# --- The page -----------------------------------------------------------------

func check_menu() -> void:
	menu.call("_open")
	# `_open` rebuilds every page, so the shaders page only exists from here on.
	await process_frame

	var pages: Dictionary = menu.get("_pages")
	if not pages.has("shaders"):
		_fail("the menu has no shaders page")
		return
	var pause: Control = pages["pause"]
	var labels := _button_texts(pause)
	if not labels.has("Shaders"):
		_fail("the pause menu has no Shaders button (it has %s)" % [labels])
	else:
		print("probe: pause buttons %s" % [labels])

	menu.call("_show_page", "shaders")
	await process_frame
	if String(menu.get("_current_page")) != "shaders":
		_fail("the shaders page did not open")

	# The Shaders page is the list of shaders: one row each, carrying the switch
	# and the icon button that opens that shader's own page. The uniforms are on
	# that page instead of here, so a shader with fourteen knobs cannot bury the
	# next shader below the fold.
	var headings: Array = []
	_walk_headings(pages["shaders"], headings)
	var definitions: Array = overlay.call("get_definitions")
	# The headings are the registry's kinds: one category per kind that has any
	# rows, in the registry's own order, named there. The expectation is read out
	# of the registry rather than written here, so a kind added to the file shows up
	# in this check the same day it shows up in the menu.
	var want: Array = []
	for kind in overlay.call("get_kinds"):
		var kind_id := String(kind.get("kind", ""))
		for definition in definitions:
			if String(definition.get("kind", "screen")) == kind_id:
				want.append(String(kind.get("name", kind_id)))
				break
	if headings != want:
		_fail("shaders headings are %s, want %s" % [headings, want])
	print("probe: shaders headings %s" % [headings])
	var list_widgets: Array = []
	_collect(pages["shaders"], list_widgets)
	print("probe: the Shaders page is %d widget(s) for %d shader(s)"
		% [list_widgets.size(), definitions.size()])
	_shot("shaders_page")

	for definition in definitions:
		var id := String(definition["id"])
		var name := String(definition["name"])
		var page_key := "shader_" + id

		# The row: its switch wears the shader's own name as its label, and the
		# button beside it opens the shader's page.
		var switch_btn: Control = _row_by_label(pages["shaders"], name)
		if switch_btn == null or not switch_btn is Button:
			_fail("the Shaders page has no switch for %s" % name)
		var gear: Button = null
		for widget in list_widgets:
			if widget is Button and String((widget as Button).tooltip_text) == "%s settings" % name:
				gear = widget
		if gear == null:
			_fail("the %s row carries no settings button" % name)
		if not pages.has(page_key):
			_fail("there is no page for %s (%s)" % [name, page_key])
			continue
		if gear == null:
			continue

		# Pressing it opens that shader's page, and Back comes back to the list.
		gear.pressed.emit()
		await process_frame
		if String(menu.get("_current_page")) != page_key:
			_fail("the settings button opened %s, want %s"
				% [menu.get("_current_page"), page_key])
		print("probe: the %s row's settings button opened %s" % [name, page_key])

		# That page: one row per uniform, each labelled by the registry, and the
		# rows are live - a row's value is what the material has. No switch of its
		# own: turning the shader on is the row on the list, one page back, and a
		# second switch here would be the same setting in two places.
		var widgets: Array = []
		_collect(pages[page_key], widgets)
		if _row_by_label(pages[page_key], name) != null:
			_fail("%s repeats the switch its row on the list already has" % page_key)
		var switches := 0
		for widget in widgets:
			if widget.get_meta("row_label", "") == "Enabled":
				switches += 1
		if switches > 0:
			_fail("%s has %d switch row(s) of its own" % [page_key, switches])
		var rows := 0
		var page_sliders := 0
		for param in definition["params"]:
			rows += 1
			var found := false
			for widget in widgets:
				if widget.get_meta("row_label", "") == String(param["label"]):
					found = true
					break
			if not found:
				_fail("no row for %s (%s)" % [param["label"], param["key"]])
		for widget in widgets:
			if widget.has_meta("slider"):
				page_sliders += 1
		print("probe: %s has %d param row(s), %d slider(s), for %d float param(s) in the registry"
			% [page_key, rows, page_sliders, _param_count([definition])])
		if page_sliders != _param_count([definition]):
			_fail("%s has %d sliders for %d float params"
				% [page_key, page_sliders, _param_count([definition])])

		# A page of sliders for a shader that is off changes nothing you can see,
		# so its title breathes. A still frame cannot show a pulse: the title is
		# sampled twice round a whole beat, once with the shader off and once with
		# it on, and it has to swing while off and hold while on. Whichever state
		# the player left it in, both halves are exercised.
		var title: Label = menu.call("_page_title", pages[page_key])
		if title == null:
			_fail("%s has no title label" % page_key)
		else:
			var was: bool = bool(overlay.call("is_enabled", id))
			var off_shades := await _sample_title(overlay, id, title, false)
			var on_shades := await _sample_title(overlay, id, title, true)
			overlay.call("set_enabled", id, was)
			var off_swing: float = off_shades[1] - off_shades[0]
			var on_swing: float = on_shades[1] - on_shades[0]
			print("probe: %s title swings %.3f (%.3f..%.3f) while its shader is off, and %.3f (%.3f..%.3f) while it is on"
				% [page_key, off_swing, off_shades[0], off_shades[1],
					on_swing, on_shades[0], on_shades[1]])
			if off_swing < 0.3:
				_fail("the title does not pulse while its shader is off (swing %.3f)" % off_swing)
			if on_swing > 0.001 or on_shades[0] < 0.999:
				_fail("the title pulses while its shader is on (%.3f..%.3f)"
					% [on_shades[0], on_shades[1]])
		_shot("shader_options")

	# The page is wired to the stack, not just built from it: dragging a row's
	# slider has to land in the overlay's state (and so on the material) with no
	# Apply button anywhere in the picture.
	var curvature_row: Control = _row_by_label(pages["shader_" + CRT_ID], "Tube Curvature")
	if curvature_row == null or not curvature_row.has_meta("slider"):
		_fail("the page has no Tube Curvature slider")
	else:
		var before := float(overlay.call("get_value", CRT_ID, "curvature"))
		(curvature_row.get_meta("slider") as HSlider).value = 0.31
		await process_frame
		var after := float(overlay.call("get_value", CRT_ID, "curvature"))
		var material := (overlay.get_node(CRT_ID) as ColorRect).material as ShaderMaterial
		print("probe: dragging Tube Curvature %.3f -> %.3f moves the shader's uniform to %.3f"
			% [before, after, float(material.get_shader_parameter("curvature"))])
		if not is_equal_approx(after, 0.31):
			_fail("the row's slider did not reach the stack (%.3f)" % after)
		if absf(float(material.get_shader_parameter("curvature")) - 0.31) > 0.001:
			_fail("the row's slider did not reach the material")
		(curvature_row.get_meta("slider") as HSlider).value = before
		await process_frame




## The row widget carrying a label, found by the metacomplete every row wears
## (`row_label`), so this does not depend on row order or on child indices.
## The title's own colour, least and most, over one whole beat with the shader in
## `on`: four samples a quarter of a pulse apart span the cycle whatever phase
## the pulse happened to be at.
func _sample_title(overlay: Node, id: String, title: Label, on: bool) -> Array:
	overlay.call("set_enabled", id, on)
	# The colour is put on the title by the menu's own _process, so the first
	# sample has to wait for a frame: taken now it would still be the colour the
	# other state left there.
	await process_frame
	await process_frame
	var samples: Array = []
	for i in range(5):
		samples.append(title.get_theme_color("font_color").r)
		await create_timer(0.5).timeout
	var low := 1.0
	var high := 0.0
	for sample in samples:
		low = minf(low, float(sample))
		high = maxf(high, float(sample))
	return [low, high]


func _shot(name: String) -> void:
	var path := "%s/%s_%dx%d.png" % [SHOT_DIR, name,
		DisplayServer.window_get_size().x, DisplayServer.window_get_size().y]
	var img: Image = root.get_texture().get_image()
	if img == null:
		return
	img.save_png(path)
	shots += 1
	print("probe: shot %s" % path)


func _row_by_label(node: Node, label: String) -> Control:
	for child in node.get_children():
		if child is Control and String(child.get_meta("row_label", "")) == label:
			return child
		var found := _row_by_label(child, label)
		if found != null:
			return found
	return null


func _param_count(definitions: Array) -> int:
	var count := 0
	for definition in definitions:
		for param in definition["params"]:
			if String(param.get("type", "float")) != "bool":
				count += 1
	return count


# --- The pass, in pixels ------------------------------------------------------

## Whatever the game draws over the glass is not glass. The FPS line and the
## hotbar sit in the corners the tube files away and the crosshair in the middle
## of the picture, and all of them are drawn *above* the overlay, so a corner
## patch full of them measures the HUD and not the tube. Hidden before anything
## below is measured, so every pixel here is the pass's own.
func _hide_gameplay_ui() -> int:
	var hud := overlay.get_parent()
	var hidden := 0
	for child in hud.get_children():
		# Not every HUD child draws - the held item is a plain Node that only
		# scripts the viewmodel - so only the CanvasItems are touched.
		if child == overlay or child == menu or not child is CanvasItem:
			continue
		(child as CanvasItem).visible = false
		hidden += 1
	return hidden


func check_rendering(size: Vector2i) -> void:
	# Start from the world with nothing switched on, and with the menu out of the
	# way, so the frames compared below differ only by what is set in between.
	for id in ids:
		overlay.call("set_enabled", id, false)
	menu.call("_close")
	var hidden := _hide_gameplay_ui()
	print("probe: %d gameplay HUD node(s) hidden, so the pixels below are the pass's" % hidden)
	await _settle()
	var off := await _capture()
	var off_stats := _stats(off)

	overlay.call("set_enabled", CRT_ID, true)
	await _settle()
	var on := await _capture()
	if not _differs(off, on, 0.005):
		_fail("the pass is on but the frame is unchanged")
	print("probe: CRT on changes the frame by %.4f mean, %.1f%% of pixels"
		% [_mean_diff(off, on), 100.0 * _changed_share(off, on)])
	_save(on, "crt_default")
	var on_stats := _stats(on)

	# Defaults: the corners wear the tube's falloff and the middle of the picture
	# is still a picture. Both are ratios against the same frame with the pass
	# off, because what is *behind* a corner is the world - a dark corner of a
	# night scene passes an absolute threshold while a lit one fails it, so an
	# absolute number here measures the scene and not the tube. The case's own
	# shape and its four filed corners are pinned on a flat field instead, by
	# probe_crt_unit.gd, where all four corners start out the same colour.
	var corner_kept: float = float(on_stats["corner"]) / maxf(float(off_stats["corner"]), 0.001)
	var centre_kept: float = float(on_stats["centre"]) / maxf(float(off_stats["centre"]), 0.001)
	print("probe: corner %.3f vs centre %.3f  (off: %.3f / %.3f); whole frame %.3f vs %.3f"
		% [on_stats["corner"], on_stats["centre"], off_stats["corner"], off_stats["centre"],
		   on_stats["mean"], off_stats["mean"]])
	print("probe: the pass keeps %.0f%% of the light in the corners and %.0f%% in the middle"
		% [corner_kept * 100.0, centre_kept * 100.0])
	if corner_kept > 0.8:
		_fail("the tube's corners are not dark (%.0f%% of the light kept)" % (corner_kept * 100.0))
	if centre_kept < 0.8:
		_fail("the pass darkens the middle of the picture (%.0f%% kept, want what it had)"
			% (centre_kept * 100.0))
	if on_stats["centre"] < 0.02:
		_fail("the pass blacked out the middle of the picture (%.3f)" % on_stats["centre"])

	await _set_param("picture_scale", 8.0)
	await _set_param("scanline_depth", 1.0)
	await _set_param("mask_strength", 0.0)
	await _set_param("curvature", 0.0)
	await _set_param("vignette", 0.0)
	await _set_param("glow", 0.0)
	await _set_param("convergence", 0.0)
	var beam := await _capture()
	print("probe: beam ripple %.4f (off %.4f)" % [_ripple(beam), _ripple(off)])
	if _ripple(beam) <= _ripple(off) * 1.25:
		_fail("scanlines do not ripple the frame (%.4f vs %.4f)" % [_ripple(beam), _ripple(off)])
	_save(beam, "crt_beam")

	# The phosphor mask: one stripe per gun, so the frame's channels stop agreeing.
	#
	# At the raster the picture is actually drawn on (picture_scale 4) rather than
	# the one-pixel raster the beam check above needs, because a triad narrower
	# than three screen pixels puts one gun in every pixel and the grille stops
	# being a pattern.
	#
	# And measured as what the grille *does* - neighbouring pixels painted by
	# different guns, so the colour direction changes sharply along a row - rather
	# than as how much colour the frame has. `_channel_split` was the first measure
	# here and it read the world: a saturated green pixel under the red stripe is
	# black, so a masked frame is not "more colour" than the picture it masks, and
	# which of the two came out larger was whichever the band happened to hold. It
	# passed on a bright band and failed on a dark one, the same failure the
	# convergence check below has.
	await _set_param("scanline_depth", 0.0)
	await _set_param("picture_scale", 4.0)
	await _set_param("mask_strength", 0.0)
	await _set_param("mask_pitch", 2.0)
	var bare := await _capture()
	await _set_param("mask_strength", 1.0)
	var mask := await _capture()
	_save(bare, "crt_mask_bare")
	var shift := _hue_shift(bare, mask)
	print("probe: the mask moves a pixel's colour direction by %.4f" % shift)
	if shift < 0.05:
		_fail("the phosphor mask does not choose a gun per pixel (%.4f, want a stripe's worth)"
			% shift)
	_save(mask, "crt_mask")

	# Guns that do not converge: the red channel lands a hair outside where the
	# blue one does, so the picture's channels separate without the mask's help.
	#
	# Two things this check got wrong the first time, both of them about what to
	# hold the moved frame against.
	#
	# It compared against the pass-off frame from the top of the run, which is a
	# different raster altogether: the guns' weights are a 3x3 stencil over the
	# cells around this one, so their *displacement* saturates at one cell however
	# far the shift asks to go - four screen pixels at the default raster, one at
	# the pass-off raster - and a channel measure reads its own raster off the
	# frame whatever the guns do. The comparison is now the same frame with the
	# knob at zero, back to back, nothing else changed.
	#
	# And it asked for the frame's chroma to *rise*, which is the opposite of what
	# a misconverged picture does: with the guns apart each channel is an average
	# of its neighbours, so the pixel-to-pixel |r-g| of a smooth world frame falls
	# as they separate. That check called a 2% fall a pass for anything. What the
	# knob is for is that the picture moves at all - and the honest reference for
	# "it moved" in a live world is the world moving on its own, so the frame the
	# knob moved is held against the same frame one capture later with the knob
	# still. Whether the shift reads as chroma or as blur is the tube's business.
	await _set_param("mask_strength", 0.0)
	await _set_param("picture_scale", 4.0)
	await _set_param("scanline_depth", 0.0)
	await _set_param("convergence", 0.0)
	var converged := await _capture()
	var drifting := await _capture()
	await _set_param("convergence", 0.01)
	var chroma := await _capture()
	var shifted := _mean_diff(converged, chroma)
	var world := _mean_diff(converged, drifting)
	print("probe: convergence 0 -> 0.01 moves the frame %.4f; the world alone moves it %.4f"
		% [shifted, world])
	if shifted < maxf(world * 2.0, 0.001):
		_fail("convergence does not move the frame (%.4f against the world's own %.4f)"
			% [shifted, world])
	_save(chroma, "crt_convergence")
	await _set_param("convergence", 0.0)

	# Glow off and on, back to back with nothing else changed between them: this is
	# a live frame, and comparing against the pass-off frame from the top of the
	# check would be measuring the world - on a bright day the water and the
	# clouds move more than the halo adds, and that capture is a whole measurement
	# run older than this one.
	#
	# At a picture raster, because the halo's reach is in picture dots: the
	# one-pixel raster the convergence check above needs would leave it a pixel
	# wide, lifting the frame by about one percent - which is the shader being
	# right and the check being wrong about what glow is for.
	await _set_param("picture_scale", 4.0)
	await _set_param("mask_strength", 0.0)
	await _set_param("glow_radius", 2.0)
	await _set_param("glow", 0.0)
	var unlit := await _capture()
	await _set_param("glow", 1.5)
	var glow := await _capture()
	print("probe: glow lifts mean luminance %.4f -> %.4f"
		% [_stats(unlit)["mean"], _stats(glow)["mean"]])
	# Only a smoke test, and it can only look for the sign: the halo takes what is
	# over the phosphor threshold (0.45) and nothing else, so on a dim live frame
	# adding almost nothing is the pass being right. What it does beside a bright
	# thing is measured on a black field with a bright block on it instead, by
	# probe_crt_unit.gd's check_glow.
	if _stats(glow)["mean"] < _stats(unlit)["mean"]:
		_fail("glow darkens the frame (%.4f vs %.4f)"
			% [_stats(glow)["mean"], _stats(unlit)["mean"]])

	await _set_param("glow", 0.0)
	await _set_param("vignette", 1.5)
	var vignetted := await _capture()
	var edge: float = _stats(vignetted)["corner"] / maxf(float(_stats(vignetted)["centre"]), 0.0001)
	var edge_off: float = off_stats["corner"] / maxf(float(off_stats["centre"]), 0.0001)
	print("probe: vignette corner/centre %.3f (off %.3f)" % [edge, edge_off])
	if edge >= edge_off:
		_fail("vignette does not darken the edges (%.3f vs %.3f)" % [edge, edge_off])

	await _set_param("vignette", 0.0)
	await _set_param("saturation", 0.0)
	var grey := await _capture()
	print("probe: saturation 0 leaves %.4f of the colour (off %.4f)"
		% [_channel_split(grey), _channel_split(off)])
	if _channel_split(grey) > _channel_split(off) * 0.4:
		_fail("saturation 0 does not drain the colour (%.4f vs %.4f)"
			% [_channel_split(grey), _channel_split(off)])

	# A dialled-in set, for the plate: curvature and mask back up, everything
	# else where the registry starts it.
	var first := _definition(CRT_ID)
	for param in first["params"]:
		overlay.call("set_value", CRT_ID, String(param["key"]), param["default"])
	await _set_param("curvature", 0.45)
	await _set_param("mask_strength", 0.5)
	await _set_param("glow", 0.7)
	await _settle()
	var strong := await _capture()
	_save(strong, "crt_strong")

	# --- World Bend, on the world -----------------------------------------------
	# Everything the bend does is the world's own vertices moved, so on a live
	# frame the two things worth checking are that the switch reaches the world at
	# all and *where* in the frame the change lands. How far the ground moved, and
	# whether it moved by the include's own arithmetic, is probe_bend_geo.gd's job:
	# that one renders a scene it controls, where a marker's position against the
	# maths is a number rather than an impression.
	#
	# A live world drifts between any two frames - the water flows, the sky moves -
	# so what the change is measured against is the world's own drift, taken as two
	# frames back to back with nothing but the switch between them.
	for id in ids:
		overlay.call("set_enabled", id, false)
	await _settle()
	var bend_off := await _capture()
	var drift := await _capture()
	overlay.call("set_enabled", BEND_ID, true)
	await _settle()
	var bend_on := await _capture()
	_save(bend_on, "bend_world")
	var moved := _mean_diff(bend_off, bend_on)
	var drifted := _mean_diff(bend_off, drift)
	print("probe: World Bend changes the frame by %.4f mean, %.1f%% of pixels (the world's own drift is %.4f)"
		% [moved, 100.0 * _changed_share(bend_off, bend_on), drifted])
	if moved < maxf(drifted * 4.0, 0.005):
		_fail("World Bend is on but the world has barely changed (%.4f vs a drift of %.4f)"
			% [moved, drifted])

	# Where it lands. With the camera level the horizon is across the middle of the
	# frame, so the band just under it is the far field - the ground a hundred
	# blocks and more out, which the bend lifts and pulls in - and the bottom band
	# is the ground at the player's feet, where the bend is the identity to within
	# a hundredth of a block. That near band is where a bend would be a lie: the
	# block you are standing on and the block you are mining must not have moved.
	var far_band := _band_diff(bend_off, bend_on, size.y / 2 + 8, size.y / 2 + 72)
	var near_band := _band_diff(bend_off, bend_on, size.y - 48, size.y - 4)
	var far_drift := _band_diff(bend_off, drift, size.y / 2 + 8, size.y / 2 + 72)
	var near_drift := _band_diff(bend_off, drift, size.y - 48, size.y - 4)
	print("probe: the far field changes %.4f (its drift %.4f); the ground underfoot %.4f (its drift %.4f)"
		% [far_band, far_drift, near_band, near_drift])
	if far_band < maxf(far_drift * 4.0, 0.02):
		_fail("World Bend barely changes the far field (%.4f, drift %.4f)" % [far_band, far_drift])
	if near_band > maxf(near_drift * 4.0, far_band * 0.5):
		_fail("World Bend moves the ground underfoot as much as the far field (%.4f vs %.4f, drift %.4f)"
			% [near_band, far_band, near_drift])

	# The two kinds on one frame: the bend is the world's own geometry and the tube
	# is a picture of it, so what the tube scans is a bent world and not a bent
	# tube. Both are registry entries drawn in the registry's order, which is this
	# file's order, so the composition is the one the player gets.
	await _set_bend("world_bend", 1.0)
	await _set_bend("world_bend_radius", 128.0)
	_save(await _capture(), "bend_strong")
	overlay.call("set_enabled", CRT_ID, true)
	await _settle()
	_save(await _capture(), "bend_crt")
	for id in ids:
		overlay.call("set_enabled", id, false)
	# ...and the bend's own rows back to the registry's numbers, so nothing below
	# depends on the values this check dialled in.
	var bend_definition := _definition(BEND_ID)
	for param in bend_definition["params"]:
		overlay.call("set_value", BEND_ID, String(param["key"]), param["default"])
	await _settle()

	for id in ids:
		overlay.call("set_enabled", id, false)
	print("probe: %d shots in %s" % [shots, SHOT_DIR])


# --- Settings and the share code ----------------------------------------------

func check_persistence() -> void:
	var definition := _definition(CRT_ID)
	var id := CRT_ID
	overlay.call("set_enabled", id, true)
	overlay.call("set_value", id, "curvature", 0.5)
	overlay.call("set_value", id, "scanline_depth", 0.75)
	menu.call("_save_settings")

	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		_fail("no settings.cfg to read back")
		return
	if not bool(cfg.get_value("shaders", id + "_enabled", false)):
		_fail("the switch was not saved")
	if not is_equal_approx(float(cfg.get_value("shaders", id + "_curvature", -1.0)), 0.5):
		_fail("curvature was not saved (%.3f)" % float(cfg.get_value("shaders", id + "_curvature", -1.0)))
	print("probe: saved shaders/%s_enabled + %d value(s)"
		% [id, 2])

	# Wipe the live state, then let the file put it back.
	overlay.call("set_enabled", id, false)
	overlay.call("set_value", id, "curvature", 0.0)
	overlay.call("set_value", id, "scanline_depth", 0.0)
	menu.call("_load_settings")
	if not bool(overlay.call("is_enabled", id)):
		_fail("_load_settings did not restore the switch")
	if not is_equal_approx(float(overlay.call("get_value", id, "curvature")), 0.5):
		_fail("_load_settings did not restore curvature (%.3f)" % float(overlay.call("get_value", id, "curvature")))

	# The world effect's switch and rows are the same setting in the same place,
	# through the same code: what differs is what "on" *is* - a uniform pushed to
	# the world's materials rather than a rect made visible - so the round trip is
	# worth making with it as well as with the CRT.
	overlay.call("set_enabled", BEND_ID, true)
	overlay.call("set_value", BEND_ID, "world_bend", 0.25)
	menu.call("_save_settings")
	overlay.call("set_enabled", BEND_ID, false)
	overlay.call("set_value", BEND_ID, "world_bend", 0.0)
	menu.call("_load_settings")
	print("probe: World Bend came back %s at Bend %.2f (from shaders/%s_enabled)"
		% ["on" if bool(overlay.call("is_enabled", BEND_ID)) else "off",
			float(overlay.call("get_value", BEND_ID, "world_bend")), BEND_ID])
	if not bool(overlay.call("is_enabled", BEND_ID)):
		_fail("_load_settings did not restore the world effect's switch")
	if not is_equal_approx(float(overlay.call("get_value", BEND_ID, "world_bend")), 0.25):
		_fail("_load_settings did not restore the world effect's Bend (%.2f)"
			% float(overlay.call("get_value", BEND_ID, "world_bend")))

	# The share code is the same state in a string: export it, change everything,
	# import it back and expect the state to match to the code's own precision.
	var code := String(menu.call("_export_shaders_code"))
	if not code.begins_with("FS-"):
		_fail("the shaders code is %s" % code)
		return
	print("probe: shaders code %s" % code)
	overlay.call("set_enabled", id, false)
	for param in definition["params"]:
		overlay.call("set_value", id, String(param["key"]), 0.0)
	if not bool(menu.call("_import_shaders_code", code)):
		_fail("the shaders code did not import back")
		return
	if not bool(overlay.call("is_enabled", id)):
		_fail("the imported code did not restore the switch")
	if absf(float(overlay.call("get_value", id, "curvature")) - 0.5) > 0.01:
		_fail("the imported code restored curvature as %.3f" % float(overlay.call("get_value", id, "curvature")))
	if absf(float(overlay.call("get_value", id, "scanline_depth")) - 0.75) > 0.01:
		_fail("the imported code restored scanline depth as %.3f" % float(overlay.call("get_value", id, "scanline_depth")))
	if bool(menu.call("_import_shaders_code", "FS-AAAA-AAAA")):
		_fail("a code of the wrong length was accepted")
	print("probe: code round trip ok")


# --- Working with the frame ---------------------------------------------------

func _settle() -> void:
	for i in range(3):
		await process_frame


func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


## The CRT is the pass whose pixel checks below drive a uniform by name.
func _set_param(key: String, value: Variant) -> void:
	overlay.call("set_value", CRT_ID, key, value)
	await _settle()


func _definition(id: String) -> Dictionary:
	for definition in overlay.call("get_definitions"):
		if String(definition["id"]) == id:
			return definition
	_fail("the registry has no %s" % id)
	return {}


## The bend's rows are the two world materials' uniforms, not a rect's; the row
## that drives one is the same row widget either way.
func _set_bend(key: String, value: Variant) -> void:
	overlay.call("set_value", BEND_ID, key, value)
	await _settle()


func _save(img: Image, name: String) -> void:
	var path := "%s/%s_%dx%d.png" % [SHOT_DIR, name, img.get_width(), img.get_height()]
	img.save_png(path)
	shots += 1
	print("probe: shot %s" % path)


func _differs(a: Image, b: Image, threshold: float) -> bool:
	return _mean_diff(a, b) > threshold


## How much two frames differ over a band of rows: for checks about where a pass
## put its effect rather than how big it is.
func _band_diff(a: Image, b: Image, from_row: int, to_row: int) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var w := a.get_width()
	var h := a.get_height()
	var total := 0.0
	var count := 0
	for y in range(clampi(from_row, 0, h - 1), clampi(to_row, from_row + 1, h)):
		for x in range(0, w, 2):
			var i := (y * w + x) * 4
			total += absf(float(pa[i]) - float(pb[i])) / 255.0
			count += 1
	return total / maxf(float(count), 1.0)


func _mean_diff(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var total := 0.0
	var count := 0
	var step := 16  # every 4th pixel
	for i in range(0, pa.size() - 3, step):
		total += absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
			+ absf(int(pa[i + 2]) - int(pb[i + 2]))
		count += 3
	return total / (255.0 * maxf(float(count), 1.0))


func _changed_share(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var changed := 0
	var count := 0
	for i in range(0, pa.size() - 3, 16):
		if absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
				+ absf(int(pa[i + 2]) - int(pb[i + 2])) > 12:
			changed += 1
		count += 1
	return float(changed) / maxf(float(count), 1.0)


## The three things the numbers below are read off: how bright the frame is, how
## bright the middle of it is, and how bright its four corners are.
func _stats(img: Image) -> Dictionary:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var mean := 0.0
	var count := 0
	for y in range(0, h, 4):
		for x in range(0, w, 4):
			mean += _lum(data, w, x, y)
			count += 1
	mean /= maxf(float(count), 1.0)

	var centre := 0.0
	var cc := 0
	for y in range(h / 8 * 3, h / 8 * 5, 2):
		for x in range(w / 8 * 3, w / 8 * 5, 2):
			centre += _lum(data, w, x, y)
			cc += 1
	centre /= maxf(float(cc), 1.0)

	var block := 24
	var corner := 0.0
	var cn := 0
	for corner_index in 4:
		var x0: int = 0 if corner_index % 2 == 0 else w - block
		var y0: int = 0 if corner_index < 2 else h - block
		for y in range(y0, y0 + block):
			for x in range(x0, x0 + block):
				corner += _lum(data, w, x, y)
				cn += 1
	corner /= maxf(float(cn), 1.0)
	return {"mean": mean, "centre": centre, "corner": corner}


## How much the frame ripples down itself: the mean jump between one row and the
## next over the middle of the picture. A beam painting bright lines with dark
## gaps between them is exactly that jump.
func _ripple(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var total := 0.0
	var count := 0
	for y in range(h / 4, h / 4 * 3):
		for x in range(w / 3, w / 3 * 2, 3):
			total += absf(_lum(data, w, x, y) - _lum(data, w, x, y + 1))
			count += 1
	return total / maxf(float(count), 1.0)


## How much of a frame's colour is in its channels rather than in its brightness:
## the mean |r - g| + |g - b| over a band of rows. This is the question the
## saturation knob is asked, and not the question a mask or a misconverged set of
## guns is asked - a frame whose channels have been pulled apart has *less* of
## this, not more - so the two checks that used to read it measure the spatial
## structure of the colour instead (`_grille_steps`).
##
## Every pixel of a band of rows, rather than every fourth pixel of the whole
## frame: a stripe pattern and a sampling stride are two periods, and two periods
## that do not divide each other report whatever their beat happens to be - the
## mask's own colour came out lower than no mask at all on a frame sampled every
## four pixels, because four and the mask's period were landing on the dark line
## between two stripes.
func _channel_split(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var total := 0.0
	var count := 0
	for y in range(h / 2 - 24, h / 2 + 24):
		for x in range(w):
			var i := (y * w + x) * 4
			var r := float(data[i]) / 255.0
			var g := float(data[i + 1]) / 255.0
			var b := float(data[i + 2]) / 255.0
			total += absf(r - g) + absf(g - b)
			count += 2
	return total / maxf(float(count), 1.0)


## Which of the three guns a pixel's colour came from, signed: green against red
## and blue. The grille paints one stripe of one gun at a time, so this is the
## value that flips sign at every stripe boundary along a row.
func _hue(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (float(data[i + 1]) - 0.5 * (float(data[i]) + float(data[i + 2]))) / 255.0


## How far the grille alone moves a pixel's colour direction: the light-weighted
## mean |hue(masked) - hue(bare)| over a band, the two frames being the same
## picture with only the mask's strength changed between them.
##
## A difference of two frames rather than a property of one, which is what two
## earlier versions of this check got wrong. A grille is not "more colour in the
## frame": a saturated green pixel under the red stripe is black, so the masked
## frame has *less* of it, and the first measure read whichever of the two the
## band happened to hold. Nor is it "fast colour changes along a row": at a
## picture raster the frame's neighbouring *cells* already jump in colour, and that
## measure answered 0.082 with the mask at a fifteenth of its strength and 0.084
## at full strength - it was reading the upscale. What the grille does to a pixel
## is decide which of three guns drew it, and that is precisely the difference
## between the frame with it and the frame without it.
##
## Weighted by the light of the unmasked frame, which is what the grille
## multiplies, so the answer is a property of the grille and not of the band.
##
## The band is two fifths of the way down the frame, so that a letterbox or a
## vignetted top does not decide the answer.
func _hue_shift(bare: Image, masked: Image) -> float:
	var pa := bare.get_data()
	var pb := masked.get_data()
	var w := bare.get_width()
	var h := bare.get_height()
	if masked.get_width() != w or masked.get_height() != h:
		return 0.0
	var top := (h * 2) / 5
	var bottom := (h * 3) / 5
	var total := 0.0
	var light := 0.0
	for y in range(top, bottom):
		for x in range(w):
			var weight := _lum(pa, w, x, y)
			if weight <= 0.0:
				continue
			total += absf(_hue(pb, w, x, y) - _hue(pa, w, x, y)) * weight
			light += weight
	if light <= 0.0:
		return 0.0
	return total / light


func _lum(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (0.2126 * float(data[i]) + 0.7152 * float(data[i + 1]) + 0.0722 * float(data[i + 2])) / 255.0


# --- Odds and ends ------------------------------------------------------------

## A vertex effect: no rect, because it is the world rather than a picture of it,
## and the materials the world is drawn with under its control. Two things are
## worth checking here that a screen pass has no equivalent of.
##
## The first is that those materials are the *loaded* resources and not copies.
## `load("res://materials/voxel_material.tres")` goes through the resource cache,
## so it hands back the instance the renderer is already drawing with, and a
## uniform set here is set on the world. A duplicate would take the same uniform,
## draw exactly as before, and leave a switch that appears to do nothing - which
## is why the check is that the switch moves the frame (below) rather than that
## some material somewhere has the value.
##
## The second is that a vertex effect has nothing to hide, so its switch is a
## uniform (`enable_key`) and it is the overlay that pushes it - to every one of
## the materials, or one half of the world would bend.
func _check_vertex_effect(definition: Dictionary) -> void:
	var id := String(definition.get("id", ""))
	var enable_key := String(definition.get("enable_key", ""))
	if enable_key == "":
		_fail("%s has no enable_key" % id)
	var materials: Array = []
	for path in definition.get("materials", []):
		var material: ShaderMaterial = load(String(path))
		if material == null or material.shader == null:
			_fail("%s: no ShaderMaterial at %s" % [id, path])
			continue
		materials.append(material)
	if materials.is_empty():
		_fail("%s drives no materials" % id)
		return

	var uniforms := {}
	for material in materials:
		for uniform in (material as ShaderMaterial).shader.get_shader_uniform_list():
			uniforms[String(uniform["name"])] = true
	if not uniforms.has(enable_key):
		_fail("%s: none of its materials has %s" % [id, enable_key])
	for param in definition["params"]:
		if not uniforms.has(String(param["key"])):
			_fail("%s: none of its materials has %s" % [id, param["key"]])
	if overlay.get_node_or_null(id) != null:
		_fail("%s has a rect on the overlay; a vertex effect has none" % id)

	# The switch, both ways, on every material: half a bent world is a seam.
	var followed := true
	overlay.call("set_enabled", id, true)
	for material in materials:
		if float((material as ShaderMaterial).get_shader_parameter(enable_key)) < 0.5:
			followed = false
	overlay.call("set_enabled", id, false)
	for material in materials:
		if float((material as ShaderMaterial).get_shader_parameter(enable_key)) != 0.0:
			followed = false
	if not followed:
		_fail("%s's switch does not reach every material it names" % id)
	var named: Array = []
	for material in materials:
		named.append(String((material as ShaderMaterial).resource_path).get_file())
	print("probe: %s is a vertex effect over %s, switch %s, %d param(s), %d uniform(s)"
		% [id, named, enable_key, definition["params"].size(), uniforms.size()])


func _walk_headings(node: Node, out: Array) -> void:
	for child in node.get_children():
		if child is Label and String(child.text) != "" and child.has_meta("is_heading"):
			out.append(String(child.text))
		_walk_headings(child, out)


func _button_texts(node: Node) -> Array:
	var out: Array = []
	for child in node.get_children():
		if child is Button:
			out.append(String(child.text))
		out.append_array(_button_texts(child))
	return out


func _collect(node: Node, out: Array) -> void:
	for child in node.get_children():
		if child is Control:
			out.append(child)
		_collect(child, out)


func _backup_settings() -> void:
	if not FileAccess.file_exists(SETTINGS_PATH):
		return
	var file := FileAccess.open(SETTINGS_PATH, FileAccess.READ)
	if file == null:
		return
	_settings_backup = file.get_buffer(file.get_length())
	file.close()
	_had_settings = true


func _restore_settings() -> void:
	if not _had_settings:
		return
	var file := FileAccess.open(SETTINGS_PATH, FileAccess.WRITE)
	if file == null:
		return
	file.store_buffer(_settings_backup)
	file.close()
	print("probe: settings.cfg restored (%d bytes)" % _settings_backup.size())


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _finish() -> void:
	# The user's settings go back, and so does the live state that is about to be
	# written over them: the menu saves on `_exit_tree`, so restoring the file and
	# then quitting would have the exit save overwrite it with whatever this probe
	# left in memory. Re-reading the restored file puts the state back in step, so
	# the exit save writes the user's own values.
	_restore_settings()
	if overlay != null and is_instance_valid(overlay):
		for id in ids:
			overlay.call("set_enabled", id, false)
	if menu != null and is_instance_valid(menu):
		menu.call("_load_settings")
		for id in ids:
			print("probe: leaving shader %s %s at its saved values"
				% [id, "on" if bool(overlay.call("is_enabled", id)) else "off"])
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)
