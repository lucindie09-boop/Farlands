extends SceneTree
## Headless check for the item registry and the held-item resting positions.
##
## Loads the real scene, because items.json is loaded by the engine controller's
## constructor (not by ResourceLoader), then asks BlockTextures for each item by
## name. That is the whole contract for adding an item: an entry in
## data/items.json, resolvable by name, reported as an item (never as a block),
## resolving to a texture under assets/textures/items/. water_bucket is checked first
## because it is the newest entry — but the other two are checked too, so a
## regression in the registry itself cannot hide behind a passing new entry.
##
## It then checks the second half of that contract: an item may name a held-item
## RESTING POSITION (items.json "pose"), which the viewmodel resolves to its own
## rest/peak pose. ITEM2 is defined as ITEM with its Z term lowered, so the two
## must always agree that way — a hand-copied ITEM2 that silently drifts from
## ITEM is exactly the regression this catches.
##
## Run: Godot --headless --path <project> --script res://probes/probe_items.gd

var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	# Every item name here must resolve, be reported as an item (never a block),
	# not be hidden (which would make /give refuse it), and load a texture.
	for name in ["water_bucket", "acid_bucket", "lava_bucket", "stick", "cactus_pickaxe",
			"wooden_hammer", "stone_hammer", "copper_hammer", "iron_hammer",
			"wooden_pickaxe", "stone_pickaxe", "copper_pickaxe", "iron_pickaxe",
			"torch", "bucket", "iron_ingot", "wand", "dummy_egg", "fishing_rod",
			"wooden_rod"]:
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			_fail("%s did not resolve by name" % name)
			continue
		if not BlockTextures.is_item(id):
			_fail("%s resolved to id %d, which is not an item" % [name, id])
			continue
		if BlockTextures.is_hidden(id):
			_fail("%s is flagged hidden, so /give would refuse it" % name)
			continue
		var tex: Texture2D = BlockTextures.get_texture(id)
		if tex == null:
			_fail("%s resolved but no texture loaded (assets/textures/items/%s.png)" % [name, name])
			continue
		print("probe: %-15s id=%-5d item=%s texture=%dx%d"
			% [name, id, BlockTextures.is_item(id), tex.get_width(), tex.get_height()])

	# --- held-item resting positions ----------------------------------------
	var viewmodel: Node = main.get_node_or_null("Player/Camera3D/Viewmodel")
	if viewmodel == null:
		_fail("no Player/Camera3D/Viewmodel to read the resting positions from")
	else:
		var poses: Dictionary = viewmodel.get("_item_poses")
		var by_id: Dictionary = viewmodel.get("_item_pose_by_id")
		if not poses.has("ITEM"):
			_fail("expected an ITEM resting position, got %s" % str(poses.keys()))
		else:
			var item: Dictionary = poses["ITEM"]
			var rot: Vector3 = item["rot"]
			var peak: Vector3 = item["peak_rot"]
			# Every derived position is ITEM with ONLY its Z term lowered by its own
			# delta -- on the rest pose and on the punch's peak alike. The peak is
			# what the swing interpolates toward, so a variant that moved the rest
			# pose alone would finish its punch a delta out from where it started.
			for variant in [["ITEM2", -90.0], ["ITEM3", -40.0]]:
				if not poses.has(variant[0]):
					_fail("expected an %s resting position, got %s" % [variant[0], str(poses.keys())])
					continue
				var variant_pose: Dictionary = poses[variant[0]]
				var delta: float = variant[1]
				var variant_rot: Vector3 = variant_pose["rot"]
				var variant_peak: Vector3 = variant_pose["peak_rot"]
				if variant_rot.z != rot.z + delta or variant_rot.x != rot.x or variant_rot.y != rot.y:
					_fail("%s rest must be ITEM rotated %s on Z: %s vs %s"
						% [variant[0], delta, variant_rot, rot])
				if variant_peak.z != peak.z + delta or variant_peak.x != peak.x or variant_peak.y != peak.y:
					_fail("%s peak must be ITEM's peak rotated %s on Z: %s vs %s"
						% [variant[0], delta, variant_peak, peak])
				if variant_pose["pos"] != item["pos"] or variant_pose["peak_pos"] != item["peak_pos"]:
					_fail("%s must keep ITEM's positions" % variant[0])
				if variant_pose["scale"] != item["scale"]:
					_fail("%s must keep ITEM's scale" % variant[0])
				print("probe: %s rest %s (ITEM %s), peak %s (ITEM peak %s)"
					% [variant[0], variant_rot, rot, variant_peak, peak])

		# Sprites whose artwork sits a quarter turn round on the sprite plane all
		# name ITEM2. The three buckets are one artwork recoloured, and the torch,
		# the ingot and the spawn egg are drawn the same way; any of them added
		# without a pose would silently render a quarter turn out of line with its
		# siblings. The two rods sit 40 degrees round from the tools instead of a
		# quarter turn, so they have their own position (ITEM3) rather than sharing
		# ITEM with the stick -- checked after this loop.
		var bucket_id := BlockTextures.get_block_id_by_name("water_bucket")
		for quarter_turn in ["water_bucket", "acid_bucket", "lava_bucket", "torch",
				"iron_ingot", "dummy_egg"]:
			var id := BlockTextures.get_block_id_by_name(quarter_turn)
			if not by_id.has(id):
				_fail("%s (id %d) names no resting position" % [quarter_turn, id])
			elif by_id[id] != "ITEM2":
				_fail("%s resolves to resting position '%s', expected ITEM2" % [quarter_turn, by_id[id]])
			else:
				print("probe: %-13s -> resting position %s" % [quarter_turn, by_id[id]])

		# The two rods: their own position, not the tools' ITEM and not ITEM2.
		for rod in ["fishing_rod", "wooden_rod"]:
			var rod_id := BlockTextures.get_block_id_by_name(rod)
			if by_id.get(rod_id, "ITEM") != "ITEM3":
				_fail("%s names resting position '%s', expected ITEM3" % [rod, by_id.get(rod_id, "ITEM")])
			else:
				print("probe: %-13s -> resting position ITEM3" % rod)

		# --- item swap -------------------------------------------------------
		# For the first half of a swap the OUTGOING item is still the one drawn, so
		# it has to render with its OWN resting position. Keying the pose off the
		# incoming id instead turned the outgoing item a quarter turn through the
		# unequip — and the sign differed depending on the swap's direction, which is
		# why both directions are checked here.
		var stick_id := BlockTextures.get_block_id_by_name("stick")
		viewmodel.set("_swing_s", 0.0)
		viewmodel.set("_swing_angle", 0.0)
		# Read defensively: the pose check above has already failed if either is
		# missing, and a runtime error here would abort the whole probe.
		# rotation_degrees reports the euler as set, not wrapped, so compare raw.
		var item_z := float(poses.get("ITEM", {}).get("rot", Vector3.ZERO).z)
		var item2_z := float(poses.get("ITEM2", {}).get("rot", Vector3.ZERO).z)
		if absf(item_z - item2_z) < 80.0:
			_fail("the two resting positions are not a quarter turn apart (%s vs %s)" % [item_z, item2_z])

		viewmodel.set("_block_id", bucket_id)   # swapping IN the bucket
		viewmodel.set("_display_id", stick_id)  # ...while the stick is still drawn
		viewmodel.call("_update_item_transform")
		var shown_z := float(viewmodel.get("_item_scale_node").rotation_degrees.z)
		if absf(shown_z - item_z) > 0.01:
			_fail("bucket<->stick: the outgoing stick rendered at Z %s, expected its own %s (bucket's is %s)"
				% [shown_z, item_z, item2_z])
		else:
			print("probe: swap in the bucket shows the outgoing stick at Z %s (its own), not %s"
				% [shown_z, item2_z])

		viewmodel.set("_block_id", stick_id)    # swapping IN the stick
		viewmodel.set("_display_id", bucket_id) # ...while the bucket is still drawn
		viewmodel.call("_update_item_transform")
		shown_z = float(viewmodel.get("_item_scale_node").rotation_degrees.z)
		if absf(shown_z - item2_z) > 0.01:
			_fail("stick<->bucket: the outgoing bucket rendered at Z %s, expected its own %s (stick's is %s)"
				% [shown_z, item2_z, item_z])
		else:
			print("probe: swap in the stick shows the outgoing bucket at Z %s (its own), not %s"
				% [shown_z, item_z])

		# The same rule across an item<->block swap: which transform applies must
		# follow what is DRAWN, or the outgoing item keeps whatever transform the
		# block path last left on the node. A sentinel proves the item path ran.
		var stone_id := BlockTextures.get_block_id_by_name("stone")
		viewmodel.set("_block_id", stone_id)    # swapping IN a block
		viewmodel.set("_display_id", stick_id)  # ...while the stick is still drawn
		viewmodel.get("_item_scale_node").rotation_degrees = Vector3.ZERO
		viewmodel.call("_update_item_transform")
		shown_z = float(viewmodel.get("_item_scale_node").rotation_degrees.z)
		if absf(shown_z - item_z) > 0.01:
			_fail("item<->block: the outgoing stick sat at Z %s, expected its own %s" % [shown_z, item_z])
		else:
			print("probe: swapping in a block still poses the outgoing stick at Z %s" % shown_z)

		# The F12 editor is shared by the two item modes, so each key must edit
		# the mode it was pressed in and leave the other resting position alone.
		viewmodel.set("_adjustment_mode", "ITEM2")
		var before_item2: Vector3 = poses["ITEM2"]["rot"]
		var before_item: Vector3 = poses["ITEM"]["rot"]
		if not viewmodel.call("_edit_item_pose", KEY_S):
			_fail("_edit_item_pose did not handle S (Z rot +) in ITEM2 mode")
		elif poses["ITEM2"]["rot"] != before_item2 + Vector3(0.0, 0.0, 5.0):
			_fail("S in ITEM2 mode gave %s, expected %s" % [poses["ITEM2"]["rot"], before_item2 + Vector3(0.0, 0.0, 5.0)])
		elif poses["ITEM"]["rot"] != before_item:
			_fail("editing ITEM2 moved ITEM's rotation to %s" % poses["ITEM"]["rot"])
		elif viewmodel.call("_edit_item_pose", KEY_PAGEUP):
			_fail("_edit_item_pose claimed an unbound key")
		else:
			print("probe: F12 S edited ITEM2 only (%s -> %s), ITEM stayed %s"
				% [before_item2, poses["ITEM2"]["rot"], poses["ITEM"]["rot"]])

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)
