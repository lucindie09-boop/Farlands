extends RefCounted
## The dice for what a mob carries, kept out of the body so a chance can be checked
## without luck.
##
## `roll_offhand` takes the roll instead of making it. That is the whole point: a probe
## can hold both sides of the boundary (0.2499 carries, 0.25 does not) and read the rate
## off a few thousand calls, which is the same shape `tests/test_loot_table.cpp` gives the
## loot tables. The body still rolls `randf()` ONCE and keeps the answer -- the helper has
## no memory and no state.
##
## Ids are resolved through `BlockTextures.get_block_id_by_name`, the registry the rest of
## the game asks for an id by name, and the answer is cached: names do not change while the
## game runs, and this is read once per body at spawn.

static var _ids: Dictionary = {}


## The id the offhand holds for one roll, or 0 for an empty hand. `chance` is the share of
## rolls that carry; a roll of exactly `chance` is on the empty side, so the chance is the
## open end of the interval [0, chance).
static func roll_offhand(item_name: String, chance: float, roll: float) -> int:
	if item_name.is_empty() or chance <= 0.0:
		return 0
	if roll >= clampf(chance, 0.0, 1.0):
		return 0
	return item_id(item_name)


## The id an item name resolves to, cached. 0 when the name is not in the registry -- the
## lookup itself answers -1, and a body carrying -1 would drop an id nothing can spawn, so
## an unknown name is an empty hand rather than a negative one.
static func item_id(item_name: String) -> int:
	if not _ids.has(item_name):
		_ids[item_name] = maxi(int(BlockTextures.get_block_id_by_name(item_name)), 0)
	return int(_ids[item_name])
