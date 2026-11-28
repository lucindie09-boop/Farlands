extends Node
## The first scene of a launch: an empty stage for the title screen.
##
## The title is drawn by the `MenuLayer` autoload and needs the world only to sit
## over one, so the game boots here and loads `Main.tscn` on a thread behind the
## menu instead of in front of it. The scene is ~0.6 s of script parsing, ~0.13 s
## to instantiate and a first frame of its own, and all of it used to happen
## before the title could appear. When the world lands it is added to the root
## and made the current scene -- the single fact `MenuLayer` watches -- so the
## title never changes and the world simply arrives behind it.
##
## A probe that instantiates `Main.tscn` by hand and sets `current_scene` itself
## never sees this node and keeps working as before.

const WORLD_SCENE := "res://Main.tscn"

## What the first goblin's spawn would otherwise pay for -- its two scripts, its mesh and
## texture, its rig, its clips and its shader -- is loaded and built here instead, behind the
## title (see `_warm_goblin`).
const GOBLIN_MODEL_SCRIPT := "res://scripts/goblin_model.gd"
const GOBLIN_BODY_SCRIPT := "res://scripts/goblin.gd"
const GOBLIN_MESH := "res://assets/models/Goblin.obj"
const GOBLIN_TEXTURE := "res://assets/models/Goblin.png"

var _loading := false


func _ready() -> void:
	add_to_group(&"boot_scene")
	# Sub-threads: the scene's scripts parse on the workers, not on this frame.
	var error := ResourceLoader.load_threaded_request(WORLD_SCENE, "PackedScene", true)
	if error != OK:
		push_error("BootScene: could not start loading %s (error %d)" % [WORLD_SCENE, error])
		return
	_loading = true
	_warm_deferred_autoloads()


## `SkinManager` and `BlockManager` are no longer autoloads (their script parse
## and their PNG reads sat in the engine's boot for two nodes the title never
## reads). Wait out the title's own frame first, so the cost lands behind it, and
## stay behind it: the world is still a second away.
func _warm_deferred_autoloads() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	if not is_inside_tree():
		return
	var loader := get_node_or_null(^"/root/ToolLoader")
	if loader != null:
		loader.call("ensure_lazy_autoloads")


func _process(_delta: float) -> void:
	if not _loading:
		return
	var status := ResourceLoader.load_threaded_get_status(WORLD_SCENE)
	if status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
		return
	_loading = false
	if status != ResourceLoader.THREAD_LOAD_LOADED:
		push_error("BootScene: %s failed to load (status %d)" % [WORLD_SCENE, status])
		return
	var packed := ResourceLoader.load_threaded_get(WORLD_SCENE) as PackedScene
	if packed == null:
		push_error("BootScene: %s did not load a PackedScene" % WORLD_SCENE)
		return
	# The world's own scripts look these up in `_ready`; make them now if the
	# load somehow beat the deferred warm-up.
	var loader := get_node_or_null(^"/root/ToolLoader")
	if loader != null:
		loader.call("ensure_lazy_autoloads")
	var world := packed.instantiate()
	get_tree().root.add_child(world)
	get_tree().current_scene = world
	# Bind in this frame: the layer would otherwise see the new scene on its next
	# `_process`, which is one frame of HUD and a captured mouse behind the title.
	var menu := get_node_or_null(^"/root/MenuLayer")
	if menu != null:
		menu.call("_try_boot")
	# The title has had its frame; the goblin's one-time cost goes into the next one, still
	# behind it (see _warm_goblin).
	await get_tree().process_frame
	_warm_goblin(world)
	await get_tree().process_frame
	queue_free()


## The first goblin's one-time cost, paid here while the title is up rather than in a fight.
##
## A goblin's first appearance pays for three things that do not depend on it, and all three are
## per SESSION rather than per body: the two scripts parse (76 ms and 70 ms on this machine),
## the obj is split into the six parts it is drawn as and the walk's clips are fitted to
## vanilla's amplitudes (~30 ms, cached for every body after the first), and the body's shader is
## compiled. Together that is a hitch you can see, in the one moment a goblin turns up.
##
## Nothing here spawns a goblin -- a live one joins the `goblin` group (the light gather and the
## punch both walk it), installs the session's nav trace and asks the world for a movement sim.
## What is built is a goblin's MODEL on no body: it fills the shared rig, the clips and the
## material, and is freed before the next frame draws.
func _warm_goblin(world: Node) -> void:
	if world == null or not is_instance_valid(world):
		return
	var mesh := load(GOBLIN_MESH) as Mesh
	var model_script := load(GOBLIN_MODEL_SCRIPT) as Script
	# The body's own script parses on its first spawn, so it is warmed too -- loaded and never
	# instantiated, because a body is what spawns a goblin.
	load(GOBLIN_TEXTURE)
	load(GOBLIN_BODY_SCRIPT)
	if mesh == null or model_script == null:
		return
	var warm := Node3D.new()
	warm.name = "GoblinPrewarm"
	warm.set_script(model_script)
	warm.set("source_mesh", mesh)
	warm.visible = false
	world.add_child(warm)
	warm.queue_free()
