extends Node3D

@export var skin_texture: Texture2D = preload("res://assets/textures/skin.png")

signal texel_painted(px: int, py: int, old_color: Color, new_color: Color)

const UV_OVERLAY_SHADER: Shader = preload("res://assets/shaders/skin_uv_overlay.gdshader")
const ATLAS_DIM := 64

# The world's own light model, the one the terrain, the dropped items and the
# first-person arm already run (assets/shaders/item_lighting.gdshaderinc): the light of
# the CELL this body is in, rather than the engine's sky and ambient, which know
# nothing about caves, torches or the hour. Only for a body standing in the world
# -- see _init_world_lighting for why a preview does not get it.
const ITEM_SHADER: Shader = preload("res://assets/shaders/item_shader.gdshader")
# The BODY's shader: the same light model, drawn OPAQUE (assets/shaders/body_shader.gdshader).
# A body is closed boxes, and ITEM_SHADER above writes ALPHA -- for a sprite's silhouette and a
# dropped item's fade -- which is what makes Godot compile it into the TRANSPARENT pipeline,
# where a body cannot be automatically instanced, cannot cast, and is depth-sorted rather than
# depth-tested. Every body wears this one; the thing held in a hand keeps ITEM_SHADER.
const BODY_SHADER: Shader = preload("res://assets/shaders/body_shader.gdshader")
# The cell light is one value per block, so a body crossing a boundary -- or a
# cell being relit under it -- steps a whole cell's worth on a single frame. This
# is the time constant of the fade that turns that step into a move. The same
# rule, and the same number, as the other two consumers;
# probes/probe_item_light_smooth.gd holds all of them to it.
const LIGHT_EASE_TIME := 0.15

# The body's two clips, and the only two it plays: `Idle.anim` while it stands and
# `Run.anim` while it walks. They are resources of their own (assets/Animations)
# rather than the model's imported clips, because a glb's clips only ever animate
# the glb's own scene -- and `Run.anim` was authored on a differently named rig,
# which is what CLIP_PATH_REMAP is for.
const CLIP_IDLE: Animation = preload("res://assets/animations/Idle.anim")
const CLIP_RUN: Animation = preload("res://assets/animations/Run.anim")
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

# The item in the third-person hand.
#
# It is a child of the rig's right arm, which is the whole reason it works: the walk's
# swing, the idle's hang and the body's own yaw all turn that node, so a grip parented to
# it is carried by every one of them without a line of code. It is under PlayerModel, so
# the F5 cycle shows it in both third-person views and hides it with the body in first
# person -- visibility is inherited, and an item cannot be seen when its parent is not.
#
# Which arm: the rig's `arm` is the -X side, and the body faces +Z (`arm2` is the left),
# so `arm` is the right hand -- the hand Minecraft holds an item in.
const HELD_ARM := "arm"
# The arm's own parent, inserted between the rig and the arm at load (see _init_held_arm),
# with the swing's pivot (SWING_PIVOT) between the two.
const HELD_ARM_PIVOT := "HeldArmPivot"
# The arm's pose while it carries something: 18 degrees FORWARD (pi/10), the way vanilla
# holds an item. It is a turn at the SHOULDER, composed OVER whatever pose the arm is in,
# which is why it lives on the pivot rather than on the arm: the AnimationTree owns the
# arm's own local rotation and rewrites it every frame, so a turn written there is replaced
# next frame (or, with the tree inactive, compounds).
#
# The sign is this rig's: the body faces +Z and the arm hangs -Y, so a rotation of -18
# degrees about X is the one that brings the hand forward. Not an inference to trust -
# probes/probe_held_pose.gd measures the held item's own move through it.
const HELD_ARM_PITCH := 18.0
# ...and the other half of the same rule, which is a SCALING rather than a turn: vanilla
# halves the arm's walk swing while that hand holds something, and only then takes the 18
# degrees off (`swing * 0.5 - pi/10`). The turn is a fixed pose and lives on the pivot
# above; the half has to be inside the walk itself, or the item is swung on the full arm --
# 49 degrees at a walk against vanilla's 25, twice the stride, and the item's arc is what
# shows it. So the half is written into the run clip's arm track, which carries the walk
# and nothing else (the idle's hang is added over it unhalved, which is vanilla's order
# too). One constant, because the two halves of the rule are one rule.
const HELD_ARM_SWING := 0.5

# The main-hand SWING: the punch and the block place, the stroke the first-person hand has
# always had, on the third-person body. The triggers are not re-read here -- viewmodel.gd
# owns every one of them (a click, a placed block, a crafting table, an item that swaps
# itself, the break loop) and says so with one signal, `swung(strength)`, which this body
# plays. See _apply_hand_swing for the arc and _connect_swing for the wire.
#
# The clock and the weight are the first-person hand's own (viewmodel.gd: SWING_SECONDS,
# SWING_PUNCH, SWING_PLACE), restated here rather than shared, because the two hands are
# separate nodes and the numbers are a POSE's, not a rule: a body that swung slower than the
# hand in front of the eye would read as a delay.
const SWING_PIVOT := "SwingPivot"
const HAND_SWING_SECONDS := 0.225
# Where the first-person hand's swing lives from the controller that punches it -- the same
# path player_controller_interact.cpp calls punch() on.
const VIEWMODEL_PATH := "Camera3D/Viewmodel"
# The arc, in degrees, at the far end of a FULL punch: the arm comes forward and up (a turn
# about X, composed over the 18-degree held turn it also wears) and a little across the body
# (about Z), which is what makes it read as a swing rather than a hinge. Ours to tune, off
# the reference behaviour: an arm goes from the side to roughly eye level and back.
const HAND_SWING_PEAK := 78.0
const HAND_SWING_SWEEP := 14.0
# The stroke's shape (see swing_amount): the strike takes this share of HAND_SWING_SECONDS,
# the return has landed by this one, and what is left is the overshoot -- this much of the
# peak, PAST rest, before the arm settles.
const HAND_SWING_STRIKE := 0.32
const HAND_SWING_RETURN := 0.80
const HAND_SWING_OVERSHOOT := 0.10
# data/items.json's `pose` field, through the ONE reader this body and the first-person
# viewmodel share (scripts/item_poses.gd): an item's pose is a claim about its ART rather than
# an item mechanic, so it is not in the registry -- and it is not read twice either.
const ItemPoses := preload("res://scripts/item_poses.gd")
# How big the item is, as a fraction of the ARM's length rather than in model units: 0.855712
# of a block (vanilla's 0.4, grown 1.3x, again 1.6x, down 15%, then up 10%, and 10% again)
# against this rig's 0.675-block arm, so 0.855712 / 0.675. Measured off the arm's own box at load, so a
# re-modelled or re-scaled arm keeps the proportion instead of leaving a stale number behind.
const HELD_ITEM_ARM_LENGTHS := 0.855712 / 0.675
# A BLOCK has a size of its own: 0.3588 of a block, and the item's growth does not move it.
# A cube reads bigger than a sprite at the same figure -- it has volume where a sprite is a
# card -- so the two are separate numbers rather than one scaled from the other.
const HELD_BLOCK_ARM_LENGTHS := 0.3588 / 0.675
# An `item2` sprite's size, and it is the figure EVERY sprite had before item1's tuning began:
# 0.52 of a block (vanilla's 0.4, grown 1.3x). Item1's growth steps belong to item1, so its
# siblings keep the old size and follow the item1 knobs no further than that.
const HELD_ITEM2_ARM_LENGTHS := 0.52 / 0.675
# How far above the hand a BLOCK rides, as a fraction of its own height: centred on the
# hand's line its middle sat in the fist and half of it hung below. One sixth lifts the cube
# so the grip lands on its lower third, and the sprite path does not use it.
const HELD_BLOCK_RISE := 1.0 / 6.0
# How far a BLOCK is pulled back toward the arm from that flush position, written in blocks
# and converted to arm lengths here: 0.05 of a block, so its near corner sits that far inside
# the palm instead of on it. Ours to tune.
const HELD_BLOCK_BACK := 0.05 / 0.675
# How a held thing is turned in the hand. A sprite's biggest face is its picture, and a
# picture standing on edge -- a bucket in the hand read as a card pasted on the air -- is
# what this is for: a quarter turn about the arm's cross axis lays it flat, its picture
# facing DOWN into the palm and its top edge along the body's facing, which is what an
# item lying in a hand looks like. Every sprite gets that much.
const HELD_SPRITE_TURN := Vector3(90.0, 0.0, 0.0)
# ...and a default item (one that names no pose in data/items.json) is turned further: half a
# turn about that same cross axis, then a quarter turn about the vertical, then 135 degrees
# about Z, the sprite's thickness axis (three quarters). In the node's YXZ order the three are
# applied Z, X, Y, so the card is spun in its own plane, laid flat and flipped, then swung
# about the vertical -- the knobs that turn the artwork in the palm. An `item3` sprite takes
# this preset and the item1 size and seat whole.
const HELD_ITEM_TURN := Vector3(180.0, 90.0, 135.0)
# A block is TURNED 45 degrees about Y, the vertical: square to the body its back face sat
# flat against the palm, so it read as a placed block glued to the hand. The turn carries it
# by a corner instead, which is how a hand holds a cube.
const HELD_BLOCK_TURN := Vector3(0.0, 45.0, 0.0)
# How far in FRONT of the palm a sprite sits, in arm lengths. A flat item has almost no
# depth of its own, so without this it reads as lying on the hand rather than held clear
# of it. One number, ours to tune; the block path does not use it (a block is already
# placed corner-on-palm).
const HELD_SPRITE_FORWARD := 0.48
# How far ABOVE the hand a sprite sits, in blocks and converted to arm lengths: flat on the
# hand's own line it read as resting on the wrist rather than held. Ours to tune.
const HELD_SPRITE_RISE := 0.05 / 0.675
# The `item2` sprites (data/items.json: the bucket family, the torch, the ingot) keep the plain
# flat turn -- no Y spin -- and their seat in the hand is its own pair of numbers, tuneable
# separately from a default item's. They sit where they sat before the preset existed: the
# forward figure is written out rather than following HELD_SPRITE_FORWARD, so moving a default
# item's hand does not drag them along unasked.
const HELD_ITEM2_FORWARD := 0.22
const HELD_ITEM2_RISE := HELD_SPRITE_RISE
# A body whose mesh looks down its own -Z instead of +Z (the goblin's does) turns every
# held thing the other way round. The seat is written in the ARM's space in _seat, so the
# hand's face is the arm box's other end and the artwork has to be spun with it: one
# 180-degree turn about the vertical, which in this node's YXZ order mirrors both the
# card's top edge and the sideways centring. Without it the item is drawn behind the hand
# and its sprite upside down -- probes/probe_goblin.gd holds the seated position to the
# face side of the mesh.
const HELD_MIRROR_YAW := 180.0

var uv_overlay_enabled := false
var paint_color := Color.WHITE
# When true the head never rotates (set by the K-key pose clone so the dummy
# is fully frozen instead of tracking the player's aim).
var skip_head_look := false

## The MAIN hand held UP in front of the face: the goblin's loot inspection
## (scripts/goblin.gd). It takes one item, raises that arm until the item is in front of its
## face, and stands looking at it for three seconds.
##
## The body is NOT frozen for it. The clips keep playing underneath, because a body stopped
## dead is a body caught mid-stride -- the goblin arrives walking and would stand there with
## its legs halfway through a step for three seconds. What is stopped is the arm's own motion:
## the arm is written from the clips' own rest (`_clip_rest`, the base every walk key is
## measured from) AFTER the tree has stepped, which is the only ordering that lets the legs
## animate and the arm be held in the same frame (TREE_PRIORITY). Releasing hands the arm to
## the clips where they got to, so nothing compounds.
##
## Why it cannot be skipped: an item hanging at a goblin's side is about 70 degrees below its
## head, and its neck pitches 40 (goblin_model.gd's HEAD_PITCH_LIMIT) -- physically outside
## the neck's reach, so "looks at it" only exists with the arm up.
var inspect_pose := false
## 1 while the item is being looked at, eased to 0 after the pose is switched off (see
## INSPECT_LOWER_SECONDS): the arm's own angle is this share of the inspection's, and the
## shoulder's held turn is its complement, so the arm goes down instead of jumping.
var _inspect_share := 0.0
## How long the arm takes to come back DOWN when the pose is switched off: the inspection
## hands the arm to the clips at 80 degrees, and handing it over in one frame is a teleport.
## The clips keep playing under it, and only the arm is on the way down.
const INSPECT_LOWER_SECONDS := 0.28

## Where the AnimationTree sits in the frame's process order: BEFORE this body, since a lower
## priority is processed first. That is what makes an arm pose written in `_process` land on
## top of the pose the clips have just produced instead of being overwritten by it -- the
## inspection needs both, a walking body and a held arm, in one frame.
const TREE_PRIORITY := -1

## How far that arm comes up, in degrees. The caller's number (`set_inspect_pose`): how far an
## inspection lifts the hand is the behaviour's to say rather than the rig's.
var inspect_pitch := 0.0

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

# The held item (see HELD_ARM): the arm it hangs from and the box that arm's own mesh
# occupies in the arm's space -- the grip is measured off it, not written down. Its
# material is the body's own light model on the held block's or item's texture, and its
# meshes are built by the shared ViewmodelMeshes binding, once per block id.
var _held_arm: MeshInstance3D = null
# The arm's parent, and the shoulder the held pose turns (_init_held_arm).
var _held_pivot: Node3D = null
# The swing's own pivot, inside the shoulder's: everything the arm already does -- the walk,
# the idle's hang, the 18-degree held turn, an inspection -- keeps its own writer, and this
# one only adds the stroke (see _apply_hand_swing).
var _swing_pivot: Node3D = null
# The swing's clock, 1 at the click and 0 when it has settled -- the first-person hand's own
# shape of timer -- and the strength the swing that set it asked for (1 punch, 0.75 place).
var _hand_swing := 0.0
var _hand_swing_strength := 1.0
var _held_arm_box := AABB()
var _held_controller: Node = null
var _held_item: MeshInstance3D = null
var _held_material: ShaderMaterial = null
var _held_meshes := {}
var _held_id := -1
# The OFF hand (the goblin's torch): a second holder, built on first use by hold_offhand
# because the player's own body has none to build -- see hold_offhand. Keys: "arm",
# "box", "item", "material", "facing".
var _offhand := {}
# The MAIN hand a MOB uses -- the goblin's loot takings and the tool it keeps. The player's
# own hand is `_held_item` above, which reads a hotbar that only a body with a controller
# has, so a mob's main hand is a holder of its own: the same shape as `_offhand`, built on
# first use by hold_main, sharing `_seat` for the grip.
var _main_hand_item := {}
# Whether the run clip's arm track is carrying the held arm's half swing (see
# HELD_ARM_SWING). The toggle is a state, not a per-frame factor: 0.5 and 2.0 are exact
# powers of two, so a hand that fills and empties is back at the keys the file has.
var _held_swing_half := false

func _manager():
	return get_node_or_null("/root/SkinManager")

func _ready():
	# The light model first: it decides what the body is actually drawn WITH, and
	# apply_skin_texture has to know that before it picks where the skin goes. It also
	# measures the body's own middle off the parts as the model places them, so the
	# shoulder pivot goes in after it.
	_init_world_lighting()
	apply_skin_texture()
	# ...and before the clips: _clip_for_rig builds its track paths with get_path_to()
	# at load, and the right arm lives under the pivot from here on.
	_init_held_arm()
	_init_body_animation()
	_head = find_child("head", true, false)
	_init_held_item()
	# ...and last, because the hand it listens to is a node of the PLAYER's, which is beside
	# this body rather than above it (see _connect_swing).
	_connect_swing()


## The two clips that turn this body's parts.
##
## The clips themselves are the RIG's, not this body's: a retarget, the run clip's fit to
## vanilla's amplitudes and the rest pose's own keys come out the same for every body of one
## rig, so they are built once and shared (_clip_library). What is this body's is the library
## around them, the player and the tree -- the pose is per body by definition.
func _init_body_animation() -> void:
	_capture_clip_base()
	var library := _clip_library().duplicate()
	_clip_player = AnimationPlayer.new()
	_clip_player.name = "Clips"
	_clip_player.add_animation_library("", library)
	add_child(_clip_player)
	_tree = AnimationTree.new()
	_tree.name = "ClipsTree"
	_tree.tree_root = _build_tree()
	add_child(_tree)
	_tree.process_priority = TREE_PRIORITY
	# Both nodes are in the tree before this: the path is between them, and the tree
	# finds the animations by name through it.
	_tree.anim_player = _tree.get_path_to(_clip_player)
	# The idle layer is added at full, always (see TREE_BLEND): it is a pose the walk
	# wears, not a state the body switches into.
	_tree.set(TREE_ADD, 1.0)


## The clips one rig's bodies share, and the run clip's halved variant beside them.
##
## Keyed by the rig's own shape (_rig_clip_key), so a body of a different rig -- the player's
## glb against a goblin's obj, or a re-modelled one -- never inherits another's clips. A goblin
## crowd used to pay the retarget, the cosine fit over every run key and the vanilla rescale
## PER BODY, for a result that cannot differ.
static var _clip_cache := {}


## The library one rig's bodies play, built once: 'idle', 'run' (the walk at vanilla's full
## swing) and 'rest' (the pose the walk's blend fades to). The returned library is the CACHED
## one -- walk it, do not change it: a body gets its own copy of the entry table
## (_init_body_animation), which is what lets one body swap its run clip without touching the
## crowd's (see _set_held_swing).
func _clip_library() -> AnimationLibrary:
	var key := _rig_clip_key()
	if _clip_cache.has(key):
		var cached: Dictionary = _clip_cache[key]
		_run_clip = cached["run"]
		return cached["library"]
	var library := AnimationLibrary.new()
	library.add_animation("idle", _clip_for_rig(CLIP_IDLE, "Idle"))
	_run_clip = _clip_for_rig(CLIP_RUN, "Run")
	library.add_animation("run", _run_clip)
	library.add_animation("rest", _rest_clip())
	_prepare_run_clip()
	_clip_cache[key] = {"library": library, "run": _run_clip}
	return library


## What tells one rig's clips from another's: the parts a clip can name, the path each is
## reached by (the held arm hangs under its shoulder pivot) and the rest rotation every key is
## measured from. Two bodies of the same model answer the same string, which is the whole point.
func _rig_clip_key() -> String:
	var key := ""
	for part in RIG_PARTS:
		var rest: Quaternion = _clip_rest.get(part, Quaternion.IDENTITY)
		key += "%s=%s|" % [_part_path(part), str(rest)]
	return key


## The rig's own run clip -- the walk at vanilla's FULL swing -- which `_run_clip` is not
## pointing at while the held arm's halved variant is installed.
func _run_clip_full() -> Animation:
	return _clip_cache[_rig_clip_key()]["run"]


## The same clip with the held arm's swing halved (HELD_ARM_SWING), built once per rig and
## kept: the halved clip is a variant of the walk, not a body's state.
func _run_clip_halved() -> Animation:
	var entry: Dictionary = _clip_cache[_rig_clip_key()]
	if entry.has("run_half"):
		return entry["run_half"]
	var arm_name := _held_arm_name()
	var half: Animation = (entry["run"] as Animation).duplicate()
	half.resource_name = "RunHalf"
	ClipRetarget.scale_stride(half, {_part_path(arm_name): _clip_rest.get(arm_name, Quaternion.IDENTITY)},
		HELD_ARM_SWING)
	entry["run_half"] = half
	return half


## Put one run clip in THIS body's library and remember it as the one playing. The library is
## the body's own (see _clip_library) and `add_animation` replaces an entry of the same name, so
## this is one dictionary write -- never a shared clip rewritten under a crowd of bodies.
func _install_run_clip(clip: Animation) -> void:
	_run_clip = clip
	if _clip_player == null:
		return
	var library := _clip_player.get_animation_library("")
	if library != null:
		library.add_animation("run", clip)


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
	var map := CLIP_PATH_MAP.duplicate()
	# The right arm is not at the body root any more: it hangs from its shoulder pivot
	# (_init_held_arm), so the clip's `arm` track is re-homed onto the path it has now.
	map[HELD_ARM] = _part_path(HELD_ARM)
	# ...and the same for the arm THIS body holds things in, whatever the clip calls it: the
	# map's values are the rig's node names, and a reparented limb is no longer reachable by
	# its own name (a goblin's `arm_001` track names `arm2`, which now hangs from a pivot).
	for from in map.keys():
		var part := String(map[from])
		if find_child(part, true, false) != null and not has_node(NodePath(part)):
			map[from] = _part_path(part)
	var clip := ClipRetarget.retarget(source, map, self, 1.0, Quaternion.IDENTITY,
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


## The TRACK PATH a part's clip track has on this body -- the path from the body root,
## which is what a clip names (the AnimationPlayer's root node is this body). It is not
## always the part's own name: the right arm sits under its shoulder pivot.
func _part_path(part: String) -> String:
	var node := find_child(part, true, false)
	return String(get_path_to(node)) if node != null else part


## The inverse of _part_path: the part a track path lands on, read off the node rather
## than off the text. Every table in this file is keyed by part, so this is what ties a
## clip's tracks to them.
func _part_of_path(path: String) -> String:
	var node := get_node_or_null(NodePath(path))
	return String(node.name) if node != null else path


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
	# A clip names each limb by its TRACK PATH, while every table here is keyed by part
	# (and the right arm's path is under its pivot now), so the path-keyed view these two
	# retarget calls want is built once.
	var rest_by_path := {}
	for part in _clip_rest:
		rest_by_path[_part_path(part)] = _clip_rest[part]
	# 1. the base every key is measured from is already captured (_capture_clip_base).
	# 2. close the cycle (clip_retarget.gd::smooth_cycle): the authored easing makes
	# the legs crawl through the loop point, which at vanilla amplitude is a hitch at
	# every wrap.
	ClipRetarget.smooth_cycle(_run_clip, rest_by_path)
	# 3. vanilla's amplitudes, measured off the SMOOTHED curve, so the peaks are the ones
	# that will actually play.
	var gains := {}
	for t in _run_clip.get_track_count():
		if _run_clip.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		var path := str(_run_clip.track_get_path(t))
		var part := _part_of_path(path)
		if not MC_PART_SWING.has(part) or not _clip_rest.has(part):
			continue
		var rest: Quaternion = _clip_rest[part]
		var peak := 0.0
		for k in range(_run_clip.track_get_key_count(t)):
			var key: Quaternion = _run_clip.track_get_key_value(t, k)
			peak = maxf(peak, (rest.inverse() * key).get_angle())
		if peak > 0.001:
			gains[path] = float(MC_PART_SWING[part]) / peak
	if not gains.is_empty():
		ClipRetarget.scale_stride(_run_clip, rest_by_path, 1.0, gains)


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
		# By the node the path lands on, not by the path: the right arm's track names
		# "HeldArmPivot/SwingPivot/arm" (see _init_held_arm), and a rest track added for
		# it anyway would be a second writer of its rotation.
		named[_part_of_path(str(clip.track_get_path(i)))] = true
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
	# First, because it is a parent of the arm: a stand-in that mirrors a batched arm (below)
	# has to be told where the arm ended up this frame.
	_apply_hand_swing(delta)
	# Before anything is drawn: the clips have already turned the arms (TREE_PRIORITY), and a
	# hand whose arm is drawn by a batch hangs from a stand-in that has to be told where it is
	# (`_hand_parent`).
	_sync_hand_mirrors()
	# Before the head-look guard: the pose clone sets skip_head_look, and it is
	# standing in the world like any other body, so it is lit like one.
	_push_world_lighting(delta)
	# Beside the body's own light, because the item is drawn with the same value.
	_update_held_item()
	# ...and the offhand's, which has no hotbar to read but the same material rule.
	_update_offhand_light()
	# ...and the main hand's, which a goblin fills from the ground rather than from a hotbar.
	_update_main_hand_light()
	if skip_head_look:
		# ...and a pose clone is frozen: no head look AND no body clip. The tree is
		# stopped rather than merely skipped, or a body frozen after it had been
		# walking would keep walking (and one frozen while standing would keep
		# breathing).
		if _tree != null and _tree.active:
			_tree.active = false
		_clip = ""
		return
	# A body inspecting something holds the main hand up (see `inspect_pose`) while its clips
	# keep playing: the walk settles out of the stride it arrived in and the body breathes, and
	# the arm is written over that pose afterwards. The head STILL tracks: it is looking at
	# what it holds.
	if inspect_pose or _inspect_share > 0.0:
		_animate_body(delta)
		_hold_inspect_pose(delta)
		_track_head_look()
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

	_body_material = _make_body_material()
	for mi in _body_meshes:
		for surface in range((mi.mesh as Mesh).get_surface_count()):
			mi.set_surface_override_material(surface, _body_material)
	# The middle of the body, from its own geometry rather than a constant: the
	# light is read at that height (see _push_world_lighting).
	_body_height = _body_centre_height()


## The material every one of this body's parts is drawn with.
##
## A body's own material: its skin is a texture SkinManager can repaint, so two bodies must not
## share one. A MOB's body overrides this and answers a material shared by every mob of its kind
## (scripts/goblin_model.gd): the same material RID on the same six mesh RIDs is what lets
## Forward+ draw a crowd of them as one instanced draw per part, and a goblin's picture is not
## repaintable.
func _make_body_material() -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = BODY_SHADER
	material.set_shader_parameter("albedo_texture", _skin_albedo())
	# Closed opaque boxes: there is no sprite texel here to cut away.
	material.set_shader_parameter("alpha_scissor", 0.0)
	return material


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
	_feed_body_material()
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


## The frame each body material was last fed the world's light uniforms on.
##
## A mob's bodies share ONE material (scripts/goblin_model.gd), so feeding it from every body
## would be the same hundred writes a frame; the frame number makes it one. Keyed by the
## material's instance id, and a body that owns its material drops its entry on the way out
## (_exit_tree), so the table holds one entry per live body material.
static var _fed_frame := {}


## The world's light uniforms onto this body's material, at most once a frame per material.
func _feed_body_material() -> void:
	_feed_material(_body_material)


## The same, for any material of the body's: a batched mob's crowd is one more material
## (scripts/goblin_model.gd), and it wants the same once-a-frame feed.
func _feed_material(material: ShaderMaterial) -> void:
	if material == null or _chunk_manager == null:
		return
	var key := material.get_instance_id()
	var frame := Engine.get_process_frames()
	if int(_fed_frame.get(key, -1)) == frame:
		return
	_fed_frame[key] = frame
	_chunk_manager.apply_item_lighting(material)


## Drop this body's material entry when it leaves: a body that owns its material takes it with
## it, and the id it left behind would otherwise sit in the table for the session. A shared one
## (a mob's crowd) only loses a frame stamp, and the next body re-feeds it in the same frame.
func _exit_tree() -> void:
	if _body_material != null:
		_fed_frame.erase(_body_material.get_instance_id())


## The shoulder pivot the held-item pose turns (see HELD_ARM_PITCH).
##
## A turn at the shoulder has to be composed OVER the arm's pose, and the arm's own local
## rotation belongs to the AnimationTree, which rewrites it every frame -- so the turn
## goes on a parent instead: a Node3D at the arm's own position, with the arm's position
## zeroed onto it and its rotation left exactly where the model put it. Turning that
## pivot composes with the tree's pose (and the held item, a child of the arm, follows by
## itself), and _clip_rest["arm"] is still the base the walk's keys are measured from.
##
## Runs before _init_body_animation, because _clip_for_rig builds its track paths with
## get_path_to() at load: the arm's path has to be final first, which is why every
## path-vs-part comparison here goes through _part_path/_part_of_path. Every body gets
## the pivot, on the arm THAT body holds things in (`_held_arm_name`): the player's right
## hand is `arm`, a goblin's is `arm2`, because the two meshes face opposite ways.
func _init_held_arm() -> void:
	var arm := find_child(_held_arm_name(), true, false) as MeshInstance3D
	if arm == null or arm.get_parent() == null:
		return
	_held_arm = arm
	var pivot := Node3D.new()
	pivot.name = HELD_ARM_PIVOT
	_held_pivot = pivot
	var parent := arm.get_parent()
	parent.add_child(pivot)
	# The pivot takes the arm's seat; the arm is centred on the joint and keeps its own
	# rotation, so the body is drawn exactly where it was and a turn of the pivot is a
	# turn of the shoulder.
	pivot.position = arm.position
	# ...and the swing pivot inside it, at the same joint and at zero: the arm hangs from
	# THAT, so the stroke is composed under the held turn and over whatever pose the clips
	# give the arm (see _apply_hand_swing). It goes in here, before _init_body_animation,
	# because the arm's track path is resolved at load -- `HeldArmPivot/SwingPivot/arm`.
	var swing := Node3D.new()
	swing.name = SWING_PIVOT
	pivot.add_child(swing)
	_swing_pivot = swing
	arm.reparent(swing, false)
	arm.position = Vector3.ZERO


## Listen to the hand that owns the swing's triggers.
##
## The third-person body does not read the mouse, and must not: every trigger that kicks the
## first-person hand -- a click, a placed block, a crafting table, an item that swaps itself,
## the break loop -- is already wired there, and `swung(strength)` is that hand saying so. A
## body the player does not drive (a preview, the K-key pose clone, a mob) has no controller
## above it and therefore never hears anything, which is right: an arm swings because its own
## player did something.
func _connect_swing() -> void:
	if _held_controller == null:
		return
	var hand := _held_controller.get_node_or_null(VIEWMODEL_PATH)
	if hand == null or not hand.has_signal("swung"):
		return
	if not hand.swung.is_connected(_on_swung):
		hand.swung.connect(_on_swung)


## A swing arrived: start the stroke from the top, at the weight it asked for.
##
## A second click mid-swing RESTARTS rather than stacking, because the stroke is a function of
## one clock and one weight -- there is nothing to add up and no rate to raise. The weight is
## taken from the newest swing, so a place during a punch is the weaker stroke at once.
func _on_swung(strength: float) -> void:
	_hand_swing = 1.0
	_hand_swing_strength = clampf(strength, 0.0, 1.0)


## The stroke's shape: how far out the arm is, as a share of its far end, `u` being how much
## of HAND_SWING_SECONDS has gone by (0 at the click, 1 when it has settled).
##
## Three pieces, and each is there for a reason a single formula did not give:
## the STRIKE is a cubic ease-out, so the hand leaves fastest on the frame of the click and
## lands softly at the top; the RETURN is a cosine, so it leaves the top with no corner and
## arrives at rest with none either; and the OVERSHOOT is a half sine on the last stretch,
## which takes the arm a little PAST rest and back -- the weight at the end of a swing, and
## the difference between a hand that arrived and a hand that faded out. Zero at 0 and at 1,
## so a finished swing is exactly the pose the arm was in before it.
static func swing_amount(u: float) -> float:
	u = clampf(u, 0.0, 1.0)
	if u < HAND_SWING_STRIKE:
		var out := u / HAND_SWING_STRIKE
		return 1.0 - pow(1.0 - out, 3.0)
	if u < HAND_SWING_RETURN:
		var back := (u - HAND_SWING_STRIKE) / (HAND_SWING_RETURN - HAND_SWING_STRIKE)
		return 0.5 + 0.5 * cos(PI * back)
	var settle := (u - HAND_SWING_RETURN) / (1.0 - HAND_SWING_RETURN)
	return -HAND_SWING_OVERSHOOT * sin(PI * settle)


## One frame of the main-hand swing, written on the pivot between the shoulder and the arm.
##
## A PARENT pivot rather than a turn on the arm, for the reason the held pose has one: the
## AnimationTree owns the arm's local rotation and rewrites it every frame, so a write there
## is replaced next frame (and compounds with the tree stopped). A parent composes with
## everything the arm already does -- the walk's swing, the idle's hang, the 18-degree held
## turn -- and being the arm's parent it carries the held block or item with it, which is why
## there is no item code here at all.
##
## "Forward" is the MESH's, exactly as the held turn's and the inspection's signs are: a +Z
## rig comes forward on a NEGATIVE turn about X, a -Z mesh on the same turn the other way. The
## sweep takes the opposite sign for the same reason -- it crosses toward the body's own
## middle, and which way that is falls out of which hand is holding.
func _apply_hand_swing(delta: float) -> void:
	if _swing_pivot == null:
		return
	_hand_swing = maxf(_hand_swing - delta / HAND_SWING_SECONDS, 0.0)
	var amount := swing_amount(1.0 - _hand_swing) * _hand_swing_strength
	var towards := _inspect_sign()
	_swing_pivot.rotation_degrees = Vector3(HAND_SWING_PEAK * towards * amount, 0.0,
		-HAND_SWING_SWEEP * towards * amount)


## The block or item the body carries in its right hand.
##
## Only a body the PLAYER is driving holds one. A preview has no cell to take a light
## from (the skin maker and the settings gallery are SubViewports, where there is no
## `_body_material`), and the K-key pose clone has no controller above it -- both are
## bodies of their own, and neither has a hotbar to read. A MOB holds things too, but not
## through this path: its offhand is its own roll, and `hold_offhand` builds that holder
## on first use (scripts/goblin_model.gd is the caller).
##
## The item is a child of the arm itself, so it is carried by the walk, the idle's hang
## and the body's yaw for free, and it is under PlayerModel, so the F5 cycle reveals it in
## both third-person views and hides it, with the body, in first person.
func _init_held_item() -> void:
	if _body_material == null:
		return
	var owner_node: Node = self
	while owner_node != null and not owner_node.has_method("get_aim_direction"):
		owner_node = owner_node.get_parent()
	if owner_node == null:
		return
	var arm := _held_arm
	if arm == null or arm.mesh == null:
		return
	_held_controller = owner_node
	# The arm's own mesh box, in the arm's space -- the space its children are placed
	# in. The rebake put this node's origin on the shoulder, so the box runs from the
	# joint down the limb.
	_held_arm_box = (arm.mesh as Mesh).get_aabb()
	_held_material = ShaderMaterial.new()
	_held_material.shader = ITEM_SHADER
	_held_item = MeshInstance3D.new()
	_held_item.name = "HeldItem"
	_held_item.material_override = _held_material
	_held_item.visible = false
	_held_arm.add_child(_held_item)


## Follow the hotbar: the selected slot's block, built and placed in the hand when it
## changes, and drawn in the light of the cell the BODY is in (the one _push_world_lighting
## already eased) so a torch in the hand and the arm holding it can never disagree.
##
## Reading the slot is two calls a frame, and the mesh is rebuilt only when the id does
## change -- a held block is one mesh, and every block id is cached as it is first held.
func _update_held_item() -> void:
	if _held_item == null or _held_controller == null:
		return
	var slot := int(_held_controller.get_selected_hotbar_slot())
	var id := int(_held_controller.get_hotbar_slot_block_id(slot))
	if id != _held_id:
		_held_id = id
		_hold(id)
	if not _held_item.visible:
		return
	# The albedo every frame, not once per id: the texture array can be regenerated
	# under a held block (the Block Maker), and the first-person hand re-reads it for
	# the same reason. The mesh is the thing worth caching; a uniform write is not.
	var tex: Texture2D = BlockTextures.get_texture(_held_id)
	if tex != null:
		_held_material.set_shader_parameter("albedo_texture", tex)
	_chunk_manager.apply_item_lighting(_held_material)
	_held_item.set_instance_shader_parameter("item_light", _body_light)


## Put one block id in the hand (0 or a texture-less id empties it).
##
## The seat itself is `_seat`, shared with the offhand so the grip maths exists once; this
## only adds what belongs to the MAIN hand -- the shoulder pose and the halved walk swing
## (`_pose_held_arm`), which an offhand item must not get.
func _hold(id: int) -> void:
	if not _seat(_main_hand(), id):
		_pose_held_arm(false)
		return
	# Beside the item, from the one place its visibility is decided, so the arm and the
	# thing in it can never disagree about whether a hand is full.
	_pose_held_arm(true)


## The main hand as `_seat` wants it: the arm `_init_held_arm` put the pivot on, its own
## mesh box, the holder and material `_init_held_item` built, and this rig's facing (+1:
## the body looks down its own +Z, which is what `_seat` mirrors for a body that does not).
func _main_hand() -> Dictionary:
	return {
		"arm": _held_arm,
		"box": _held_arm_box,
		"item": _held_item,
		"material": _held_material,
		"facing": 1.0,
	}


## Put one block id in one hand: the mesh, the size, the turn and the seat in the palm.
##
## `slot` is a holder (`_main_hand` or the offhand built by `_init_offhand`): the arm it
## hangs from and that arm's own mesh box, the MeshInstance3D drawn, the material it is
## drawn with, and the body's facing. What a hand then does with its POSE is the caller's
## business -- see `_hold` and `hold_offhand`.
##
## The geometry is the shared ViewmodelMeshes binding, the same one the first-person hand
## and the dropped items build from -- an item as the extruded sprite, a block as its own
## shape's boxes (so a slab is held as a slab and a stair as its two boxes).
##
## The grip is MEASURED off the arm's box: the end of the limb (its bottom, since the node
## sits on the shoulder) on the face the body looks along, which is the hand. The item
## is centred there, X and Y both, and pushed out by half its own depth so its back rests
## against the palm instead of inside the forearm -- one number per mesh, because a block
## is deeper than an item's sprite.
##
## `facing` is the body's own: +1 for a rig that looks down its own +Z, -1 for a mesh that
## looks down -Z (the goblin). The seat is written in the ARM's space, so the other way
## round is the arm box's other end as the hand, and the artwork turned with it
## (`HELD_MIRROR_YAW`) -- otherwise the item is drawn behind the hand and a sprite upside
## down.
func _seat(slot: Dictionary, id: int) -> bool:
	var item := slot["item"] as MeshInstance3D
	var material := slot["material"] as ShaderMaterial
	var arm_box: AABB = slot["box"]
	var facing := float(slot["facing"])
	if item == null or material == null:
		return false
	if id <= 0:
		item.visible = false
		return false
	var tex: Texture2D = BlockTextures.get_texture(id)
	if tex == null:
		item.visible = false
		return false
	var is_sprite := BlockTextures.is_item(id)
	# The pose the item names ("" for a default one, "ITEM2"/"ITEM3" for a posed sprite), from
	# the one reader the viewmodel shares.
	var pose := ItemPoses.pose_for(id) if is_sprite else ""
	# Two presets: `item2` keeps the plain flat turn and a smaller seat of its own; everything
	# else a sprite can be -- a default item and an `item3` one -- takes the item1 preset whole.
	var is_item2 := pose == "ITEM2"
	var mesh := _held_mesh_of(id, tex)
	if mesh == null:
		item.visible = false
		return false
	item.mesh = mesh
	item.visible = true
	var size_fraction := HELD_BLOCK_ARM_LENGTHS
	if is_sprite:
		size_fraction = HELD_ITEM2_ARM_LENGTHS if is_item2 else HELD_ITEM_ARM_LENGTHS
	var size: float = arm_box.size.y * size_fraction
	item.scale = Vector3.ONE * size
	# The turn: every sprite lies flat in the palm, an `item2` sprite stays there, everything else
	# turns on to HELD_ITEM_TURN, and a block is carried by a corner (HELD_BLOCK_TURN).
	var turn := HELD_BLOCK_TURN
	if is_sprite:
		turn = HELD_SPRITE_TURN if is_item2 else HELD_ITEM_TURN
	if facing < 0.0:
		turn = Vector3(turn.x, turn.y + HELD_MIRROR_YAW, turn.z)
	item.rotation_degrees = turn
	# The box the item occupies AFTER the turn: a shaped block's bounds are not symmetric
	# about its own origin (a stair's boxes sit off its middle), so a turn swings that
	# off-centre into x, and only the turned box says where the visible thing reaches and
	# sits.
	var placed: AABB = Transform3D(Basis.from_euler(turn * (PI / 180.0)), Vector3.ZERO) \
		* mesh.get_aabb()
	# How far it reaches forward. A block is pushed out until its nearest point is at the
	# palm, measured on that turned box: a cube on the diagonal reaches further than its back
	# face did, and half the untouched depth would sink that corner into the forearm. A sprite
	# lying flat is a card thick, so its own depth is what it reaches by.
	var reach: float = mesh.get_aabb().size.z if is_sprite else placed.size.z
	var forward: float = reach * size * 0.5
	if is_sprite:
		forward += arm_box.size.y * (HELD_ITEM2_FORWARD if is_item2 else HELD_SPRITE_FORWARD)
	else:
		forward -= arm_box.size.y * HELD_BLOCK_BACK
	# Centre the VISIBLE thing on the arm's centre line -- the BOX's, not the arm node's, and
	# not the texture cell's. Two offsets are cancelled here and they are different things: a
	# sprite's art is drawn inside its cell with transparent margins and only its solid texels
	# have vertices, so its bounds can sit off the cell's middle (which put the bucket to one
	# side of the hand), and a rig's joint is not always on the middle of its limb's box -- the
	# goblin's arms hang from the inner face of theirs, so the same grip maths that centres
	# perfectly on the player's re-baked rig put a pickaxe half an arm's width off the goblin's.
	# `arm_box` is that box, in the arm's own space, which is the space this seat is written in.
	# The art's half of it is a CANCELLATION and so it has ONE sign for both facings: the box
	# measured is the one after the mirror, so its x is already the way the art runs on this
	# body, and flipping the correction with the facing counted the mirror twice -- which is
	# invisible on a torch, and put every `item2` sprite (a quarter turn keeps the art's own x)
	# a whole art's width off a goblin's arm.
	var centre: Vector3 = placed.get_center()
	var rise: float = 0.0
	if is_sprite:
		rise = arm_box.size.y * (HELD_ITEM2_RISE if is_item2 else HELD_SPRITE_RISE)
	else:
		rise = mesh.get_aabb().size.y * size * HELD_BLOCK_RISE
	var seat_z: float = arm_box.end.z + forward if facing >= 0.0 else arm_box.position.z - forward
	item.position = Vector3(arm_box.get_center().x - centre.x * size,
		arm_box.position.y + rise, seat_z)
	material.set_shader_parameter("albedo_texture", tex)
	# A block's texture is opaque, so the scissor is pure downside on it (it can only
	# discard edge pixels whose UV rounds onto a texel boundary); an item's sprite needs
	# it for its transparent silhouette. The same rule as the first-person hand.
	material.set_shader_parameter("alpha_scissor", 0.5 if is_sprite else 0.0)
	return true


## Put one block id in the body's OFF hand, on the arm the caller names.
##
## The main hand's grip maths and none of its pose: an offhand item hangs at the side and
## rides that arm's own walk swing because it is a child of the arm. Nothing here calls
## `_pose_held_arm` -- the 18-degree forward turn and the halved swing are MAIN-hand rules,
## and an offhand torch held up like a weapon is not what a torch in a left hand looks like.
##
## This is built on first use rather than in `_ready` like `_init_held_item`: a body with
## no controller above it has no hotbar to read, but a MOB has a roll of its own
## (`scripts/goblin_model.gd` reads its body's field every frame and calls this). `facing`
## is that body's: the goblin's mesh looks down its own -Z, so -1.
func hold_offhand(arm_name: String, id: int, facing := 1.0) -> void:
	if _body_material == null:
		return
	if id <= 0 and _offhand.is_empty():
		return
	var arm := find_child(arm_name, true, false) as MeshInstance3D
	if arm == null or arm.mesh == null:
		return
	if _offhand.is_empty() or _offhand["arm"] != arm:
		_init_offhand(arm)
	_offhand["facing"] = facing
	_seat(_offhand, id)


## The offhand holder: one MeshInstance3D under the named arm with its own item material.
## It needs no pivot of its own -- the arm already carries it -- and no sweep of the rig,
## because the arm it hangs from is the one the clips turn.
func _init_offhand(arm: MeshInstance3D) -> void:
	var material := ShaderMaterial.new()
	material.shader = ITEM_SHADER
	var item := MeshInstance3D.new()
	item.name = "OffhandItem"
	item.material_override = material
	item.visible = false
	_hand_parent(arm).add_child(item)
	_offhand = {
		"arm": arm,
		"box": (arm.mesh as Mesh).get_aabb(),
		"item": item,
		"material": material,
		"facing": 1.0,
	}


## The arms that are not DRAWN by their own MeshInstance3D (a batched mob's: the crowd's
## MultiMeshes draw every pose of every body -- scripts/body_batch.gd), and the stand-in node
## each one's hand items hang from.
var _hand_mirrors := {}


## Whether this body's arms are drawn by something other than their own nodes. False for the
## player's own body and for a preview; a goblin answers true (scripts/goblin_model.gd).
func _arms_are_batched() -> bool:
	return false


## The node a hand item hangs from: the arm itself, or -- when that arm is not drawn -- a
## stand-in under the body that copies the arm's transform every frame
## (`_sync_hand_mirrors`). The grip maths is written in the ARM's space and does not change:
## a stand-in whose global transform IS the arm's has the arm's space.
##
## Without this a hand item vanishes with the node it was parented to, which is how a batching
## pass loses every goblin's torch.
func _hand_parent(arm: MeshInstance3D) -> Node3D:
	if not _arms_are_batched():
		return arm
	var mirror := Node3D.new()
	mirror.name = "%sHand" % String(arm.name)
	add_child(mirror)
	mirror.global_transform = arm.global_transform
	_hand_mirrors[arm] = mirror
	return mirror


## Every stand-in on the arm it stands in for, before this frame is drawn.
func _sync_hand_mirrors() -> void:
	for arm in _hand_mirrors:
		var node := arm as Node3D
		var mirror := _hand_mirrors[arm] as Node3D
		if is_instance_valid(node) and is_instance_valid(mirror):
			mirror.global_transform = node.global_transform


## Where the DRAWN offhand item is, in world space -- a torch's light is at the torch, not
## at the body's feet, so it moves with the arm's swing. `scripts/mob_lights.gd` reads it.
func get_offhand_global_position() -> Vector3:
	if _offhand.is_empty():
		return global_position
	var item := _offhand["item"] as MeshInstance3D
	if item == null or not item.visible:
		return global_position
	return item.global_position


## The offhand's half of the world light model, every frame for the same reason the main
## hand's is: the day/night values move continuously, and a walking body crosses cells it
## must be re-read from. It shares the body's own eased `item_light`, so the torch and the
## arm holding it cannot disagree about the light they are seen in.
func _update_offhand_light() -> void:
	if _offhand.is_empty() or _chunk_manager == null:
		return
	var item := _offhand["item"] as MeshInstance3D
	var material := _offhand["material"] as ShaderMaterial
	if item == null or material == null or not item.visible:
		return
	_chunk_manager.apply_item_lighting(material)
	item.set_instance_shader_parameter("item_light", _body_light)


## Put one block id in the body's MAIN hand, on the arm the caller names.
##
## A mob's main hand, held the way the PLAYER's is: the same `_seat` for the grip (size, turn,
## seat, alpha scissor) and the same pose over it -- the shoulder's HELD_ARM_PITCH forward and
## the walk's swing halved (`_pose_held_arm`), because an item hanging dead straight at a
## side does not read as held. The holder is built on first use rather than in `_ready`
## because the player's own hand reads a hotbar and a mob has none. The goblin's main hand is
## `arm2` -- its RIGHT, the +x side, because the mesh looks down its own -Z -- at facing -1
## (scripts/goblin_model.gd), and its shoulder pivot is on that same arm.
##
## `facing` mirrors the seat for a body whose mesh looks down -Z, exactly as hold_offhand's
## does: without it the item is drawn behind the hand and a sprite upside down.
func hold_main(arm_name: String, id: int, facing := 1.0) -> void:
	if _body_material == null:
		return
	if id <= 0 and _main_hand_item.is_empty():
		return
	var arm := find_child(arm_name, true, false) as MeshInstance3D
	if arm == null or arm.mesh == null:
		return
	if _main_hand_item.is_empty() or _main_hand_item["arm"] != arm:
		_init_main_hand(arm)
	_main_hand_item["facing"] = facing
	_pose_held_arm(_seat(_main_hand_item, id))


## The main-hand holder: one MeshInstance3D under the named arm with its own item material,
## beside `_init_offhand`'s. It needs no pivot of its own -- the arm it hangs from is the one
## `_init_held_arm` put a shoulder pivot on, and the inspection pose is written on that same
## arm (see `inspect_pose`), so the item follows both.
func _held_arm_name() -> String:
	return HELD_ARM


## Whether this body's main hand has something drawn in it -- the player's hotbar holder, or
## a mob's `_main_hand_item`. The held-item pose follows this and nothing else, so the arm and
## the thing in it can never disagree about whether the hand is full.
func _main_hand_filled() -> bool:
	if _held_item != null and _held_item.visible:
		return true
	var item := _main_hand_item.get("item") as MeshInstance3D
	return item != null and item.visible


func _init_main_hand(arm: MeshInstance3D) -> void:
	var material := ShaderMaterial.new()
	material.shader = ITEM_SHADER
	var item := MeshInstance3D.new()
	item.name = "MainHandItem"
	item.material_override = material
	item.visible = false
	_hand_parent(arm).add_child(item)
	_main_hand_item = {
		"arm": arm,
		"box": (arm.mesh as Mesh).get_aabb(),
		"item": item,
		"material": material,
		"facing": 1.0,
	}


## Where the DRAWN main-hand item is, in world space -- a goblin's inspection looks at it and
## `scripts/mob_lights.gd` lights the world from it. A hand with nothing drawn in it answers
## the body's own origin, the offhand's rule.
func get_main_hand_global_position() -> Vector3:
	if _main_hand_item.is_empty():
		return global_position
	var item := _main_hand_item["item"] as MeshInstance3D
	if item == null or not item.visible:
		return global_position
	return item.global_position


## The main hand's half of the world light model, every frame for the reason the offhand's
## is: an item taken off the ground a moment ago and lifted to a face is in a cell whose
## light is the body's, and the day/night values under it keep moving.
func _update_main_hand_light() -> void:
	if _main_hand_item.is_empty() or _chunk_manager == null:
		return
	var item := _main_hand_item["item"] as MeshInstance3D
	var material := _main_hand_item["material"] as ShaderMaterial
	if item == null or material == null or not item.visible:
		return
	_chunk_manager.apply_item_lighting(material)
	item.set_instance_shader_parameter("item_light", _body_light)


## Hold the main hand up for an inspection, or let the clips have it back. `pitch` is degrees
## and is only read when the pose is switched ON -- a release writes nothing, and the tree
## takes the arm back where it left it.
func set_inspect_pose(on: bool, pitch := 0.0) -> void:
	if on and pitch > 0.0:
		inspect_pitch = pitch
	inspect_pose = on
	# Switching it OFF is not an instant: `_inspect_share` eases to 0 over
	# INSPECT_LOWER_SECONDS and `_hold_inspect_pose` writes the arm down that far each frame.


## The arm an inspection raises, and which way that turn goes: the pose is
## `_clip_rest[arm] * Quaternion(RIGHT, pitch * sign)`. This rig faces +Z with its item hand on
## `arm`, and an arm hanging -Y comes FORWARD on a negative turn -- the sign HELD_ARM_PITCH
## uses for the same reason. A body whose mesh looks down -Z mirrors both
## (scripts/goblin_model.gd), and probes/probe_goblin.gd holds the raised hand to the front of
## the mesh it is on.
func _inspect_arm() -> String:
	return HELD_ARM


func _inspect_sign() -> float:
	return -1.0


## One frame of the inspection pose: the arm up, written over whatever the clips just did to
## the rest of the body. The tree is left running (and runs first, TREE_PRIORITY), so the
## legs and the other arm keep moving under this -- a body frozen mid-stride is what this used
## to look like, and a goblin takes its item at the end of a walk.
func _hold_inspect_pose(delta: float) -> void:
	# 1 while it is being looked at, then down to 0 over INSPECT_LOWER_SECONDS. The pose is
	# written every frame either way, which is what makes the way down a movement rather than
	# one frame of 80 degrees followed by the clips' own pose.
	_inspect_share = 1.0 if inspect_pose \
		else maxf(_inspect_share - delta / INSPECT_LOWER_SECONDS, 0.0)
	var arm_name := _inspect_arm()
	# The shoulder's held-item turn belongs to the HOLDING pose, not to this one, and the two
	# are poses of the same arm: composed they would add and the hand would end up above the
	# face. So the inspection takes the pivot's share as its own angle shrinks -- the total is
	# continuous from the item in front of the face down to the item at the side.
	if _held_pivot != null and arm_name == _held_arm_name():
		_held_pivot.rotation_degrees = Vector3(
			HELD_ARM_PITCH * _inspect_sign() * (1.0 - _inspect_share), 0.0, 0.0)
	if not _clip_rest.has(arm_name):
		return
	var arm := find_child(arm_name, true, false) as Node3D
	if arm == null:
		return
	arm.quaternion = (_clip_rest[arm_name] as Quaternion) \
		* Quaternion(Vector3.RIGHT, deg_to_rad(inspect_pitch * _inspect_share) * _inspect_sign())
	if _inspect_share > 0.0:
		return
	# Down: the tree has the arm again, and a hand that is still full is posed by its own rule
	# -- a kept tool back at its side, at the same 18 degrees this frame wrote.
	_pose_held_arm(_main_hand_filled())


## The right arm's pose for what it is (or is not) carrying -- see HELD_ARM_PITCH.
##
## Vanilla switches it in the frame the item appears, with no fade, and so does this: an
## ease here would be a deviation of ours, and one the eye reads as the arm lagging the
## hotbar by a beat. A body with no pivot (a model without the arm) simply does nothing.
func _pose_held_arm(held: bool) -> void:
	if _held_pivot == null:
		return
	# "Forward" is the MESH's, exactly as the inspection's sign is: a +Z rig comes forward on a
	# negative turn, a -Z mesh on the same turn the other way, and either way it is the arm
	# tipping toward the face the body looks out of.
	_held_pivot.rotation_degrees = Vector3(HELD_ARM_PITCH * _inspect_sign() if held else 0.0,
		0.0, 0.0)
	_set_held_swing(held)


## The walk half of the held-arm rule (see HELD_ARM_SWING): the run clip's arm track
## scaled about the arm's rest -- 0.5 while the hand is full, back to 1.0 when it empties.
##
## Event-driven, not per frame: the clip IS the walk, and a factor re-applied every frame
## would compound the moment the tree stopped rewriting the arm (a frozen body keeps its
## last pose, and _process still runs).
##
## The two amplitudes are two CLIPS rather than one clip scaled in place, because the clips
## belong to every body of the rig: scaling one body's arm would halve every goblin's. The
## halved variant is built once per rig (_run_clip_halved) and swapped into THIS body's own
## library, which is one dictionary write. The mixer caches the keys it read, so the two
## clear_caches calls are still what makes the swap land -- without them it would apply only
## the next time that clip was re-entered.
func _set_held_swing(held: bool) -> void:
	var arm_name := _held_arm_name()
	if _run_clip == null or held == _held_swing_half or not _clip_rest.has(arm_name):
		return
	_install_run_clip(_run_clip_halved() if held else _run_clip_full())
	_held_swing_half = held
	if _clip_player != null:
		_clip_player.clear_caches()
	if _tree != null:
		_tree.clear_caches()


## The mesh a block id is held as, built once per id. A block is its shape's own boxes;
## an item -- a stick, a torch -- is the flat sprite mesh the first-person hand uses for it.
func _held_mesh_of(block_id: int, tex: Texture2D) -> ArrayMesh:
	if _held_meshes.has(block_id):
		return _held_meshes[block_id]
	var mesh: ArrayMesh = null
	if BlockTextures.is_item(block_id):
		mesh = _mesh_from(ViewmodelMeshes.build_item_mesh(tex))
	else:
		var boxes := []
		if _chunk_manager != null and _chunk_manager.has_method("get_selection_boxes"):
			for box in _chunk_manager.get_selection_boxes(block_id):
				var b := PackedFloat32Array()
				for i in range(mini(box.size(), 6)):
					b.append(float(box[i]))
				if b.size() == 6:
					boxes.append(b)
		if boxes.is_empty():
			# No world to ask, or a block with no boxes at all: a full cube is what the
			# registry answers with too.
			boxes.append(PackedFloat32Array([0.0, 0.0, 0.0, 1.0, 1.0, 1.0]))
		mesh = _mesh_from(ViewmodelMeshes.build_shaped_mesh(boxes))
	if mesh == null:
		return null
	_held_meshes[block_id] = mesh
	return mesh


## One surface of the binding's interleaved arrays as a mesh (the dropped items' own
## `_mesh_from`, which is what a held block and a dropped one being drawn alike needs).
func _mesh_from(data: Dictionary) -> ArrayMesh:
	if data.is_empty():
		return null
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = data["verts"]
	arrays[Mesh.ARRAY_TEX_UV] = data["uvs"]
	arrays[Mesh.ARRAY_NORMAL] = data["normals"]
	arrays[Mesh.ARRAY_INDEX] = data["indices"]
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	if mesh.get_surface_count() == 0:
		return null
	return mesh


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
		img = Image.load_from_file("res://assets/textures/skin.png")
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
