extends RefCounted

## What an item is called on screen.
##
## The registry's own name, title-cased for a player: `oak_planks` reads "Oak
## Planks". One place, because the inventory's tooltip and the hotbar's held-item
## label have to agree word for word.
##
## Items are NOT blocks: they have their own registry, and their ids start above
## the block range. `ChunkManager.get_block_name` only knows the block registry, so
## it answers "air" for a torch or a sword - which is what used to be drawn as
## their name. `BlockTextures` is the one surface that knows both, and it only
## offers `get_block_names` (blocks then items, in id order) and
## `get_block_id_by_name`, so the id -> name table here is built by asking each
## listed name for its own id. That covers both registries with no second lookup
## and no rebuild of the extension.

const CHUNK_MANAGER_PATH := "/root/Main/ChunkManager"

## id -> registry name, built once. Both registries are loaded at startup, before
## any of this is asked for anything, so the table cannot go stale in a session.
static var _names: Dictionary = {}

## Registry name -> what to CALL it, from items.json's optional "display" field,
## built once with the table above. An item whose state is a second id (the rods'
## thrown version) is one thing to a player, so it may name the title both states
## read; without it a name is its own display name. Keyed by name because ids are
## positional.
static var _display: Dictionary = {}

## The display name of `block_id`, or "" when there is no item there. An id no
## registry knows still gets a name, so a new block shows as "block 42" rather
## than as nothing.
##
## `from` is any node in the game's tree - the caller is the natural one - because
## the fallback lookup is a node path, which only a Node can follow (a SceneTree
## cannot).
static func display_name(from: Node, block_id: int) -> String:
	if block_id <= 0:
		return ""
	if _names.is_empty():
		_build_names()
	var raw := String(_names.get(block_id, ""))
	if raw == "":
		var manager: Node = from.get_node_or_null(CHUNK_MANAGER_PATH) if from != null else null
		if manager != null and manager.has_method("get_block_name"):
			raw = String(manager.call("get_block_name", block_id))
	if raw == "" or raw.to_lower() == "air":
		raw = "block %d" % block_id
	return title_case(String(_display.get(raw, raw)).replace("_", " "))

## Every listed name's own id, so items (above the block ids) are named too. An
## empty registry means no world is up yet - the table stays empty and the next
## call tries again rather than trusting a table built too early.
static func _build_names() -> void:
	for entry in BlockTextures.get_block_names():
		var name_text := String(entry)
		var id := int(BlockTextures.get_block_id_by_name(name_text))
		if id > 0:
			_names[id] = name_text
	_build_display_names()

## The items.json half of the table: only the entries that name a "display" of
## their own. Read here rather than through the registry because it is a naming
## concern, and the registry is C++ that has no opinion about what a player reads.
static func _build_display_names() -> void:
	var file := FileAccess.open("res://data/items.json", FileAccess.READ)
	if file == null:
		return
	var json := JSON.new()
	var ok := json.parse(file.get_as_text()) == OK
	file.close()
	if not ok or not (json.data is Dictionary):
		return
	for entry in json.data.get("items", []):
		if entry is Dictionary and entry.has("display"):
			_display[String(entry.get("name", ""))] = String(entry["display"])

## `oak planks` -> `Oak Planks`, the way an item name reads.
static func title_case(text: String) -> String:
	var out := PackedStringArray()
	for word in text.split(" ", false):
		out.append(word.substr(0, 1).to_upper() + word.substr(1))
	return " ".join(out)
