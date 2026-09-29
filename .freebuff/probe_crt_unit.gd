extends SceneTree
## The CRT pass, on a flat grey field, where its structure can be measured
## exactly instead of glimpsed through a world.
##
## Runs WINDOWED through .freebuff/run_probe_shot.sh. It builds its own two-rect
## scene - a field, then the shader over it - so it never loads the world and
## never touches the player's settings.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_crt_unit.gd [timeout]
##
## What it pins, in order:
##   - the tube: with no curvature the picture fills the frame corner to corner;
##     with curvature the four edges still reach the bezel, and only the corners
##     are filed round - measured, not assumed
##   - the raster and the beam: the picture is drawn on a grid of picture_scale-
##     pixel dots, so a frame of H pixels gets H/picture_scale lines and that
##     many dark bands, and a dot is the size of the picture's own pixels;
##     scanline_depth is how far down those bands take it
##   - the stretch: a one-frame-pixel checkerboard - the sharpest thing a frame
##     can hold - comes back as soft spots with its brightness intact, because
##     the beam is a wide spot over a coarse raster and not a filter over a sharp
##     image. The zoomed map this prints (one character per frame pixel) is how a
##     person can see it: spots the size of the picture's pixels, with dark rows
##     between them
##   - the halo: beside a bright block it lifts the dark, and a long way from one
##     it leaves the dark alone - measured on a black field with one bright
##     block on it, because how much a *live* frame has over the phosphor
##     threshold decides how much glow there is to see in it
##   - the face: the mask is a mask_pitch-picture-pixel stripe per gun with the
##     three guns one pitch apart, and switching it off leaves the frame colourless
##   - the glass: vignette's falloff at the corner, as the shader defines it
##
## It also writes .freebuff/shots/shader_shots/flat_*.png of each state.

const SHADER_PATH := "res://shaders/crt.gdshader"
const SHOT_DIR := "user://shader_shots"
const FIELD := 0.5        # the flat frame the tube is given
const INSIDE := 0.02      # above this, a pixel is picture rather than bezel
const SPOT_PX := 32       # the bright block on the halo's field, in frame pixels

var field: TextureRect
var crt: ColorRect
var material: ShaderMaterial
var failures := 0
var shots := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	var size := Vector2i(1280, 720)
	if OS.get_environment("PROBE_W").is_valid_int():
		size.x = int(OS.get_environment("PROBE_W"))
	if OS.get_environment("PROBE_H").is_valid_int():
		size.y = int(OS.get_environment("PROBE_H"))
	DisplayServer.window_set_size(size)
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)

	field = TextureRect.new()
	field.texture = _field_image("flat")
	field.stretch_mode = TextureRect.STRETCH_SCALE
	field.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	field.set_anchors_preset(Control.PRESET_FULL_RECT)
	field.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(field)

	crt = ColorRect.new()
	crt.color = Color(0, 0, 0, 0)
	crt.set_anchors_preset(Control.PRESET_FULL_RECT)
	crt.mouse_filter = Control.MOUSE_FILTER_IGNORE
	material = ShaderMaterial.new()
	material.shader = load(SHADER_PATH)
	crt.material = material
	root.add_child(crt)

	if material.shader == null:
		_fail("no shader at %s" % SHADER_PATH)
		_finish()
		return
	for i in range(4):
		await process_frame

	# The reference is the field with the pass out of the way: what the pass hands
	# back with nothing dialled in has to be that frame, sample for sample.
	crt.visible = false
	await _settle()
	var reference := await _capture()
	crt.visible = true
	await _all_off()
	print("probe: window %s, a %.2f flat field renders at %.4f"
		% [DisplayServer.window_get_size(), FIELD, _mean(reference)])
	var flat := await _capture()
	var drift := _worst_drift(reference, flat)
	print("probe: with nothing dialled in the pass drifts %.4f from the field" % drift)
	if drift > 0.01:
		_fail("with everything off the frame drifts %.4f from the field" % drift)
	if _black_corners(flat) > 0:
		_fail("with curvature off the corners are still filed off")
	if absf(_mean(flat) - _mean(reference)) > 0.01:
		_fail("with everything off the frame is %.4f, not the field's %.4f"
			% [_mean(flat), _mean(reference)])

	await check_tube()
	await check_beam()
	await check_stretch()
	await check_glow()
	await check_face()
	await check_glass()
	_finish()


## The flat field the tube, beam and glass are measured on, or the sharpest
## thing a frame can hold: a checkerboard of single frame pixels, which a real
## set cannot draw and which is therefore the thing that shows whether the pass
## is drawing the picture with a beam or filtering a sharp image.
func _field_image(kind: String) -> ImageTexture:
	var size := Vector2i(maxi(DisplayServer.window_get_size().x, 1),
		maxi(DisplayServer.window_get_size().y, 1))
	if kind == "flat":
		var one := Image.create(1, 1, false, Image.FORMAT_RGBA8)
		one.fill(Color(FIELD, FIELD, FIELD, 1.0))
		return ImageTexture.create_from_image(one)
	if kind == "spot":
		# A bright block in the middle of a black field: the halo's subject, and
		# the only field here with anything in it for the halo to find.
		var spot := Image.create(size.x, size.y, false, Image.FORMAT_RGBA8)
		spot.fill(Color.BLACK)
		var half := SPOT_PX / 2
		for y in range(size.y / 2 - half, size.y / 2 + half):
			for x in range(size.x / 2 - half, size.x / 2 + half):
				spot.set_pixel(x, y, Color.WHITE)
		return ImageTexture.create_from_image(spot)
	var block := 1 if kind == "checker" else 4
	var img := Image.create(size.x, size.y, false, Image.FORMAT_RGBA8)
	for y in size.y:
		for x in size.x:
			var on := ((x / block) + (y / block)) % 2 == 0
			img.set_pixel(x, y, Color.WHITE if on else Color.BLACK)
	return ImageTexture.create_from_image(img)


# --- The tube -----------------------------------------------------------------

func check_tube() -> void:
	await _all_off()
	await _set_param("curvature", 0.15)
	var img := await _capture()
	_save(img, "flat_tube")
	_print_inside_map(img)

	var extents := _bezel(img)
	var size := DisplayServer.window_get_size()
	print("probe: picture reaches x %d..%d of %d and y %d..%d of %d"
		% [extents["left"], extents["right"], size.x, extents["top"], extents["bottom"], size.y])
	# The corners are filed round, the edges are not: a picture that stops short
	# of the side is a black frame, and one that reaches the corner is no tube.
	if extents["left"] > 3 or extents["right"] < size.x - 4:
		_fail("the picture does not reach the side bezel (%d..%d)" % [extents["left"], extents["right"]])
	if extents["top"] > 3 or extents["bottom"] < size.y - 4:
		_fail("the picture does not reach the top/bottom bezel (%d..%d)" % [extents["top"], extents["bottom"]])

	var cut := _corner_cut(img)
	for line in cut:
		print("probe: %s corner: first picture at x %.0f, y %.0f, %.0f px in on the diagonal"
			% [line["name"], line["edge_x"], line["edge_y"], line["diag"]])
	if cut[0]["diag"] < 10.0:
		_fail("the corners are barely filed (%.0f px on the diagonal)" % cut[0]["diag"])
	if cut[0]["edge_x"] > size.x * 0.45 or cut[0]["edge_y"] > size.y * 0.45:
		_fail("the corner cut is a black frame, not a fillet (%.0f x %.0f px)"
			% [cut[0]["edge_x"], cut[0]["edge_y"]])
	for line in cut:
		if absf(float(line["diag"]) - float(cut[0]["diag"])) > 4.0:
			_fail("the %s corner is filed %.0f px, not %.0f" % [line["name"], line["diag"], cut[0]["diag"]])

	# Curvature is monotonic: more of it files more of the corner away.
	await _set_param("curvature", 0.5)
	var more := _corner_cut(await _capture())
	print("probe: at curvature 0.5 the corner is %.0f px in on the diagonal" % more[0]["diag"])
	if more[0]["diag"] <= cut[0]["diag"]:
		_fail("curvature 0.5 does not file the corner further than 0.15")


# --- The beam -----------------------------------------------------------------

func check_beam() -> void:
	await _all_off()
	# The coarsest raster the shader offers, so the bands are easy to count and
	# the period is far from a frame pixel: one band per dot, and the profile
	# measures the middle half of the frame.
	await _set_param("picture_scale", 8.0)
	await _set_param("scanline_depth", 1.0)
	var frame := DisplayServer.window_get_size().y
	var shown := int(frame / 8.0)
	var want := int(shown / 2.0)
	var full := _profile(await _capture())
	print("probe: 8 px dots over %d px -> %d lines, %d bands in the middle half, period %.1f px, deepest %.3f of the light"
		% [frame, shown, full["bands"], full["period"], full["dip"]])
	if absf(float(full["bands"]) - float(want)) > 2.0:
		_fail("picture_scale 8 drew %d dark bands in the middle half, want %d" % [full["bands"], want])
	if absf(float(full["period"]) - 8.0) > 0.5:
		_fail("picture_scale 8 drew lines %.1f px apart, want 8" % full["period"])
	if full["dip"] > 0.15:
		_fail("scanline_depth 1.0 left %.2f of the light in the gaps" % full["dip"])

	await _set_param("scanline_depth", 0.5)
	var half := _profile(await _capture())
	print("probe: depth 0.5 leaves %.3f in the gaps (the pass adds %.2f%% back to the lines)"
		% [half["dip"], (half["peak"] / FIELD - 1.0) * 100.0])
	if absf(float(half["dip"]) - 0.5) > 0.12:
		_fail("scanline_depth 0.5 leaves %.2f of the light, want about 0.5" % half["dip"])
	if half["peak"] < FIELD:
		_fail("the beam darkens the lines instead of paying back what the gaps took")

	await _set_param("scanline_depth", 0.0)
	var none := _profile(await _capture())
	if none["bands"] > 4:
		_fail("scanline_depth 0 still draws %d bands" % none["bands"])
	print("probe: depth 0 leaves the frame flat (%.4f)" % none["dip"])


# --- The raster and the beam's own spot ---------------------------------------

func check_stretch() -> void:
	var size := DisplayServer.window_get_size()
	# The picture is drawn with half the frame's lines here (a two-pixel dot), so
	# one of the picture's pixels is a 2x2 block of frame pixels: big enough to
	# see, and the "whole number of pixels" the raster needs.
	var lines := size.y / 2

	# A one-frame-pixel checkerboard is the sharpest thing a frame can hold and a
	# real set cannot draw it at all: the beam's spot is wider than that, so the
	# pattern averages out into the grey it was made of. A filter laid over the
	# frame would keep it.
	await _all_off()
	field.texture = _field_image("checker")
	await _settle()
	var off := await _capture()
	await _set_param("picture_scale", 2.0)
	await _set_param("beam_softness", 0.6)
	var washed := await _capture()
	var fine_off := _fine_detail(off)
	var fine_on := _fine_detail(washed)
	print("probe: 1 px checker detail %.4f -> %.4f over a %d-line raster, mean %.4f -> %.4f"
		% [fine_off, fine_on, lines, _mean(off), _mean(washed)])
	if fine_on > fine_off * 0.2:
		_fail("a one-pixel checker survived the beam (%.4f of %.4f)" % [fine_on, fine_off])
	# Spreading the picture must not eat it: the gather is re-normalised, so a
	# checkerboard of black and white comes back at the same average.
	if absf(_mean(washed) - _mean(off)) > 0.03:
		_fail("the beam changed the picture's average (%.4f -> %.4f)" % [_mean(off), _mean(washed)])

	# A pattern the raster *can* draw - 4-frame-px blocks, two picture pixels of
	# each colour - keeps its plateaus, because a picture pixel is drawn as one
	# spot: what softens at the beat is the boundary between the two colours, not
	# the colours themselves.
	field.texture = _field_image("blocks")
	await _set_param("picture_scale", 1.0)
	# The spot is held at its sharpest for both halves of this, so what is being
	# measured is the raster's own averaging and not the spot's overlap: at a
	# quarter of a cell the gather is a point sample, and any contrast the
	# picture loses here it lost to the cells it was averaged into.
	await _set_param("beam_softness", 0.25)
	var blocks_off := _block_contrast(await _capture())
	await _set_param("picture_scale", 2.0)
	var blocks_on := _block_contrast(await _capture())
	print("probe: 4 px block contrast %.4f -> %.4f" % [blocks_off, blocks_on])
	# The plateaus keep their contrast: a picture the raster can draw is drawn.
	if blocks_on < blocks_off * 0.75:
		_fail("the raster flattened the blocks the picture can actually draw (%.4f of %.4f)"
			% [blocks_on, blocks_off])

	# The shape of it, at one character per frame pixel, so the blobs and the dark
	# rows between them can be looked at instead of only counted.
	await _set_param("scanline_depth", 0.45)
	await _set_param("beam_softness", 0.6)
	_print_zoom(await _capture(), 0, 0, 72, 30)
	field.texture = _field_image("flat")
	await _settle()


## The contrast between the middle of a white block and the middle of a black
## one, on the 4-frame-px checkerboard: what a picture the raster *can* draw
## looks like after the beam has spread it.
func _block_contrast(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var block := 4
	var white := 0.0
	var black := 0.0
	var count := 0
	for by in range(4, h - 8, 24):
		for bx in range(4, w - 8, 24):
			# The pixel one in from a block's corner, and the same pixel of the
			# block beside it: both are inside a plateau, not on a boundary.
			var x := bx + 1
			var y := by + 1
			var a := _lum(data, w, x, y)
			var b := _lum(data, w, x + block, y)
			if ((x / block) + (y / block)) % 2 == 0:
				white += a
				black += b
			else:
				white += b
				black += a
			count += 1
	return absf(white - black) / maxf(float(count), 1.0)


# --- The halo -----------------------------------------------------------------

## The halo is a blur of what is over the phosphor threshold and nothing else:
## beside a bright block it lifts the dark, and a long way from one it does not
## touch the dark at all. A flat field cannot show this, so this one has a block
## on it.
func check_glow() -> void:
	await _all_off()
	field.texture = _field_image("spot")
	# A four-pixel dot, so the halo's reach in dots is a reach in pixels: at a
	# one-pixel raster it is a pixel wide and there is nothing to measure.
	await _set_param("picture_scale", 4.0)
	var dark := await _capture()
	await _set_param("glow", 1.5)
	await _set_param("glow_radius", 1.0)
	var lit := await _capture()
	# The patch just outside the block's edge, and one in the corner: no dot whose
	# neighbourhood reaches the far one has any light in it at all.
	var near := SPOT_PX / 2 + 1
	var near_dark := _patch_lum(dark, 0.5, 0.5, near, -3, 6)
	var near_lit := _patch_lum(lit, 0.5, 0.5, near, -3, 6)
	var far_dark := _patch_lum(dark, 0.05, 0.05, 0, 0, 8)
	var far_lit := _patch_lum(lit, 0.05, 0.05, 0, 0, 8)
	print("probe: glow lifts the dark beside the block %.3f -> %.3f, and leaves the far corner %.3f -> %.3f"
		% [near_dark, near_lit, far_dark, far_lit])
	if near_lit < near_dark + 0.02:
		_fail("the halo does not lift the dark beside a bright block (%.3f -> %.3f)"
			% [near_dark, near_lit])
	if far_lit > far_dark + 0.005:
		_fail("the halo smears light into a corner with none (%.3f -> %.3f)"
			% [far_dark, far_lit])
	# Back to the plain field: the checks below are all about a frame with nothing
	# in it, and this is the only one that needs something bright to look at.
	field.texture = _field_image("flat")
	await _settle()


# --- The face -----------------------------------------------------------------

func check_face() -> void:
	await _all_off()
	var plain := await _capture()
	var before := _channel_split_row(plain)
	# A phosphor triad is one picture pixel wide, so the picture has to be coarse
	# enough for three stripes inside one of its pixels to be measurable: at an
	# eight-pixel dot those stripes are 8/3 of a frame pixel apart and the triad
	# repeats every 8.
	await _set_param("picture_scale", 8.0)
	await _set_param("mask_strength", 1.0)
	await _set_param("mask_pitch", 1.0)
	var img := await _capture()
	_save(img, "flat_mask")
	var period := 8   # one mask_pitch-wide triad, mask_pitch being one 8 px dot
	var phase := _channel_phase(img, period)
	print("probe: mask 1 px/pixel -> period %d frame px, guns at R %.2f / G %.2f / B %.2f px into it"
		% [period, phase["r"], phase["g"], phase["b"]])
	var pitch := float(period) / 3.0
	var rg: float = fposmod(float(phase["g"]) - float(phase["r"]), float(period))
	var gb: float = fposmod(float(phase["b"]) - float(phase["g"]), float(period))
	if absf(rg - pitch) > 0.5:
		_fail("green sits %.2f px from red, want %.2f" % [rg, pitch])
	if absf(gb - pitch) > 0.5:
		_fail("blue sits %.2f px from green, want %.2f" % [gb, pitch])
	var after := _channel_split_row(img)
	print("probe: mask split %.4f vs %.4f with it off" % [after, before])
	if after <= before * 4.0:
		_fail("the mask barely colours the frame (%.4f vs %.4f)" % [after, before])

	# The pattern is periodic, not noise: one period on is the frame again.
	var same := _row_correlation(img, period)
	print("probe: one period on, the row matches itself at %.4f" % same)
	if same < 0.98:
		_fail("the mask pattern does not repeat every %d px (%.3f)" % [period, same])


# --- The glass ----------------------------------------------------------------

func check_glass() -> void:
	await _all_off()
	await _set_param("vignette", 0.5)
	var img := await _capture()
	var centre := _region_lum(img, 0.5, 0.5)
	var corner := _region_lum(img, 0.02, 0.02)
	print("probe: vignette 0.5 -> centre %.3f (of a %.2f field), corner %.3f" % [centre, FIELD, corner])
	if centre < FIELD * 0.97:
		_fail("vignette 0.5 dims the middle of the glass (%.3f)" % centre)
	if absf(corner - FIELD * 0.5) > FIELD * 0.08:
		_fail("vignette 0.5 leaves the corner at %.3f, want about %.2f" % [corner, FIELD * 0.5])


# --- Driving it ---------------------------------------------------------------

func _all_off() -> void:
	for uniform in material.shader.get_shader_uniform_list():
		var name := String(uniform["name"])
		if name == "frame_size":
			continue
		var default: Variant = uniform.get("default_value", 0.0)
		material.set_shader_parameter(name, 0.0 if default == null else default)
	# Whatever the shader's own defaults are, the checks below want a clean slate,
	# and the one uniform that is not a look - the frame it measures - has to be
	# right before anything is measured.
	material.set_shader_parameter("curvature", 0.0)
	material.set_shader_parameter("corner_band", 0.5)
	# A frame-pixel raster and the sharpest beam the shader allows: the pass is
	# then a copy of the frame to within a fraction of a level, which is the
	# reference the tube, the glass and the stretch are measured against.
	# Anything that spreads the picture is switched on by the check about it.
	material.set_shader_parameter("picture_scale", 1.0)
	material.set_shader_parameter("beam_softness", 0.25)
	material.set_shader_parameter("scanline_depth", 0.0)
	material.set_shader_parameter("mask_strength", 0.0)
	material.set_shader_parameter("mask_pitch", 3.0)
	material.set_shader_parameter("convergence", 0.0)
	material.set_shader_parameter("glow", 0.0)
	material.set_shader_parameter("glow_radius", 4.0)
	material.set_shader_parameter("vignette", 0.0)
	material.set_shader_parameter("brightness", 1.0)
	material.set_shader_parameter("contrast", 1.0)
	material.set_shader_parameter("saturation", 1.0)
	material.set_shader_parameter("hum", 0.0)
	await _settle()


func _set_param(key: String, value: Variant) -> void:
	material.set_shader_parameter(key, value)
	await _settle()


func _settle() -> void:
	material.set_shader_parameter("frame_size", Vector2(root.get_visible_rect().size))
	for i in range(3):
		await process_frame


func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


# --- Reading the frame --------------------------------------------------------

## The largest per-channel gap anywhere in the frame, as a fraction of full
## scale: "the same picture" has to mean the same picture everywhere.
func _worst_drift(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 1.0
	var worst := 0.0
	for i in pa.size():
		worst = maxf(worst, absf(int(pa[i]) - int(pb[i])))
	return worst / 255.0


func _mean(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var total := 0.0
	var count := 0
	# An odd stride on purpose: an even one lines up with a one-pixel
	# checkerboard and reports it as one flat colour.
	for y in range(0, h, 3):
		for x in range(0, w, 3):
			total += _lum(data, w, x, y)
			count += 1
	return total / maxf(float(count), 1.0)


func _region_lum(img: Image, fx: float, fy: float) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var x0 := int(fx * float(w))
	var y0 := int(fy * float(h))
	var total := 0.0
	var count := 0
	for y in range(y0, y0 + 8):
		for x in range(x0, x0 + 8):
			total += _lum(data, w, x, y)
			count += 1
	return total / maxf(float(count), 1.0)


## What each corner's fillet costs, in pixels: how far in along the top edge (or
## bottom, for the lower corners) the picture starts on that first row, the same
## down the first column, and how far in the pixel diagonal the picture starts.
func _corner_cut(img: Image) -> Array:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var out: Array = []
	for corner_index in 4:
		var sx: int = 1 if corner_index % 2 == 0 else -1
		var sy: int = 1 if corner_index < 2 else -1
		var cx: int = 0 if sx > 0 else w - 1
		var cy: int = 0 if sy > 0 else h - 1
		var x_run := 0
		while x_run < w and _lum(data, w, cx + sx * x_run, cy) <= INSIDE:
			x_run += 1
		var y_run := 0
		while y_run < h and _lum(data, w, cx, cy + sy * y_run) <= INSIDE:
			y_run += 1
		var diag := 0
		while diag < mini(w, h) and _lum(data, w, cx + sx * diag, cy + sy * diag) <= INSIDE:
			diag += 1
		out.append({
			"name": ["top left", "top right", "bottom left", "bottom right"][corner_index],
			"edge_x": x_run,
			"edge_y": y_run,
			"diag": diag,
		})
	return out


func _black_corners(img: Image) -> int:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var black := 0
	for corner_index in 4:
		var x: int = 1 if corner_index % 2 == 0 else w - 2
		var y: int = 1 if corner_index < 2 else h - 2
		if _lum(data, w, x, y) <= INSIDE:
			black += 1
	return black


## Where the picture starts, on the frame's own midlines: the bezel's width along
## the middle of each edge.
func _bezel(img: Image) -> Dictionary:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var y := h / 2
	var x := w / 2
	var left := 0
	while left < w and _lum(data, w, left, y) <= INSIDE:
		left += 1
	var right := w - 1
	while right > 0 and _lum(data, w, right, y) <= INSIDE:
		right -= 1
	var top := 0
	while top < h and _lum(data, w, x, top) <= INSIDE:
		top += 1
	var bottom := h - 1
	while bottom > 0 and _lum(data, w, x, bottom) <= INSIDE:
		bottom -= 1
	return {"left": left, "right": right, "top": top, "bottom": bottom}


## The beam, measured down one column band: how many dark bands the middle half
## of the frame has, how far apart they are, and how far they take the light.
func _profile(img: Image) -> Dictionary:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var x0 := w / 2 - w / 8
	var x1 := w / 2 + w / 8
	var rows: Array = []
	for y in range(h / 4, h / 4 * 3):
		var total := 0.0
		var count := 0
		for x in range(x0, x1, 4):
			total += _lum(data, w, x, y)
			count += 1
		rows.append(total / maxf(float(count), 1.0))
	var peak := 0.0
	var low := 1.0
	for value in rows:
		peak = maxf(peak, float(value))
		low = minf(low, float(value))
	var bands := 0
	var first := -1
	var last := -1
	for i in range(1, rows.size() - 1):
		if rows[i] < rows[i - 1] and rows[i] <= rows[i + 1] and float(rows[i]) < peak * 0.9:
			bands += 1
			if first < 0:
				first = i
			last = i
	var period := 0.0
	if bands > 1:
		period = float(last - first) / float(bands - 1)
	return {"bands": bands, "period": period, "peak": peak, "dip": low / maxf(peak, 0.0001)}


## How much the sharpest thing a frame can hold - a one-frame-pixel checkerboard
## - survives, as the mean step between neighbouring frame pixels.
func _fine_detail(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var total := 0.0
	var count := 0
	for y in range(h / 4, h / 4 * 3):
		for x in range(w / 4, w / 4 * 3):
			total += absf(_lum(data, w, x, y) - _lum(data, w, x + 1, y))
			total += absf(_lum(data, w, x, y) - _lum(data, w, x, y + 1))
			count += 2
	return total / maxf(float(count), 1.0)


## How much the three guns disagree on one row of the mask pattern.
func _channel_split_row(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var y := img.get_height() / 2
	var total := 0.0
	var count := 0
	for x in range(w / 4, w / 4 * 3):
		var i := (y * w + x) * 4
		total += absf(float(data[i]) - float(data[i + 1])) + absf(float(data[i + 1]) - float(data[i + 2]))
		count += 2
	return total / (255.0 * maxf(float(count), 1.0))


## Where each gun's stripe sits inside one period of the mask, in pixels from the
## start of the period. The stripe is read as a circular mean over many periods
## rather than as its brightest pixel: with mask_strength up the stripe is a flat
## plateau several pixels wide, and "the first pixel of the plateau" is not where
## the stripe is - the mean is, to a fraction of a pixel.
func _channel_phase(img: Image, period: int) -> Dictionary:
	var data := img.get_data()
	var w := img.get_width()
	var y := img.get_height() / 2
	var re := {"r": 0.0, "g": 0.0, "b": 0.0}
	var im := {"r": 0.0, "g": 0.0, "b": 0.0}
	for x in range(w / 4, w / 4 * 3):
		var i := (y * w + x) * 4
		var angle := TAU * float(x % period) / float(period)
		var cos_a := cos(angle)
		var sin_a := sin(angle)
		re["r"] += float(data[i]) * cos_a
		im["r"] += float(data[i]) * sin_a
		re["g"] += float(data[i + 1]) * cos_a
		im["g"] += float(data[i + 1]) * sin_a
		re["b"] += float(data[i + 2]) * cos_a
		im["b"] += float(data[i + 2]) * sin_a
	var out := {}
	for channel in ["r", "g", "b"]:
		var mean_angle := atan2(im[channel], re[channel])
		out[channel] = fposmod(mean_angle / TAU * float(period), float(period))
	return out


func _row_correlation(img: Image, period: int) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var y := img.get_height() / 2
	var a: Array = []
	var b: Array = []
	for x in range(w / 4, w / 2):
		a.append(float(data[(y * w + x) * 4]))
		b.append(float(data[(y * w + x + period) * 4]))
	var mean_a := 0.0
	var mean_b := 0.0
	for i in a.size():
		mean_a += a[i]
		mean_b += b[i]
	mean_a /= float(a.size())
	mean_b /= float(b.size())
	var num := 0.0
	var da := 0.0
	var db := 0.0
	for i in a.size():
		num += (a[i] - mean_a) * (b[i] - mean_b)
		da += (a[i] - mean_a) * (a[i] - mean_a)
		db += (b[i] - mean_b) * (b[i] - mean_b)
	return num / maxf(sqrt(da * db), 0.0000001)


## The mean luminance of a `size` square patch whose top-left corner is `dx`/`dy`
## frame pixels from the point at (fx, fy) of the frame.
func _patch_lum(img: Image, fx: float, fy: float, dx: int, dy: int, size: int) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var x0 := clampi(int(fx * float(w)) + dx, 0, w - size)
	var y0 := clampi(int(fy * float(h)) + dy, 0, h - size)
	var total := 0.0
	for y in range(y0, y0 + size):
		for x in range(x0, x0 + size):
			total += _lum(data, w, x, y)
	return total / float(size * size)


func _lum(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (0.2126 * float(data[i]) + 0.7152 * float(data[i + 1]) + 0.0722 * float(data[i + 2])) / 255.0


# --- Odds and ends ------------------------------------------------------------

## The bezel's shape, as text: the corners should read as four rounded fillets,
## which a wall of numbers does not show.
func _print_inside_map(img: Image) -> void:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	for region in [["whole frame", 0, 0, w, h], ["top left eighth", 0, 0, w / 3, h / 3]]:
		var cols := 72
		var rows := 26
		var x0: int = region[1]
		var y0: int = region[2]
		var bw: int = region[3]
		var bh: int = region[4]
		var out := ""
		for row in rows:
			var line := ""
			for col in cols:
				var x := x0 + col * bw / cols
				var y := y0 + row * bh / rows
				line += "#" if _lum(data, w, x, y) > INSIDE else "."
			# Every line is tagged so the probe harness, which keeps only lines it
			# recognises, passes the shape through to be read.
			out += "probe: map %s\n" % line
		print("probe: bezel map, %s (# picture, . bezel)\n%s" % [region[0], out])


## One character per frame pixel, over a window of the frame: the resolution the
## tube's own pixels and the dark rows between them are visible at.
func _print_zoom(img: Image, ox: int, oy: int, cols: int, rows: int) -> void:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var ramp := " .:-=+*#%@"
	var out := ""
	for row in rows:
		if oy + row >= h:
			break
		var line := ""
		for col in cols:
			if ox + col >= w:
				break
			var value := _lum(data, w, ox + col, oy + row)
			line += ramp[clampi(int(value * 10.0), 0, 9)]
		out += "probe: zoom %s\n" % line
	print("probe: zoom of (%d,%d) %dx%d, one character per frame pixel\n%s"
		% [ox, oy, cols, rows, out])


func _save(img: Image, name: String) -> void:
	var path := "%s/%s_%dx%d.png" % [SHOT_DIR, name, img.get_width(), img.get_height()]
	img.save_png(path)
	shots += 1
	print("probe: shot %s" % path)


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _finish() -> void:
	print("probe: %d shots, %d failures" % [shots, failures])
	quit(1 if failures > 0 else 0)
