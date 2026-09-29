extends SceneTree
## Dumps every block-registry field that is visible from GDScript, one line per
## block, in block_definitions.json order. Used as a before/after fingerprint of
## the loader: run it, refactor BlockRegistry::load_from_json, rebuild, run it
## again and diff the two files. Anything the loader parses (name interning and
## registration order, the hidden flag, texture names, shape resolution and the
## slab/stair/wall family tables, which show up as hidden placement variants)
## changes a line here.
##
## Run: Godot --headless --path <project> --script res://probes/probe_registry_digest.gd -- <out path>

var _lines: Array[String] = []

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("DIGEST FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("DIGEST FAIL: ChunkManager missing")
		quit(1)
		return

	var f := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if f == null:
		print("DIGEST FAIL: data/block_definitions.json is not readable")
		quit(1)
		return
	var defs: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	if typeof(defs) != TYPE_ARRAY:
		print("DIGEST FAIL: block_definitions.json did not parse to a block list")
		quit(1)
		return

	var blocks: Array = defs
	_lines.append("blocks in file: %d" % blocks.size())
	for i in range(blocks.size()):
		var b: Dictionary = blocks[i]
		var name := String(b.get("name", ""))
		var id := BlockTextures.get_block_id_by_name(name)
		var parts := ["%4d %-28s" % [i, name], "id=%d" % id]
		if id > 0:
			parts.append("hidden=%s" % BlockTextures.is_hidden(id))
			parts.append("side=%s" % BlockTextures.get_side_texture_name(id))
			parts.append("item=%s" % BlockTextures.is_item(id))
			var live: Array = cm.get_selection_boxes(id)
			parts.append("boxes=%d" % live.size())
			for box in live:
				var packed := PackedFloat32Array(box)
				var flat: Array[String] = []
				for v in packed:
					flat.append("%.4f" % v)
				parts.append("[" + ",".join(flat) + "]")
			parts.append("name_in_registry=%s" % cm.get_block_name(id))
		else:
			parts.append("UNRESOLVED")
		_lines.append(" ".join(parts))

	_lines.append("visible names: %d" % BlockTextures.get_block_names().size())
	_lines.append("DIGEST DONE")

	var out_path := "user://registry_digest.txt"
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out_path = args[0]
	var out := FileAccess.open(out_path, FileAccess.WRITE)
	if out == null:
		print("DIGEST FAIL: cannot write %s" % out_path)
		quit(1)
		return
	for line in _lines:
		out.store_line(line)
	out.close()
	print("DIGEST wrote %d lines to %s" % [_lines.size(), out_path])
	quit(0)
