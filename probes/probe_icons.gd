extends SceneTree
## A fingerprint of the rendered block icons, for a before/after comparison.
##
## Waits for the BlockIconRenderer cache to fill, then prints a hash and an alpha
## summary per sample block, so a change to the icon path (the batched render)
## can be compared with the previous run's numbers.
##
##   probes/run_probe_shot.sh probes/probe_icons.gd 60

const SAMPLE_IDS := [1, 2, 3, 4, 5, 6, 7, 8]

func _initialize() -> void:
	_run()

func _run() -> void:
	await process_frame
	var renderer := root.get_node_or_null("BlockIconRenderer")
	if renderer == null:
		print("PROBE icon FAIL: BlockIconRenderer missing")
		quit(1)
		return
	var defs: Array = renderer.get("_block_defs")
	var ids: Array[int] = []
	for block_id in SAMPLE_IDS:
		if block_id < defs.size():
			ids.append(block_id)
	# A shaped block as well, so an orientation error cannot hide behind a cube.
	for block_id in range(defs.size()):
		var shape: String = defs[block_id].get("shape", "")
		if not shape.is_empty() and not ids.has(block_id):
			ids.append(block_id)
			break
	var deadline := Time.get_ticks_usec() + 15_000_000
	while Time.get_ticks_usec() < deadline:
		var ready := true
		for block_id in ids:
			if renderer.get_block_icon(block_id) == null:
				ready = false
				break
		if ready:
			break
		await process_frame
	print("PROBE icon cached=%d" % int(renderer.get("_icon_cache").size()))
	for block_id in ids:
		var texture: ImageTexture = renderer.get_block_icon(block_id)
		if texture == null:
			print("PROBE icon id=%d missing" % block_id)
			continue
		var image := texture.get_image()
		var filled := 0
		var min_x := image.get_width()
		var min_y := image.get_height()
		var max_x := -1
		var max_y := -1
		for y in range(image.get_height()):
			for x in range(image.get_width()):
				if image.get_pixel(x, y).a > 0.01:
					filled += 1
					min_x = mini(min_x, x)
					min_y = mini(min_y, y)
					max_x = maxi(max_x, x)
					max_y = maxi(max_y, y)
		print("PROBE icon id=%d name=%s hash=%d size=%dx%d alpha=%d box=%d,%d-%d,%d" % [
			block_id, str(defs[block_id].get("name", "")), hash(image.get_data()),
			image.get_width(), image.get_height(), filled,
			min_x, min_y, max_x, max_y])
	quit(0)
