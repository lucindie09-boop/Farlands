extends SceneTree
## Headless check for the "Spawn Goblin" tool (scripts/goblin_spawn.gd, registered
## in tool_loader.gd's TOOLS and reached by the Tools page's button and the G key).
##
## The world is stubbed rather than streamed: what the tool owes is an ENTITY, not
## terrain. A goblin has to be a body carrying goblin.gd in the `pose_clone` group (the
## physics and the group PlayerController punches), standing in the middle of the cell
## the aim ray would place a block in, with the model riding it on the world's own
## light material and the light of the cell its middle is in. Over its head are its six
## hearts, a punch takes a half-heart and a resisted hit takes nothing, and the twelfth
## half-heart kills it. A miss must spawn nothing.
##
## Needs no world and no display: the "world" is `probes/goblin_probe_world.gd`, one aim
## ray and the two light calls, so it runs headless.
##
##   probes/probe_goblin.gd   (Godot --headless --path <project> --script res://probes/probe_goblin.gd)

const TOOL_PATH := "res://scripts/goblin_spawn.gd"
const BODY_SCRIPT := "res://scripts/goblin.gd"
const FAKE_MANAGER := preload("res://probes/goblin_probe_world.gd")
const HEART_FULL: Texture2D = preload("res://Assets/textures/gui/heart_full.png")
const HEART_HALF: Texture2D = preload("res://Assets/textures/gui/heart_half.png")
const HEART_EMPTY: Texture2D = preload("res://Assets/textures/gui/heart_empty.png")
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


func _hearts_of(body: Node) -> Array:
	var bar := body.get_node_or_null("HealthBar")
	if bar == null:
		_fail("the goblin has no HealthBar node")
		return []
	var hearts: Array = []
	for child in bar.get_children():
		if child is Sprite3D:
			var sprite := child as Sprite3D
			hearts.append(sprite)
			# A thing in the world, not a HUD mark: depth-tested (no no_depth_test, or
			# the hearts float visible through hills) and cut out in the opaque pass.
			if sprite.no_depth_test:
				_fail("%s has no_depth_test: it would be drawn through walls" % sprite.name)
			if sprite.alpha_cut != SpriteBase3D.ALPHA_CUT_DISCARD:
				_fail("%s is not alpha-cut (alpha_cut %d)" % [sprite.name, sprite.alpha_cut])
		else:
			_fail("a child of HealthBar is not a Sprite3D (%s)" % child.get_class())
	return hearts


## Full/half/empty hearts as a compact string, low health first, so a wrong bar reads
## as one line in the output.
func _bar_state(hearts: Array) -> String:
	var state := ""
	for heart in hearts:
		match (heart as Sprite3D).texture:
			HEART_FULL:
				state += "F"
			HEART_HALF:
				state += "h"
			HEART_EMPTY:
				state += "."
			_:
				state += "?"
	return state


func _run() -> void:
	# The tool asks the CURRENT SCENE for its ChunkManager and Player, so this is
	# the whole world it needs.
	var world := Node3D.new()
	world.name = "FakeWorld"
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

	var model := body.get_node_or_null(NodePath("Model")) as MeshInstance3D
	if model == null:
		_fail("no Model child under the goblin body")
		quit(1)
		return
	if not model.position.is_equal_approx(BODY_OFFSET):
		_fail("model offset %s, expected %s" % [model.position, BODY_OFFSET])

	# The obj's own mesh, and its feet on the model's origin (aabb.y == 0), so the
	# body position above really is where it stands.
	if model.mesh == null:
		_fail("goblin has no mesh")
	else:
		var aabb: AABB = model.mesh.get_aabb()
		if absf(aabb.position.y) > 0.01:
			_fail("mesh feet are at y=%.3f, not 0 (the goblin would float or sink)" % aabb.position.y)
		if absf(aabb.size.y - 1.44) > 0.02:
			_fail("mesh is %.3f tall, expected ~1.44 blocks" % aabb.size.y)

	# The world's own light model, exactly as the player's body is drawn: the item
	# shader over item_lighting.gdshaderinc, not an engine-lit StandardMaterial3D.
	var mat := model.material_override as ShaderMaterial
	if mat == null:
		_fail("model has no ShaderMaterial override (the obj's .mtl is not in the tree)")
	else:
		if mat.shader == null or not String(mat.shader.resource_path).ends_with("item_shader.gdshader"):
			_fail("material shader is not item_shader.gdshader (the world's light model)")
		var albedo = mat.get_shader_parameter("albedo_texture")
		if albedo == null:
			_fail("material has no albedo texture")
		elif not String((albedo as Texture2D).resource_path).ends_with("Goblin2.png"):
			_fail("material texture is %s, not Goblin2.png" % (albedo as Texture2D).resource_path)
		if not is_zero_approx(float(mat.get_shader_parameter("alpha_scissor"))):
			_fail("alpha_scissor is not 0 (the model is opaque boxes, not a sprite)")

	# The cell it stands in, pushed onto its instance the way the body does it, and
	# the world's own light uniforms pushed onto the material every frame.
	var light_var = model.get_instance_shader_parameter("item_light")
	if typeof(light_var) != TYPE_VECTOR4:
		_fail("item_light was never set on the model (got %s)" % str(light_var))
	else:
		var light: Vector4 = light_var
		if not light.is_equal_approx(FAKE_MANAGER.CELL_LIGHT):
			_fail("item_light is %s, not the cell's %s" % [light, FAKE_MANAGER.CELL_LIGHT])
	for i in 2:
		await process_frame
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

	# A hit behaves like the K dummy's: the base 0.4 away from the attacker and up
	# (dummy.gd's own numbers), from the body's position rather than its picture.
	body.call("apply_knockback", Vector3.ZERO)
	var motion: Vector3 = body.get("_motion")
	if motion.y <= 0.0:
		_fail("apply_knockback did not launch it upward (motion.y %.3f)" % motion.y)
	if motion.z >= 0.0 or motion.x <= 0.0:
		_fail("knockback went the wrong way (motion %s, away from the origin is +x/-z)" % motion)

	# --- six hearts -------------------------------------------------------------
	var hearts := _hearts_of(body)
	if hearts.size() != 6:
		_fail("%d hearts, expected 6" % hearts.size())
	if not is_equal_approx(float(body.call("get_max_health")), 12.0):
		_fail("max health %.1f half-hearts, expected 12 (six hearts)" % float(body.call("get_max_health")))
	# The math call above was already a punch: one half-heart gone. (The world is
	# stubbed, so no tick of knockback landed and the window is still open.)
	if not is_equal_approx(float(body.call("get_health")), 11.0):
		_fail("health %.1f after one hit, expected 11 of 12" % float(body.call("get_health")))
	if _bar_state(hearts) != "FFFFFh":
		_fail("bar reads '%s' at 11 half-hearts, expected 'FFFFFh'" % _bar_state(hearts))

	# Inside the hurt window a repeat hit is resisted: no knockback, no damage.
	body.call("apply_knockback", Vector3.ZERO)
	if not is_equal_approx(float(body.call("get_health")), 11.0):
		_fail("a resisted hit still took damage (health %.1f)" % float(body.call("get_health")))

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
			+ "middle, model on the world's item shader with the cell's own light, six "
			+ "hearts taking a half-heart per punch and resisting repeats, dead at zero, "
			+ "one per press, none on a miss, no light pushed when empty")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)
