extends SceneTree
## Headless check for the "Spawn Goblin" tool (scripts/goblin_spawn.gd, registered
## in tool_loader.gd's TOOLS and reached by the Tools page's button and the G key).
##
## The world is stubbed rather than streamed: what the tool owes is an ENTITY, not
## terrain. A goblin has to be a body carrying goblin.gd in the `pose_clone` group (the
## body and the group PlayerController punches), moved by the player's own movement sim
## (`ChunkManager.make_mob_body`; the stub hands it a body with no world to move
## through, so its shove and hurt window are real and its walking is not), standing in
## the middle of the cell
## the aim ray would place a block in, with the model riding it on the world's own
## light material and the light of the cell its middle is in. Its health is its own
## state and NOTHING is drawn over its head: a punch takes a half-heart, a resisted hit
## takes nothing, and the twelfth half-heart kills it. A miss must spawn nothing.
##
## Needs no world and no display: the "world" is `probes/goblin_probe_world.gd`, one aim
## ray and the two light calls, so it runs headless.
##
##   probes/probe_goblin.gd   (Godot --headless --path <project> --script res://probes/probe_goblin.gd)

const TOOL_PATH := "res://scripts/goblin_spawn.gd"
const BODY_SCRIPT := "res://scripts/goblin.gd"
const FAKE_MANAGER := preload("res://probes/goblin_probe_world.gd")
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)
const CELL := Vector3(10, 64, -6)
const PLAYER_AT := Vector3(11.5, 64.0, -20.0)

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _goblins(scene: Node) -> Array:
	var found: Array = []
	for node in scene.get_tree().get_nodes_in_group("goblin"):
		if scene.is_ancestor_of(node):
			found.append(node)
	return found


func _run() -> void:
	# The tool asks the CURRENT SCENE for its ChunkManager and Player, and the goblin
	# asks for /root/Main/ChunkManager by the same absolute path the C++ side uses, so
	# this stand-in world is named what the real one is. This is all of it the entity
	# needs.
	var world := Node3D.new()
	world.name = "Main"
	root.add_child(world)
	current_scene = world
	var player := Node3D.new()
	player.name = "Player"
	player.position = PLAYER_AT
	world.add_child(player)
	var chunk_manager := Node.new()
	chunk_manager.name = "ChunkManager"
	chunk_manager.set_script(load("res://probes/goblin_probe_world.gd"))
	world.add_child(chunk_manager)
	await process_frame

	# The button's path: the loader knows the name, and the tool loads by it.
	var loader := load("res://scripts/tool_loader.gd")
	if loader == null:
		_fail("tool_loader.gd did not load")
		quit(1)
		return
	if String(loader.TOOLS.get("goblin", "")) != TOOL_PATH:
		_fail("ToolLoader's \"goblin\" is not %s" % TOOL_PATH)

	var tool: Node = (load(TOOL_PATH) as Script).new()
	if tool == null:
		_fail("%s did not load" % TOOL_PATH)
		quit(1)
		return
	root.add_child(tool)
	tool.call("spawn")
	await process_frame

	var body := world.get_node_or_null("Goblin") as Node3D
	if body == null:
		_fail("no Goblin body in the scene")
		quit(1)
		return

	# An entity, not scenery: goblin.gd (dummy.gd's physics plus hearts) on a body, in
	# the group the punch picks from, and able to take the hit.
	if body.get_script() == null or not String(body.get_script().resource_path).ends_with("goblin.gd"):
		_fail("the goblin body is not carrying goblin.gd")
	if not body.is_in_group("pose_clone"):
		_fail("the goblin is not in the `pose_clone` group PlayerController punches")
	if not body.is_in_group("goblin"):
		_fail("the goblin is not in its own `goblin` group")
	if not body.has_method("apply_knockback"):
		_fail("the goblin body has no apply_knockback (the punch's landing)")

	# Feet in the placement cell's floor, centred in the cell (not on its corner: a box
	# on the seam is half inside a wall). The model's own offset is on the child, so
	# the box the click is tested against is the body's.
	var want_body := CELL + Vector3(0.5, 0.0, 0.5)
	if not body.global_position.is_equal_approx(want_body):
		_fail("goblin body at %s, expected %s" % [body.global_position, want_body])

	var model := body.get_node_or_null(NodePath("Model")) as Node3D
	if model == null:
		_fail("no Model child under the goblin body")
		quit(1)
		return
	if not model.position.is_equal_approx(BODY_OFFSET):
		_fail("model offset %s, expected %s" % [model.position, BODY_OFFSET])

	# The obj's eleven surfaces split into the six parts it was authored as, each hung
	# from its own joint -- the rig the player's own clips turn (goblin_model.gd). By
	# SEARCH rather than by path: `_init_held_arm` hangs the main arm under HeldArmPivot
	# so an item pose can turn it, so `arm` is no longer the model's own child.
	var parts := {}
	for part in ["arm", "arm2", "leg", "leg2", "torso", "head"]:
		var mi := model.find_child(part, true, false) as MeshInstance3D
		if mi == null or mi.mesh == null:
			_fail("the goblin's rig has no `%s` part" % part)
			continue
		parts[part] = mi
	if parts.size() != 6:
		_fail("the rig has %d of 6 parts" % parts.size())

	# Its feet on the model's origin (y == 0) and 1.44 tall, so the body position above
	# really is where it stands -- measured over the parts, the only geometry there is.
	var union := AABB()
	var any := false
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		var box: AABB = (model.global_transform.affine_inverse() * mi.global_transform
			* (mi.mesh as Mesh).get_aabb())
		union = box if not any else union.merge(box)
		any = true
	if not any:
		_fail("the rig has no geometry at all")
	else:
		if absf(union.position.y) > 0.01:
			_fail("rig feet are at y=%.3f, not 0 (the goblin would float or sink)" % union.position.y)
		if absf(union.size.y - 1.44) > 0.02:
			_fail("the rig is %.3f tall, expected ~1.44 blocks" % union.size.y)

	# The world's own light model on every part, exactly as the player's body is drawn:
	# the item shader over item_lighting.gdshaderinc, not an engine-lit
	# StandardMaterial3D, and the goblin's own picture rather than the skin.
	var lit := 0
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		for surface in range((mi.mesh as Mesh).get_surface_count()):
			var mat := mi.get_surface_override_material(surface) as ShaderMaterial
			if mat == null:
				continue
			lit += 1
			if mat.shader == null or not String(mat.shader.resource_path).ends_with("item_shader.gdshader"):
				_fail("`%s`'s shader is not item_shader.gdshader (the world's light model)" % part)
			var albedo = mat.get_shader_parameter("albedo_texture")
			if albedo == null:
				_fail("`%s` has no albedo texture" % part)
			elif not String((albedo as Texture2D).resource_path).ends_with("Goblin2.png"):
				_fail("`%s`'s texture is %s, not Goblin2.png" % [part, (albedo as Texture2D).resource_path])
			if not is_zero_approx(float(mat.get_shader_parameter("alpha_scissor"))):
				_fail("`%s`'s alpha_scissor is not 0 (the model is opaque boxes, not a sprite)" % part)
	if lit == 0:
		_fail("no part of the rig wears the world's light material")

	# The world's own light uniforms pushed onto that material every frame, and the cell
	# the goblin stands in pushed onto each part's instance the way the body does it.
	for i in 2:
		await process_frame
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		var light_var = mi.get_instance_shader_parameter("item_light")
		if typeof(light_var) != TYPE_VECTOR4:
			_fail("`%s` never got item_light (got %s)" % [part, str(light_var)])
		elif not (light_var as Vector4).is_equal_approx(FAKE_MANAGER.CELL_LIGHT):
			_fail("`%s`'s item_light is %s, not the cell's %s" % [part, light_var, FAKE_MANAGER.CELL_LIGHT])
	if int(chunk_manager.lighting_pushes) < 1:
		_fail("apply_item_lighting was never pushed onto the goblin's material")
	if int(chunk_manager.light_calls) < 1:
		_fail("the goblin's cell light was never read")

	# The face is the model's own -Z, so the yaw that turns -Z onto the player is
	# what the tool must set -- on the model, so the body's axes stay square.
	var dir := PLAYER_AT - (CELL + Vector3(0.5, 0.0, 0.5))
	var want_yaw := atan2(-dir.x, -dir.z)
	if not is_equal_approx(model.rotation.y, want_yaw):
		_fail("model yaw %.4f, expected %.4f (facing the player)" % [model.rotation.y, want_yaw])
	if not is_zero_approx(body.rotation.y):
		_fail("the body itself is rotated %.3f: its box would no longer be axis-aligned" % body.rotation.y)

	# --- the offhand torch: one roll, one home for the answer --------------------
	# A quarter of goblins spawn carrying one, and `offhand_item` is the ONE home for that
	# answer: the model draws whatever it says and `die()` drops that same value, no second
	# roll anywhere. What a body owes this probe is registry-free, because THIS WORLD HAS
	# NO REGISTRIES: `VoxelEngineController` loads data/items.json and data/block_definitions
	# .json, and a stubbed world has no controller -- so no name resolves, the spawn roll can
	# only answer "empty", and the torch itself lives where there is a world:
	# probe_goblin_loot.gd (the 25%'s arithmetic and the always-drop over real kills) and
	# probe_entity_light.gd (the drawn torch, its side and its light).
	var torch := BlockTextures.get_block_id_by_name("torch")
	if torch > 0:
		_fail("this world resolved `torch` (%d): the checks below assume no item registry" % torch)
	var rolled := int(body.get("offhand_item"))
	if rolled != 0:
		_fail("a goblin in a world with no registry carries %d, not an empty hand" % rolled)
	if int(body.call("get_offhand_item")) != 0:
		_fail("the body reports %d for an empty offhand" % int(body.call("get_offhand_item")))
	# An empty hand builds no holder: nothing is drawn under the arm, and the position the
	# model reports is its OWN origin rather than a hand's (get_offhand_global_position).
	if model.find_child("OffhandItem", true, false) != null:
		_fail("an empty-handed goblin carries an OffhandItem node")
	var no_hand: Vector3 = body.call("get_offhand_position")
	if no_hand.distance_to(model.global_position) > 0.01:
		_fail("an empty hand reports %s, not the model's own origin %s"
			% [no_hand, model.global_position])

	# The dice, on the other hand, need no registry for the answer that matters here: a name
	# nothing knows is an EMPTY HAND, never the -1 the lookup itself answers, because the
	# field is read as an id everywhere and 0 is the only "nothing".
	var equipment := load("res://scripts/mob_equipment.gd")
	if equipment == null:
		_fail("scripts/mob_equipment.gd did not load")
	else:
		if int(equipment.item_id("torch")) != 0:
			_fail("an unknown item name resolved to %d, not 0" % int(equipment.item_id("torch")))
		for roll in [0.0, 0.125, 0.2499, 0.25, 0.5, 1.0]:
			if int(equipment.roll_offhand("torch", 0.25, roll)) != 0:
				_fail("a roll of %.4f armed a goblin with an item no registry knows" % roll)
		if int(equipment.roll_offhand("torch", 0.0, 0.0)) != 0:
			_fail("a chance of 0 carried an item")
		if int(equipment.roll_offhand("", 1.0, 0.0)) != 0:
			_fail("an empty item name carried something")

	# A hit is the player's own: the shove the sim applies to a queued hit -- 0.4 away
	# from the attacker and up -- lands in the velocity the body is moved by.
	var sim = body.get("_sim")
	if sim == null:
		_fail("the goblin has no movement sim (ChunkManager.make_mob_body)")
	body.call("apply_knockback", Vector3.ZERO)
	if sim != null:
		var motion: Vector3 = sim.get_velocity()
		if motion.y <= 0.0:
			_fail("apply_knockback did not launch it upward (motion.y %.3f)" % motion.y)
		if motion.z >= 0.0 or motion.x <= 0.0:
			_fail("knockback went the wrong way (motion %s, away from the origin is +x/-z)" % motion)

	# --- six hearts, and nothing drawn over them --------------------------------
	# The health is the goblin's own state, not a thing in the world: no bar, no
	# sprite. Six hearts over the head would be red dots over the terrain from every
	# angle the body can be seen from.
	if body.get_node_or_null("HealthBar") != null:
		_fail("the goblin carries a HealthBar node: nothing may be drawn over its head")
	for child in body.get_children():
		if child is Sprite3D or child is SpriteBase3D:
			_fail("the goblin carries a %s sprite: nothing may be drawn over its head" % child.name)
	if not is_equal_approx(float(body.call("get_max_health")), 12.0):
		_fail("max health %.1f half-hearts, expected 12 (six hearts)" % float(body.call("get_max_health")))
	# The math call above was already a punch: one half-heart gone. (The world is
	# stubbed, so no tick of knockback landed and the window is still open.)
	if not is_equal_approx(float(body.call("get_health")), 11.0):
		_fail("health %.1f after one hit, expected 11 of 12" % float(body.call("get_health")))

	# Inside the hurt window a repeat hit is resisted: no knockback, no damage.
	body.call("apply_knockback", Vector3.ZERO)
	if not is_equal_approx(float(body.call("get_health")), 11.0):
		_fail("a resisted hit still took damage (health %.1f)" % float(body.call("get_health")))
	# Off the fight again: that punch made the player its enemy, and a chase owns the head
	# and the pace, which the checks below are not about.
	body.call("_give_up")

	# --- movement speed: a spider's 0.30, and the states over it ----------------
	# The value is a per-tick acceleration, so a mob's blocks a second is value * 9.52:
	# 0.30 is 2.86 blocks/s, two thirds of the player's own 4.317 walk. Every state is
	# available; the wander is the only one a behaviour drives today.
	const MOB_TO_BLOCKS := 9.52
	var base_speed := 0.30 * MOB_TO_BLOCKS
	if String(body.get("speed_state")) != "idle":
		_fail("a fresh goblin is in state '%s', expected 'idle'" % String(body.get("speed_state")))
	if not is_equal_approx(float(body.call("get_move_speed")), base_speed * 0.6):
		_fail("wandering at %.3f blocks/s, expected %.3f (0.6 of 0.30)"
			% [float(body.call("get_move_speed")), base_speed * 0.6])
	for state in [["chasing", 1.0], ["loot", 1.1], ["fleeing", 1.3], ["sneaking", 0.7]]:
		body.set("speed_state", String(state[0]))
		var want := base_speed * float(state[1])
		if not is_equal_approx(float(body.call("get_move_speed")), want):
			_fail("'%s' moves at %.3f blocks/s, expected %.3f"
				% [state[0], float(body.call("get_move_speed")), want])
		if not is_equal_approx(float(body.call("get_speed_multiplier")), want / 4.317):
			_fail("'%s' paces at %.4f of the player's walk, expected %.4f"
				% [state[0], float(body.call("get_speed_multiplier")), want / 4.317])
	body.set("speed_state", "idle")

	# --- the head leads, the body follows --------------------------------------
	# A look the neck cannot reach turns the whole goblin, after the wait the game's own mobs
	# take (10 ticks). That is the "it turns round when it looks about" an idle mob has and a
	# head clamped in place does not -- a glance behind used to leave the neck pinned at its
	# limit with an arm in its face.
	body.set("_yaw", 0.0)
	body.set("_yaw_target", 0.0)
	body.set("_yaw_ready", true)
	body.set("_look_ticks", 400)
	var here: Vector3 = body.global_position
	# Driven a tick at a time rather than by waiting: this world's body has no resolver, so
	# its sim never ticks (MobBody::step returns at once), and the tick path is the thing
	# under test.
	body.call("_set_look", here + Vector3(0.0, 1.0, -3.0))
	for i in 4:
		body.call("_advance_ticks", 1)
	if not is_zero_approx(float(body.get("_yaw_target"))):
		_fail("a look straight ahead turned the body (target %.3f)" % float(body.get("_yaw_target")))
	body.call("_set_look", here + Vector3(0.0, 1.0, 3.0))
	for i in 4:
		body.call("_advance_ticks", 1)
	if absf(float(body.get("_yaw_target"))) < 1.0:
		_fail("a look behind did not turn the body round (target %.3f)" % float(body.get("_yaw_target")))
	body.set("_yaw_target", 0.0)
	body.set("_look_ticks", 0)

	# A fight. The punch puts the player in front of it as its revenge target (the punch path
	# itself is covered above); these are the rules it then runs on -- chase at the chasing
	# pace, swing in reach once a second, run when badly hurt, let go once far enough.
	body.set("_health", 12.0)
	body.set("_gazing", false)
	body.set("_look_ticks", 0)
	player.position = body.global_position + Vector3(6.0, 0.0, 0.0)
	body.set("_enemy", player)
	# A fight runs on anger, and anger runs out: a target planted by hand still needs its clock, or
	# the first tick of it is the last (the goblin drops a target it is not angry at any more).
	body.set("_anger_ticks", 600)
	body.set("_fleeing", false)
	body.set("_attack_ticks", 0)
	body.call("_update_enemy", 1)
	if String(body.get("speed_state")) != "chasing":
		_fail("a goblin with a player to fight is on '%s', not chasing" % body.get("speed_state"))
	player.position = body.global_position + Vector3(1.0, 0.0, 0.0)
	body.call("_update_enemy", 1)
	if int(body.get("_attack_ticks")) != 20:
		_fail("it did not swing at a player in reach (cooldown %d)" % int(body.get("_attack_ticks")))
	body.call("_update_enemy", 1)
	if int(body.get("_attack_ticks")) != 19:
		_fail("the swing's cooldown is not on ticks (%d after one)" % int(body.get("_attack_ticks")))
	body.set("_health", 2.0)
	player.position = body.global_position + Vector3(4.0, 0.0, 0.0)
	body.call("_update_enemy", 1)
	if not bool(body.get("_fleeing")) or String(body.get("speed_state")) != "fleeing":
		_fail("a goblin on 2 of 12 half-hearts is on '%s', not fleeing" % body.get("speed_state"))
	player.position = body.global_position + Vector3(20.0, 0.0, 0.0)
	body.call("_update_enemy", 1)
	if bool(body.get("_fleeing")) or body.get("_enemy") != null:
		_fail("it kept running with the player 20 blocks off")
	if String(body.get("speed_state")) != "idle":
		_fail("it let go of the fight but stayed on '%s'" % body.get("speed_state"))
	# The blow itself: one heart off whatever it is fighting. A body with only health to give
	# is the fallback path -- what a build from before `take_mob_hit` has -- and it is the
	# one that silently did nothing when the extension was older than the script.
	var stub := GDScript.new()
	stub.source_code = """extends Node3D
var health := 20
func get_health() -> float:
	return float(health)
func set_health(value: int) -> void:
	health = value
"""
	stub.reload()
	var victim := Node3D.new()
	victim.set_script(stub)
	world.add_child(victim)
	victim.position = body.global_position + Vector3(1.0, 0.0, 0.0)
	body.set("_health", 12.0)
	body.set("_enemy", victim)
	body.set("_anger_ticks", 600)
	body.set("_fleeing", false)
	body.set("_attack_ticks", 0)
	body.call("_update_enemy", 1)
	if int(victim.get("health")) != 18:
		_fail("a swing took the player to %d of 20, expected 18 (a heart)"
			% int(victim.get("health")))
	victim.queue_free()
	body.call("_give_up")
	# Back to the state the checks above left it in: one landed punch, 11 of 12.
	body.set("_health", 11.0)
	player.position = PLAYER_AT

	# Walking, the aim drifts side to side on top of the downward tilt, so the head never holds
	# one exact line for the whole of a walk. The swing is the whole aim, so the tilt stays.
	body.set("_sway", 0.0)
	body.call("_pick_walk_look", Vector3(0.0, 0.0, -1.0))
	var ahead: Vector3 = body.get("_look_point")
	body.set("_sway", PI * 0.5)
	body.call("_pick_walk_look", Vector3(0.0, 0.0, -1.0))
	var swung: Vector3 = body.get("_look_point")
	var neck: Vector3 = body.global_position + Vector3(0.0, 1.0, 0.0)
	var drift := rad_to_deg(
		Vector2(ahead.x - neck.x, ahead.z - neck.z).angle_to(
			Vector2(swung.x - neck.x, swung.z - neck.z)))
	if absf(drift) < 6.0 or absf(drift) > 10.0:
		_fail("the walk's head drifted %.1f degrees, expected about 8" % drift)
	var neck_y: float = body.global_position.y + 1.0
	if ahead.y > neck_y - 0.5 or swung.y > neck_y - 0.5:
		_fail("the walk's look left its downward tilt (%.2f, %.2f against a neck at %.2f)"
			% [ahead.y, swung.y, neck_y])
	body.set("_sway", 0.0)

	# A player in reach gets looked at, on the goal's structure with the reach and hold tuned
	# down: half a percent a tick to start, 4 blocks,
	# then 40-80 ticks of watching, dropped the moment they are out of range. Driven a tick at
	# a time for the same reason as the body turn above.
	body.set("_gazing", false)
	body.set("_gaze_ticks", 0)
	for i in 200:
		body.call("_update_gaze", 1)
	if bool(body.get("_gazing")):
		_fail("it watched a player 14 blocks off, and the range is 8")
	player.position = body.global_position + Vector3(2.0, 0.0, 0.0)
	# The watch and the glance compete for the head, as the two goals do: a held glance wins it.
	body.set("_look_ticks", 100)
	body.set("_gazing", false)
	for i in 400:
		body.call("_update_gaze", 1)
	if bool(body.get("_gazing")):
		_fail("it started watching a player while a glance still held the head")
	body.set("_look_ticks", 0)
	var watching := false
	for i in 2000:
		body.call("_update_gaze", 1)
		if bool(body.get("_gazing")):
			watching = true
			break
	if not watching:
		_fail("it never looked at the player standing two blocks away")
	else:
		var eyes: Vector3 = player.global_position + Vector3(0.0, 1.62, 0.0)
		var looked: Vector3 = body.get("_look_point")
		if looked.distance_to(eyes) > 0.01:
			_fail("its look point %s is not the player's eyes %s" % [str(looked), str(eyes)])
	# Out of range ends the watch at once, hold or no hold.
	player.position = body.global_position + Vector3(0.0, 0.0, 20.0)
	body.call("_update_gaze", 1)
	if bool(body.get("_gazing")):
		_fail("it kept watching a player who walked out of range")
	player.position = PLAYER_AT
	body.set("_gazing", false)
	body.set("_gaze_ticks", 0)

	# A second press makes a second goblin, and a ray that hits nothing makes none.
	tool.call("spawn")
	await process_frame
	if _goblins(world).size() != 2:
		_fail("%d goblins after two spawns, expected 2" % _goblins(world).size())
	chunk_manager.hit = false
	tool.call("spawn")
	await process_frame
	if _goblins(world).size() != 2:
		_fail("a missed ray still spawned (%d goblins)" % _goblins(world).size())
	if int(chunk_manager.calls) != 3:
		_fail("%d raycasts, expected 3" % int(chunk_manager.calls))

	# The last half-heart kills it: the body goes, and the tool keeps the other one.
	body.call("take_damage", 11.0)
	for i in 3:
		await process_frame
	if is_instance_valid(body):
		_fail("the goblin survived its last half-heart")
	if _goblins(world).size() != 1:
		_fail("%d goblins left after one died, expected 1" % _goblins(world).size())

	# The world's light stops being pushed once the last goblin is gone.
	for goblin in _goblins(world):
		goblin.queue_free()
	for i in 3:
		await process_frame
	var pushes := int(chunk_manager.lighting_pushes)
	for i in 3:
		await process_frame
	if int(chunk_manager.lighting_pushes) != pushes:
		_fail("the tool keeps pushing world light with no goblin left in the scene")

	if ok:
		print("PROBE PASS: goblin is a goblin.gd body in `pose_clone` in the aimed cell's "
			+ "middle, model on the world's item shader with the cell's own light, twelve "
			+ "half-hearts taking one apiece and resisting repeats, dead at zero, nothing "
			+ "drawn over its head, one per press, none on a miss, no light pushed when empty")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)
