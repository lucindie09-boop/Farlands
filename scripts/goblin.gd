extends "res://scripts/dummy.gd"
## A spawned goblin: everything the K-key dummy is -- vanilla 1.8.8 gravity, drag and
## knockback at 20 tps -- plus health and a row of hearts floating over its head.
##
## Six hearts, counted in half-hearts like the player's own health (12 = 6 x 2), and
## a punch takes one half-heart, vanilla's empty fist, so the twelfth hit is the last.
## The knockback and the damage share ONE gate: dummy.gd's hurt-resistance window is
## read before the base call answers, so a resisted hit takes nothing -- hold-clicking
## cannot empty it faster than the window allows, exactly as it cannot stack knockback.
##
## The bar is the HUD's own heart art on Sprite3D billboards, so it is drawn crisp,
## unshaded, at a constant size and always facing the camera, and it is not lit by the
## world the way the body is (a UI mark over a body has no cell to take light from).

## 6 hearts, in the half-hearts `PlayerController.get_health()` also counts in.
const MAX_HEALTH := 12.0
## Vanilla's empty fist: one half-heart per hit.
const PUNCH_DAMAGE := 1.0
## The art is a 9-texel sprite on an 8-texel pitch, so neighbouring outlines share a
## column; at HEART_SIZE blocks across the pitch is this much, and a row of six spans
## about 0.85 blocks -- a head and a bit, over the head.
const HEART_SIZE := 0.16
const HEART_PITCH := HEART_SIZE * 8.0 / 9.0
## Above the feet: the model is 1.44 tall.
const BAR_HEIGHT := 1.62

const HEART_FULL: Texture2D = preload("res://Assets/textures/gui/heart_full.png")
const HEART_HALF: Texture2D = preload("res://Assets/textures/gui/heart_half.png")
const HEART_EMPTY: Texture2D = preload("res://Assets/textures/gui/heart_empty.png")

var _health := MAX_HEALTH
var _hearts: Array[Sprite3D] = []


func _ready() -> void:
	super()
	_build_bar()


## The bar follows the body: it is a child of it, so it travels with a knockback and
## turns to the camera on its own.
func _build_bar() -> void:
	var bar := Node3D.new()
	bar.name = "HealthBar"
	bar.position = Vector3(0.0, BAR_HEIGHT, 0.0)
	add_child(bar)
	var count := int(MAX_HEALTH / 2.0)
	for i in count:
		var heart := Sprite3D.new()
		heart.name = "Heart%d" % i
		heart.texture = HEART_FULL
		# One texel per HEART_SIZE/9 blocks: the hearts are sized in the world, not in
		# pixels, so they do not grow as the camera backs away.
		heart.pixel_size = HEART_SIZE / 9.0
		heart.shaded = false
		heart.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		heart.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
		# A thing in the world, not a HUD mark: DEPTH-TESTED, and drawn as an opaque
		# cut-out rather than a transparent quad. No `no_depth_test`, so a goblin behind
		# a hill takes its hearts with it instead of leaving six red dots floating over
		# the terrain, and the cut-out puts them in the opaque pass where they sort like
		# the blocks around them instead of being blended in draw order.
		heart.alpha_cut = SpriteBase3D.ALPHA_CUT_DISCARD
		heart.position = Vector3((float(i) - float(count - 1) * 0.5) * HEART_PITCH, 0.0, 0.0)
		bar.add_child(heart)
		_hearts.append(heart)
	_refresh_bar()


## Full, half or empty per heart, from the health left: heart i owns half-hearts
## 2i and 2i+1.
func _refresh_bar() -> void:
	for i in _hearts.size():
		var remaining := _health - float(i) * 2.0
		if remaining >= 2.0:
			_hearts[i].texture = HEART_FULL
		elif remaining >= 1.0:
			_hearts[i].texture = HEART_HALF
		else:
			_hearts[i].texture = HEART_EMPTY


## A punch, from PlayerController. The hurt window decides both halves of the hit, so
## it is read BEFORE the base call: a resisted hit is neither knockback nor damage.
func apply_knockback(attacker_position: Vector3, extra_velocity := Vector3.ZERO) -> void:
	var resisted: bool = _hurt_resistant_ticks > HURT_RESIST_WINDOW
	super.apply_knockback(attacker_position, extra_velocity)
	if not resisted:
		take_damage(PUNCH_DAMAGE)


func take_damage(amount: float) -> void:
	if _health <= 0.0:
		return
	_health = maxf(_health - amount, 0.0)
	_refresh_bar()
	print("Goblin hit: %.0f of %.0f half-hearts left" % [_health, MAX_HEALTH])
	if _health <= 0.0:
		die()


## At zero it goes: the body is freed, which the spawn tool notices on its next frame
## (it drops the dead body's light entry with it).
func die() -> void:
	print("Goblin dies")
	queue_free()


func get_health() -> float:
	return _health


func get_max_health() -> float:
	return MAX_HEALTH
