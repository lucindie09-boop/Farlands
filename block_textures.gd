class_name BlockTextures
extends RefCounted
# Shared block/item->icon texture lookup built from data/block_definitions.json
# (the C++ BlockRegistry's single source of truth, where array index = block id)
# plus data/items.json for non-placeable items (ids start at ITEM_ID_BASE).
# Block icons use the side face (index 0): every block's four side faces are
# identical in the current data, so any of the 4 works and index 0 is simplest.

const BLOCK_DEFINITIONS_PATH = "res://data/block_definitions.json"
const ITEMS_PATH = "res://data/items.json"
const ITEM_ID_BASE = 1024
const SIDE_FACE_INDEX = 0

static var _loaded := false
static var _side_texture_names: Array = []
static var _texture_cache: Dictionary = {}
static var _name_to_id: Dictionary = {}
static var _block_names: PackedStringArray = PackedStringArray()
static var _hidden_ids: Dictionary = {}
static var _item_texture_names: PackedStringArray = PackedStringArray()

static func _ensure_loaded() -> void:
	if _loaded:
		return
	_loaded = true
	var text = FileAccess.get_file_as_string(BLOCK_DEFINITIONS_PATH)
	var parsed = JSON.parse_string(text)
	if parsed is Array:
		for i in range(parsed.size()):
			var block = parsed[i]
			var name := ""
			if block.has("textures"):
				var textures = block["textures"]
				if textures is Array and textures.size() > SIDE_FACE_INDEX:
					name = textures[SIDE_FACE_INDEX]
			_side_texture_names.append(name)
			if block.has("name"):
				var block_name := str(block["name"])
				_name_to_id[block_name.to_lower()] = i
				var hidden: bool = block.get("hidden", false) == true
				if hidden:
					_hidden_ids[i] = true
				if i > 0 and not hidden:
					_block_names.append(block_name)
	var items_text = FileAccess.get_file_as_string(ITEMS_PATH)
	var items_parsed = JSON.parse_string(items_text)
	if items_parsed is Dictionary and items_parsed.has("items"):
		for entry in items_parsed["items"]:
			if entry is Dictionary and entry.has("name"):
				var item_name := str(entry["name"])
				var index := _item_texture_names.size()
				_name_to_id[item_name.to_lower()] = ITEM_ID_BASE + index
				_item_texture_names.append(str(entry.get("texture", item_name)))
				_block_names.append(item_name)

static func is_item(block_id: int) -> bool:
	return block_id >= ITEM_ID_BASE and block_id - ITEM_ID_BASE < _item_texture_names.size()

static func get_side_texture_name(block_id: int) -> String:
	_ensure_loaded()
	if is_item(block_id):
		return _item_texture_names[block_id - ITEM_ID_BASE]
	if block_id >= 0 and block_id < _side_texture_names.size():
		return _side_texture_names[block_id]
	return ""

static func get_block_id_by_name(block_name: String) -> int:
	_ensure_loaded()
	return int(_name_to_id.get(block_name.to_lower(), -1))

static func get_block_names() -> PackedStringArray:
	_ensure_loaded()
	return _block_names

static func is_hidden(block_id: int) -> bool:
	_ensure_loaded()
	return _hidden_ids.has(block_id)

static func _load_texture(path: String) -> Texture2D:
	if ResourceLoader.exists(path):
		return load(path)
	# Newly added PNGs may not have gone through the editor import step yet
	# (no .import/.ctex), so ResourceLoader can't see them. Read the file
	# directly instead — same path pack textures outside res:// already take.
	var img := Image.load_from_file(path)
	return ImageTexture.create_from_image(img) if img else null

static func get_texture(block_id: int) -> Texture2D:
	_ensure_loaded()
	if _texture_cache.has(block_id):
		return _texture_cache[block_id]
	var texture: Texture2D = null
	var name := get_side_texture_name(block_id)
	if not name.is_empty():
		# Items resolve straight to textures/items/ — they are not part of
		# the block texture array, and the pack resolver's built-in fallback
		# would otherwise return stone.png for any unknown name.
		var path := "res://textures/items/" + name + ".png" if is_item(block_id) else "res://textures/blocks/" + name + ".png"
		if not is_item(block_id):
			var player = _player_controller()
			if player and player.has_method("resolve_texture_path"):
				path = player.resolve_texture_path(name)
		texture = _load_texture(path)
	_texture_cache[block_id] = texture
	return texture

static func _player_controller() -> Node:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree:
		return null
	return tree.root.get_node_or_null("/root/Main/Player")

static func invalidate_cache() -> void:
	_texture_cache.clear()
