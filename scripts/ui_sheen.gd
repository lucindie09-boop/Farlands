extends Control
## One surface's travelling sheen - the effect itself is [shaders/ui_sheen.gdshader]. This node IS
## the surface: it draws one quad and its material paints the light, so a menu row and a card's
## frame are the same node with different uniforms.
##
## A node rather than a branch in the screen's own `_draw`, because a material belongs to a
## CanvasItem: the only way to shade one row and leave the wordmark alone is to give the row an
## item of its own. It also means the light costs nothing when it is off - `visible` takes the
## whole draw away instead of running a shader that outputs zero.
##
## One caller today, in the second of its two modes: the three right-hand cards light only along
## their 2-texel bevel (`ring`), which is the one part of a card that is an edge. The rows used to
## light across their faces and the wordmark had a label-material version of its own; both are gone
## (see `docs/gui-notes.md` for why), and the face mode is still here because it is what the shader
## is for.

# Loaded once for the whole screen: a preload here is a load at script parse, which is fine for a
# shader (unlike a script that names an autoload).
const SHEEN: Shader = preload("res://shaders/ui_sheen.gdshader")

var _material: ShaderMaterial
var _moving := true


func _init() -> void:
	# The sheen lies OVER rows the player is aiming at, so it must take neither the click nor the
	# pointer: a Control on top with the default filter would swallow both, and the menu would
	# stop responding wherever the light is.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# It draws its own quad, so it has no texture to be filtered; nearest keeps the house rule's
	# spirit but costs nothing here. The shader already works in whole pixels.
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_material = ShaderMaterial.new()
	_material.shader = SHEEN
	material = _material
	resized.connect(_sync)
	visible = false


## `amount` is how bright the light gets, `offset` where in its cycle this surface starts (five
## rows lit from one place are phase-shifted, not synchronised), `ring` is the frame width in
## pixels - 0 for a whole face - `rounded` the face's own corner radius (0 in frame mode), and
## `units` the GUI scale in pixels, because the bands are measured in interface units so their
## proportions survive a window or scale change. Every caller passes all five, so a surface can
## never keep a stale mode.
func configure(amount: float, offset: float, ring: float, rounded: float,
		units: float) -> void:
	_material.set_shader_parameter("strength", amount)
	_material.set_shader_parameter("phase", offset)
	_material.set_shader_parameter("ring_width", ring)
	_material.set_shader_parameter("corner", rounded)
	_material.set_shader_parameter("unit", units)
	_sync()


## Reduce Motion STOPS the light rather than slowing it: a sheen that still creeps is motion, and
## the setting is the one place this screen promises none. Off is invisible, not black - an
## additive pass drawing zero is still a draw.
func set_moving(on: bool) -> void:
	_moving = on
	_sync()


func _sync() -> void:
	_material.set_shader_parameter("size_px", size)
	visible = _moving and size.x > 0.0 and size.y > 0.0


func _draw() -> void:
	# White because the shader never reads COLOR: it writes the light it computed. The rect is the
	# whole node, which is what makes `VERTEX` in the shader the node's own pixel coordinates.
	draw_rect(Rect2(Vector2.ZERO, size), Color.WHITE)
