extends Node3D

@export var skin_texture: Texture2D = preload("res://Assets/textures/skin.png")

signal texel_painted(px: int, py: int, old_color: Color, new_color: Color)

const UV_OVERLAY_SHADER: Shader = preload("res://Assets/shaders/skin_uv_overlay.gdshader")
const ATLAS_DIM := 64

# The world's own light model, the one the terrain, the dropped items and the
# first-person arm already run (Assets/shaders/item_lighting.gdshaderinc): the light of
# the CELL this body is in, rather than the engine's sky and ambient, which know
# nothing about caves, torches or the hour. Only for a body standing in the world
# -- see _init_world_lighting for why a preview does not get it.
const ITEM_SHADER: Shader = preload("res://Assets/shaders/item_shader.gdshader")
# The cell light is one value per block, so a body crossing a boundary -- or a
# cell being relit under it -- steps a whole cell's worth on a single frame. This
# is the time constant of the fade that turns that step into a move. The same
# rule, and the same number, as the other two consumers;
# probes/probe_item_light_smooth.gd holds all of them to it.
const LIGHT_EASE_TIME := 0.15

# The body's two clips, and the only two it plays: `Idle.anim` while it stands and
# `Run.anim` while it walks. They are resources of their own (Assets/Animations)
# rather than the model's imported clips, because a glb's clips only ever animate
# the glb's own scene -- and `Run.anim` was authored on a differently named rig,
# which is what CLIP_PATH_REMAP is for. The code pose this replaced
# (scripts/body_animator.gd, a hand-port of FA+Player's moves) is parked with its
# probe and its credit in docs/gui-notes.md.
const CLIP_IDLE: Animation = preload("res://Assets/Animations/Idle.anim")
const CLIP_RUN: Animation = preload("res://Assets/Animations/Run.anim")
const ClipRetarget := preload("res://scripts/clip_retarget.gd")
# Source track path -> this body's node for it. The clips name their second arm and
# leg `arm.001`/`leg.001` (which the import turns into `arm_001`/`leg_001`), and this
# body calls them `arm2`/`leg2`. The keys are the clip's own; only the path a track
# finds its limb by is rewritten. The head is here so a clip that ever names it is
# re-homed rather than dropped -- its rotation is the code's (see _clip_for_rig).
const CLIP_PATH_MAP := {"arm": "arm", "arm_001": "arm2", "leg": "leg", "leg_001": "leg2",
	"torso": "torso", "head": "head"}
# Every part a clip may turn, and therefore every part a clip has to ACCOUNT for
# (see _add_rest_tracks). The head is not here: _track_head_look owns it.
# Horizontal speed, blocks/s, under which the body counts as standing.
const STAND_SPEED := 0.15
# How far the walk amount closes on its target in one tick: 40% of the gap. The walk
# fades in and out over about 0.2 s as its amplitude, so there is no crossfade to look
# wrong -- and the legs land on rest because the amount reaches 0, not because a
# different pose took over.
const SWING_EASE := 0.4
const TICK_RATE := 20.0
# Minecraft derives a whole walk from ONE number: the horizontal distance the body
# covered in a tick, times 4, capped at 1, eased 40% a tick toward the target (SWING_EASE
# is that 40%). Its phase then advances by the same number every tick, and the walk angles
# are that phase's cosine twice over -- a leg swings 1.4 rad and an arm 1.0 rad, both
# multiplied by the number. Three figures fall out of that, and they are what this body's
# clip is measured against: the swing is FULL at a quarter of a block per tick (5 blocks/s),
# a cycle takes 2pi / (0.6662 * 20) s at a full swing (the cosine turns 0.6662 rad per unit
# of phase), and a full swing is 80 degrees of leg against 57 of arm.
#
# The run clip's OWN base is not those numbers (it was authored at a small
# amplitude, 16.8 degrees, and one cycle a second), which is the whole difference
# between it and vanilla: _prepare_run_clip scales each part so the clip's peak IS
# vanilla's, and the pace below is then vanilla's cadence.
const MC_FULL_SWING_SPEED := 5.0    # blocks/s where the walk amount reaches 1
const MC_CYCLE_SECONDS := 0.4716    # one cycle at a full swing
const MC_LEG_SWING := 1.4           # rad, a leg at a full swing
const MC_ARM_SWING := 1.0           # rad, an arm at a full swing
# The parts the walk drives, and how far each may swing. Any other part of the
# clip (its torso rest key) keeps its own angles.
const MC_PART_SWING := {"leg": MC_LEG_SWING, "leg2": MC_LEG_SWING,
	"arm": MC_ARM_SWING, "arm2": MC_ARM_SWING}

# The body is not one clip at a time. It is a BLEND TREE, and that is Minecraft's own
# structure: its walk pose is the swing, PLUS two arm motions that have nothing to do
# with walking -- a roll that never leaves the body's side (0 to 0.1 rad out and back over
# a 3.5 s cycle, so it sits 0.05 rad out on average) and a 0.05 rad nod over 4.7 s.
# `Idle.anim` already carries both of those for the arms, so the walk gets them by having
# the idle ADDED OVER it:
#
#   blend (idle <-> run)  -->  add (idle on top)  -->  output
#
# The idle layer is ALWAYS on (its amount is 1 and never changes), so its playhead runs
# from the moment the body exists and is never re-entered: a body that stops keeps
# breathing exactly where it was. What changes is the walk: `blend` crossfades the REST
# pose into the run, and its weight is the walk amount -- so the fade is the walk's own
# amplitude shrinking, not a timed morph between two poses, which is why
# nothing snaps when the body stops. `timescale` is that same amount again, because the
# phase advances by it in vanilla too.
#
# The tree itself is built by _build_tree.
const TREE_BLEND := "parameters/blend/blend_amount"
const TREE_ADD := "parameters/add/add_amount"
const TREE_RUN_SCALE := "parameters/timescale/scale"

var uv_overlay_enabled := false
var paint_color := Color.WHITE
# When true the head never rotates (set by the K-key pose clone so the dummy
# is fully frozen instead of tracking the player's aim).
var skip_head_look := false

var _uv_overlay_mat: ShaderMaterial
var _paint_image: Image
var _paint_texture: ImageTexture
var _head: Node3D = null
# The parts a clip may turn, in this body's own names (see _add_rest_tracks).
const RIG_PARTS := ["arm", "arm2", "leg", "leg2", "torso"]

# The clips' HOST (built here rather than in the scene, so a body used in a preview
# has one too). It is never played: the tree turns the parts, and two writers would
# fight over the same quaternions. It exists so the tree can find the animations by
# name.
var _clip_player: AnimationPlayer = null
# The tree that actually turns the parts (see TREE_BLEND).
var _tree: AnimationTree = null
# Which state the body is in ("idle"/"run", "" when frozen), so a change is one step.
var _clip := ""
# The walk amount: its amplitude and its cadence in one number, eased 40% a tick toward
# the distance the body is covering -- so it fades in and out in about 0.2 s and the legs
# SHRINK to rest instead of morphing from one pose into another.
var _swing := 0.0
# The run clip this body owns (the retargeted copy, not the preloaded file). Its keys
# are vanilla's FULL swing, always: how far the body is actually swinging is the tree's
# blend weight, not the clip's angles (see _animate_body).
var _run_clip: Animation = null
# The pose every clip is played ON TOP OF, per part: the model's own rest rotation
# (_capture_clip_base). A clip's key is `base * turn`, so this is the pose its angles
# are measured from and scaled about. It is deliberately NOT where the arms hang:
# that is the idle's, added over the walk while it plays, so there is no constant
# here for a clip to copy.
var _clip_rest := {}

# The world light model, when this body is in the world: one material for every
# surface (the whole model is one skin), the cell's light eased between frames.
var _body_material: ShaderMaterial = null
var _body_meshes: Array[MeshInstance3D] = []
var _chunk_manager: Node = null
var _body_height := 0.0
var _body_light := Vector4.ZERO
var _body_light_ready := false

func _manager():
	return get_node_or_null("/root/SkinManager")

func _ready():
	# The light model first: it decides what the body is actually drawn WITH, and
	# apply_skin_texture has to know that before it picks where the skin goes.
	_init_world_lighting()
	apply_skin_texture()
	_init_body_animation()
	_head = find_child("head", true, false)


## The two clips that turn this body's parts.
##
## Each is re-homed onto THIS body (see _clip_for_rig), so two bodies -- the
## player's and a preview's -- never share a resource whose paths were rewritten
## for the other's rig.
func _init_body_animation() -> void:
	_capture_clip_base()
	var library := AnimationLibrary.new()
	library.add_animation("idle", _clip_for_rig(CLIP_IDLE, "Idle"))
	_run_clip = _clip_for_rig(CLIP_RUN, "Run")
	library.add_animation("run", _run_clip)
	library.add_animation("rest", _rest_clip())
	_prepare_run_clip()
	_clip_player = AnimationPlayer.new()
	_clip_player.name = "Clips"
	_clip_player.add_animation_library("", library)
	add_child(_clip_player)
	_tree = AnimationTree.new()
	_tree.name = "ClipsTree"
	_tree.tree_root = _build_tree()
	add_child(_tree)
	# Both nodes are in the tree before this: the path is between them, and the tree
	# finds the animations by name through it.
	_tree.anim_player = _tree.get_path_to(_clip_player)
	# The idle layer is added at full, always (see TREE_BLEND): it is a pose the walk
	# wears, not a state the body switches into.
	_tree.set(TREE_ADD, 1.0)


## The body's blend tree (see TREE_BLEND for what it is for).
##
## Two nodes rather than one because they are two different operations, and that is what
## makes the stop clean: `blend` fades the walk in and out of the REST pose (its weight is
## the amplitude), and `add` lays the idle's hang and breathing over the result at a
## constant amount. The idle therefore has exactly one node, always playing -- nothing
## re-enters it, so there is no phase to reset.
func _build_tree() -> AnimationNodeBlendTree:
	var root := AnimationNodeBlendTree.new()
	root.add_node("rest", _anim_node("rest"), Vector2(0, 0))
	root.add_node("run", _anim_node("run"), Vector2(0, 1))
	root.add_node("timescale", AnimationNodeTimeScale.new(), Vector2(1, 1))
	root.add_node("blend", AnimationNodeBlend2.new(), Vector2(2, 1))
	root.add_node("idle", _anim_node("idle"), Vector2(2, 2))
	root.add_node("add", AnimationNodeAdd2.new(), Vector2(3, 1))
	root.connect_node("timescale", 0, "run")
	root.connect_node("blend", 0, "rest")
	root.connect_node("blend", 1, "timescale")
	root.connect_node("add", 0, "blend")
	root.connect_node("add", 1, "idle")
	root.connect_node("output", 0, "add")
	return root


## Every part a clip may turn, held at its own rest rotation, and nothing else.
##
## This is the far side of the walk's blend, and it is why the weight of that blend IS
## the walk amount: at 0 the legs are held still, at 1 they are the whole
## walk, and half way they swing half as far. It exists as a clip rather than as "leave
## the legs alone" because a blend holds nothing for a part neither side names, so the
## body would keep the last stride pose instead of settling out of it.
func _rest_clip() -> Animation:
	var clip := Animation.new()
	clip.resource_name = "rest"
	clip.length = 1.0
	clip.loop_mode = Animation.LOOP_LINEAR
	for part in RIG_PARTS:
		var node := find_child(part, true, false) as Node3D
		if node == null:
			continue
		var t := clip.add_track(Animation.TYPE_ROTATION_3D)
		clip.track_set_path(t, NodePath(String(get_path_to(node))))
		clip.track_insert_key(t, 0.0, _clip_rest.get(part, node.quaternion))
	return clip


## One clip of the body's library as a node of the tree. The run goes through a
## TimeScale (TREE_RUN_SCALE): the tree has no speed_scale of its own, and the walk's
## cadence is that same amount.
func _anim_node(animation: StringName) -> AnimationNodeAnimation:
	var node := AnimationNodeAnimation.new()
	node.animation = animation
	return node


## One clip, re-homed onto this body's own limb names.
##
## A clip is not portable as it stands: its tracks name NODES, and the rigs these two
## came from call the second arm and leg `arm_001`/`leg_001` where this body has
## `arm2`/`leg2`. `clip_retarget.gd` is the house answer, and it is what says out loud
## when a track names a limb this body does not have -- a track left pointing at
## nothing plays as no motion at all, silently. The head is named as the code's own,
## because _track_head_look writes its quaternion every frame.
func _clip_for_rig(source: Animation, clip_name: String) -> Animation:
	var clip := ClipRetarget.retarget(source, CLIP_PATH_MAP, self, 1.0, Quaternion.IDENTITY,
		PackedStringArray(["head"]))
	clip.resource_name = clip_name
	# Both clips are cycles: one that ends is a body frozen mid-stride, and `Idle.anim`
	# was saved without a loop because nothing had played it yet.
	clip.loop_mode = Animation.LOOP_LINEAR
	_add_rest_tracks(clip)
	return clip


## The pose the clips are played on top of.
##
## Read off the model before any clip has played, so it is the model's own rest: a key
## is `base * turn`, which is what lets a clip's angles be measured and scaled about
## the part they turn. Nothing is added here -- the arms' hang is the idle's, layered
## over the walk as it plays (see _build_tree).
func _capture_clip_base() -> void:
	_clip_rest.clear()
	for part in RIG_PARTS + ["head"]:
		var node := find_child(part, true, false) as Node3D
		if node == null:
			continue
		_clip_rest[part] = node.quaternion


## Put the run clip on Minecraft's own scale.
##
## Two things about it are not vanilla's, and both are fixed here once, at load:
## its limbs swing 16.8 degrees where Minecraft swings a leg 1.4 rad (80) and an arm
## 1.0 rad (57), and the clip's own cycle is 1 s where a
## full swing's is 0.4716 s (the pace is applied when it plays). Each part's peak
## is measured off its own keys and scaled to the vanilla angle for it, so the
## clip's SHAPE is kept and only its base is replaced -- a body whose model or
## clip changes does not need this file edited.
##
## The pose every key is measured from is _clip_rest, captured before any clip
## played (a key is `base * axis-turn(angle)`), so the fit and the gains see the same
## base the clip will play on.
func _prepare_run_clip() -> void:
	if _run_clip == null:
		return
	# 1. the base every key is measured from is already captured (_capture_clip_base).
	# 2. close the cycle (clip_retarget.gd::smooth_cycle): the authored easing makes
	# the legs crawl through the loop point, which at vanilla amplitude is a hitch at
	# every wrap.
	ClipRetarget.smooth_cycle(_run_clip, _clip_rest)
	# 3. vanilla's amplitudes, measured off the SMOOTHED curve, so the peaks are the ones
	# that will actually play.
	var gains := {}
	for t in _run_clip.get_track_count():
		if _run_clip.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		var path := str(_run_clip.track_get_path(t))
		if not MC_PART_SWING.has(path) or not _clip_rest.has(path):
			continue
		var rest: Quaternion = _clip_rest[path]
		var peak := 0.0
		for k in range(_run_clip.track_get_key_count(t)):
			var key: Quaternion = _run_clip.track_get_key_value(t, k)
			peak = maxf(peak, (rest.inverse() * key).get_angle())
		if peak > 0.001:
			gains[path] = float(MC_PART_SWING[path]) / peak
	if not gains.is_empty():
		ClipRetarget.scale_stride(_run_clip, _clip_rest, 1.0, gains)


## Every part this clip does not move, held at its rest rotation.
##
## A clip animates only the limbs it has tracks for, and an `AnimationPlayer` leaves
## every other property exactly as the previous clip left it -- so stopping a walk
## under `Idle.anim`, which names nothing but the two arms, froze the legs at their
## last stride pose and left the body standing mid-step. One key at the rest rotation
## makes each clip the WHOLE pose: a switch between them cannot inherit a limb from
## the other, and a leg crossing from the run to the idle eases back to rest over the
## blend instead of snapping there.
func _add_rest_tracks(clip: Animation) -> void:
	var named := {}
	for i in clip.get_track_count():
		named[str(clip.track_get_path(i))] = true
	for part in RIG_PARTS:
		if named.has(part):
			continue
		var node := find_child(part, true, false) as Node3D
		if node == null:
			continue
		var t := clip.add_track(Animation.TYPE_ROTATION_3D)
		clip.track_set_path(t, NodePath(String(get_path_to(node))))
		# The base, not the node's pose: a part this clip does not move still has to be
		# held where every clip holds it, or the blend would carry it over from the
		# other side of the blend instead of resting.
		clip.track_insert_key(t, 0.0, _clip_rest.get(part, node.quaternion))


func _process(delta: float) -> void:
	# Before the head-look guard: the pose clone sets skip_head_look, and it is
	# standing in the world like any other body, so it is lit like one.
	_push_world_lighting(delta)
	if skip_head_look:
		# ...and a pose clone is frozen: no head look AND no body clip. The tree is
		# stopped rather than merely skipped, or a body frozen after it had been
		# walking would keep walking (and one frozen while standing would keep
		# breathing).
		if _tree != null and _tree.active:
			_tree.active = false
		_clip = ""
		return
	_animate_body(delta)
	_track_head_look()


## The body's one number: the walk amount, from the speed it is moving at.
##
## A body with no controller in its ancestry (the skin maker and settings gallery
## previews put one in a SubViewport of their own) has no speed to read, so it stands
## at 0 -- and those previews are frozen anyway (skip_head_look), so they play nothing
## at all. A preview is a still; the walk is for a body in the world.
##
## That number IS the walk: it is the blend's weight, so a slower body takes a SMALLER
## step (the swing shrinks toward the rest pose) at a slower cadence (it is the time
## scale too), which is the difference between a walk and a shuffle -- and the same pair
## Minecraft gets from its one number. A sprinting body reaches the full 80 degrees of
## leg at 0.4716 s a cycle.
func _animate_body(delta: float) -> void:
	if _tree == null:
		return
	if not _tree.active:
		# A body that was frozen and is not any more plays again, from wherever its clips
	# had got to -- nothing is rewound.
		_tree.active = true
	var speed := _walk_speed()
	# The walk amount: the tick's distance times 4, capped at 1, eased 40% a tick. It is
	# the walk's amplitude AND its cadence, so a stop is the swing shrinking to nothing
	# rather than one pose being replaced by another.
	var target := clampf(speed / MC_FULL_SWING_SPEED, 0.0, 1.0)
	_swing += (target - _swing) * (1.0 - pow(1.0 - SWING_EASE, delta * TICK_RATE))
	if _swing < 0.002:
		_swing = 0.0
	_clip = "run" if _swing > 0.0 else "idle"
	_tree.set(TREE_BLEND, _swing)
	_tree.set(TREE_RUN_SCALE, _swing * _full_swing_rate())


## The body's horizontal speed in blocks/s, read from whatever moves it: the
## controller for the player's body, the movement sim for a mob's (scripts/goblin.gd
## answers `get_velocity` too). The nearest ancestor that answers owns the body, so a
## body with nothing moving it stands at 0 instead of borrowing the player's speed.
func _walk_speed() -> float:
	var n: Node = self
	while n != null:
		if n.has_method("get_velocity"):
			# The sim's velocity is blocks per TICK; the clips are timed in seconds.
			var v: Vector3 = n.get_velocity() * TICK_RATE
			return Vector2(v.x, v.z).length()
		n = n.get_parent()
	return 0.0


## Clip lengths per second the run has to play at for its phase to advance the way
## vanilla's does: one cycle every MC_CYCLE_SECONDS at a full swing (the clip is its
## own length, so a longer clip would play proportionally slower).
func _full_swing_rate() -> float:
	return (_run_clip.length if _run_clip != null else MC_CYCLE_SECONDS) / MC_CYCLE_SECONDS


## Give the body the world's own light model, when it is standing IN the world.
##
## A preview does not get it: the skin maker and the settings gallery put the
## model in a SubViewport with a world of their own, an orbiting camera and their
## own sun and fill (skin_preview.gd), where there is no world cell under the
## body to read a light from. Those keep the engine's lighting, which is what
## they are lit for. Everything else -- the player's own body, and the K-key pose
## clone -- is in the real world, and is lit by it.
##
## All of the model is one skin, so all of it gets ONE material: one albedo, one
## set of world uniforms, and one light value for the body it draws.
func _init_world_lighting() -> void:
	if get_viewport() is SubViewport:
		return
	_chunk_manager = _world_chunk_manager()
	if _chunk_manager == null:
		return
	for child in find_children("", "MeshInstance3D", true, false):
		var mi := child as MeshInstance3D
		if mi != null and mi.mesh != null:
			_body_meshes.append(mi)
	if _body_meshes.is_empty():
		_chunk_manager = null
		return

	_body_material = ShaderMaterial.new()
	_body_material.shader = ITEM_SHADER
	_body_material.set_shader_parameter("albedo_texture", _skin_albedo())
	# Closed opaque boxes: there is no sprite texel here to cut away.
	_body_material.set_shader_parameter("alpha_scissor", 0.0)
	for mi in _body_meshes:
		for surface in range((mi.mesh as Mesh).get_surface_count()):
			mi.set_surface_override_material(surface, _body_material)
	# The middle of the body, from its own geometry rather than a constant: the
	# light is read at that height (see _push_world_lighting).
	_body_height = _body_centre_height()


## The ChunkManager of the world this body is in, if any. The player's body is a
## child of the Player node, so the manager is found by the same route the
## controller and the dropped items use.
func _world_chunk_manager() -> Node:
	var cm := get_node_or_null("/root/Main/ChunkManager")
	if cm != null:
		return cm
	var scene := get_tree().current_scene
	if scene != null:
		return scene.get_node_or_null("ChunkManager")
	return null


## How far above the model's origin -- the feet, for both the player's body and
## the pose clone -- the middle of the body is, in the model's own units. The
## union of the parts' own boxes, so a model changed in Blockbench moves this
## with it instead of leaving a stale number behind.
func _body_centre_height() -> float:
	var total := AABB()
	var any := false
	for mi in _body_meshes:
		var box: AABB = mi.transform * (mi.mesh as Mesh).get_aabb()
		total = box if not any else total.merge(box)
		any = true
	return total.get_center().y if any else 0.0


## The world light half of the item model, onto the body's material, plus the
## light of the cell the body is in per mesh instance. Every frame: the day/night
## values move continuously, and a walking body crosses cells it must be re-read
## from.
##
## The cell light is the one part of that which does not move continuously, so
## `delta` is what turns its step into a short fade (LIGHT_EASE_TIME) -- the same
## model, applied the same way, as the dropped items and the first-person arm.
##
## The light is read at the body's own middle, one cell for the whole body: a
## body is not shaded limb by limb, and its middle is the cell it is most in (a
## body standing on a slab has its feet in the slab's own cell and its middle in
## the air above it, which is where the light is).
func _push_world_lighting(delta: float) -> void:
	if _body_material == null or _chunk_manager == null:
		return
	_chunk_manager.apply_item_lighting(_body_material)
	# The body is fed the same neutral sky-light curve as the world and every
	# other item; probes/probe_body_light.gd reads both materials.
	var at := to_global(Vector3(0.0, _body_height, 0.0))
	var cell: Vector4 = _chunk_manager.get_light_at(floori(at.x), floori(at.y), floori(at.z))
	if not _body_light_ready:
		# The first frame has nothing to ease FROM: land on the cell's own value
		# instead of fading in from a black that was never on screen.
		_body_light = cell
		_body_light_ready = true
	else:
		_body_light = _ease_light(_body_light, cell, delta)
	for mi in _body_meshes:
		mi.set_instance_shader_parameter("item_light", _body_light)


## Framerate-independent exponential ease toward `target`: LIGHT_EASE_TIME is the
## time constant, so a step takes the same time to play out at any frame rate.
## probes/probe_item_light_smooth.gd holds this rule, for all of its consumers, to
## the value the shader is actually handed.
func _ease_light(current: Vector4, target: Vector4, delta: float) -> Vector4:
	if LIGHT_EASE_TIME <= 0.0:
		return target
	return current.lerp(target, 1.0 - exp(-delta / LIGHT_EASE_TIME))


## The skin in use: the shared SkinManager's texture in game, so the body and the
## skin maker show the same editable pixels, and the exported fallback otherwise.
func _skin_albedo() -> Texture2D:
	var mgr = _manager()
	return mgr.get_texture() if mgr != null else skin_texture

# Minecraft-style head look: the head follows the player's LOOK direction
# (mouse yaw+pitch), never the camera. The camera is placed on the look ray in
# every view, so in the back view (mode 1) its forward coincides with the look
# ray and copying it was harmless — but the front view (mode 2) camera is
# yaw-flipped 180 degrees to look back at the face, so copying IT spun the
# head a full 180 and showed the back of the head. The vanilla model drives the head
# from yaw/pitch state instead, so we rebuild the look basis
# from the controller's yaw (its world basis) + pitch (the aim direction) and
# fold in the 180-degree offset that maps the model's +Z face onto the look
# ray. Unwinding the parent's global transform makes it work regardless of the
# model's scale, export flip, body-yaw lag or the player's yaw. Runs on the
# real (third-person) model and the pose clone alike. Nothing fights it: the
# body's clip turns its two arms and never the head, so this quaternion has one
# writer.
func _look_controller() -> Node:
	# The live model lives under PlayerController's ModelPivot, so its ancestor
	# chain has the controller. The pose clone is a detached copy under a plain
	# Node3D — it has no controller of its own, so fall back to the scene's
	# live Player node (whose look all these heads mirror).
	var n: Node = self
	while n != null:
		if n.has_method("get_aim_direction"):
			return n
		n = n.get_parent()
	var scene_root := get_tree().current_scene
	if scene_root != null:
		var player := scene_root.get_node_or_null("Player")
		if player != null and player.has_method("get_aim_direction"):
			return player
	return null

func _track_head_look() -> void:
	if _head == null:
		return
	var ctrl := _look_controller()
	var parent_q: Quaternion = _head.get_parent().global_transform.basis.get_rotation_quaternion()
	if ctrl != null:
		# Pitch from the aim direction expressed in the controller's own frame:
		# aim = ctrl_basis * rotX(pitch) * (0,0,-1), so local.y == sin(pitch) and
		# look_q = ctrl_q * rotX(pitch) reproduces the exact basis the camera gets
		# in modes 0/1 — view-independent by construction.
		var ctrl_q: Quaternion = (ctrl as Node3D).global_transform.basis.get_rotation_quaternion()
		var dir: Vector3 = ctrl.get_aim_direction()
		var local: Vector3 = (ctrl_q.inverse() * dir).normalized()
		var pitch := asin(clampf(local.y, -1.0, 1.0))
		var look_q: Quaternion = ctrl_q * Quaternion(Vector3.RIGHT, pitch)
		_head.quaternion = parent_q.inverse() * look_q * Quaternion(Vector3.UP, PI)
	else:
		# No gameplay controller (skin-maker preview, menu screens): follow the
		# viewport camera as before — those cameras orbit the model and never
		# get the yaw-flipped front-view treatment, so copying is correct here.
		var cam := get_viewport().get_camera_3d()
		if cam == null:
			return
		var cam_q: Quaternion = cam.global_transform.basis.get_rotation_quaternion()
		_head.quaternion = parent_q.inverse() * cam_q * Quaternion(Vector3.UP, PI)

func set_uv_overlay(enabled: bool) -> void:
	uv_overlay_enabled = enabled
	if enabled and _uv_overlay_mat == null:
		_uv_overlay_mat = ShaderMaterial.new()
		_uv_overlay_mat.shader = UV_OVERLAY_SHADER
		_uv_overlay_mat.set_shader_parameter("cells", 64.0)
	for mesh_instance in find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for surface_index in range(mi.mesh.get_surface_count()):
			var material := mi.get_surface_override_material(surface_index)
			if material == null:
				material = mi.mesh.surface_get_material(surface_index)
			if material == null:
				continue
			material.next_pass = _uv_overlay_mat if enabled else null

func apply_skin_texture():
	# In-game the skin lives in the shared SkinManager (autoload), so the
	# in-game model and the skin-maker preview show the same editable texture.
	# Without the manager, fall back to the base skin texture.
	var albedo := _skin_albedo()
	if _body_material != null:
		# In the world the body is one material of the world's own light model
		# (see _init_world_lighting), and the skin is a uniform of it: there is
		# no StandardMaterial3D here, and nothing of the engine's lighting for
		# one to be the albedo of.
		_body_material.set_shader_parameter("albedo_texture", albedo)
		return
	var mesh_instances = find_children("", "MeshInstance3D", true, false)
	
	for mesh_instance in mesh_instances:
		# Get the mesh to check surface count
		var mesh = mesh_instance.mesh
		if mesh == null:
			continue
			
		# Iterate through all surfaces
		for surface_index in range(mesh.get_surface_count()):
			# Get the current material
			var material = mesh_instance.get_surface_override_material(surface_index)
			
			if material == null:
				# If no override material, try to get the surface material from the mesh
				material = mesh.surface_get_material(surface_index)
			
			if material != null and material is StandardMaterial3D:
				# Apply the skin texture to the albedo texture
				material.albedo_texture = albedo
				# The skin is a tightly-packed 64x64 pixel-art atlas (like the
				# block textures), so use nearest filtering with no mipmaps.
				# Otherwise linear/mipmap filtering blends texels across
				# neighboring UV islands, causing the smeared/aliased look on
				# angled or minified (side) faces.
				material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
				# Create an override material if one doesn't exist
				if mesh_instance.get_surface_override_material(surface_index) == null:
					mesh_instance.set_surface_override_material(surface_index, material)

func set_paint_color(color: Color) -> void:
	paint_color = color

func paint_texel(uv: Vector2, color: Color) -> void:
	# Nearest sampling: the texel whose span contains the sample point.
	var px := clampi(int(uv.x * ATLAS_DIM), 0, ATLAS_DIM - 1)
	var py := clampi(int(uv.y * ATLAS_DIM), 0, ATLAS_DIM - 1)
	var mgr = _manager()
	if mgr != null:
		var old: Color = mgr.get_image().get_pixel(px, py)
		if mgr.set_pixel(px, py, color):
			texel_painted.emit(px, py, old, color)
		return
	_ensure_paint_texture()
	var old_color := _paint_image.get_pixel(px, py)
	if old_color.is_equal_approx(color):
		return
	_paint_image.set_pixel(px, py, color)
	_paint_texture.update(_paint_image)
	texel_painted.emit(px, py, old_color, color)

func undo_texel(px: int, py: int, color: Color) -> void:
	var mgr = _manager()
	if mgr != null:
		mgr.set_pixel(px, py, color)
		return
	_ensure_paint_texture()
	px = clampi(px, 0, ATLAS_DIM - 1)
	py = clampi(py, 0, ATLAS_DIM - 1)
	if _paint_image.get_pixel(px, py).is_equal_approx(color):
		return
	_paint_image.set_pixel(px, py, color)
	_paint_texture.update(_paint_image)

func _ensure_paint_texture() -> void:
	if _paint_texture != null:
		return
	var img := skin_texture.get_image()
	if img == null:
		img = Image.load_from_file("res://Assets/textures/skin.png")
	if img == null:
		return
	_paint_image = img.duplicate()
	_paint_texture = ImageTexture.create_from_image(_paint_image)
	# Swap every surface's albedo to the editable copy so the painted texels
	# show up across all UV islands at once.
	_swap_albedo_texture(_paint_texture)

# The currently painted image (guaranteed to be initialised from the base skin
# even if nothing has been painted yet) — the source of truth for saving.
func get_paint_image() -> Image:
	var mgr = _manager()
	if mgr != null:
		return mgr.get_image()
	_ensure_paint_texture()
	if _paint_image == null:
		return skin_texture.get_image()
	return _paint_image

# Replace the whole skin with a new image (e.g. loaded from a save). The shared
# texture is pointed at the new image so every model updates. Callers should
# reset undo history, which no longer matches the new pixels.
# Since saved skins are now clean (noise-free), this also resets the noise base
# so noise can be re-applied from the sidecar value after loading.
func load_skin_image(img: Image) -> void:
	if img == null:
		return
	var mgr = _manager()
	if mgr != null:
		mgr.set_from_image(img)
		# Reset noise base since the loaded image is clean (noise-free)
		if mgr.has_method("reset_noise_base"):
			mgr.reset_noise_base()
		_swap_albedo_texture(mgr.get_texture())
		return
	_paint_image = img.duplicate()
	_paint_texture = ImageTexture.create_from_image(_paint_image)
	_swap_albedo_texture(_paint_texture)

func apply_gray_noise(base: Image, noise_map: Image, amount: float) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("apply_gray_noise"):
		mgr.apply_gray_noise(base, noise_map, amount)

func set_noise(severity: float) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("set_noise"):
		mgr.set_noise(severity)

func fill_uv_rect(lo: Vector2, hi: Vector2, color: Color) -> void:
	var mgr = _manager()
	if mgr != null and mgr.has_method("fill_uv_rect"):
		mgr.fill_uv_rect(lo, hi, color)
		return
	fill_box_local(lo, hi, color)

func fill_box_local(lo: Vector2, hi: Vector2, color: Color) -> void:
	_ensure_paint_texture()
	if _paint_image == null:
		return
	var x0 := clampi(int(ceil(lo.x * ATLAS_DIM - 0.5)), 0, ATLAS_DIM - 1)
	var x1 := clampi(int(ceil(hi.x * ATLAS_DIM - 0.5)) - 1, 0, ATLAS_DIM - 1)
	var y0 := clampi(int(ceil(lo.y * ATLAS_DIM - 0.5)), 0, ATLAS_DIM - 1)
	var y1 := clampi(int(ceil(hi.y * ATLAS_DIM - 0.5)) - 1, 0, ATLAS_DIM - 1)
	if x1 < x0 or y1 < y0:
		return
	var changed := false
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			if not _paint_image.get_pixel(x, y).is_equal_approx(color):
				_paint_image.set_pixel(x, y, color)
				changed = true
	if changed and _paint_texture != null:
		_paint_texture.update(_paint_image)

func _swap_albedo_texture(tex: Texture2D) -> void:
	if _body_material != null:
		# Same reason as apply_skin_texture: in the world the albedo is a uniform
		# of the one material every surface of the body is drawn with.
		_body_material.set_shader_parameter("albedo_texture", tex)
		return
	for mesh_instance in find_children("", "MeshInstance3D", true, false):
		var mi := mesh_instance as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for surface_index in range(mi.mesh.get_surface_count()):
			var material := mi.get_surface_override_material(surface_index)
			if material == null:
				material = mi.mesh.surface_get_material(surface_index)
			if material is StandardMaterial3D:
				material.albedo_texture = tex
