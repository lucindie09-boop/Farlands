extends Node

# Manages block texture creation for the block maker tool.
# Similar to SkinManager but for block textures on cubes.

const DEFAULT_BLOCK_PATH := "res://textures/blocks/stone.png"
const CURRENT_BLOCK_PATH := "user://current_block.png"
const ATLAS_DIM := 16  # Block textures are typically 16x16

var image := Image.new()
var texture: ImageTexture

func _ready() -> void:
	if FileAccess.file_exists(CURRENT_BLOCK_PATH):
		var prev := Image.load_from_file(CURRENT_BLOCK_PATH)
		if prev != null:
			_set_image(prev)
			return
	_set_image(_default_image())

func _exit_tree() -> void:
	if not image.is_empty():
		image.save_png(CURRENT_BLOCK_PATH)

func _default_image() -> Image:
	var tex := load(DEFAULT_BLOCK_PATH) as Texture2D
	var img := tex.get_image() if tex != null else null
	if img == null:
		img = Image.load_from_file(DEFAULT_BLOCK_PATH)
	if img == null:
		img = Image.create(ATLAS_DIM, ATLAS_DIM, false, Image.FORMAT_RGBA8)
	return img

func get_texture() -> ImageTexture:
	return texture

func get_image() -> Image:
	return image

func set_from_image(img: Image) -> void:
	if img == null:
		return
	_set_image(img.duplicate())

func _set_image(img: Image) -> void:
	image = img
	if texture == null:
		texture = ImageTexture.create_from_image(image)
	else:
		texture.update(image)

# Paint one texel on the block texture
func set_pixel(px: int, py: int, color: Color) -> bool:
	px = clampi(px, 0, ATLAS_DIM - 1)
	py = clampi(py, 0, ATLAS_DIM - 1)
	if image.get_pixel(px, py).is_equal_approx(color):
		return false
	image.set_pixel(px, py, color)
	texture.update(image)
	_schedule_save()
	return true

# Bulk fill texel rectangle
func fill_rect(x0: int, y0: int, x1: int, y1: int, color: Color) -> void:
	x0 = clampi(x0, 0, ATLAS_DIM - 1)
	y0 = clampi(y0, 0, ATLAS_DIM - 1)
	x1 = clampi(x1, 0, ATLAS_DIM - 1)
	y1 = clampi(y1, 0, ATLAS_DIM - 1)
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
