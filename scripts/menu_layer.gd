extends CanvasLayer
## The title screen and the transition, in one autoload that owns their state.
##
## Why an autoload rather than a node in `Main.tscn`: the probe suite instantiates
## `Main.tscn` directly and drives the player, and a title screen living inside
## that scene would freeze every one of those probes. This layer only arms itself
## when `SceneTree.current_scene` is the scene — that is, when the game booted
## normally — so a fixture that was instantiated by hand never sees it.
##
## Why it reuses `PlayerController.set_settings_open`: that flag is already the one
## gate every system respects (input, mouse capture, the block outline, the
## crosshair, the compass, the interaction door). A second "menu is open" flag
## would be one a future system could forget, and the failure would be a player
## who can break blocks through the title screen. The semantic cost is that
## `is_settings_open()` is true while the title is up, which is why the docs say so.
##
## The world is live behind the menu — it streams while the title is up and the
## player is standing in it — so CONTINUE really does mean "you are already
## there", and the cover only has to hide a mouse-mode change, not a load.

const TitleScreen := preload("res://scripts/title_screen.gd")
const BlockCurtain := preload("res://scripts/ui_block_curtain.gd")

const SETTINGS_PATH := "user://settings.cfg"
const META_PATH := "user://chunks/world.meta"
const DRIFT_RAD_PER_SEC := 0.035
# The gameplay HUD belongs to the player, not to the title screen. Hidden while
# the title is up and restored to whatever it was before, one by one, so a node
# that was already hidden stays hidden.
const HIDE_AT_TITLE := [
	"FPSCounter", "Hotbar", "Healthbar", "Airbar", "Inventory", "CraftingTableMenu",
	"Chat", "Wand", "Crosshair", "Compass", "HurtOverlay", "DeathScreen",
]

var _title: Control
var _curtain: Control
var _player: Node
var _chunk_manager: Node
var _settings_menu: Node
var _hud: Node
var _hidden: Dictionary = {}
var _open := false
var _booted := false
var _busy := false
# Whether a world existed BEFORE this launch. The world behind the title is
# already running, so by the time the title is up `world.meta` always exists —
# asking the file then would report CONTINUE on a first run. This is read in
# `_ready`, which is the one moment before the scene has had a chance to write
# anything.
var _had_world := false


func _ready() -> void:
	layer = 128
	_had_world = FileAccess.file_exists(META_PATH)
	# A CanvasLayer's children draw in insertion order: the curtain is added last
	# so it covers the title as well as the world.
	_title = TitleScreen.new()
	add_child(_title)
	_title.hide()
	_title.start_pressed.connect(_on_start)
	# A picked world is the same action as the primary row: the world behind the title is
	# already running, so there is nothing to load and the same cover hides the change.
	_title.world_chosen.connect(_on_start)
	_title.settings_pressed.connect(_on_settings)
	_title.quit_pressed.connect(_on_quit)
	_curtain = BlockCurtain.new()
	add_child(_curtain)
	_curtain.hide()


func _process(delta: float) -> void:
	if not _booted:
		_try_boot()
		return
	if not _open or _busy:
		return
	# The settings menu owns the flag while it is up; it clears it on close, and
	# this is what puts it back without a frame of captured mouse between them.
	if _settings_menu != null and _settings_menu.is_open:
		return
	if not _title.visible:
		_show_title()
	if _player.is_settings_open():
		return
	_player.set_settings_open(true)
	# A slow drift, not a cut: a still frame behind a menu reads as a screenshot.
	# It is applied to the body because the camera's own transform is recomputed
	# from the body's rotation every frame in C++.
	_player.rotate_y(DRIFT_RAD_PER_SEC * delta)


## Find the scene this layer belongs to. Anything that is not the running game —
## a probe's hand-made fixture, the editor — is left alone, which is what keeps
## `probes/` working without a flag to remember.
func _try_boot() -> void:
	var scene := get_tree().current_scene
	if scene == null:
		return
	var player := scene.get_node_or_null("Player")
	var chunks := scene.get_node_or_null("ChunkManager")
	var hud := scene.get_node_or_null("HUD")
	if player == null or chunks == null or hud == null:
		# A scene of ours, but not the world. Nothing to show a title over.
		_booted = true
		set_process(false)
		return
	_player = player
	_chunk_manager = chunks
	_hud = hud
	_settings_menu = hud.get_node_or_null("SettingsMenu")
	_booted = true
	_open = true

	_hide_hud()
	# The title screen has no Escape of its own: with this off, Escape on the
	# title does nothing rather than opening settings behind a screen that is
	# about to be hidden.
	if _settings_menu != null:
		_settings_menu.set_process_input(false)
	_player.set_settings_open(true)
	var warmth: Color = _chunk_manager.get_body_sky_warmth() \
		if _chunk_manager.has_method("get_body_sky_warmth") else Color(1.0, 0.76, 0.42)
	_title.open(_had_world, _current_seed(),
		String(ProjectSettings.get_setting("application/config/version", "dev")), warmth,
		not _reduce_motion(), _worlds())


func _on_start() -> void:
	if _busy:
		return
	_busy = true
	_title.hide()
	# The blocks start over the world, not over the title: hide first, then cover.
	_curtain.play_cover(_reduce_motion())
	await _curtain.covered
	_release()
	_curtain.play_reveal()
	await _curtain.revealed
	_busy = false


func _on_settings() -> void:
	_title.hide()
	if _settings_menu == null:
		_show_title()
		return
	_settings_menu.set_process_input(true)
	_settings_menu._open()


func _on_quit() -> void:
	get_tree().quit()


## Hand the screen back to the player: the HUD returns, the settings menu gets its
## input back, and the gate opens. Called at the covered frame, so the mouse is
## captured only while nothing can be seen anyway.
func _release() -> void:
	_open = false
	_restore_hud()
	if _settings_menu != null:
		_settings_menu.set_process_input(true)
	_player.set_settings_open(false)


func _show_title() -> void:
	_hide_hud()
	if _settings_menu != null:
		_settings_menu.set_process_input(false)
	_title.open(_had_world, _current_seed(),
		String(ProjectSettings.get_setting("application/config/version", "dev")),
		_chunk_manager.get_body_sky_warmth() \
			if _chunk_manager.has_method("get_body_sky_warmth") else Color(1.0, 0.76, 0.42),
		not _reduce_motion(), _worlds())
	_title.refocus()


func _hide_hud() -> void:
	if _hud == null or not _hidden.is_empty():
		return
	for child_name in HIDE_AT_TITLE:
		var node := _hud.get_node_or_null(child_name)
		if node != null and node is CanvasItem:
			_hidden[node] = node.visible
			node.visible = false


func _restore_hud() -> void:
	for node in _hidden:
		if is_instance_valid(node):
			node.visible = _hidden[node]
	_hidden.clear()


## The saves this install has, for the world list. There is one: the engine keeps a single
## save at a fixed path, so the list is this screen's shape rather than its content - when
## the engine can hold more than one world, this is the function that grows, and nothing
## in the title screen changes.
func _worlds() -> Array:
	if not FileAccess.file_exists(META_PATH):
		return []
	return [{
		"name": "MY WORLD",
		"seed": _current_seed(),
		"when": int(FileAccess.get_modified_time(META_PATH)),
	}]


## The seed of the world that is running now, or null when the file is not there
## yet. Read rather than asked of the engine so the title can show it before the
## world has finished streaming; the format is `world.meta`'s first three u32s:
## magic, version, seed.
func _current_seed():
	if not FileAccess.file_exists(META_PATH):
		return null
	var file := FileAccess.open(META_PATH, FileAccess.READ)
	if file == null:
		return null
	var magic := file.get_32()
	var version := file.get_32()
	if magic != 0x574F524C or version > 3:
		return null
	var raw := file.get_32()
	file.close()
	# Back to the signed int the generator carries.
	return raw - 0x100000000 if raw > 0x7FFFFFFF else raw


## The accessibility switch the settings menu owns. Read from the same file it
## writes, because the menu layer must not depend on that scene's internals.
func _reduce_motion() -> bool:
	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		return false
	return bool(cfg.get_value("gui", "reduce_motion", false))
