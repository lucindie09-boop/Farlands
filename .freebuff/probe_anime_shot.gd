extends SceneTree
## The Anime Look pass, and what makes a world effect a world effect.
##
## Runs WINDOWED - every check below is about pixels that were actually rendered,
## and the dummy renderer of --headless has no frame to read:
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_anime_shot.gd [timeout]
##
## What it pins:
##
##   - A 'world' entry draws at the world's own depth: the hotbar's pixels come
##     back *identical* with the pass on and with it off, while the same frame
##     with the CRT on moves them. That is the whole of the difference between the
##     registry's two pass kinds, and it is not a thing the shader can be asked -
##     a shader is handed one frame and has no idea where in the stack it was
##     drawn - so it is checked on the layer and on the pixels together.
##   - The flattening flattens: on the ground the picture loses its per-pixel
##     colour noise and keeps its brightness, and the same pass with the flatten
##     wound back does not lose it, so what took the noise off is the Kuwahara and
##     not the banded shading that runs after it.
##   - The ink is lines rather than speckle: with the outline up a small share of
##     the ground goes dark, those pixels come in contiguous runs rather than as
##     salt, and the share grows with the knob while the picture's shape does not.
##
## Where the pass's *taste* is judged is the screenshots - user://shader_shots,
## harvested into .freebuff/shots/shader_shots. What can be measured is that the
## three things it claims to do are done, and that it does them to the world
## alone.
##
## The comparisons are back to back with one knob changed between the two
## captures. Where a check needs a reference frame taken earlier, the world's own
## drift between two consecutive captures is measured and printed beside the
## number it is being held against, so a check that is really measuring the world
## says so out loud.
##
## The user's own settings are read at the start and written back at the end.

const SHOT_DIR := "user://shader_shots"
const SETTINGS_PATH := "user://settings.cfg"
const OVERLAY_PATH := "HUD/ShaderOverlay"
const ANIME_ID := "anime"
const CRT_ID := "crt"
# The band of the frame the ground is in: below the horizon, above the hotbar.
# The sky is deliberately not in it - a gradient is not noise, and the flattening
# is supposed to leave it alone.
const BAND_TOP_PCT := 55
const BAND_BOTTOM_PCT := 85

var overlay: Control = null
var menu: Control = null
var main: Node3D = null
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
		_fail("Main.tscn missing")
		_finish()
		return
	main = scene.instantiate()
	root.add_child(main)
	overlay = main.get_node_or_null(OVERLAY_PATH)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	if overlay == null:
		_fail("no %s in Main.tscn" % OVERLAY_PATH)
		_finish()
		return
	# The world is given time to stream in with the game running, and then the tree
	# is paused: a frame that is still is what makes a difference between two
	# captures the pass's and nothing else's, and the drift measured below is the
	# check that it really is still. Paused rather than left alone-and-hoped, and
	# with no menu over the top, because the HUD is part of what this probe is
	# measuring and a page over it would hide the thing being checked.
	for i in range(90):
		await process_frame
	paused = true
	for i in range(5):
		await process_frame

	ids = overlay.call("get_shader_ids")
	_check_kind_wiring()
	await _check_hud_is_not_graded()
	await _check_flattening()
	await _check_ink()

	_finish()


# --- The kind: what a world pass draws over, and what draws over it -----------

## The layer the kind implies: a world entry is drawn at the world's depth and a
## screen entry over the HUD art below this node. Checked here as well as in
## probe_shaders_shot.gd because it is the premise of the pixel check below - if
## the anime layer were built as a screen pass, the frame the pass reads would
## have the hotbar in it and the pixels would say so.
func _check_kind_wiring() -> void:
	for definition in overlay.call("get_definitions"):
		var id := String(definition.get("id", ""))
		var kind := String(definition.get("kind", "screen"))
		if kind == "vertex":
			continue
		var layer := overlay.get_node_or_null(id) as ColorRect
		if layer == null:
			_fail("%s is a %s effect with no rect to draw through" % [id, kind])
			continue
		var want := -1 if kind == "world" else 0
		print("probe: %s is a %s effect, drawn at z_index %d (wanted %d)"
			% [id, kind, layer.z_index, want])
		if layer.z_index != want:
			_fail("%s draws at z_index %d, wanted %d" % [id, layer.z_index, want])
	# The world's pass has to be under everything else this node owns, or "under
	# the HUD" would be true of the HUD only by luck of the tree order.
	var anime := overlay.get_node_or_null(ANIME_ID) as ColorRect
	var crt := overlay.get_node_or_null(CRT_ID) as ColorRect
	if anime == null or crt == null:
		_fail("the registry has no %s or %s entry" % [ANIME_ID, CRT_ID])
		return
	if anime.z_index >= crt.z_index:
		_fail("the world pass (%d) is not below the screen pass (%d)"
			% [anime.z_index, crt.z_index])


## The hotbar is a Control in the HUD, so its rect is where the HUD is: a pixel
## count over that rect is a pixel count over HUD art that the world pass must
## not have touched and the screen pass must have.
func _hotbar_rect() -> Rect2i:
	var node := main.get_node_or_null("HUD/Hotbar") as Control
	if node == null or not node.visible:
		return Rect2i()
	var rect := node.get_global_rect()
	return Rect2i(rect.position.round(), rect.size.round())


func _check_hud_is_not_graded() -> void:
	for id in ids:
		overlay.call("set_enabled", id, false)
	await _settle()
	var off := await _capture()
	# Two captures with nothing changed in between: the world's own drift, which
	# every number below has to be bigger than to mean anything.
	var drift := await _capture()
	var hotbar := _hotbar_rect()
	print("probe: the hotbar is %s in a %dx%d frame; the world's own drift is %.4f"
		% [hotbar, off.get_width(), off.get_height(), _mean_diff(off, drift)])
	if hotbar.size.x <= 0 or hotbar.size.y <= 0:
		_fail("the hotbar is not on screen, so there is no HUD art to leave alone")
		return

	await _knob(ANIME_ID, "anime_flatten", 0.8)
	await _knob(ANIME_ID, "anime_bands", 5.0)
	await _knob(ANIME_ID, "anime_outline", 0.7)
	overlay.call("set_enabled", ANIME_ID, true)
	await _settle()
	var anime_on := await _capture()
	var world_diff := _mean_diff(off, anime_on)
	print("probe: the world pass moves the frame by %.4f (the world's own drift is %.4f)"
		% [world_diff, _mean_diff(off, drift)])
	if world_diff <= maxf(_mean_diff(off, drift) * 4.0, 0.002):
		_fail("the world pass is on but the frame is unchanged (%.4f)" % world_diff)
	var hud_here := _region_diff(off, anime_on, hotbar)
	print("probe: over the hotbar the same two frames differ by %.5f (%d of %d pixels)"
		% [hud_here["mean"], hud_here["changed"], hud_here["count"]])
	if hud_here["changed"] != 0:
		_fail("the world pass graded the HUD: %d of %d hotbar pixels moved"
			% [hud_here["changed"], hud_here["count"]])
	_save(anime_on, "anime_default")

	# The contrast that gives the number above its meaning: the same hotbar, the
	# same world, a pass of the other kind switched on instead.
	overlay.call("set_enabled", ANIME_ID, false)
	overlay.call("set_enabled", CRT_ID, true)
	await _settle()
	var crt_on := await _capture()
	var hud_crt := _region_diff(off, crt_on, hotbar)
	print("probe: with the CRT on instead, the same pixels differ by %.5f (%d of %d moved)"
		% [hud_crt["mean"], hud_crt["changed"], hud_crt["count"]])
	if hud_crt["changed"] == 0:
		_fail("the screen pass left the hotbar alone too, so this check proves nothing")
	overlay.call("set_enabled", CRT_ID, false)
	await _settle()


# --- The flattening ----------------------------------------------------------

func _check_flattening() -> void:
	var flatten := _definition(ANIME_ID)
	if flatten.is_empty():
		return
	for param in flatten.get("params", []):
		overlay.call("set_value", ANIME_ID, String(param["key"]), param.get("default", 0.0))

	# The reference: the world with every effect off.
	var off := await _capture()
	var band := _band(off)
	var noise_off := _noise(off, band)
	var lum_off := _band_luminance(off, band)

	# The pass with the flatten wound back but the banding on, so that what is
	# measured turning the flatten up is the flatten turned up.
	await _knob(ANIME_ID, "anime_flatten", 0.0)
	await _knob(ANIME_ID, "anime_outline", 0.0)
	await _knob(ANIME_ID, "anime_saturation", 1.0)
	overlay.call("set_enabled", ANIME_ID, true)
	await _settle()
	var unflattened := await _capture()
	var noise_plain := _noise(unflattened, band)

	await _knob(ANIME_ID, "anime_flatten", 1.0)
	await _knob(ANIME_ID, "anime_radius", 2.0)
	await _knob(ANIME_ID, "anime_bands", 8.0)
	await _settle()
	var flattened := await _capture()
	var noise_flat := _noise(flattened, band)
	var lum_flat := _band_luminance(flattened, band)

	print("probe: ground noise %.4f off, %.4f with the pass on and the flatten at 0, %.4f at 1"
		% [noise_off, noise_plain, noise_flat])
	print("probe: ground brightness %.4f off, %.4f with the frame flattened (%.0f%%)"
		% [lum_off, lum_flat, 100.0 * lum_flat / maxf(lum_off, 0.0001)])
	if noise_flat >= noise_plain * 0.7:
		_fail("flattening did not take the noise off the ground (%.4f against %.4f)"
			% [noise_flat, noise_plain])
	if noise_plain < noise_off * 0.8:
		_fail("the pass flattens the ground with the flatten at zero (%.4f against %.4f)"
			% [noise_plain, noise_off])
	var brightness := lum_flat / maxf(lum_off, 0.0001)
	if brightness < 0.9 or brightness > 1.1:
		_fail("flattening moved the ground's brightness to %.0f%% of what it was"
			% (brightness * 100.0))
	_save(flattened, "anime_flat")

	# The wider window is the knob that costs, so it has to be the knob that
	# changes something: at radius 1 the flattening is narrower than at 3.
	await _knob(ANIME_ID, "anime_radius", 1.0)
	await _settle()
	var narrow := await _capture()
	await _knob(ANIME_ID, "anime_radius", 3.0)
	await _settle()
	var wide := await _capture()
	var narrowness := _mean_diff(flattened, narrow)
	var wideness := _mean_diff(flattened, wide)
	print("probe: the colour window moves the frame by %.4f at 1 px and %.4f at 3 px"
		% [narrowness, wideness])
	if wideness <= narrowness:
		_fail("the colour window's width does nothing (%.4f at 1 px against %.4f at 3)"
			% [narrowness, wideness])
	overlay.call("set_enabled", ANIME_ID, false)
	await _settle()


# --- The ink -----------------------------------------------------------------

func _check_ink() -> void:
	var definition := _definition(ANIME_ID)
	if definition.is_empty():
		return
	for param in definition.get("params", []):
		overlay.call("set_value", ANIME_ID, String(param["key"]), param.get("default", 0.0))
	overlay.call("set_enabled", ANIME_ID, true)

	# Three captures of one frame that differ by the ink's strength and by nothing
	# else, so the first is the reference the other two are read against: a pixel
	# the ink is on is a pixel the second or third took most of the light out of,
	# and no knob but the ink's own moves between them.
	await _knob(ANIME_ID, "anime_outline", 0.0)
	await _settle()
	var bare := await _capture()
	await _knob(ANIME_ID, "anime_outline", 0.25)
	await _settle()
	var light := await _capture()
	await _knob(ANIME_ID, "anime_outline", 1.0)
	await _settle()
	var inked := await _capture()
	var band := _band(inked)

	var stats := _ink_stats(bare, inked, band, 0.7)
	print("probe: ink covers %.2f%% of the ground at 1.0, and %.0f%% of those pixels touch more ink"
		% [stats["share"] * 100.0, stats["clustered"] * 100.0])
	print("probe: a pixel carrying ink has %.1f times the neighbourhood contrast of the band's average"
		% stats["on_edges"])
	if stats["share"] < 0.002:
		_fail("the outline draws almost no ink (%.4f of the ground)" % stats["share"])
	if stats["share"] > 0.2:
		_fail("the outline inks a fifth of the ground (%.4f), which is a wash and not a line"
			% stats["share"])
	if stats["clustered"] < 0.5:
		_fail("the ink is speckle rather than lines (only %.0f%% of it touches more ink)"
			% (stats["clustered"] * 100.0))
	# The claim the pass makes about its lines: they are drawn where the shading
	# jumps, not scattered over the surfaces it just flattened. The reference frame
	# is the same picture with the outline at zero, so any pixel it darkened is
	# ink, and what is measured here is where those pixels are.
	if stats["on_edges"] < 2.0:
		_fail("the ink lands on flat ground rather than on jumps (%.2f times the band's average contrast)"
			% stats["on_edges"])
	_save(inked, "anime_ink")

	# And the knob is a dial and not a switch: at a quarter strength the ink has
	# touched pixels - fewer of them, and by less - than it has at full strength.
	var touched_light := _ink_stats(bare, light, band, 0.95)
	var touched_full := _ink_stats(bare, inked, band, 0.95)
	print("probe: the ink has darkened %.2f%% of the ground at 0.25 and %.2f%% at 1.0"
		% [touched_light["share"] * 100.0, touched_full["share"] * 100.0])
	if touched_full["share"] <= touched_light["share"]:
		_fail("the outline's strength does not change how much of the frame it inks (%.4f against %.4f)"
			% [touched_full["share"], touched_light["share"]])
	if touched_light["share"] <= 0.0:
		_fail("the outline at a quarter strength inks nothing at all")
	overlay.call("set_enabled", ANIME_ID, false)
	await _settle()


# --- The frame ----------------------------------------------------------------

func _band(img: Image) -> Dictionary:
	var h := img.get_height()
	return {"top": h * BAND_TOP_PCT / 100, "bottom": h * BAND_BOTTOM_PCT / 100}


## The picture's per-pixel colour noise, per unit of light: how much a pixel's
## brightness differs from its neighbour's, weighted by what light there is, so a
## dark band and a bright one answer about the same noise. Weighted rather than
## averaged because a voxel frame's lower half is mostly in shadow near the camera
## and mostly haze far away, and a plain mean would report the haze.
func _noise(img: Image, band: Dictionary) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var total := 0.0
	var light := 0.0
	for y in range(int(band["top"]), int(band["bottom"])):
		for x in range(0, w - 1):
			var here := _lum(data, w, x, y)
			var next := _lum(data, w, x + 1, y)
			var weight := minf(here, next)
			if weight <= 0.0:
				continue
			total += absf(next - here) * weight
			light += weight
	if light <= 0.0:
		return 0.0
	return total / light


func _band_luminance(img: Image, band: Dictionary) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var total := 0.0
	var count := 0
	for y in range(int(band["top"]), int(band["bottom"])):
		for x in range(w):
			total += _lum(data, w, x, y)
			count += 1
	return total / maxf(float(count), 1.0)


## The pixels the ink has darkened, and what kind of thing they are: `share` of
## the band, the share of those pixels that have another one four-neighbourly (a
## line one pixel wide is a run, so almost every one of its pixels touches two of
## its own while speckle touches nothing), and how much local contrast those same
## pixels carry against the band's own average - a line is drawn along a jump in
## the picture, and the surfaces either side of it are what the flattening made
## flat.
func _ink_stats(reference: Image, inked: Image, band: Dictionary, cut: float) -> Dictionary:
	var pa := reference.get_data()
	var pb := inked.get_data()
	var w := reference.get_width()
	var top := int(band["top"])
	var bottom := int(band["bottom"])
	var marks := PackedByteArray()
	marks.resize(w * (bottom - top))
	var count := 0
	var marked := 0
	var contrast_here := 0.0
	var contrast_inked := 0.0
	for y in range(top, bottom):
		for x in range(w):
			var before := _lum(pa, w, x, y)
			var after := _lum(pb, w, x, y)
			var step := _step_at(pa, w, top, bottom, x, y)
			count += 1
			contrast_here += step
			# A pixel the ink is on is one the ink took most of the light out of.
			# The cut is what "most" means for the question being asked: at three
			# quarters the pixel keeps a quarter of its light, and at the looser cut
			# used for the strength check above even a fifth of it counts, because
			# what is being asked there is whether the knob has touched the pixel at
			# all.
			if before > 0.08 and after < before * cut:
				marks[(y - top) * w + x] = 1
				marked += 1
				contrast_inked += step
	var touching := 0
	for y in range(top, bottom):
		for x in range(w):
			if marks[(y - top) * w + x] == 0:
				continue
			var alone := true
			if x > 0 and marks[(y - top) * w + x - 1] == 1:
				alone = false
			if x < w - 1 and marks[(y - top) * w + x + 1] == 1:
				alone = false
			if y > top and marks[(y - top - 1) * w + x] == 1:
				alone = false
			if y < bottom - 1 and marks[(y - top + 1) * w + x] == 1:
				alone = false
			if not alone:
				touching += 1
	var mean_here := contrast_here / maxf(float(count), 1.0)
	var mean_inked := contrast_inked / maxf(float(marked), 1.0)
	return {
		"share": float(marked) / maxf(float(count), 1.0),
		"clustered": float(touching) / maxf(float(marked), 1.0),
		"on_edges": mean_inked / maxf(mean_here, 1e-5),
		"count": marked,
	}


## How much the picture jumps around a pixel: the largest brightness difference to
## its four neighbours, which is what a line is drawn along.
func _step_at(data: PackedByteArray, w: int, top: int, bottom: int, x: int, y: int) -> float:
	var here := _lum(data, w, x, y)
	var step := 0.0
	if x > 0:
		step = maxf(step, absf(_lum(data, w, x - 1, y) - here))
	if x < w - 1:
		step = maxf(step, absf(_lum(data, w, x + 1, y) - here))
	if y > top:
		step = maxf(step, absf(_lum(data, w, x, y - 1) - here))
	if y < bottom - 1:
		step = maxf(step, absf(_lum(data, w, x, y + 1) - here))
	return step


# --- Working with the frame ---------------------------------------------------

func _definition(id: String) -> Dictionary:
	for definition in overlay.call("get_definitions"):
		if String(definition.get("id", "")) == id:
			return definition
	_fail("the registry has no %s" % id)
	return {}


func _knob(id: String, key: String, value: Variant) -> void:
	overlay.call("set_value", id, key, value)
	await _settle()


func _settle() -> void:
	for i in range(3):
		await process_frame


func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _lum(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (0.2126 * float(data[i]) + 0.7152 * float(data[i + 1]) + 0.0722 * float(data[i + 2])) / 255.0


func _mean_diff(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var total := 0.0
	var count := 0
	for i in range(0, pa.size() - 3, 16):
		total += absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
			+ absf(int(pa[i + 2]) - int(pb[i + 2]))
		count += 3
	return total / (255.0 * maxf(float(count), 1.0))


## The same difference, but over one rect of the frame and counting the pixels
## that moved at all: "the HUD is untouched" is a claim about every pixel of it,
## so the count is the number that matters and the mean only says how much.
func _region_diff(a: Image, b: Image, rect: Rect2i) -> Dictionary:
	var pa := a.get_data()
	var pb := b.get_data()
	var w := a.get_width()
	var h := a.get_height()
	var left := clampi(rect.position.x, 0, w - 1)
	var right := clampi(rect.position.x + rect.size.x, left + 1, w)
	var top := clampi(rect.position.y, 0, h - 1)
	var bottom := clampi(rect.position.y + rect.size.y, top + 1, h)
	var total := 0.0
	var changed := 0
	var count := 0
	for y in range(top, bottom):
		for x in range(left, right):
			var i := (y * w + x) * 4
			var delta := absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
				+ absf(int(pa[i + 2]) - int(pb[i + 2]))
			total += float(delta) / 3.0
			if delta > 0:
				changed += 1
			count += 1
	return {
		"mean": total / (255.0 * maxf(float(count), 1.0)),
		"changed": changed,
		"count": count,
	}


func _save(img: Image, name: String) -> void:
	var path := "%s/%s_%dx%d.png" % [SHOT_DIR, name, img.get_width(), img.get_height()]
	img.save_png(path)
	shots += 1
	print("probe: shot %s" % path)


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
	# Restoring the file and then quitting would have the menu's own exit save
	# write this probe's state over it, so the state is re-read from the file that
	# was put back before it can be written again.
	_restore_settings()
	if overlay != null and is_instance_valid(overlay):
		for id in ids:
			overlay.call("set_enabled", id, false)
	if menu != null and is_instance_valid(menu):
		menu.call("_load_settings")
		for id in ids:
			print("probe: leaving shader %s %s at its saved values"
				% [id, "on" if bool(overlay.call("is_enabled", id)) else "off"])
	print("probe: %d shot(s) in %s" % [shots, SHOT_DIR])
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)
