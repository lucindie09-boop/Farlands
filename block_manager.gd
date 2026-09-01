extends Node

# Manages block texture creation for the block maker tool.
# Mirrors SkinManager so the block maker behaves identically to the skin maker:
# a single persistent ImageTexture (every cube's albedo points at it), debounced
# saves to user://current_block.png, and a reversible grayscale-noise slider.

const DEFAULT_BLOCK_PATH := "res://textures/blocks/stone.png"
const CURRENT_BLOCK_PATH := "user://current_block.png"
# Restart-recovery snapshot of the clean pre-noise texture: while noise is active
# the reversibility base is kept on disk so the slider can still undo it in a
# fresh game session (the working image itself is saved noisy).
const NOISE_BASE_PATH := "user://block_noise_base.png"
const ATLAS_WIDTH := 16  # Single 16x16 texture for all faces
const ATLAS_HEIGHT := 16

const NOISE_SEED := 20240829
const MAX_GRAIN := 0.35

var image := Image.new()
var texture: ImageTexture

# Active noise severity (0..100) plus the clean base it reverts to. These live
# HERE (not in the block maker page) because the settings page is rebuilt every
# time the menu opens: the autoload survives rebuilds, so the value and the
# reversibility snapshot follow the texture around the UI.
var noise_severity := 0.0
var noise_base: Image
var _noise_map: Image

func _ready() -> void:
	_noise_map = _make_noise_map()
	var prev := _load_current_image()
	if prev != null:
		_set_image(prev)
		return
	_set_image(_default_image())

func _exit_tree() -> void:
	if not image.is_empty():
		image.save_png(CURRENT_BLOCK_PATH)

func _load_current_image() -> Image:
	if not FileAccess.file_exists(CURRENT_BLOCK_PATH):
		return null
	var img := Image.load_from_file(CURRENT_BLOCK_PATH)
	if img == null:
		return null
	if img.get_width() != ATLAS_WIDTH or img.get_height() != ATLAS_HEIGHT:
		img = img.duplicate()
		img.resize(ATLAS_WIDTH, ATLAS_HEIGHT, Image.INTERPOLATE_NEAREST)
	return img

func _default_image() -> Image:
	var tex := load(DEFAULT_BLOCK_PATH) as Texture2D
	var src_img := tex.get_image() if tex != null else null
	if src_img == null:
		src_img = Image.load_from_file(DEFAULT_BLOCK_PATH)
	
	# Use the source texture directly (should be 16x16)
	if src_img != null:
		if src_img.get_width() != 16 or src_img.get_height() != 16:
			src_img = src_img.duplicate()
			src_img.resize(16, 16, Image.INTERPOLATE_NEAREST)
		return src_img
	
	# Fallback: create a 16x16 white image
	var img := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	img.fill(Color.WHITE)
	return img

func get_texture() -> ImageTexture:
	return texture

func get_image() -> Image:
	return image

# Get the clean (noise-free) image for saving. Returns noise_base if noise is
# active, otherwise the current image. This ensures saved blocks can have noise
# re-applied or adjusted later without losing the base colors.
func get_clean_image() -> Image:
	if noise_severity > 0.0 and noise_base != null:
		return noise_base.duplicate()
	return image.duplicate()

# Reset the noise base (e.g. when loading a clean block image). This allows noise
# to be re-applied from a new base image without keeping stale state.
func reset_noise_base() -> void:
	noise_base = null
	noise_severity = 0.0
	DirAccess.remove_absolute(ProjectSettings.globalize_path(NOISE_BASE_PATH))

# Apply a whole new block texture (named load or factory default). Duplicates the
# input so later external mutations can't corrupt the shared copy.
func set_from_image(img: Image) -> void:
	if img == null:
		return
	_set_image(img.duplicate())
	_schedule_save()

func _set_image(img: Image) -> void:
	image = img
	if texture == null:
		texture = ImageTexture.create_from_image(image)
	else:
		texture.update(image)

# Paint one texel. Returns true when the pixel actually changed; callers read
# the old colour from get_image() beforehand (for undo stroke recording). While
# the grain effect is active the clean base must mirror the edit, or the next
# set_noise() redraw would silently discard it.
func set_pixel(px: int, py: int, color: Color) -> bool:
	px = clampi(px, 0, ATLAS_WIDTH - 1)
	py = clampi(py, 0, ATLAS_HEIGHT - 1)
	if image.get_pixel(px, py).is_equal_approx(color):
		return false
	image.set_pixel(px, py, color)
	texture.update(image)
	if noise_severity > 0.0 and noise_base != null:
		noise_base.set_pixel(px, py, color)
	_schedule_save()
	return true

# Bulk fill texel rectangle
func fill_rect(x0: int, y0: int, x1: int, y1: int, color: Color) -> void:
	x0 = clampi(x0, 0, ATLAS_WIDTH - 1)
	y0 = clampi(y0, 0, ATLAS_HEIGHT - 1)
	x1 = clampi(x1, 0, ATLAS_WIDTH - 1)
	y1 = clampi(y1, 0, ATLAS_HEIGHT - 1)
	if x1 < x0 or y1 < y0:
		return
	var changed := false
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			if not image.get_pixel(x, y).is_equal_approx(color):
				image.set_pixel(x, y, color)
				changed = true
	if changed:
		texture.update(image)
		_schedule_save()

# Add grayscale luminance noise to the whole texture. `base` is the clean snapshot
# to redraw from, `noise_map` a fixed 0..1 per-texel random field, and `amount`
# the strength (0..1). The same delta is applied to R/G/B so the noise stays
# monochromatic. One texture update for the whole 16x16 map.
func apply_gray_noise(base: Image, noise_map: Image, amount: float) -> void:
	if base == null or noise_map == null:
		return
	var out: Image = base.duplicate()
	if out == null:
		return
	for y in range(ATLAS_HEIGHT):
		for x in range(ATLAS_WIDTH):
			var c: Color = out.get_pixel(x, y)
			var delta := (noise_map.get_pixel(x, y).r - 0.5) * 2.0 * amount
			out.set_pixel(x, y, Color(
				clampf(c.r + delta, 0.0, 1.0),
				clampf(c.g + delta, 0.0, 1.0),
				clampf(c.b + delta, 0.0, 1.0),
				c.a))
	image = out
	texture.update(image)
	_schedule_save()

# Set live noise severity (0..100). The effect always redraws from the clean
# base snapshot taken on the first positive value, so repeated slider changes
# never compound. Going (or restarting) to 0 restores the base and forgets it.
func set_noise(severity: float) -> void:
	severity = clampf(severity, 0.0, 100.0)
	if severity <= 0.0:
		# Restore the clean base. Within a session it is kept in memory; after a
		# restart it must be pulled back from the snapshot file that was saved
		# while the noise was active.
		var base := noise_base
		noise_base = null
		if base == null:
			base = _try_load_noise_base()
		if base != null:
			_set_image(base.duplicate())
			DirAccess.remove_absolute(ProjectSettings.globalize_path(NOISE_BASE_PATH))
		noise_severity = 0.0
		_schedule_save()
		return
	if noise_base == null:
		noise_base = _try_load_noise_base()
		if noise_base == null:
			noise_base = image.duplicate()
		# Persist the snapshot so a later game start can still revert the noise
		# it finds baked into current_block.png.
		noise_base.save_png(NOISE_BASE_PATH)
	apply_gray_noise(noise_base, _noise_map, severity / 100.0 * MAX_GRAIN)
	noise_severity = severity

func _make_noise_map() -> Image:
	var img := Image.create(ATLAS_WIDTH, ATLAS_HEIGHT, false, Image.FORMAT_RGBA8)
	var rng := RandomNumberGenerator.new()
	rng.seed = NOISE_SEED
	for y in range(ATLAS_HEIGHT):
		for x in range(ATLAS_WIDTH):
			img.set_pixel(x, y, Color(rng.randf(), 0.0, 0.0, 1.0))
	return img

func _try_load_noise_base() -> Image:
	if FileAccess.file_exists(NOISE_BASE_PATH):
		var img := Image.load_from_file(NOISE_BASE_PATH)
		if img != null:
			return img
	return null

# Bulk fill the axis-aligned texel rectangle between `lo` and `hi` (the two UV
# corners, a min/max pair) with one colour and a single texture update (used by
# the box-fill tool). Texels are picked by CENTRE position so clamping the rect
# to a face boundary never bleeds a neighbouring texel.
func fill_uv_rect(lo: Vector2, hi: Vector2, color: Color) -> void:
	var x0 := clampi(int(ceil(lo.x * ATLAS_WIDTH - 0.5)), 0, ATLAS_WIDTH - 1)
	var x1 := clampi(int(ceil(hi.x * ATLAS_WIDTH - 0.5)) - 1, 0, ATLAS_WIDTH - 1)
	var y0 := clampi(int(ceil(lo.y * ATLAS_HEIGHT - 0.5)), 0, ATLAS_HEIGHT - 1)
	var y1 := clampi(int(ceil(hi.y * ATLAS_HEIGHT - 0.5)) - 1, 0, ATLAS_HEIGHT - 1)
	if x1 < x0 or y1 < y0:
		return
	var changed := false
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			if not image.get_pixel(x, y).is_equal_approx(color):
				image.set_pixel(x, y, color)
				if noise_severity > 0.0 and noise_base != null:
					noise_base.set_pixel(x, y, color)
				changed = true
	if changed:
		texture.update(image)
		_schedule_save()

var _save_timer: Timer

func _schedule_save() -> void:
	if _save_timer == null:
		_save_timer = Timer.new()
		_save_timer.one_shot = true
		add_child(_save_timer)
		_save_timer.timeout.connect(_save_current)
	_save_timer.start(0.5)

func _save_current() -> void:
	image.save_png(CURRENT_BLOCK_PATH)
	# Keep the restart-recovery snapshot current too: pixel edits made under the
	# grain are mirrored into noise_base, so persist that alongside the block.
	if noise_severity > 0.0 and noise_base != null:
		noise_base.save_png(NOISE_BASE_PATH)
