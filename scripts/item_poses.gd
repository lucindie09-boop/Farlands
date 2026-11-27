extends RefCounted
## The ONE reader of `data/items.json`'s optional `pose` field.
##
## An item's pose is a claim about its ART, not an item mechanic: which resting position the
## first-person viewmodel draws it with, and how the third-person hand turns and seats it.
## That is why it lives outside `ItemRegistry` — and why it is read here, once, for both
## consumers rather than twice, so the rule cannot drift between the two hands that obey it.
##
## Consumers: `scripts/viewmodel.gd` (the resting position a held item renders with) and
## `scripts/player_model.gd` (the `item2` turn and seat in the third-person hand).
##
## Entries are matched by NAME, not by id: item ids are positional (`FIRST_ITEM_ID + index`),
## so they shift whenever an entry is inserted rather than appended. The pose is upper-cased
## here because both consumers compare it that way (`"item2"` in the file is `ITEM2` to them).

const ITEMS_JSON := "res://data/items.json"

## id -> {"name": String, "pose": String}. The file does not change while the game runs, so it
## is read on first use and kept.
static var _entries := {}
static var _loaded := false


## Every entry that names a pose, keyed by its resolved item id.
static func by_id() -> Dictionary:
	if not _loaded:
		_load()
	return _entries


## The pose an id names, upper-cased, or "" for an item that names none (or for a block).
static func pose_for(id: int) -> String:
	var entry: Dictionary = by_id().get(id, {})
	return String(entry.get("pose", ""))


static func _load() -> void:
	_loaded = true
	var file := FileAccess.open(ITEMS_JSON, FileAccess.READ)
	if file == null:
		return
	var text := file.get_as_text()
	file.close()
	var json := JSON.new()
	if json.parse(text) != OK:
		print("Failed to parse items.json: " + json.get_error_message())
		return
	if not (json.data is Dictionary):
		return
	var items: Array = (json.data as Dictionary).get("items", [])
	for entry in items:
		var pose := String(entry.get("pose", "")).to_upper()
		if pose.is_empty():
			continue
		var name := String(entry.get("name", ""))
		var id := BlockTextures.get_block_id_by_name(name)
		if id > 0:
			_entries[id] = {"name": name, "pose": pose}
