extends SceneTree
## Bakes a static block texture for a liquid from the generator's own preset.
##
##   godot --headless --path . --script res://tools/bake_liquid_textures.gd -- [style ...]
##
## Every liquid block draws through a PNG (textures/blocks/<name>.png), which is
## what the texture array is built from, and the LiquidAnimator only replaces
## that layer's *contents* at runtime. So the PNG is what you see for the first
## frames of a session, what the array falls back to, and what `restore` puts
## back when animation is switched off — it has to exist and it has to look like
## the liquid.
##
## Rather than hand-painting one, this takes the frame a preset has settled into
## a quarter of the way through its strip (past the warm-up, before the loop's
## arrival tail), so the baked still and the animation are the same liquid.
## Defaults to lava and acid: water.png is a hand-authored texture and is left
## alone unless it is named explicitly.
##
## Writing into res:// only works in a project run from source (not an export),
## which is all this is for. Godot has to re-import the PNGs afterwards; the
## usual `godot --headless --path . --import` does that.

const OUTPUT_DIR := "res://textures/blocks"
const DEFAULT_STYLES := ["lava", "acid"]

func _init() -> void:
	var styles: Array = OS.get_cmdline_user_args()
	if styles.is_empty():
		styles = DEFAULT_STYLES
	var failures := 0
	for style in styles:
		if not _bake(String(style)):
			failures += 1
	print("bake_liquid_textures: %d baked, %d failed" % [styles.size() - failures, failures])
	quit(1 if failures > 0 else 0)

func _bake(style: String) -> bool:
	var settings: Dictionary = LiquidTextureGen.default_settings(style, 16)
	var strip: Image = LiquidTextureGen.generate_strip(settings)
	if strip == null or strip.get_width() <= 0 or strip.get_height() <= 0:
		print("bake_liquid_textures: %s produced no strip" % style)
		return false
	var size := strip.get_width()
	var frames := int(strip.get_height() / size)
	if frames <= 0:
		print("bake_liquid_textures: %s produced no frames" % style)
		return false
	# A quarter in: the automaton has settled by then, and on a looping strip it
	# is still far from the arriving tail that repeats the first frame.
	var frame := strip.get_region(Rect2i(0, (frames / 4) * size, size, size))
	var path := "%s/%s.png" % [OUTPUT_DIR, style]
	var error := frame.save_png(path)
	if error != OK:
		print("bake_liquid_textures: could not write %s (%s)" % [path, error])
		return false
	print("bake_liquid_textures: %s -> %s (%dx%d, frame %d of %d)" % [style, path, size, size, frames / 4, frames])
	return true
