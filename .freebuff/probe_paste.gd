extends SceneTree
## Headless integration probe for block-file paste (`/paste`, ChunkManager.paste_schematic).
##
## Loads the real scene so the world streams in, then pastes a real .schematic
## into open air above the player and holds the writer to exactly what it
## reported:
##   - every planned cell is non-air afterwards, and the box holds nothing else
##   - pasting the same file again with "gaps" writes nothing (nothing to fill)
##   - undo empties the box back to air, cell for cell
##   - the chat command resolves a bare file name, parses its option words, and
##     reports a missing file rather than failing quietly
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_paste.gd

const FILE := "res://schematics/church.schematic"

# The other two builds, which are the shapes a file can arrive in and the ones
# that used to be refused: a palette-format build wearing a `.schematic` name, and
# a classic one whose nibble array is one byte longer than the packed size.
const OTHER_FILES := [
	{"path": "res://schematics/29761.schematic", "format": "palette", "padded": false},
	{"path": "res://schematics/14664.schematic", "format": "classic", "padded": true},
	{"path": "res://schematics/10179.schematic", "format": "classic", "padded": false},
]

var main: Node3D
var cm: Node
var chat: Node
var player: Node3D
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _chat_says(text: String) -> bool:
	for m in chat.messages:
		if String(m.get("text", "")).find(text) != -1:
			return true
	return false

func _initialize() -> void:
	_run()

# Topmost solid block in the column, returned as the feet position on top of it.
func _surface_point(from: Vector3) -> Vector3:
	var x := int(floor(from.x))
	var z := int(floor(from.z))
	var y := int(from.y) + 64
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(x, y + 1, z)
		y -= 1
	return Vector3.INF

# --- A build file the probe writes itself ------------------------------------
#
# A 5×5 stone floor with exactly one water cell on it, written through Godot's
# own gzip writer rather than kept as an asset: the bytes the reader is handed
# here were not produced by this project's encoder, which is the point of a test
# of the import path.
func _water_build_bytes() -> PackedByteArray:
	const W := 5
	const H := 2
	const L := 5
	var blocks := PackedByteArray()
	var data := PackedByteArray()
	for y in range(H):
		for z in range(L):
			for x in range(W):
				if x == 2 and y == 1 and z == 2:
					blocks.append(9)   # still water
				elif y == 0:
					blocks.append(1)   # stone floor
				else:
					blocks.append(0)
				data.append(0)
	var payload := PackedByteArray()
	payload.append(10)  # a compound, the classic root's own tag
	payload.append_array(_nbt_string("Schematic"))
	payload.append(2)
	payload.append_array(_nbt_string("Width"))
	payload.append_array(_be16(W))
	payload.append(2)
	payload.append_array(_nbt_string("Height"))
	payload.append_array(_be16(H))
	payload.append(2)
	payload.append_array(_nbt_string("Length"))
	payload.append_array(_be16(L))
	payload.append(7)
	payload.append_array(_nbt_string("Blocks"))
	payload.append_array(_be32(blocks.size()))
	payload.append_array(blocks)
	payload.append(7)
	payload.append_array(_nbt_string("Data"))
	payload.append_array(_be32(data.size()))
	payload.append_array(data)
	payload.append(8)
	payload.append_array(_nbt_string("Materials"))
	payload.append_array(_nbt_string("Alpha"))
	payload.append(0)  # end of the compound

	# A real gzip stream (not Godot's own compressed-file container, which the
	# reader would rightly refuse): PackedByteArray.compress wraps the payload the
	# same way every writer of these files does.
	var path := "user://probe_water.schematic"
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return PackedByteArray()
	file.store_buffer(payload.compress(FileAccess.COMPRESSION_GZIP))
	file.close()
	return FileAccess.get_file_as_bytes(path)

func _be16(value: int) -> PackedByteArray:
	return PackedByteArray([(value >> 8) & 0xFF, value & 0xFF])

func _be32(value: int) -> PackedByteArray:
	return PackedByteArray([(value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF])

func _nbt_string(text: String) -> PackedByteArray:
	var bytes := text.to_utf8_buffer()
	var out := _be16(bytes.size())
	out.append_array(bytes)
	return out

func _non_air(min_p: Vector3i, max_p: Vector3i) -> int:
	var count := 0
	for y in range(min_p.y, max_p.y + 1):
		for z in range(min_p.z, max_p.z + 1):
			for x in range(min_p.x, max_p.x + 1):
				if cm.get_block(x, y, z) != 0:
					count += 1
	return count

# Bails on the first non-air cell: the box is tens of thousands of cells and most
# candidates fail immediately.
func _box_is_air(origin: Vector3i, w: int, h: int, l: int) -> bool:
	for y in range(origin.y, origin.y + h):
		for z in range(origin.z, origin.z + l):
			for x in range(origin.x, origin.x + w):
				if cm.get_block(x, y, z) != 0:
					return false
	return true

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)

	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	chat = main.get_node_or_null("HUD/Chat")
	if cm == null or player == null or chat == null:
		_fail("ChunkManager/Player/HUD/Chat missing")
		quit(1)
		return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited < 30:
			continue
		if player.is_on_floor() and _surface_point(player.global_position) != Vector3.INF:
			break
	print("probe: player landed after %d frames at %s" % [waited, player.global_position])

	var bytes := FileAccess.get_file_as_bytes(FILE)
	if bytes.is_empty():
		_fail("cannot read " + FILE)
		quit(1)
		return
	print("probe: %s is %d bytes" % [FILE, bytes.size()])

	# The file's dimensions come from the engine rather than being hardcoded here:
	# inspection is read-only, so asking costs the world nothing.
	var probe: Dictionary = cm.inspect_schematic(bytes)
	if not probe.get("ok", false):
		_fail("inspect refused: %s" % probe.get("error", "?"))
		quit(1)
		return
	print("probe: inspect says %dx%dx%d, %d states, %d non-air cells, %s, %d planned"
		% [int(probe.get("file_width", 0)), int(probe.get("file_height", 0)),
		   int(probe.get("file_length", 0)), int(probe.get("palette_states", 0)),
		   int(probe.get("non_air_cells", 0)), probe.get("container", "?"),
		   int(probe.get("planned", 0))])
	var w := int(probe.get("file_width", 0))
	var h := int(probe.get("file_height", 0))
	var l := int(probe.get("file_length", 0))
	if w <= 0 or h <= 0 or l <= 0:
		_fail("the engine did not report the file's dimensions: %s" % probe)
		quit(1)
		return

	# Somewhere in the open air near the player, low enough that every chunk the
	# box needs is resident, and clear enough that the whole build lands in air
	# (the box is checked exactly, and the written cells are counted exactly, so
	# terrain anywhere in it would make the arithmetic meaningless). Terrain is
	# whatever the world happens to be, so the search sweeps sideways as well as
	# up rather than assuming the column above the player is open.
	var base := _surface_point(player.global_position)
	var placed := {}
	for offset in [Vector2i(0, 0), Vector2i(-18, 0), Vector2i(18, 0), Vector2i(0, -18),
			Vector2i(0, 18), Vector2i(-18, -18), Vector2i(18, 18), Vector2i(-18, 18),
			Vector2i(18, -18), Vector2i(-34, 0), Vector2i(34, 0), Vector2i(0, -34),
			Vector2i(0, 34)]:
		var spot := _surface_point(Vector3(base.x + offset.x, base.y, base.z + offset.y))
		if spot == Vector3.INF:
			continue
		for lift in [2, 6, 12, 20, 30, 40]:
			var origin := Vector3i(int(spot.x) - (w / 2), int(spot.y) + lift, int(spot.z) - (l / 2))
			if not _box_is_air(origin, w, h, l):
				continue
			var attempt: Dictionary = cm.paste_schematic(bytes, origin.x, origin.y, origin.z, {})
			if not attempt.get("ok", false):
				_fail("paste refused: %s" % attempt.get("error", "?"))
				quit(1)
				return
			if int(attempt.get("unloaded_chunks", 0)) > 0:
				print("probe: %s lift %d: %d cells in unloaded chunks, trying elsewhere"
					% [offset, lift, int(attempt.get("unloaded_chunks", 0))])
				cm.undo_paste()
				continue
			if int(attempt.get("cells", 0)) != int(attempt.get("planned", 0)):
				print("probe: %s lift %d: %d of %d planned cells written, trying elsewhere"
					% [offset, lift, int(attempt.get("cells", 0)), int(attempt.get("planned", 0))])
				cm.undo_paste()
				continue
			placed = attempt
			placed["origin"] = origin
			break
		if not placed.is_empty():
			break

	if placed.is_empty():
		_fail("no clear, loaded air above the player to paste into")
		quit(1)
		return

	var origin: Vector3i = placed["origin"]
	var min_p: Vector3i = placed["min"]
	var max_p: Vector3i = placed["max"]
	var cells := int(placed["cells"])
	var planned := int(placed["planned"])
	print("probe: pasted %d of %d planned cells at %s (box %s..%s, %d chunks)"
		% [cells, planned, origin, min_p, max_p, int(placed["chunks"])])
	print("probe: substituted=%d skipped=%d unknown=%d declined_fluid=%d air_ignored=%d"
		% [int(placed.get("substituted", 0)), int(placed.get("skipped", 0)),
		   int(placed.get("unknown", 0)), int(placed.get("declined_fluid", 0)),
		   int(placed.get("air_ignored", 0))])

	if planned < 1000:
		_fail("the file planned only %d cells; that is not the build we expect" % planned)
	# A paste that reports no chunks would mean no remesh and no relight was
	# queued: the blocks would be there and invisible until something else
	# happened to dirty the chunk.
	if int(placed.get("chunks", 0)) <= 0:
		_fail("the paste reported %d chunks touched" % int(placed.get("chunks", 0)))
	if int(placed.get("unknown", 0)) != 0:
		_fail("%d cells have no row in the translation table" % int(placed.get("unknown", 0)))
	if int(placed.get("unresolved", 0)) != 0:
		_fail("%d cells name a block this build does not have" % int(placed.get("unresolved", 0)))
	if int(placed.get("declined_fluid", 0)) != 0:
		print("probe: note: this file holds %d liquid cells (declined)" % int(placed.get("declined_fluid", 0)))

	# The strong one: every planned cell landed, and the box holds nothing else.
	var in_box := _non_air(min_p, max_p)
	if in_box != cells:
		_fail("box holds %d non-air blocks after writing %d cells" % [in_box, cells])
	else:
		print("probe: box holds exactly the %d written cells" % in_box)

	# Everything outside the written box must be untouched air.
	var outside := _non_air(min_p - Vector3i(1, 1, 1), max_p + Vector3i(1, 1, 1))
	if outside != cells:
		_fail("the paste touched cells outside its own bounds (border holds %d, expected %d)"
			% [outside, cells])

	# A second identical paste has nothing to do: every cell is either already
	# that block or (with gaps) occupied.
	var again: Dictionary = cm.paste_schematic(bytes, origin.x, origin.y, origin.z,
		{"replace_solid": false})
	if not again.get("ok", false):
		_fail("second paste refused: %s" % again.get("error", "?"))
	elif int(again.get("cells", 0)) != 0:
		_fail("a repeat paste with gaps wrote %d cells" % int(again.get("cells", 0)))
	elif int(again.get("covered", 0)) + int(again.get("unchanged", 0)) != planned:
		_fail("a repeat paste with gaps accounted for %d of %d cells"
			% [int(again.get("covered", 0)) + int(again.get("unchanged", 0)), planned])
	else:
		print("probe: repeat paste with gaps wrote nothing (%d unchanged, %d covered)"
			% [int(again.get("unchanged", 0)), int(again.get("covered", 0))])

	# Undo puts the volume back. The build was pasted into open air, so the box
	# has to come back empty.
	if int(placed.get("undo_cells", 0)) != cells:
		_fail("undo would restore %d cells, but %d were written"
			% [int(placed.get("undo_cells", 0)), cells])
	var back: Dictionary = cm.undo_paste()
	if not back.get("ok", false):
		_fail("undo refused: %s" % back.get("error", "?"))
	elif int(back.get("cells", 0)) != cells:
		_fail("undo restored %d of %d cells" % [int(back.get("cells", 0)), cells])
	var emptied := _non_air(min_p, max_p)
	if emptied != 0:
		_fail("box still holds %d non-air blocks after undo" % emptied)
	else:
		print("probe: undo emptied the box (%d cells restored)" % int(back.get("cells", 0)))

	# A second undo has nothing left to do, and says so.
	var twice: Dictionary = cm.undo_paste()
	if twice.get("ok", false):
		_fail("a second undo claimed to undo something")
	else:
		print("probe: second undo refused: %s" % twice.get("error", ""))

	# Every file in the schematics folder has to inspect and paste, whatever shape
	# it is in, and the padded array has to be reported rather than fatal.
	for spec in OTHER_FILES:
		var path: String = spec["path"]
		var file_bytes := FileAccess.get_file_as_bytes(path)
		if file_bytes.is_empty():
			_fail("cannot read " + path)
			continue
		var info: Dictionary = cm.inspect_schematic(file_bytes)
		if not info.get("ok", false):
			_fail("%s was refused: %s" % [path, info.get("error", "?")])
			continue
		var format := String(info.get("format", ""))
		if format.find(String(spec["format"])) == -1:
			_fail("%s was read as \"%s\"" % [path, format])
		if String(info.get("container", "")) != "gzip":
			_fail("%s did not sniff as gzip (got \"%s\")" % [path, info.get("container", "?")])
		if int(info.get("planned", 0)) <= 0:
			_fail("%s planned no cells" % path)
		print("probe: %s -> %s, %dx%dx%d, %d states, %d planned, unknown=%d, unresolved=%d, container=%s"
			% [path.get_file(), format, int(info.get("file_width", 0)),
			   int(info.get("file_height", 0)), int(info.get("file_length", 0)),
			   int(info.get("palette_states", 0)), int(info.get("planned", 0)),
			   int(info.get("unknown", 0)), int(info.get("unresolved", 0)),
			   info.get("container", "?")])

		var anchor := Vector3i(int(base.x), int(base.y) + 2, int(base.z))
		var pasted: Dictionary = cm.paste_schematic(file_bytes, anchor.x, anchor.y, anchor.z, {})
		if not pasted.get("ok", false):
			_fail("%s paste refused: %s" % [path, pasted.get("error", "?")])
			continue
		if int(pasted.get("chunks", 0)) <= 0:
			_fail("%s paste touched no chunks" % path)
		var file_planned := int(pasted.get("planned", 0))
		var emitted := int(pasted.get("cells", 0))
		var waiting := int(pasted.get("pending_cells", 0))
		print("probe: %s pasted %d of %d cells (%d waiting for chunks, %d chunks, format=%s)"
			% [path.get_file(), emitted, file_planned, waiting, int(pasted.get("chunks", 0)),
			   pasted.get("format", "?")])

		# Cells whose chunk was not loaded are not skipped any more: the chunks
		# are asked for and pinned, and the cells land as they generate. Waiting
		# for the job is the whole point of the check — and the accounting below
		# is what proves the wait actually placed them.
		var frames := 0
		while frames < 3000:
			await process_frame
			frames += 1
			if not bool(cm.get_pending_paste().get("active", false)):
				break
		var still: Dictionary = cm.get_pending_paste()
		if bool(still.get("active", false)):
			_fail("%s still had %d cells waiting after %d frames"
				% [path.get_file(), int(still.get("remaining_cells", 0)), frames])
			cm.undo_paste()
			continue

		var finished := {}
		if waiting > 0:
			finished = cm.take_paste_completion()
			if finished.is_empty():
				_fail("%s waited for chunks but reported no completion" % path.get_file())
				cm.undo_paste()
				continue
			if bool(finished.get("abandoned", false)):
				_fail("%s gave up with %d cells unplaced"
					% [path.get_file(), int(finished.get("leftover_cells", 0))])
				cm.undo_paste()
				continue
			print("probe: %s finished after %d frames: %d cells over %d chunks, waited %.0f ms"
				% [path.get_file(), frames, int(finished.get("cells", 0)),
				   int(finished.get("chunks", 0)), float(finished.get("waiting_ms", 0.0))])

		# Every planned cell is accounted for, once the job has had its wait:
		# written, already identical, covered by `gaps`, or left unplaced (which
		# is 0 for a job that finished). Anything else means cells were lost.
		var written_total := emitted
		var unchanged := int(pasted.get("unchanged", 0))
		var covered := int(pasted.get("covered", 0))
		if not finished.is_empty():
			written_total = int(finished.get("cells", emitted))
			unchanged = int(finished.get("unchanged", 0))
			covered = int(finished.get("covered", 0))
		var accounted := written_total + unchanged + covered + int(finished.get("leftover_cells", 0))
		if accounted != file_planned:
			_fail("%s: %d written + %d unchanged + %d covered + %d left != %d planned"
				% [path.get_file(), written_total, unchanged, covered,
				   int(finished.get("leftover_cells", 0)), file_planned])
		if waiting > 0 and written_total <= emitted:
			_fail("%s waited for chunks but wrote no more cells (%d -> %d)"
				% [path.get_file(), emitted, written_total])

		# The undo reverts exactly the cells that were written, and it can come up
		# short when a build that wide has had its outer chunks evicted since the
		# paste. The record keeps what it could not reach, so the invariant is the
		# sum: every recorded cell is either restored or still queued. That is
		# what catches a cell dropped on the floor — a partial revert is a state,
		# not a loss.
		var restored_total := 0
		var already_total := 0
		var oob_total := 0
		var reverted := {}
		for attempt in 6:
			reverted = cm.undo_paste()
			if not reverted.get("ok", false):
				_fail("%s undo refused: %s" % [path, reverted.get("error", "?")])
				break
			restored_total += int(reverted.get("cells", 0))
			already_total += int(reverted.get("unchanged", 0))
			oob_total += int(reverted.get("out_of_bounds", 0))
			if int(reverted.get("remaining", 0)) == 0:
				break
			for wait in 10:
				await process_frame
		var still_queued := int(reverted.get("remaining", 0))
		var settled := restored_total + already_total + oob_total + still_queued
		if settled != written_total:
			_fail("%s undo: %d restored + %d already + %d out of bounds + %d queued != %d written"
				% [path.get_file(), restored_total, already_total, oob_total, still_queued,
				   written_total])
		elif oob_total != 0:
			_fail("%s undo: %d of its own cells fell outside the world"
				% [path.get_file(), oob_total])
		else:
			print("probe: %s undo restored %d of %d cells (%d already back, %d queued)"
				% [path.get_file(), restored_total, written_total, already_total, still_queued])

	# --- A build's liquid lands, and by default it does not run -----------------
	# The file here is built by the probe itself rather than taken from the
	# schematics folder: a 5×5 stone floor with ONE water cell standing on it, so
	# "did the water land" and "did it stay put" are both one cell to look at.
	var water_bytes := _water_build_bytes()
	if water_bytes.is_empty():
		_fail("the probe could not write its own water build")
	else:
		var winfo: Dictionary = cm.inspect_schematic(water_bytes)
		if int(winfo.get("stilled", 0)) != 1:
			_fail("the water build planned %d still cells, expected 1 (%s)"
				% [int(winfo.get("stilled", 0)), winfo])
		elif int(winfo.get("declined_fluid", 0)) != 0:
			_fail("the water build declined %d liquid cells" % int(winfo.get("declined_fluid", 0)))
		else:
			print("probe: water build plans %d cells, %d of them still liquid"
				% [int(winfo.get("planned", 0)), int(winfo.get("stilled", 0))])

		# Somewhere empty: a 5×5×2 box of clear air near the player.
		var spot := Vector3i.ZERO
		var found_spot := false
		for offset in [Vector2i(0, 6), Vector2i(14, 0), Vector2i(-14, 0), Vector2i(0, -14),
				Vector2i(0, 14), Vector2i(14, 14), Vector2i(-14, -14)]:
			var here := _surface_point(Vector3(base.x + offset.x, base.y, base.z + offset.y))
			if here == Vector3.INF:
				continue
			for lift in [2, 8, 16]:
				var box := Vector3i(int(here.x) - 2, int(here.y) + lift, int(here.z) - 2)
				if _box_is_air(box, 5, 2, 5):
					spot = box
					found_spot = true
					break
			if found_spot:
				break
		if not found_spot:
			_fail("no clear air for the water build")
		else:
			var still_result: Dictionary = cm.paste_schematic(water_bytes, spot.x, spot.y, spot.z, {})
			var centre := Vector3i(spot.x + 2, spot.y + 1, spot.z + 2)
			var landed := int(cm.get_block(centre.x, centre.y, centre.z))
			if not still_result.get("ok", false):
				_fail("the water build was refused: %s" % still_result.get("error", "?"))
			elif int(still_result.get("stilled", 0)) != 1:
				_fail("the paste reported %d still cells, expected 1" % int(still_result.get("stilled", 0)))
			elif landed != int(cm.BLOCK_SURFACE_WATER):
				_fail("the water cell landed as block %d, expected still water (%d)"
					% [landed, int(cm.BLOCK_SURFACE_WATER)])
			else:
				# It hangs on its floor rather than falling or running: the still forms
				# declare no fluid state, so the simulation never touches them.
				var spread := 0
				for wait in 90:
					await process_frame
				# The four cells at its own level were air on both sides of the paste.
				# The cell below is the build's own floor, so it is not a spread.
				for step in [Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1)]:
					if cm.get_block(centre.x + step.x, centre.y + step.y, centre.z + step.z) != 0:
						spread += 1
				if spread != 0:
					_fail("still water spread into %d neighbouring cells" % spread)
				else:
					print("probe: pasted water landed as still water and stayed one cell")

				# The same file with `fluids` is the running kind — which is what makes
				# the still default a choice rather than a limitation.
				var live_spot := Vector3i.ZERO
				var found_live := false
				for offset in [Vector2i(0, -22), Vector2i(22, 0), Vector2i(-22, 0), Vector2i(0, 22),
						Vector2i(22, 22), Vector2i(-22, -22)]:
					var here2 := _surface_point(Vector3(base.x + offset.x, base.y, base.z + offset.y))
					if here2 == Vector3.INF:
						continue
					for lift2 in [2, 8, 16]:
						var box2 := Vector3i(int(here2.x) - 2, int(here2.y) + lift2, int(here2.z) - 2)
						if _box_is_air(box2, 5, 2, 5):
							live_spot = box2
							found_live = true
							break
					if found_live:
						break
				if not found_live:
					_fail("no clear air for the live water build")
				else:
					var live_result: Dictionary = cm.paste_schematic(
						water_bytes, live_spot.x, live_spot.y, live_spot.z, {"fluids": true})
					var live_centre := Vector3i(live_spot.x + 2, live_spot.y + 1, live_spot.z + 2)
					var live_block := int(cm.get_block(live_centre.x, live_centre.y, live_centre.z))
					if int(live_result.get("cells", 0)) == 0:
						_fail("the live water paste wrote no cells")
					elif live_block != int(cm.BLOCK_WATER):
						_fail("with fluids the water landed as block %d, expected a source (%d)"
							% [live_block, int(cm.BLOCK_WATER)])
					else:
						# And it runs: a neighbour at its own level becomes runoff.
						var ran := false
						for wait in 240:
							await process_frame
							if cm.get_block(live_centre.x + 1, live_centre.y, live_centre.z) != 0 \
									or cm.get_block(live_centre.x, live_centre.y, live_centre.z + 1) != 0:
								ran = true
								break
						if not ran:
							_fail("a live pasted source never flowed")
						else:
							print("probe: with fluids the same cell is a source and it flows")
						cm.undo_paste()
				cm.undo_paste()

	# The chat command: a bare file name, option words, and a missing file.
	chat.messages.clear()
	chat._run_command("/paste church.schematic")
	var chat_pasted := _chat_says("Pasted church.schematic")
	if not chat_pasted:
		_fail("the chat command did not report a successful paste")
		for m in chat.messages:
			print("  chat: " + String(m.get("text", "")))
	else:
		print("probe: chat paste reported success")
		for m in chat.messages:
			print("  chat: " + String(m.get("text", "")))
	chat.messages.clear()
	chat._run_command("/paste undo")
	if not _chat_says("Paste undone"):
		_fail("the chat undo did not report success")
	chat.messages.clear()
	chat._run_command("/paste nope.schematic")
	if not _chat_says("No such file"):
		_fail("a missing file did not produce a clear error")
	chat.messages.clear()
	chat._run_command("/paste church.schematic nonsense")
	if not _chat_says("Unknown option"):
		_fail("an unknown option was accepted")
	# The folder is discoverable without knowing a file name, and a name listed by
	# it is one the paste path can actually resolve.
	chat.messages.clear()
	chat._run_command("/paste list")
	var listed := _chat_says("church.schematic") and _chat_says("29761.schematic") \
		and _chat_says("14664.schematic")
	if not listed:
		_fail("the build files in the schematics folder were not listed")
		for m in chat.messages:
			print("  chat: " + String(m.get("text", "")))
	else:
		print("probe: /paste list found the folder's builds")

	print("PROBE %s" % ("PASS" if ok else "FAIL"))
	quit(0 if ok else 1)
