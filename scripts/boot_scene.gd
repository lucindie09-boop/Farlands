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

var _loading := false


func _ready() -> void:
	add_to_group(&"boot_scene")
	# Sub-threads: the scene's scripts parse on the workers, not on this frame.
	var error := ResourceLoader.load_threaded_request(WORLD_SCENE, "PackedScene", true)
	if error != OK:
		push_error("BootScene: could not start loading %s (error %d)" % [WORLD_SCENE, error])
		return
	_loading = true


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
	var world := packed.instantiate()
	get_tree().root.add_child(world)
	get_tree().current_scene = world
	# Bind in this frame: the layer would otherwise see the new scene on its next
	# `_process`, which is one frame of HUD and a captured mouse behind the title.
	var menu := get_node_or_null(^"/root/MenuLayer")
	if menu != null:
		menu.call("_try_boot")
	queue_free()
