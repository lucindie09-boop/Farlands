extends SceneTree
## Lifts a model file's own animations out into standalone Animation resources.
##
##   "$GODOT" --headless --path . --script res://tools/extract_model_animations.gd
##
## Godot's glTF importer keeps a file's clips inside the imported scene, on that
## scene's own AnimationPlayer, where they can only ever animate that scene. The
## player's body does not use the imported scene: it loads its clips from
## res://Animations/*.anim (scripts/player_model.gd), so a clip has to exist as a
## resource of its own before anything can play it.
##
## The mapping is by the clip's name in the source file. A name the file does not
## carry is an error rather than a skip -- a typo would otherwise leave the old
## resource in place and look like a successful run.
##
## Nothing is deleted: an output path that is already there is rewritten, and the
## source file and every other .anim are left alone. Writing into res:// only
## works in a project run from source, which is all this is for.
##
## The save is read back before the run is called good: track count, every track
## path, every track's key count and the length are compared against the clip it
## came from, because a resource that saves and loads is not the same claim as a
## resource that carries the animation. Every path is also resolved against the
## rig the clip came out of, from the AnimationPlayer's own parent -- a track path
## is how a clip finds the limb it turns, and a clip whose paths name nothing
## plays as no motion at all, silently.

const SOURCE_SCENE := "res://models/minecraft-idle-and-walking-animation/source/model.gltf"
const OUTPUT_DIR := "res://Animations"

# clip name in the glTF file -> [output path, resource name]
const CLIPS := {
	"still_test": ["res://Animations/Idle_new.anim", "Idle"],
	"walking_test": ["res://Animations/Walk.anim", "Walk"],
}

func _init() -> void:
	var failures := 0
	var written := 0

	var scene: PackedScene = load(SOURCE_SCENE)
	if scene == null:
		print("extract: FAILED to load %s" % SOURCE_SCENE)
		quit(1)
		return
	var root: Node = scene.instantiate()
	var player := _find_animation_player(root)
	if player == null:
		print("extract: FAILED -- no AnimationPlayer in %s" % SOURCE_SCENE)
		root.free()
		quit(1)
		return

	var sources := _collect(player)
	print("extract: %s carries %d clip(s): %s" % [
		SOURCE_SCENE, sources.size(), ", ".join(sources.keys())])

	if not DirAccess.dir_exists_absolute(OUTPUT_DIR):
		DirAccess.make_dir_recursive_absolute(OUTPUT_DIR)

	for clip_name in CLIPS:
		if not sources.has(clip_name):
			print("extract: FAILED -- %s has no clip named '%s'" % [SOURCE_SCENE, clip_name])
			failures += 1
			continue
		var source: Animation = sources[clip_name]
		var output: String = CLIPS[clip_name][0]
		var clip := _standalone(source, CLIPS[clip_name][1])
		var err := ResourceSaver.save(clip, output)
		if err != OK:
			print("extract: FAILED to save %s (error %d)" % [output, err])
			failures += 1
			continue
		written += 1
		var clip_failures := _verify(clip, output)
		clip_failures += _verify_paths(player.get_parent(), clip, output)
		failures += clip_failures
		print("extract: %s -- %s" % [output, "all checks passed" if clip_failures == 0
			else "%d check(s) FAILED" % clip_failures])

	root.free()
	print("extract: %d of %d clip(s) written, %d check(s) failed" % [
		written, CLIPS.size(), failures])
	quit(1 if failures > 0 else 0)


## The clip as a resource of its own: renamed, looping, and detached from the
## imported scene it was baked into (a duplicate, so nothing downstream of the
## scene's own player is touched).
##
## Both clips in a Minecraft-style rig are cycles -- an idle sways and returns, a
## walk steps and returns -- so they loop. A one-shot would need its own entry
## here, and would be worth naming as one.
func _standalone(source: Animation, clip_name: String) -> Animation:
	var clip: Animation = source.duplicate()
	clip.resource_name = clip_name
	clip.loop_mode = Animation.LOOP_LINEAR
	return clip


## Read the saved file back and hold it to the clip it came from, in the terms
## that matter to a player: the same tracks, the same paths, the same keys, the
## same duration. Returns the number of failed checks.
func _verify(clip: Animation, path: String) -> int:
	var loaded: Animation = load(path)
	if loaded == null:
		print("extract: FAILED -- %s does not load back" % path)
		return 1
	var failures := 0
	var where := "%s (%s)" % [path, clip.resource_name]
	if loaded.get_track_count() != clip.get_track_count():
		print("extract: FAILED -- %s kept %d of %d tracks" % [
			where, loaded.get_track_count(), clip.get_track_count()])
		failures += 1
	for t in range(mini(loaded.get_track_count(), clip.get_track_count())):
		if loaded.track_get_path(t) != clip.track_get_path(t):
			print("extract: FAILED -- %s track %d is %s, source had %s" % [
				where, t, loaded.track_get_path(t), clip.track_get_path(t)])
			failures += 1
		if loaded.track_get_key_count(t) != clip.track_get_key_count(t):
			print("extract: FAILED -- %s track %d kept %d of %d keys" % [
				where, t, loaded.track_get_key_count(t), clip.track_get_key_count(t)])
			failures += 1
	if not is_equal_approx(loaded.length, clip.length):
		print("extract: FAILED -- %s is %.3f s, source was %.3f s" % [
			where, loaded.length, clip.length])
		failures += 1
	print("extract: %s -- %.3f s, %d tracks, loop %d" % [
		where, loaded.length, loaded.get_track_count(), loaded.loop_mode])
	for t in range(loaded.get_track_count()):
		print("extract:     track %2d  %-28s %d key(s)" % [
			t, loaded.track_get_path(t), loaded.track_get_key_count(t)])
	return failures


## Every track path must name a node in the rig, resolved the way the player
## resolves it (from the AnimationPlayer's parent, which glTF import leaves at
## the scene root). This rig is why it is worth checking: the imported nodes are
## `Node/Head2` and `Node/Body2`, not `Node/Head`/`Node/Body` -- Godot suffixes a
## name that collides, and here the plain names went to the head's and body's own
## MESH nodes -- and the whole rig sits one `Node` deeper than the model root.
## A path written from the file's own names would resolve to nothing and the clip
## would turn no limb while playing perfectly happily.
func _verify_paths(base: Node, clip: Animation, path: String) -> int:
	var failures := 0
	var seen := {}
	for t in range(clip.get_track_count()):
		var track_path: NodePath = clip.track_get_path(t)
		if seen.has(track_path):
			continue
		seen[track_path] = true
		if base.get_node_or_null(track_path) == null:
			print("extract: FAILED -- %s track path '%s' names no node in %s" % [
				path, track_path, SOURCE_SCENE])
			failures += 1
	print("extract:     %d track path(s) all resolve against the rig" % seen.size())
	return failures


## Every clip the imported scene carries, by name, across all of its libraries
## (the importer names one after the scene file and may add a "RESET" library
## beside it).
func _collect(player: AnimationPlayer) -> Dictionary:
	var out := {}
	for library_name in player.get_animation_library_list():
		var library := player.get_animation_library(library_name)
		for clip_name in library.get_animation_list():
			var clip: Animation = library.get_animation(clip_name)
			if clip != null:
				out[clip_name] = clip
	return out


func _find_animation_player(node: Node) -> AnimationPlayer:
	if node is AnimationPlayer:
		return node
	for child in node.get_children():
		var found := _find_animation_player(child)
		if found != null:
			return found
	return null
