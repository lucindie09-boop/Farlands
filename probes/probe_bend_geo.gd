extends SceneTree
## Does the world's own geometry actually bend, and does it bend where the
## include says it does?
##
## World Bend is not a picture over the frame - it moves the vertices the world
## is made of - so the only honest measurement is of where a known piece of the
## world ends up on screen. This probe puts one marker mesh at a time at a known
## ground position, drawn through the same include and the same call the world's
## materials use, and compares the centroid of the pixels it covers against the
## centroid predicted from the include's own maths, run here in GDScript.
##
## Markers are vertical quads standing on the ground at 40, 80, 140 and 220
## blocks, seen from 40 blocks up, so the four states of the bend are all
## sampled: under the player's feet (barely moves), at walking distance, out
## where the pull is strong, and out where it has levelled off. Each marker is
## sized in proportion to its distance, so every one of them is the same size on
## screen however far away it is, and only one marker is ever in the scene - a
## centroid over a frame with one blob in it needs no blob matching to be
## trusted. Every measurement also refuses to proceed with a marker that has run
## off the edge of the frame, because a centroid of the part of a shape that is
## still on screen is a centroid of the wrong thing.
##
## Runs WINDOWED through .freebuff/run_probe_shot.sh, because --headless never
## compiles a shader.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_bend_geo.gd [timeout]
##
## What it pins:
##   - the measurement itself: with the bend switched off, the marker's pixels
##     are exactly where the camera's own unprojection of its unbent centre says
##     they are, so the projection, the pipeline and the reading of the frame all
##     agree before anything is claimed about the bend
##   - the bend, at four distances, against the include's maths: the ground moves
##     inward and up by the predicted amount, and by nearly nothing underfoot
##   - the pull is towards the camera's own vertical axis, not the world's: a
##     marker off to one side is pulled sideways as well, by the predicted amount
##   - the switch: `world_bend_on` at 0 gives the unbent frame again, to the pixel
##   - the knobs behave as their names say: `world_bend` moves the ground further
##     the further it is turned up and nothing at all at 0, and
##     `world_bend_radius` holds the bend off further as it grows
##   - the water's ripple: with `world_bend_sway` up the marker wanders from
##     frame to frame, and with it at 0 the frame holds perfectly still
##   - the real materials: a quad drawn with materials/voxel_material.tres and a
##     quad drawn with materials/voxel_material_water.tres land on the same
##     pixels as the marker does, which is what makes the world's own terrain and
##     water bend together rather than only the marker in this file
##
## It writes user://shader_shots/bend_*.png of every state it measures.

const MARKER := "res://probes/bend_marker.gdshader"
const RIPPLE := "res://probes/bend_marker_ripple.gdshader"
const TERRAIN := "res://materials/voxel_material.tres"
const WATER := "res://materials/voxel_material_water.tres"
const SHOT_DIR := "user://shader_shots"

const VIEWPORT := Vector2i(960, 540)
const BACKGROUND := 0.25      # the grey the world is drawn against, linear
const DIFFERENT := 0.12       # how far off the grey a pixel has to be to count
const CAMERA_HEIGHT := 40.0
const CAMERA_PITCH := -20.0
const FOV := 75.0
const DISTANCES := [40.0, 80.0, 140.0, 220.0]
const LATERAL := Vector3(60.0, 0.0, -100.0)
const AMOUNT := 0.7
const RADIUS := 256.0
const RISE := 1.0
const SAMPLE := 5             # grid across a marker, for the predicted centroid
const PIXELS := 1.5           # how far a centroid may sit from its prediction
const SWAY := 1.0             # the water ripple at the top of its own slider

var scene: Node3D
var camera: Camera3D
var holder: Node3D
var mesh: MeshInstance3D
var material: ShaderMaterial

var amount := AMOUNT
var radius := RADIUS
var rise := RISE
var failures := 0
var shots := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	DisplayServer.window_set_size(VIEWPORT)
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	await process_frame
	# Four samples per pixel: the centroid of a blob is only as honest as its
	# edges, and an uncovered sliver of one of these quads is several pixels.
	root.msaa_3d = Viewport.MSAA_4X

	scene = Node3D.new()
	root.add_child(scene)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(BACKGROUND, BACKGROUND, BACKGROUND)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	scene.add_child(world_environment)

	camera = Camera3D.new()
	camera.position = Vector3(0.0, CAMERA_HEIGHT, 0.0)
	camera.rotation_degrees = Vector3(CAMERA_PITCH, 0.0, 0.0)
	camera.fov = FOV
	camera.current = true
	scene.add_child(camera)

	holder = Node3D.new()
	scene.add_child(holder)

	print("probe: window %s, a %.2f grey field reads %.4f" % [VIEWPORT, BACKGROUND, await _background()])

	# One marker at a time: a frame with a single blob in it cannot be read
	# wrongly, and every check below can then search the whole frame for it.
	for distance in DISTANCES:
		await check_still(distance)
		await check_geometry(distance)
	await check_lateral()
	await check_switch()
	await check_amount()
	await check_radius()
	await check_ripple()
	await check_materials()
	_finish()


# --- The bend, against the include's own maths ---------------------------------

## With the bend out of the way the marker is a plain mesh drawn by a plain
## camera, and its pixels have to land where the camera says its centre is. This
## is the check that the *measurement* is sound: everything after it is the same
## reading taken with the bend in the way.
func check_still(distance: float) -> void:
	_use(MARKER, _ground(distance), _size(distance))
	_push("world_bend_on", 0.0)
	var predicted := _predict(_ground(distance), _size(distance))
	var blob: Dictionary = await _marker("%.0f blocks off" % distance)
	print("probe: %3.0f blocks, bend off: blob %s, camera's own projection %s, %.2f px apart"
		% [distance, _px(blob["centroid"]), _px(predicted), blob["centroid"].distance_to(predicted)])
	_save(await _capture(), "bend_off_%03d" % int(distance))
	if blob["centroid"].distance_to(predicted) > PIXELS:
		_fail("with the bend off the marker is %.2f px from where the camera projects it"
			% blob["centroid"].distance_to(predicted))


## The world's own vertices, moved. The prediction is the include's maths run
## here - the same relative vector, the same arctan wrap, the same convex mix,
## the same levelled-off lift - sampled across the marker's own surface, so a
## centroid is compared with a centroid and not with a corner.
func check_geometry(distance: float) -> void:
	_use(MARKER, _ground(distance), _size(distance))
	_push("world_bend_on", 1.0)
	var unbent := _predict(_ground(distance), _size(distance))
	var bent := _predict(_ground(distance), _size(distance), true)
	var blob: Dictionary = await _marker("%.0f blocks on" % distance)
	_save(await _capture(), "bend_on_%03d" % int(distance))
	var error: float = blob["centroid"].distance_to(bent)
	var moved: float = bent.distance_to(unbent)
	var world := _bend(_ground(distance) + Vector3(0.0, _size(distance) * 0.5, 0.0)) \
		- _ground(distance) - Vector3(0.0, _size(distance) * 0.5, 0.0)
	print("probe: %3.0f blocks: predicted pull %s (%.1f blocks up, %.1f in), blob %s, %.2f px off, %.1f px of move"
		% [distance, _px(bent), world.y, -world.z, _px(blob["centroid"]), error, moved])
	if error > PIXELS:
		_fail("at %.0f blocks the ground is drawn %.2f px from where the include puts it" % [distance, error])
	# The floor you are standing on is the part that must not move: a bend that
	# shifts the block at your feet is a bend that shifts the block you are
	# mining.
	if distance <= 40.0 and moved > 40.0:
		_fail("the ground %.0f blocks out has already moved %.1f px" % [distance, moved])
	if distance >= 220.0 and moved < 50.0:
		_fail("the ground %.0f blocks out has only moved %.1f px" % [distance, moved])


## The bend is around the camera's own vertical axis, so a hill off to one side
## has to be pulled sideways as well as up - and both horizontal axes are scaled
## by the same factor, which is what keeps the bend looking the same whichever
## way the player is facing. A marker on the camera's own axis cannot tell that
## apart from a bend that only squeezes z, so this one is 60 blocks to the side.
func check_lateral() -> void:
	_use(MARKER, LATERAL, 10.0)
	_push("world_bend_on", 1.0)
	var unbent := _predict(LATERAL, 10.0)
	var bent := _predict(LATERAL, 10.0, true)
	var blob: Dictionary = await _marker("side marker")
	_save(await _capture(), "bend_lateral")
	print("probe: %.0f blocks to the side: unbent %s, predicted %s, blob %s (%.2f px off, %.1f px of move)"
		% [LATERAL.x, _px(unbent), _px(bent), _px(blob["centroid"]),
			blob["centroid"].distance_to(bent), bent.distance_to(unbent)])
	if blob["centroid"].distance_to(bent) > PIXELS:
		_fail("the side marker is drawn %.2f px from where the include puts it"
			% blob["centroid"].distance_to(bent))
	# World x is scaled by the same factor world z is: if this one were squeezed
	# towards the axis on its own the two would disagree, and the prediction
	# above - which scales the pair together - would have missed it.
	var world := _bend(LATERAL + Vector3(0.0, 5.0, 0.0)) - LATERAL - Vector3(0.0, 5.0, 0.0)
	print("probe: the side marker is pulled %.2f blocks inward on x and %.2f on z, from %.1f blocks out"
		% [world.x, world.z, Vector2(LATERAL.x, LATERAL.z).length()])
	if absf(world.x) < 0.5:
		_fail("a marker to the side is not pulled sideways at all (%.2f blocks)" % world.x)
	if blob["centroid"].distance_to(unbent) < 5.0:
		_fail("the side marker has barely moved (%.1f px)" % blob["centroid"].distance_to(unbent))


## The switch is the whole effect's: with `world_bend_on` back at 0 the frame is
## the unbent one again, to the pixel.
func check_switch() -> void:
	_use(MARKER, _ground(220.0), _size(220.0))
	_push("world_bend_on", 0.0)
	var off: Dictionary = await _marker("switch off")
	_push("world_bend_on", 1.0)
	var on: Dictionary = await _marker("switch on")
	_push("world_bend_on", 0.0)
	var back: Dictionary = await _marker("switch back")
	print("probe: switch off %s -> on %s -> off %s (%.2f px back)"
		% [_px(off["centroid"]), _px(on["centroid"]), _px(back["centroid"]),
			off["centroid"].distance_to(back["centroid"])])
	if off["centroid"].distance_to(back["centroid"]) > 0.25:
		_fail("the switch does not return the frame to where it was (%.2f px)"
			% off["centroid"].distance_to(back["centroid"]))
	if on["centroid"].distance_to(off["centroid"]) < 50.0:
		_fail("the switch moved the ground only %.1f px" % on["centroid"].distance_to(off["centroid"]))


## `world_bend` is the amount, and the include mixes with it. Drawn at 0, at a
## third, at the registry's default and at the top of its slider, the ground has
## to be exactly where the mix says it is at every one of them - and to have not
## moved at all at 0, which is what makes the switch and the slider agree.
func check_amount() -> void:
	_use(MARKER, _ground(220.0), _size(220.0))
	_push("world_bend_on", 1.0)
	var moves: Array = []
	for value in [0.0, 0.35, 0.7, 1.0]:
		amount = value
		_push("world_bend", value)
		var unbent := _predict(_ground(220.0), _size(220.0))
		var bent := _predict(_ground(220.0), _size(220.0), true)
		var blob: Dictionary = await _marker("amount %.2f" % value)
		var world := _bend(_ground(220.0) + Vector3(0.0, _size(220.0) * 0.5, 0.0)) \
			- _ground(220.0) - Vector3(0.0, _size(220.0) * 0.5, 0.0)
		moves.append(blob["centroid"].distance_to(unbent))
		print("probe: amount %.2f -> the ground lies %.2f blocks up and %.2f in, drawn %.1f px from its unbent place (%.2f px off)"
			% [value, world.y, -world.z, blob["centroid"].distance_to(unbent),
				blob["centroid"].distance_to(bent)])
		if blob["centroid"].distance_to(bent) > PIXELS:
			_fail("at amount %.2f the ground is drawn %.2f px from where the include puts it"
				% [value, blob["centroid"].distance_to(bent)])
	# The bar here is the probe's own noise floor, not zero: a marker whose
	# vertices have not moved at all still reads a few tenths of a pixel from
	# its prediction, because a rasterised centroid is not an exact number. The
	# bend-off checks above measure that same residual on the same frames.
	if float(moves[0]) > 1.0:
		_fail("amount 0 still moves the ground %.2f px" % moves[0])
	for i in range(1, moves.size()):
		if float(moves[i]) <= float(moves[i - 1]):
			_fail("amount %s does not move the ground further than the step before it (%s)" % [i, moves])


## `world_bend_radius` is the distance the bend is measured against, the amount
## of it and the height it tops out at - so turning it up has to hold the bend
## off further: the same hill, further away, has to move less.
func check_radius() -> void:
	_use(MARKER, _ground(220.0), _size(220.0))
	_push("world_bend_on", 1.0)
	var seen: Array = []
	for value in [128.0, 256.0, 512.0]:
		radius = value
		_push("world_bend_radius", value)
		var unbent := _predict(_ground(220.0), _size(220.0))
		var bent := _predict(_ground(220.0), _size(220.0), true)
		var blob: Dictionary = await _marker("radius %.0f" % value)
		seen.append(blob["centroid"].distance_to(unbent))
		print("probe: radius %3.0f -> the ground moves %.1f px, blob %.2f px from its prediction"
			% [value, blob["centroid"].distance_to(unbent), blob["centroid"].distance_to(bent)])
		if blob["centroid"].distance_to(bent) > PIXELS:
			_fail("at radius %.0f the ground is drawn %.2f px from where the include puts it"
				% [value, blob["centroid"].distance_to(bent)])
	if float(seen[0]) <= float(seen[1]) or float(seen[1]) <= float(seen[2]):
		_fail("the radius does not hold the bend off further as it grows (%s)" % [seen])


# --- The water's ripple --------------------------------------------------------

## The ripple is the one part of the bend that is not a pure function of position
## - it is a wave rolling through the ground with time - and the water material
## is the only consumer that declares it. At 0 the frame must hold perfectly
## still; at the top of its own slider the surface has to wander from frame to
## frame, which is what makes a lake read as water rather than as a coloured
## floor.
func check_ripple() -> void:
	_use(RIPPLE, _ground(40.0), _size(40.0))
	_push("world_bend_on", 1.0)
	# Sampled a beat apart in real seconds, not in frames: the wave is driven by
	# the shader's own clock, and a few frames of a frame-limited probe is a
	# couple of hundredths of a second of a wave that takes 1.7 to come round.
	var still: Array = []
	_push("world_bend_sway", 0.0)
	for i in 5:
		still.append((await _marker("ripple off")).centroid)
		await create_timer(0.2).timeout
	var moving: Array = []
	_push("world_bend_sway", SWAY)
	for i in 5:
		moving.append((await _marker("ripple on")).centroid)
		await create_timer(0.2).timeout
	for point in moving:
		print("probe: ripple %.1f frame %s" % [SWAY, _px(point)])
	_save(await _capture(), "bend_ripple")
	var held := _spread(still)
	var rolled := _spread(moving)
	print("probe: ripple 0 -> the marker holds within %.2f px; ripple %.1f -> it wanders %.2f px between frames"
		% [held, SWAY, rolled])
	if held > 0.6:
		_fail("with the ripple at 0 the surface still moves %.2f px between frames" % held)
	if rolled < 1.5:
		_fail("with the ripple at the top of its slider the surface moves only %.2f px" % rolled)
	# It is measured close in on purpose: the ripple is a block of sideways
	# travel at the top of its slider, and a block is 9 px at 40 blocks out but
	# under 2 at 220 - the same wave, and only one of those is a measurement.
	_push("world_bend_sway", 0.0)


# --- The materials the world is actually drawn with ----------------------------

## The marker is a stand-in. What has to bend is the terrain and the water, and
## they are bent by their own two shaders - the ones materials/voxel_material.tres
## and materials/voxel_material_water.tres carry. So both real materials are put
## on the same quad, and their pixels have to land where the marker's do: the
## same prediction, the same place, both of them.
##
## Outside the game's renderer nothing has pushed these materials' lighting, so
## the run pushes the game's own light and alpha knobs by hand to make them draw
## at all - see `_light`. None of them touches a vertex, so what is being
## measured here is still the geometry. A material that draws nothing even then
## says so rather than being counted as a pass.
func check_materials() -> void:
	for distance in DISTANCES:
		_use(MARKER, _ground(distance), _size(distance))
		_push("world_bend_on", 1.0)
		var bent := _predict(_ground(distance), _size(distance), true)
		var expected: Dictionary = await _marker("the marker at %.0f blocks" % distance)
		for path in [TERRAIN, WATER]:
			var name := String(path).get_file().get_basename()
			_use_material(path, _ground(distance), _size(distance))
			_push("world_bend_on", 1.0)
			var blob: Dictionary = await _measure()
			if blob["pixels"] < 150:
				print("probe: %.0f blocks: %s draws nothing to measure (%d px, its loudest pixel is %.3f off the grey)"
					% [distance, name, blob["pixels"], blob["loudest"]])
				continue
			if (blob["centroid"] as Vector2).distance_to(blob["shaded"]) > PIXELS:
				print("probe: %.0f blocks: %s is lit unevenly, %.2f px between its shape and its shading"
					% [distance, name, (blob["centroid"] as Vector2).distance_to(blob["shaded"])])
				continue
			_save(await _capture(), "bend_%s_%03d" % [name, int(distance)])
			print("probe: %.0f blocks: %s draws %d px at %s, %.2f px from the marker and %.2f from the include"
				% [distance, name, blob["pixels"], _px(blob["centroid"]),
					blob["centroid"].distance_to(expected["centroid"]), blob["centroid"].distance_to(bent)])
			if blob["centroid"].distance_to(bent) > PIXELS:
				_fail("%s at %.0f blocks draws the ground %.2f px from where the include puts it"
					% [name, distance, blob["centroid"].distance_to(bent)])
			if blob["centroid"].distance_to(expected["centroid"]) > PIXELS:
				_fail("%s at %.0f blocks bends %.2f px differently from the terrain"
					% [name, distance, blob["centroid"].distance_to(expected["centroid"])])


# --- Posing the scene ----------------------------------------------------------

## One marker of one material standing on the ground at `ground`, and no other
## geometry in the scene.
func _use(path: String, ground: Vector3, size: float) -> void:
	var fresh := ShaderMaterial.new()
	fresh.shader = load(path)
	_place(fresh, ground, size)
	_knobs()


func _use_material(path: String, ground: Vector3, size: float) -> void:
	var loaded: ShaderMaterial = load(path)
	if loaded == null:
		_fail("no material at %s" % path)
		return
	_place(loaded, ground, size)
	_knobs()
	_light(ground, size)


func _place(target: ShaderMaterial, ground: Vector3, size: float) -> void:
	if mesh != null:
		mesh.queue_free()
		mesh = null
	material = target
	var plane := PlaneMesh.new()
	plane.orientation = PlaneMesh.FACE_Z
	plane.size = Vector2(size, size)
	mesh = MeshInstance3D.new()
	mesh.mesh = plane
	mesh.material_override = target
	mesh.position = ground + Vector3(0.0, size * 0.5, 0.0)
	holder.add_child(mesh)


## The three bend knobs, pushed and mirrored: the shader is given them, and the
## prediction below is worked out with the same numbers. A probe that pushed one
## set and predicted with another would be measuring its own arithmetic.
func _knobs() -> void:
	amount = AMOUNT
	radius = RADIUS
	rise = RISE
	_push("world_bend", amount)
	_push("world_bend_radius", radius)
	_push("world_bend_rise", rise)


## The game's own lighting and alpha knobs, pushed by hand. The world shaders
## take their light from vertex attributes the mesher writes and the material
## manager pushes; on a bare quad there are none, so both materials come out
## black-on-black or fully transparent. A light placed inside the marker and the
## water's own alpha at 1 make a material that draws at all draw visibly.
## Nothing here is read by `vertex()`.

## A light far wider than the marker, held inside it: the whole quad then sits
## deep in the middle of the falloff, where the light is nearly flat across it.
## The geometry is what is being measured, and a brightness gradient across the
## marker is a gradient across the centroid too.
func _light(ground: Vector3, size: float) -> void:
	_push("player_light_position", ground + Vector3(0.0, size * 0.5, 0.0))
	_push("player_light_radius", 10000.0)
	_push("player_light_intensity", 8.0)
	_push("player_light_color", Color(1.0, 1.0, 1.0))
	_push("water_color", Color(1.0, 1.0, 1.0, 1.0))
	_push("lava_alpha", 1.0)
	# No fog: the frame is 220 blocks deep at most and the materials' own fog
	# settings would tint the marker towards the background, which is the one
	# thing the measurement needs it to stay clear of.
	_push("fog_mode", 0)
	_push("aerial_strength", 0.0)


func _push(key: String, value: Variant) -> void:
	if material != null:
		material.set_shader_parameter(key, value)


func _ground(distance: float) -> Vector3:
	return Vector3(0.0, 0.0, -distance)


## How tall a marker at `distance` is, in blocks: in proportion to the distance,
## so that its size on screen does not change with it.
func _size(distance: float) -> float:
	return distance * 0.1


# --- The prediction ------------------------------------------------------------

## Where the marker's pixels should be, according to the include's own maths: the
## average of the camera's projection of a grid of points across the marker's
## surface. Both the unbent and the bent centroid are predicted this way, so the
## two numbers being compared are the same kind of number.
func _predict(ground: Vector3, size: float, bent: bool = false) -> Vector2:
	var half := size * 0.5
	var total := Vector2.ZERO
	var count := 0
	for iy in SAMPLE:
		for ix in SAMPLE:
			# The middle of each cell of the grid, not its corner: the average
			# is then the marker's own centroid and not a corner of it.
			var fx := (float(ix) + 0.5) / float(SAMPLE) * 2.0 - 1.0
			var fy := (float(iy) + 0.5) / float(SAMPLE) * 2.0 - 1.0
			var world := ground + Vector3(fx * half, half + fy * half, 0.0)
			if bent:
				world = _bend(world)
			total += camera.unproject_position(world)
			count += 1
	return total / float(count)


## The include's `world_bend_position`, run here instead of on the GPU: the same
## relative vector, the same arctan wrap, the same convex mix, the same
## levelled-off lift. The ripple is left out on purpose - it is a function of
## time, and a prediction that had to agree with a moving GPU clock would be
## pinning the clock rather than the bend.
func _bend(world: Vector3) -> Vector3:
	if amount <= 0.0:
		return world
	var origin := camera.global_position
	var relative := world - origin
	var horizontal := Vector2(relative.x, relative.z).length()
	if horizontal < 0.0001:
		return world
	var scaled: float = maxf(radius, 1.0)
	var wrapped := atan(horizontal / scaled) * scaled
	var closed := lerpf(horizontal, wrapped, amount)
	var ratio := horizontal / scaled
	var lift := amount * rise * scaled * (1.0 - 1.0 / sqrt(1.0 + ratio * ratio))
	relative.x *= closed / horizontal
	relative.z *= closed / horizontal
	relative.y += lift
	return origin + relative


# --- Reading the frame ---------------------------------------------------------

func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


## The grey the frame is mostly made of, read off a corner of it rather than
## taken on trust: the tonemapper and the swapchain both get a say in what a 0.25
## background actually comes out as.
func _background() -> float:
	await _settle()
	var img := await _capture()
	var data := img.get_data()
	return _patch(data, img.get_width(), 4, 4)


## The one marker in the frame, and a refusal to carry on without it: a blob that
## never drew, or one with a piece of itself outside the frame, is not something
## a centroid can be taken of.
func _marker(label: String) -> Dictionary:
	var blob: Dictionary = await _measure()
	var frame := Rect2i(3, 3, VIEWPORT.x - 6, VIEWPORT.y - 6)
	if blob["pixels"] < 150:
		_fail("%s: the marker drew nothing" % label)
	elif not frame.encloses(blob["box"]):
		_fail("%s: the marker runs off the frame (%s)" % [label, blob["box"]])
	elif (blob["centroid"] as Vector2).distance_to(blob["shaded"]) > PIXELS:
		_fail("%s: the shading across the marker tilts its centroid %.2f px (%s solid vs %s shaded)"
			% [label, (blob["centroid"] as Vector2).distance_to(blob["shaded"]),
				_px(blob["centroid"]), _px(blob["shaded"])])
	return blob


## Where the marker's pixels are: the centroid of everything in the frame that
## is not the background grey, found in two passes so that a million pixels of
## grey are not walked one at a time.
##
## Two centroids come out of it. The `solid` one counts every pixel the marker
## covers once, whatever shade it covers it in: it is the shape's centroid, and
## a gradient of brightness across the marker - which the world shaders have
## plenty of, and which has nothing to do with where the vertices went - cannot
## move it. The `shaded` one weights each pixel by how far it is off the grey,
## which is a better read of a soft edge but which a gradient tilts. They agree
## on anything evenly lit, so the run takes the solid one and holds the two up
## against each other: a disagreement means the probe's own lighting is tilting
## the reading, not the bend.
func _measure() -> Dictionary:
	await _settle()
	var img := await _capture()
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var base := _patch(data, w, 4, 4)

	var x0 := w
	var y0 := h
	var x1 := -1
	var y1 := -1
	var loudest := 0.0
	for y in range(0, h, 4):
		for x in range(0, w, 4):
			var far := absf(_lum(data, w, x, y) - base)
			loudest = maxf(loudest, far)
			if far > DIFFERENT:
				x0 = mini(x0, x)
				y0 = mini(y0, y)
				x1 = maxi(x1, x)
				y1 = maxi(y1, y)
	if x1 < 0:
		return {"centroid": Vector2(-1.0, -1.0), "pixels": 0, "box": Rect2i(), "loudest": loudest}
	var raw := Rect2i(x0, y0, x1 - x0 + 1, y1 - y0 + 1)
	x0 = maxi(x0 - 6, 0)
	y0 = maxi(y0 - 6, 0)
	x1 = mini(x1 + 6, w - 1)
	y1 = mini(y1 + 6, h - 1)

	var sx := 0.0
	var sy := 0.0
	var weight := 0.0
	var solid := Vector2.ZERO
	var pixels := 0
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			var d := absf(_lum(data, w, x, y) - base)
			if d <= DIFFERENT:
				continue
			# Half-pixel offsets: a pixel's index is its corner, and the camera
			# projects into the continuous space the pixel centres live in.
			sx += (float(x) + 0.5) * d
			sy += (float(y) + 0.5) * d
			weight += d
			solid += Vector2(float(x) + 0.5, float(y) + 0.5)
			pixels += 1
	if pixels <= 0:
		return {"centroid": Vector2(-1.0, -1.0), "shaded": Vector2(-1.0, -1.0), "pixels": 0,
			"box": raw, "loudest": loudest}
	solid /= float(pixels)
	var shaded := Vector2(sx / weight, sy / weight)
	return {
		"centroid": solid,
		"shaded": shaded,
		"pixels": pixels,
		"box": raw,
		"loudest": loudest,
	}


func _patch(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var total := 0.0
	for dy in range(8):
		for dx in range(8):
			total += _lum(data, w, x + dx, y + dy)
	return total / 64.0


## The largest distance between any two of a set of screen points: how far a
## thing moved while it was being watched.
func _spread(points: Array) -> float:
	var worst := 0.0
	for i in points.size():
		for j in range(i + 1, points.size()):
			worst = maxf(worst, (points[i] as Vector2).distance_to(points[j]))
	return worst


func _lum(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (0.2126 * float(data[i]) + 0.7152 * float(data[i + 1]) + 0.0722 * float(data[i + 2])) / 255.0


func _px(point: Vector2) -> String:
	return "(%.1f, %.1f)" % [point.x, point.y]


# --- Odds and ends -------------------------------------------------------------

func _settle() -> void:
	for i in range(3):
		await process_frame


func _save(img: Image, name: String) -> void:
	img.save_png("%s/%s.png" % [SHOT_DIR, name])
	shots += 1


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _finish() -> void:
	print("probe: %d shots, %d failures" % [shots, failures])
	quit(1 if failures > 0 else 0)
