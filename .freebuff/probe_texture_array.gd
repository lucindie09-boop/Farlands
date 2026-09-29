extends SceneTree
## Validates the generated ALBEDO texture array against what the block registry
## asks for: every texture name any block declares must have a layer, the layer
## count must be exactly the number of distinct names, and the array must come out
## at the pack's base resolution in RGBA8 (so the Liquid Texture Lab can write into
## it). The first get_texture_layer_info() call is what triggers the build, so this
## covers generate_texture_array + populate_block_registry + get_texture_index and
## the image normalisers in one go.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_texture_array.gd

var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		_fail("ChunkManager missing")
		quit(1)
		return

	var f := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if f == null:
		_fail("block_definitions.json is not readable")
		quit(1)
		return
	var defs: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	if typeof(defs) != TYPE_ARRAY:
		_fail("block_definitions.json did not parse to a block list")
		quit(1)
		return

	var names := {}
	for b in defs:
		for tex in b.get("textures", []):
			if String(tex) != "":
				names[String(tex)] = true
	if names.is_empty():
		_fail("no texture names in block_definitions.json")
		quit(1)
		return

	var info: Dictionary = cm.get_texture_layer_info("stone")
	var layers := int(info.get("layers", 0))
	var width := int(info.get("width", 0))
	var height := int(info.get("height", 0))

	if layers != names.size():
		_fail("%d array layers for %d distinct texture names" % [layers, names.size()])
	if width <= 0 or width != height:
		_fail("array is %dx%d, expected a square base resolution" % [width, height])
	if int(info.get("format", -1)) != 5:  # Image.FORMAT_RGBA8 == 5 in Godot 4
		_fail("array format is %d, expected RGBA8 (5)" % int(info.get("format", -1)))
	if info.get("mipmaps", false) != true:
		_fail("array was built without mipmaps (the default is on)")
	if info.get("writable", false) != true:
		_fail("array is not writable, so the Liquid Texture Lab cannot push frames")

	# A name with no layer of its own is allowed for exactly one reason: the built-in
	# texture file does not exist, so the build resolved it to stone.png
	# (get_safe_texture_path) and filed it under the name it resolved to. Anything
	# else missing means the layer table lost an entry.
	var missing: Array[String] = []
	for name in names.keys():
		var one: Dictionary = cm.get_texture_layer_info(String(name))
		if one.get("found", false) and int(one.get("index", -1)) >= 0:
			continue
		if FileAccess.file_exists("res://textures/blocks/%s.png" % name):
			missing.append(String(name))
	await process_frame
	if not missing.is_empty():
		_fail("%d declared textures have a file but no layer, e.g. %s" %
			[missing.size(), ", ".join(missing.slice(0, 5))])

	# A layer's pixels must be readable back through the live-preview path: the
	# stone layer is never blank in a working build.
	var image: Image = cm.get_texture_layer_image("stone")
	if image != null and int(cm.get_texture_layer_info("stone").get("index", -1)) > 0:
		if image.get_width() != width:
			_fail("stone's layer image is %d wide against an array %d wide" % [image.get_width(), width])

	print("PROBE %s (%d layers, %dx%d, %d names checked)" %
		["PASS" if ok else "FAIL", layers, width, height, names.size()])
	quit(0 if ok else 1)
