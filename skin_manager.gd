extends Node

# Owns the current player skin for the whole session. The skin-maker preview
# and the in-game player model both point their albedo at `texture`, so a paint
# edit updates both live. The working skin is also persisted to
# user://current_skin.png (debounced) so edits survive a game restart; the
# named SAVE/LOAD buttons work the same way on explicit files.

const DEFAULT_SKIN_PATH := "res://skin.png"
const CURRENT_SKIN_PATH := "user://current_skin.png"
# Restart-recovery snapshot of the clean pre-noise skin: while noise is active
# the reversibility base is kept on disk so the slider can still undo it in a
# fresh game session (the working image itself is saved noisy).
const NOISE_BASE_PATH := "user://skin_noise_base.png"
const ATLAS_DIM := 64

const NOISE_SEED := 20240829
const MAX_GRAIN := 0.35

var image := Image.new()
# A single persistent ImageTexture: every model's albedo points at this object
# forever, so ALL image swaps (set_from_image, noise restore, attribute updates)
# go through texture.update() and never leave a stale texture behind.
var texture: ImageTexture

# Active noise severity (0..100) plus the clean base it reverts to. These live
# HERE (not in the skin-maker page) because the settings page is rebuilt every
# time the menu opens: the autoload survives rebuilds, so the value and the
# reversibility snapshot follow the texture around the UI.
var noise_severity := 0.0
var noise_base: Image
var _noise_map: Image

var _save_timer: Timer

func _ready() -> void:
	_noise_map = _make_noise_map()
	if FileAccess.file_exists(CURRENT_SKIN_PATH):
		var prev := Image.load_from_file(CURRENT_SKIN_PATH)
		if prev != null:
			_set_image(prev)
			return
	_set_image(_default_image())

func _exit_tree() -> void:
	if not image.is_empty():
		image.save_png(CURRENT_SKIN_PATH)

func _default_image() -> Image:
	var tex := load(DEFAULT_SKIN_PATH) as Texture2D
	var img := tex.get_image() if tex != null else null
	if img == null:
		img = Image.load_from_file(DEFAULT_SKIN_PATH)
	if img == null:
		img = Image.create(ATLAS_DIM, ATLAS_DIM, false, Image.FORMAT_RGBA8)
	return img

func get_texture() -> ImageTexture:
	return texture

func get_image() -> Image:
	return image

# Get the clean (noise-free) image for saving. Returns noise_base if noise is
# active, otherwise the current image. This ensures saved skins can have noise
# re-applied or adjusted later without losing the base colors.
func get_clean_image() -> Image:
	if noise_severity > 0.0 and noise_base != null:
		return noise_base.duplicate()
	return image.duplicate()

# Reset the noise base (e.g. when loading a clean skin image). This allows noise
# to be re-applied from a new base image without keeping stale state.
func reset_noise_base() -> void:
	noise_base = null
	noise_severity = 0.0
	DirAccess.remove_absolute(ProjectSettings.globalize_path(NOISE_BASE_PATH))

# Apply a whole new skin (named load or factory default). Duplicates the input
# so later external mutations can't corrupt the shared copy.
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
	px = clampi(px, 0, ATLAS_DIM - 1)
	py = clampi(py, 0, ATLAS_DIM - 1)
	if image.get_pixel(px, py).is_equal_approx(color):
		return false
	image.set_pixel(px, py, color)
	texture.update(image)
	if noise_severity > 0.0 and noise_base != null:
		noise_base.set_pixel(px, py, color)
	_schedule_save()
	return true

# Add grayscale luminance noise to the whole skin. `base` is the clean snapshot
# to redraw from, `noise_map` a fixed 0..1 per-texel random field, and `amount`
# the strength (0..1). The same delta is applied to R/G/B so the noise stays
# monochromatic. One texture update for the whole 64x64 map.
func apply_gray_noise(base: Image, noise_map: Image, amount: float) -> void:
	if base == null or noise_map == null:
		return
	var out := SkinPixels.apply_gray_noise(base, noise_map, amount)
	if out == null:
		return
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
		# it finds baked into current_skin.png.
		noise_base.save_png(NOISE_BASE_PATH)
	apply_gray_noise(noise_base, _noise_map, severity / 100.0 * MAX_GRAIN)
	noise_severity = severity

func _make_noise_map() -> Image:
	return SkinPixels.make_noise_map(ATLAS_DIM, NOISE_SEED)

func _try_load_noise_base() -> Image:
	if FileAccess.file_exists(NOISE_BASE_PATH):
		var img := Image.load_from_file(NOISE_BASE_PATH)
		if img != null:
			return img
	return null

# Bulk fill the axis-aligned texel rectangle between `lo` and `hi` (the two UV
# corners, a min/max pair) with one colour and a single texture update (used by
# the box-fill tool). Texels are picked by CENTRE position so clamping the rect
# to a face boundary never bleeds a column into the neighbouring island.
func fill_uv_rect(lo: Vector2, hi: Vector2, color: Color) -> void:
	var bounds := SkinPixels.uv_texel_bounds(lo.x, lo.y, hi.x, hi.y, ATLAS_DIM)
	var x0 := bounds[0]
	var y0 := bounds[1]
	var x1 := bounds[2]
	var y1 := bounds[3]
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

func _schedule_save() -> void:
	if _save_timer == null:
		_save_timer = Timer.new()
		_save_timer.one_shot = true
		add_child(_save_timer)
		_save_timer.timeout.connect(_save_current)
	_save_timer.start(0.5)

func _save_current() -> void:
	image.save_png(CURRENT_SKIN_PATH)
	# Keep the restart-recovery snapshot current too: pixel edits made under the
	# grain are mirrored into noise_base, so persist that alongside the skin.
	if noise_severity > 0.0 and noise_base != null:
		noise_base.save_png(NOISE_BASE_PATH)