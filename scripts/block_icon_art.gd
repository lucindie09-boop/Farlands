extends RefCounted

# The art a block icon is drawn from, handed to the shatter as pixels: which
# texels of it are ink, and what colour each of them is. An icon is sampled down
# to the 16 units it is actually drawn at -- one art texel per drawn texel at GUI
# scale 1 -- before anything is thrown, so a shard is a whole block of the icon
# rather than a piece of a 300-unit render. Cached per block id: the art does not
# change while the game runs.
#
# The hotbar, the inventory and the crafting table all come apart through this, so
# an icon shatters the same way whichever of them it was spent in.

const UIShatter := preload("res://scripts/ui_shatter.gd")

const ART := 16  # one art texel per drawn unit, on an icon's 16-unit box

static var _cache: Dictionary = {}


## `block_id`'s icon as pixels: `mask` is 1 per texel that draws anything and
## `colours` is what that texel draws, both on the ART x ART grid (row-major,
## which is the grid `UIShatter.burst()` takes).
static func pixels(block_id: int) -> Dictionary:
	if not _cache.has(block_id):
		_cache[block_id] = _build(block_id)
	return _cache[block_id]


## Where a slot's icon sits, given the slot's own rect: the ART-unit box centred
## in it, which is the one layout every surface draws icons at.
static func icon_rect(slot: Rect2, ui_scale: float) -> Rect2:
	var size := ART * ui_scale
	return Rect2(slot.position + (slot.size - Vector2(size, size)) * 0.5, Vector2(size, size))


## The icon a slot draws for `block_id` -- the isometric render where the renderer
## has one, the block's own texture otherwise -- or null where the fallback is a
## plain rectangle with no art of its own to come apart.
static func texture(block_id: int) -> Texture2D:
	var tree := Engine.get_main_loop() as SceneTree
	if tree != null:
		var renderer := tree.root.get_node_or_null("/root/BlockIconRenderer")
		if renderer != null:
			var icon = renderer.get_block_icon(block_id)
			if icon:
				return icon
	return BlockTextures.get_texture(block_id)


static func _build(block_id: int) -> Dictionary:
	var art := Vector2i(ART, ART)
	var mask := PackedByteArray()
	var colours := PackedColorArray()
	var tex := texture(block_id)
	if tex != null:
		# Every texel the icon draws anything on is ink; the empty corners around a
		# block's silhouette are what it does not then come apart into.
		mask = UIShatter.mask_from_texture(tex, art, func(px: Color): return px.a > 0.5)
		colours = UIShatter.colours_from_texture(tex, art)
	return {"art": art, "mask": mask, "colours": colours}
