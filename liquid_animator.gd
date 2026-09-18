extends Node
## LiquidAnimator — the world's own liquid animation, with no lab open.
##
## The Liquid Texture Lab authors a strip and the Lab's Live button pushes frames
## into the world's texture array, but both only exist while the panel is up.
## This node is the other half: it animates every liquid in the game, from the
## moment a world exists, by the same route (ChunkManager.push_texture_frame ->
## Texture2DArray.update_layer) so a pool of water, a lava flow and a splash of
## acid all ripple as you walk past them.
##
## Where a liquid's animation comes from, in order:
##   1. user://liquids/<liquid>.json — a strip you saved in the Lab and bound to
##      that liquid (the Lab's "Bind to world" button writes exactly this name).
##      The settings in that file are re-generated here, so what animates is what
##      the Lab showed, byte for byte: the generator is deterministic and its
##      seed travels in the settings.
##   2. the built-in preset for that liquid (LiquidTextureGen.default_settings),
##      so water, lava and acid animate out of the box with no saved strip at
##      all.
##
## Two things worth knowing:
##
## * Hold time is `frame_time` game ticks, exactly as the Lab's preview plays it,
##   and a looping strip advances from its last frame to index 1 rather than 0
##   (index 0 already played as the last frame — see liquid_texture.hpp), so the
##   world and the Lab show the same animation at the same speed.
## * A GPU-compressed texture array cannot take a raw RGBA frame and nothing on
##   the CPU can build a matching compressed chain, so while this node is
##   animating it turns texture compression OFF for the session and puts your
##   setting back when it stops (set_enabled(false)). The array is rebuilt from
##   the same small PNGs, which is a few milliseconds once.
##
## Both of the above are why this is a node of its own rather than more code in
## the Lab: the Lab is a tool you open, this runs whenever the game does.

const LIQUIDS := ["water", "lava", "acid"]
const SAVE_DIR := "user://liquids"
const TICK_RATE := 20.0  # frames are held for `frame_time` game ticks
const DEFAULT_RESOLUTION := 16

## While false, nothing is pushed and any layer this node touched goes back to
## its PNG. The world keeps rendering liquids, they simply stop moving.
var enabled := true
## Whether this node may turn the project's texture compression off for the
## session. False leaves a compressed array alone and reports the layer as
## read-only, which is the honest outcome: animation is impossible there.
var allow_compression_flip := true

var _manager: Node = null
var _states: Dictionary = {}   # liquid -> state dictionary (see _state_for)
var _compression_flipped := false
var _compression_was := false

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS

func _process(delta: float) -> void:
	if not enabled:
		return
	var manager := _chunk_manager()
	if manager == null:
		return
	for liquid in LIQUIDS:
		var state: Dictionary = _state_for(liquid)
		if bool(state["paused"]):
			continue
		if not bool(state["ready"]) and not _prepare(liquid, state):
			continue
		_advance(state, delta)
		if int(state["pushed"]) != int(state["frame"]):
			if manager.push_texture_frame(liquid, state["frames"][int(state["frame"])]):
				state["pushed"] = state["frame"]

# ---------------------------------------------------------------------------
# Public API (the Lab, and anything else that wants to talk to this)
# ---------------------------------------------------------------------------

## Turn world animation on or off. Off also puts every layer this node wrote
## back to its PNG, so the world looks exactly as it did before this ran.
func set_enabled(value: bool) -> void:
	if enabled == value:
		return
	enabled = value
	if value:
		# Nothing to do: the next _process prepares each liquid again.
		return
	var manager := _chunk_manager()
	if manager != null:
		for liquid in LIQUIDS:
			if bool(_state_for(liquid)["ready"]):
				manager.restore_texture_layer(liquid)
	for liquid in LIQUIDS:
		_reset(_state_for(liquid))
	_restore_compression()

func is_enabled() -> bool:
	return enabled

## Forget everything known about one liquid and rebuild it from disk on the next
## tick. The Lab calls this after binding a strip to a liquid.
func reload(liquid: String) -> void:
	_reset(_state_for(liquid))
	if not enabled:
		return
	var state: Dictionary = _state_for(liquid)
	var manager := _chunk_manager()
	if manager != null and bool(state["ready"]):
		manager.restore_texture_layer(liquid)

## Stop touching one liquid's layer for a while (the Lab's Live button owns it
## while you are tuning), then hand it back.
func pause(liquid: String) -> void:
	_state_for(liquid)["paused"] = true

func resume(liquid: String) -> void:
	var state: Dictionary = _state_for(liquid)
	state["paused"] = false
	if enabled and bool(state["ready"]):
		state["pushed"] = -1  # force a push on the next tick

## The strip bound to a liquid, or "" when it is running its built-in preset.
func bound_name(liquid: String) -> String:
	return String(_state_for(liquid)["source"])

## Per-liquid description of what is happening, for the Lab's status line and
## for probes: source, frame count, current frame, hold in ticks, and why a
## liquid is not animating when it is not.
func get_status() -> Dictionary:
	var out := {}
	for liquid in LIQUIDS:
		var state: Dictionary = _state_for(liquid)
		out[liquid] = {
			"source": state["source"],
			"frames": (state["frames"] as Array).size(),
			"frame": int(state["frame"]),
			# The frame the last successful push wrote into the world's array, or
			# -1 when nothing has been pushed yet. This is the field that says the
			# animation is reaching the world and not just counting frames.
			"pushed": int(state["pushed"]),
			"hold": int(state["hold"]),
			"looped": bool(state["looped"]),
			"animating": enabled and bool(state["ready"]) and not bool(state["paused"]),
			"paused": bool(state["paused"]),
			"note": state["note"],
		}
	out["enabled"] = enabled
	out["compression_flipped"] = _compression_flipped
	return out

# ---------------------------------------------------------------------------
# Preparation
# ---------------------------------------------------------------------------

func _chunk_manager() -> Node:
	if _manager == null or not is_instance_valid(_manager):
		_manager = get_node_or_null("/root/Main/ChunkManager")
	return _manager

func _state_for(liquid: String) -> Dictionary:
	if not _states.has(liquid):
		var state := {
			"settings": {},
			"frames": [] as Array[Image],
			"looped": false,
			"hold": 3,
			"frame": 0,
			"pushed": -1,
			"accum": 0.0,
			"ready": false,
			"paused": false,
			"source": "",
			"note": "not prepared yet",
		}
		_states[liquid] = state
	return _states[liquid]

func _reset(state: Dictionary) -> void:
	state["frames"] = [] as Array[Image]
	state["ready"] = false
	state["frame"] = 0
	state["pushed"] = -1
	state["accum"] = 0.0
	state["note"] = "not prepared yet"

# Everything that can stop a liquid animating, checked once per attempt rather
# than per frame: the layer has to exist (a block has to use textures/<name>.png)
# and it has to be writable (an uncompressed array).
func _prepare(liquid: String, state: Dictionary) -> bool:
	var manager := _chunk_manager()
	if manager == null:
		return false
	var info: Dictionary = manager.get_texture_layer_info(liquid)
	if not bool(info.get("found", false)):
		state["note"] = "no texture layer named \"%s\"" % liquid
		return false
	if not bool(info.get("writable", false)):
		if not allow_compression_flip:
			state["note"] = "layer is GPU-compressed; animation needs compression off"
			return false
		_flip_compression(manager)
		info = manager.get_texture_layer_info(liquid)
		if not bool(info.get("writable", false)):
			state["note"] = "layer is GPU-compressed and could not be made writable"
			return false
	return _build(liquid, state)

# Re-generating the settings the Lab would show is the whole load path: the
# generator is deterministic, so a saved strip is reproduced from its JSON alone
# — which is why nothing here reads the saved PNG.
func _build(liquid: String, state: Dictionary) -> bool:
	var settings: Dictionary = {}
	var source := ""
	var path := SAVE_DIR + "/" + liquid + ".json"
	if FileAccess.file_exists(path):
		var file := FileAccess.open(path, FileAccess.READ)
		if file != null:
			var parsed = JSON.parse_string(file.get_as_text())
			file.close()
			if typeof(parsed) == TYPE_DICTIONARY:
				settings = parsed
				source = liquid + ".json"
			else:
				push_warning("LiquidAnimator: %s is not a settings dictionary; using the preset" % path)
	if settings.is_empty():
		settings = LiquidTextureGen.default_settings(liquid, DEFAULT_RESOLUTION)
		source = "preset"
	# JSON hands the ramp back as a plain Array; the C++ side wants floats.
	settings["ramp"] = PackedFloat32Array(settings.get("ramp", PackedFloat32Array()))
	var strip: Image = LiquidTextureGen.generate_strip(settings)
	if strip == null or strip.get_width() <= 0 or strip.get_height() <= 0:
		state["note"] = "generating the strip failed"
		return false
	var size := strip.get_width()
	var count := int(strip.get_height() / float(size))
	var frames: Array[Image] = []
	for index in count:
		frames.append(strip.get_region(Rect2i(0, index * size, size, size)))
	if frames.is_empty():
		state["note"] = "the strip held no frames"
		return false
	var described: Dictionary = LiquidTextureGen.describe(settings)
	state["settings"] = settings
	state["frames"] = frames
	state["looped"] = bool(described.get("looped", false))
	state["hold"] = maxi(1, int(settings.get("frame_time", 3)))
	state["frame"] = 0
	state["pushed"] = -1
	state["accum"] = 0.0
	state["source"] = source
	state["note"] = ""
	state["ready"] = true
	return true

# ---------------------------------------------------------------------------
# Playback
# ---------------------------------------------------------------------------

# One playback step per `hold` ticks, the same rule the Lab's preview uses — a
# looping strip's last frame is the first frame's image, so the frame after it is
# index 1 (showing index 0 again would hold that image for two frame times).
func _advance(state: Dictionary, delta: float) -> void:
	state["accum"] = float(state["accum"]) + delta
	var hold := maxf(1.0, float(state["hold"])) / TICK_RATE
	var guard := 0
	while float(state["accum"]) >= hold and guard < 64:
		state["accum"] = float(state["accum"]) - hold
		_advance_frame(state)
		guard += 1

func _advance_frame(state: Dictionary) -> void:
	var frames: Array = state["frames"]
	var next := int(state["frame"]) + 1
	if next >= frames.size():
		next = 1 if bool(state["looped"]) and frames.size() > 1 else 0
	state["frame"] = next

# ---------------------------------------------------------------------------
# Compression
# ---------------------------------------------------------------------------

func _flip_compression(manager: Node) -> void:
	if _compression_flipped:
		return
	_compression_was = bool(manager.get_compression_enabled())
	manager.set_compression_enabled(false)
	_compression_flipped = true
	push_warning("LiquidAnimator: texture compression off while liquids animate; "
		+ "it comes back when world animation is turned off.")

func _restore_compression() -> void:
	if not _compression_flipped:
		return
	_compression_flipped = false
	var manager := _chunk_manager()
	if manager != null and bool(manager.get_compression_enabled()) != _compression_was:
		manager.set_compression_enabled(_compression_was)
