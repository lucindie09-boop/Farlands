class_name BlockTextures
extends RefCounted
# Shared block->icon texture lookup built from data/block_definitions.json
# (the C++ BlockRegistry's single source of truth, where array index = block id).
# Icons use the side face (index 0): every block's four side faces are identical
# in the current data, so any of the 4 works and index 0 is simplest.

const BLOCK_DEFINITIONS_PATH = "res://data/block_definitions.json"
const SIDE_FACE_INDEX = 0

static var _loaded := false
static var _side_texture_names: Array = []
static var _texture_cache: Dictionary = {}
static var _name_to_id: Dictionary = {}
static var _block_names: PackedStringArray = PackedStringArray()
static var _hidden_ids: Dictionary = {}

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

static func get_side_texture_name(block_id: int) -> String:
	_ensure_loaded()
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

static func get_texture(block_id: int) -> Texture2D:
	_ensure_loaded()
	if _texture_cache.has(block_id):
		return _texture_cache[block_id]
	var texture: Texture2D = null
	var name := get_side_texture_name(block_id)
	if not name.is_empty():
		var path := "res://textures/blocks/" + name + ".png"
		var player = _player_controller()
		if player and player.has_method("resolve_texture_path"):
			path = player.resolve_texture_path(name)
		if path.begins_with("user://"):
			# Pack textures live outside the import system: load the raw PNG
			# directly and wrap it, same as the C++ side does for the array.
			var img := Image.load_from_file(path)
			if img:
				texture = ImageTexture.create_from_image(img)
		elif ResourceLoader.exists(path):
			texture = load(path)
	_texture_cache[block_id] = texture
	return texture

static func _player_controller() -> Node:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree:
		return null
	return tree.root.get_node_or_null("/root/Main/Player")

static func invalidate_cache() -> void:
	_texture_cache.clear()
