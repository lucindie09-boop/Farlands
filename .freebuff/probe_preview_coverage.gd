extends SceneTree
## What does a capped preview actually leave out?
##
## The ghost thins a large build out — 20,000 cells drawn for a 193,000-cell one —
## and the thinning walks the plan's CELL LIST. The plan is ordered by chunk (a
## paste writes it in chunk batches), so whether that starves chunks is a question
## of arithmetic on real data, not of reasoning about averages: it keeps stride 20
## out of a chunk of 30 cells and NOTHING out of a chunk of 6 whenever the global
## index does not land inside its run.
##
## `preview_cells: 0` lifts the cap, so this compares the whole plan against the
## capped preview spatially, per 16-block chunk:
##   * chunks the full plan fills but the preview leaves completely empty
##   * cells lost in those chunks
##   * the spread of kept/planned per chunk
## plus an ASCII map of the empty chunks, so a missing patch shows up as a shape.
##
##   "/c/Users/lucin/Documents/Developement/Godot/Godot_v4.7.1-stable_win64_console.exe" \
##     --headless --path . --script res://.freebuff/probe_preview_coverage.gd
##
## Read-only: it never writes a block, so it does not need the world snapshot that
## run_probe.sh wraps a probe in.

const FILES := ["10179.schematic", "church.schematic"]

func _initialize() -> void:
	_run()

func _chunk_key(x: int, y: int, z: int) -> Vector3i:
	# The engine's own chunk width, not 16: keying at 16 measures sub-chunks, where
	# a bucket holding one or two cells of a build is empty for honest reasons.
	return Vector3i(x >> 5, y >> 5, z >> 5)

func _counts(cells: PackedByteArray) -> Dictionary:
	var counts := {}
	var at := 0
	while at + 16 <= cells.size():
		var key := _chunk_key(cells.decode_s32(at), cells.decode_s32(at + 4), cells.decode_s32(at + 8))
		counts[key] = int(counts.get(key, 0)) + 1
		at += 16
	return counts

func _report(main: Node, file: String) -> void:
	var path := "res://schematics/%s" % file
	if not FileAccess.file_exists(path):
		print("PROBE FAIL: no %s" % path)
		return
	var bytes := FileAccess.get_file_as_bytes(path)
	print("PROBE ==== %s (%d bytes) ====" % [file, bytes.size()])

	var manager: Node = main.get_node_or_null("ChunkManager")
	if manager == null:
		print("PROBE FAIL: no ChunkManager in the scene")
		return

	var start := Time.get_ticks_msec()
	var full: Dictionary = manager.preview_schematic(bytes, 0, 0, 0, {"preview_cells": 0})
	if not bool(full.get("ok", false)):
		print("PROBE FAIL: full preview refused: %s" % str(full.get("error", "?")))
		return
	var all_cells: PackedByteArray = full.get("cells", PackedByteArray())
	var planned := int(full.get("cells_returned", 0))
	print("PROBE full plan cells=%d in %d ms (capped=%s)" % [
		planned, Time.get_ticks_msec() - start, str(full.get("cells_sampled"))])

	var capped: Dictionary = manager.preview_schematic(bytes, 0, 0, 0, {"preview_cells": 20000})
	if not bool(capped.get("ok", false)):
		print("PROBE FAIL: capped preview refused: %s" % str(capped.get("error", "?")))
		return
	var kept_cells: PackedByteArray = capped.get("cells", PackedByteArray())
	var instances: PackedFloat32Array = capped.get("instances", PackedFloat32Array())
	print("PROBE capped preview cells=%d capped=%s instances=%d floats (12/cell expected %d)" % [
		int(capped.get("cells_returned", 0)), str(capped.get("cells_sampled")),
		instances.size(), int(capped.get("cells_returned", 0)) * 12])

	var full_counts := _counts(all_cells)
	var kept_counts := _counts(kept_cells)

	# The wand's own cap, so the density the game actually previews is measured.
	var shipped: Dictionary = manager.preview_schematic(bytes, 0, 0, 0, {"preview_cells": 150000})
	if bool(shipped.get("ok", false)):
		var shipped_counts := _counts(shipped.get("cells", PackedByteArray()))
		var shipped_empty := 0
		for key in full_counts:
			if not shipped_counts.has(key):
				shipped_empty += 1
		print("PROBE at the wand's 150000: cells=%d (%.1f%% of the build), chunks left empty=%d of %d" % [
			int(shipped.get("cells_returned", 0)),
			100.0 * float(int(shipped.get("cells_returned", 0))) / float(maxi(planned, 1)),
			shipped_empty, full_counts.size()])
	var empty := 0
	var empty_cells := 0
	var worst := Vector3i.ZERO
	var have_worst := false
	var worst_cells := 0
	var sizes := []
	for key in full_counts:
		var total := int(full_counts[key])
		var got := int(kept_counts.get(key, 0))
		sizes.append(total)
		if got == 0:
			empty += 1
			empty_cells += total
			if not have_worst or total > worst_cells:
				worst = key
				worst_cells = total
				have_worst = true
	print("PROBE chunks: occupied=%d  preview-empty=%d  cells lost in empty chunks=%d" % [
		full_counts.size(), empty, empty_cells])
	sizes.sort()
	if not sizes.is_empty():
		print("PROBE cells per occupied chunk: min=%d median=%d max=%d" % [
			sizes[0], sizes[sizes.size() / 2], sizes[sizes.size() - 1]])
	if have_worst:
		print("PROBE biggest chunk left empty: %s with %d of the %d cells it holds" % [
			str(worst), int(kept_counts.get(worst, 0)), worst_cells])
	print("PROBE overall: %d of %d planned cells drawn (%.2f%%) in %d chunks" % [
		int(capped.get("cells_returned", 0)), planned,
		100.0 * float(int(capped.get("cells_returned", 0))) / float(maxi(planned, 1)), maxi(full_counts.size(), 1)])

	# ASCII map of the chunks: '#' both, '+' kept at least one, '.' full but the
	# preview dropped every cell. Rows are z, columns are x, at the busiest y.
	var lo := Vector3i(1 << 30, 1 << 30, 1 << 30)
	var hi := Vector3i(-(1 << 30), -(1 << 30), -(1 << 30))
	for key in full_counts:
		lo = lo.min(key)
		hi = hi.max(key)
	if hi.x < lo.x:
		return
	var row_counts := {}
	for key in full_counts:
		row_counts[key.y] = int(row_counts.get(key.y, 0)) + int(full_counts[key])
	var busiest_y: int = lo.y
	for y in row_counts:
		if int(row_counts[y]) > int(row_counts.get(busiest_y, 0)):
			busiest_y = int(y)
	print("PROBE map at chunk y=%d (z down, x across; #=planned+previewed +=partial .=preview dropped the chunk)" % busiest_y)
	for cz in range(lo.z, hi.z + 1):
		var line := ""
		for cx in range(lo.x, hi.x + 1):
			var key := Vector3i(cx, busiest_y, cz)
			if not full_counts.has(key):
				line += " "
			elif int(kept_counts.get(key, 0)) == 0:
				line += "."
			elif int(kept_counts[key]) >= int(full_counts[key]):
				line += "#"
			else:
				line += "+"
		print("PROBE   %s" % line)

func _run() -> void:
	var main: Node = (load("res://main.tscn") as PackedScene).instantiate()
	root.add_child(main)
	for i in 30:
		await process_frame
	for file in FILES:
		_report(main, file)
	print("PROBE done")
	quit(0)
