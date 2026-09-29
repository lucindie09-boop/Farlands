extends SceneTree
## Does the Horizon Curve actually move the world's own geometry, and does it
## move it where the include says?
##
## Like World Bend, this effect is not a picture over the frame - it sinks the
## vertices the world is made of - so the only honest measurement is of where a
## known piece of the world ends up on screen. This probe puts one marker mesh at
## a time at a known ground position, drawn through the same include and the same
## call the world's materials use, and compares the centroid of the pixels it
## covers against the centroid predicted from the include's own maths, run here in
## GDScript.
##
## Markers stand at 40, 160, 400 and 800 blocks, seen from 40 blocks up, and each
## is sized in proportion to its distance so that every one of them is the same
## size on screen however far away it is. A vertical marker at a distance moves
## *down* and by nothing else - that is the whole shape of the effect, and it is
## measured against the flat one; a horizontal plate spanning 150 to 700 blocks is
## measured as well, because that is what the world actually is: a surface whose
## far edge is drawn lower than its near edge, which is the curve itself. Only one
## marker is ever in the scene, because a centroid over a frame with one blob in
## it needs no blob matching to be trusted, and every measurement refuses to
## proceed with a marker that has run off the edge of the frame.
##
## Runs WINDOWED through .freebuff/run_probe_shot.sh, because --headless never
## compiles a shader.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_horizon_geo.gd [timeout]
##
## What it pins:
##   - the measurement itself: with the curve switched off, the marker's pixels
##     are exactly where the camera's own unprojection of its centre says they
##     are, so the projection, the pipeline and the reading of the frame all agree
##     before anything is claimed about the curve
##   - the curve, at four distances, against the include's maths: the ground is
##     drawn lower by the sphere's own sagitta and by nearly nothing underfoot
##   - that it is *only* downward: the marker does not move sideways by a pixel at
##     any of those distances, which is what keeps every distance measured on the
##     ground, and every vertical edge, where they were
##   - a horizontal plate: the far edge of a flat surface is drawn lower than its
##     near edge by the predicted amount, which is the curve as a player sees it
##   - the switch: `world_horizon_on` at 0 gives the unbent frame again, to the
##     pixel
##   - the knob behaves as its name says: a smaller planet sinks the far ground
##     further, and every value lands where the include puts it
##   - the two effects together, which is what makes them independent: with World
##     Bend on as well the marker lands where the two includes applied in order
##     put it
##   - the real materials: a quad drawn with materials/voxel_material.tres and a
##     quad drawn with materials/voxel_material_water.tres land on the same pixels
##     as the marker does, which is what keeps a shoreline welded under the curve
##
## It writes user://shader_shots/curve_*.png of every state it measures.

const MARKER := "res://probes/horizon_marker.gdshader"
const TERRAIN := "res://materials/voxel_material.tres"
const WATER := "res://materials/voxel_material_water.tres"
const SHOT_DIR := "user://shader_shots"

const VIEWPORT := Vector2i(960, 540)
const BACKGROUND := 0.25      # the grey the world is drawn against, linear
const DIFFERENT := 0.12       # how far off the grey a pixel has to be to count
const CAMERA_HEIGHT := 40.0
const CAMERA_PITCH := -20.0
const FOV := 75.0
const DISTANCES := [40.0, 160.0, 400.0, 800.0]
const AMOUNT := 0.7           # the bend's amount, for the composed check
const BEND_RADIUS := 256.0
const BEND_RISE := 1.0
const DEFAULT_RADIUS := 1500.0  # the registry's own default, so the probe measures what ships
const SAMPLE := 5             # grid across a marker, for the predicted centroid
const PIXELS := 1.5           # how far a marker's centroid may sit from its prediction
# A horizontal surface is the one measurement here whose prediction is built a
# different way - area-weighted projected cells, see `_predicted_plate` - and the
# two centroids are then not quite the same kind of number: a covered pixel at the
# edge of a large shape is counted whole whatever fraction of it the shape covers.
# Two and a half pixels is that difference, and it is why the plate gets its own
# bar rather than the markers'
const PLATE_PIXELS := 2.5
# The plate: flat ground from 150 blocks out to 700, seen from the camera at 40
# blocks up, so its near edge and its far edge are sunk by very different amounts
# and the surface it draws is a curve rather than a quad.
const PLATE_NEAR := 150.0
const PLATE_FAR := 700.0
const PLATE_HALF_WIDTH := 120.0
# Cells across the plate, so it is a surface rather than four corners: 81 vertices
# is 81 places the include's own maths has to be right on one shape, and it is
# what makes the drawn plate the piecewise-linear curve a player actually sees
# (the terrain's own vertices are denser still).
const PLATE_CELLS := 8

# How a sample point is warped before it is projected. The same order the world's
# materials apply them in.
enum { AS_IS, BENT, CURVED, BOTH }

var scene: Node3D
var camera: Camera3D
var holder: Node3D
var mesh: MeshInstance3D
var material: ShaderMaterial

var radius := DEFAULT_RADIUS
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
	await check_plate()
	await check_switch()
	await check_radius()
	await check_compose()
	await check_materials()
	_finish()


# --- The curve, against the include's own maths ---------------------------------

## With the curve out of the way the marker is a plain mesh drawn by a plain
## camera, and its pixels have to land where the camera says its centre is. This
## is the check that the *measurement* is sound: everything after it is the same
## reading taken with the curve in the way.
func check_still(distance: float) -> void:
	_use(MARKER, _ground(distance), _size(distance))
	_push("world_horizon_on", 0.0)
	var predicted := _predicted(_ground(distance), _size(distance), AS_IS)
	var blob: Dictionary = await _marker("%.0f blocks off" % distance)
	print("probe: %4.0f blocks, curve off: blob %s, camera's own projection %s, %.2f px apart"
		% [distance, _px(blob["centroid"]), _px(predicted), blob["centroid"].distance_to(predicted)])
	_save(await _capture(), "curve_off_%04d" % int(distance))
	if blob["centroid"].distance_to(predicted) > PIXELS:
		_fail("with the curve off the marker is %.2f px from where the camera projects it"
			% blob["centroid"].distance_to(predicted))


## The world's own vertices, sunk. The prediction is the include's maths run here
## - the same horizontal distance from the camera, the same ratio, the same
## sagitta - sampled across the marker's own surface, so a centroid is compared
## with a centroid and not with a corner. And the move has to be *downward*: the
## marker's x is where it was, to the pixel, because the include changes y and
## nothing else.
func check_geometry(distance: float) -> void:
	_use(MARKER, _ground(distance), _size(distance))
	_push("world_horizon_on", 1.0)
	var unbent := _predicted(_ground(distance), _size(distance), AS_IS)
	var sunk := _predicted(_ground(distance), _size(distance), CURVED)
	var blob: Dictionary = await _marker("%.0f blocks on" % distance)
	_save(await _capture(), "curve_on_%04d" % int(distance))
	var error: float = blob["centroid"].distance_to(sunk)
	var dropped: float = float(blob["centroid"].y) - float(unbent.y)
	var sideways: float = absf(float(blob["centroid"].x) - float(unbent.x))
	var world := _curve(_ground(distance) + Vector3(0.0, _size(distance) * 0.5, 0.0)) \
		- _ground(distance) - Vector3(0.0, _size(distance) * 0.5, 0.0)
	print("probe: %4.0f blocks: the ground is %.2f blocks lower, drawn %.1f px down and %.1f px across, blob %s, %.2f px off"
		% [distance, -world.y, dropped, sideways, _px(blob["centroid"]), error])
	if error > PIXELS:
		_fail("at %.0f blocks the ground is drawn %.2f px from where the include puts it" % [distance, error])
	if sideways > 1.0:
		_fail("at %.0f blocks the ground has also moved %.2f px sideways, which nothing in the include does"
			% [distance, sideways])
	# The floor you are standing on is the part that must not move: a curve that
	# shifts the block at your feet is a curve that shifts the block you are
	# mining, and the include's own numbers say it does not.
	if distance <= 40.0 and (-world.y > 1.0 or absf(dropped) > 8.0):
		_fail("the ground %.0f blocks out has already dropped %.2f blocks / %.1f px"
			% [distance, -world.y, dropped])
	if distance >= 800.0 and -world.y < 20.0:
		_fail("the ground %.0f blocks out has only dropped %.2f blocks" % [distance, -world.y])


## Flat ground, from PLATE_NEAR out to PLATE_FAR, one horizontal quad. This is the
## shape the effect is for: the far edge of a level surface drawn lower than the
## near edge, which is what makes a plain look like a plain on a planet rather
## than a plain on a table. The prediction is the same grid of samples through the
## plate's own surface, and the *spread* of the drop across the plate's depth is
## printed, because that is the curve and a single number could not show it.
func check_plate() -> void:
	var width := PLATE_HALF_WIDTH * 2.0
	var depth := PLATE_FAR - PLATE_NEAR
	var centre := Vector3(0.0, 0.0, -(PLATE_NEAR + PLATE_FAR) * 0.5)
	_use(MARKER, centre, Vector2(width, depth), true)
	_push("world_horizon_on", 1.0)
	var unbent := _predicted_plate(centre, width, depth, AS_IS)
	var sunk := _predicted_plate(centre, width, depth, CURVED)
	var blob: Dictionary = await _marker("the plate")
	_save(await _capture(), "curve_plate")
	var near_drop := -(_curve(Vector3(0.0, 0.0, -PLATE_NEAR)) - Vector3(0.0, 0.0, -PLATE_NEAR)).y
	var far_drop := -(_curve(Vector3(0.0, 0.0, -PLATE_FAR)) - Vector3(0.0, 0.0, -PLATE_FAR)).y
	print("probe: plate %.0f..%.0f blocks: near edge %.1f blocks down, far edge %.1f, blob %s (%.2f px off its own prediction), %.1f px lower than flat"
		% [PLATE_NEAR, PLATE_FAR, near_drop, far_drop, _px(blob["centroid"]),
			blob["centroid"].distance_to(sunk), float(blob["centroid"].y) - float(unbent.y)])
	if blob["centroid"].distance_to(sunk) > PLATE_PIXELS:
		_fail("the plate is drawn %.2f px from where the include puts it"
			% blob["centroid"].distance_to(sunk))
	# The curve itself, as the player sees it: the far edge of the same flat surface
	# is drawn far lower than the near edge, and the surface's own centroid has
	# moved down a visible distance rather than merely shifted as a rigid bed would.
	if far_drop < near_drop * 8.0:
		_fail("the plate's far edge is only %.2f blocks lower than its near edge, which is not a curve"
			% (far_drop - near_drop))
	if unbent.distance_to(sunk) < 3.0:
		_fail("the plate's own prediction moves only %.2f px, so this frame measures nothing"
			% unbent.distance_to(sunk))


## The switch is the whole effect's: with `world_horizon_on` back at 0 the frame is
## the unbent one again, to the pixel.
func check_switch() -> void:
	_use(MARKER, _ground(800.0), _size(800.0))
	_push("world_horizon_on", 0.0)
	var off: Dictionary = await _marker("switch off")
	_push("world_horizon_on", 1.0)
	var on: Dictionary = await _marker("switch on")
	_push("world_horizon_on", 0.0)
	var back: Dictionary = await _marker("switch back")
	print("probe: switch off %s -> on %s -> off %s (%.2f px back)"
		% [_px(off["centroid"]), _px(on["centroid"]), _px(back["centroid"]),
			off["centroid"].distance_to(back["centroid"])])
	if off["centroid"].distance_to(back["centroid"]) > 0.25:
		_fail("the switch does not return the frame to where it was (%.2f px)"
			% off["centroid"].distance_to(back["centroid"]))
	if on["centroid"].distance_to(off["centroid"]) < 8.0:
		_fail("the switch moved the ground only %.1f px" % on["centroid"].distance_to(off["centroid"]))


## `world_horizon_radius` is the planet the world is bent onto, so a smaller one
## has to sink the same ground further - and every one of them has to land exactly
## where the include's own expression puts it.
func check_radius() -> void:
	_use(MARKER, _ground(800.0), _size(800.0))
	_push("world_horizon_on", 1.0)
	var seen: Array = []
	for value in [32000.0, 8000.0, 2000.0]:
		radius = value
		_push("world_horizon_radius", value)
		var sunk := _predicted(_ground(800.0), _size(800.0), CURVED)
		var blob: Dictionary = await _marker("radius %.0f" % value)
		seen.append(blob["centroid"].distance_to(sunk))
		print("probe: planet radius %6.0f -> the far ground is drawn %.2f px from its prediction, %.1f px from flat"
			% [value, blob["centroid"].distance_to(sunk),
				blob["centroid"].distance_to(_predicted(_ground(800.0), _size(800.0), AS_IS))])
		if blob["centroid"].distance_to(sunk) > PIXELS:
			_fail("at planet radius %.0f the ground is drawn %.2f px from where the include puts it"
				% [value, blob["centroid"].distance_to(sunk)])
	radius = DEFAULT_RADIUS
	_push("world_horizon_radius", radius)


## The two geometry effects are independent - each is a function of where a vertex
## really is - so with the bend on as well the marker has to be drawn where the
## bend and then the curve put it, and not where either of them alone would.
func check_compose() -> void:
	_use(MARKER, _ground(400.0), _size(400.0))
	_push("world_horizon_on", 1.0)
	_push("world_bend_on", 1.0)
	var unbent := _predicted(_ground(400.0), _size(400.0), AS_IS)
	var curved := _predicted(_ground(400.0), _size(400.0), CURVED)
	var bent := _predicted(_ground(400.0), _size(400.0), BENT)
	var both := _predicted(_ground(400.0), _size(400.0), BOTH)
	var blob: Dictionary = await _marker("both effects")
	_save(await _capture(), "curve_and_bend")
	print("probe: both on: blob %s, both predicted %s (%.2f px), curve alone %s, bend alone %s"
		% [_px(blob["centroid"]), _px(both), blob["centroid"].distance_to(both), _px(curved), _px(bent)])
	if blob["centroid"].distance_to(both) > PIXELS:
		_fail("with both effects on the ground is drawn %.2f px from where the two includes put it"
			% blob["centroid"].distance_to(both))
	if blob["centroid"].distance_to(unbent) < 10.0:
		_fail("with both effects on the ground has barely moved (%.1f px)"
			% blob["centroid"].distance_to(unbent))
	_push("world_bend_on", 0.0)


# --- The materials the world is actually drawn with -----------------------------

## The marker is a stand-in. What has to sink is the terrain and the water, and
## they are moved by their own two shaders - the ones materials/voxel_material.tres
## and materials/voxel_material_water.tres carry. So both real materials are put on
## the same quad, and their pixels have to land where the marker's do: the same
## prediction, the same place, both of them. That is also the check that the
## terrain and the water sink by the same amount at the same place, which is what
## keeps a shoreline welded.
##
## Outside the game's renderer nothing has pushed these materials' lighting, so
## the run pushes the game's own light and alpha knobs by hand to make them draw
## at all - see `_light`. None of them touches a vertex, so what is being measured
## here is still the geometry. A material that draws nothing even then says so
## rather than being counted as a pass.
func check_materials() -> void:
	for distance in [400.0, 800.0]:
		_use(MARKER, _ground(distance), _size(distance))
		_push("world_horizon_on", 1.0)
		var sunk := _predicted(_ground(distance), _size(distance), CURVED)
		var expected: Dictionary = await _marker("the marker at %.0f blocks" % distance)
		for path in [TERRAIN, WATER]:
			var name := String(path).get_file().get_basename()
			_use_material(path, _ground(distance), _size(distance))
			_push("world_horizon_on", 1.0)
			var blob: Dictionary = await _measure()
			if blob["pixels"] < 150:
				print("probe: %.0f blocks: %s draws nothing to measure (%d px, its loudest pixel is %.3f off the grey)"
					% [distance, name, blob["pixels"], blob["loudest"]])
				continue
			if (blob["centroid"] as Vector2).distance_to(blob["shaded"]) > PIXELS:
				print("probe: %.0f blocks: %s is lit unevenly, %.2f px between its shape and its shading"
					% [distance, name, (blob["centroid"] as Vector2).distance_to(blob["shaded"])])
				continue
			_save(await _capture(), "curve_%s_%04d" % [name, int(distance)])
			print("probe: %.0f blocks: %s draws %d px at %s, %.2f px from the marker and %.2f from the include"
				% [distance, name, blob["pixels"], _px(blob["centroid"]),
					blob["centroid"].distance_to(expected["centroid"]), blob["centroid"].distance_to(sunk)])
			if blob["centroid"].distance_to(sunk) > PIXELS:
				_fail("%s at %.0f blocks draws the ground %.2f px from where the include puts it"
					% [name, distance, blob["centroid"].distance_to(sunk)])
			if blob["centroid"].distance_to(expected["centroid"]) > PIXELS:
				_fail("%s at %.0f blocks sinks %.2f px differently from the terrain"
					% [name, distance, blob["centroid"].distance_to(expected["centroid"])])


# --- Posing the scene ----------------------------------------------------------

## One marker of one material in the scene and no other geometry: a vertical quad
## standing on the ground at `ground`, or a horizontal one lying in it when
## `flat`.
func _use(path: String, ground: Vector3, size, flat: bool = false) -> void:
	var fresh := ShaderMaterial.new()
	fresh.shader = load(path)
	_place(fresh, ground, size, flat)
	_knobs()


func _use_material(path: String, ground: Vector3, size: float) -> void:
	var loaded: ShaderMaterial = load(path)
	if loaded == null:
		_fail("no material at %s" % path)
		return
	_place(loaded, ground, size, false)
	_knobs()
	_light(ground, size)


func _place(target: ShaderMaterial, ground: Vector3, size, flat: bool) -> void:
	if mesh != null:
		mesh.queue_free()
		mesh = null
	material = target
	var plane := PlaneMesh.new()
	plane.orientation = PlaneMesh.FACE_Y if flat else PlaneMesh.FACE_Z
	plane.size = size if size is Vector2 else Vector2(float(size), float(size))
	if flat:
		plane.subdivide_width = PLATE_CELLS
		plane.subdivide_depth = PLATE_CELLS
	mesh = MeshInstance3D.new()
	mesh.mesh = plane
	mesh.material_override = target
	# A vertical quad stands its own height above the ground; a flat one lies on
	# it, and the ground is the y of the centre it is given.
	mesh.position = ground if flat else ground + Vector3(0.0, float(plane.size.y) * 0.5, 0.0)
	holder.add_child(mesh)


## The curve's knob and the bend's three, pushed and mirrored: the shader is given
## them and the predictions below are worked out with the same numbers. A probe
## that pushed one set and predicted with another would be measuring its own
## arithmetic.
func _knobs() -> void:
	radius = radius if radius > 0.0 else DEFAULT_RADIUS
	_push("world_horizon_radius", radius)
	_push("world_bend", AMOUNT)
	_push("world_bend_radius", BEND_RADIUS)
	_push("world_bend_rise", BEND_RISE)


## The game's own lighting and alpha knobs, pushed by hand: the world shaders take
## their light from vertex attributes the mesher writes and the material manager
## pushes, and on a bare quad there are none. A light far wider than the marker,
## held inside it, and the water's own alpha at 1 make a material that draws at
## all draw visibly and evenly. Nothing here is read by `vertex()`.
func _light(ground: Vector3, size: float) -> void:
	_push("player_light_position", ground + Vector3(0.0, size * 0.5, 0.0))
	_push("player_light_radius", 10000.0)
	_push("player_light_intensity", 8.0)
	_push("player_light_color", Color(1.0, 1.0, 1.0))
	_push("water_color", Color(1.0, 1.0, 1.0, 1.0))
	_push("lava_alpha", 1.0)
	# No fog: the frame is under a kilometre deep and the materials' own fog
	# settings would tint the marker towards the background, which is the one
	# thing the measurement needs it to stay clear of.
	_push("fog_mode", 0)
	_push("aerial_strength", 0.0)


func _push(key: String, value: Variant) -> void:
	if material != null:
		material.set_shader_parameter(key, value)


func _ground(distance: float) -> Vector3:
	return Vector3(0.0, 0.0, -distance)


## How big a marker at `distance` is, in blocks: in proportion to the distance, so
## that its size on screen does not change with it.
func _size(distance: float) -> float:
	return distance * 0.1


# --- The prediction ------------------------------------------------------------

## Where a horizontal surface's pixels should be, according to the include's own
## maths. A grid of points through the plate is warped, projected and then
## **area-weighted** cell by cell, because a tilted surface's projection is not
## affine: the average of a grid of world points is not the same screen point as
## the average of the pixels the shape covers, and only the second one is what a
## centroid of the frame measures. The cells are the mesh's own (see
## PLATE_CELLS), so this is the shape the renderer draws.
func _predicted_plate(centre: Vector3, width: float, depth: float, mode: int) -> Vector2:
	var total := Vector2.ZERO
	var area := 0.0
	for iz in PLATE_CELLS:
		for ix in PLATE_CELLS:
			var corner: Array = []
			for step in [Vector2i(0, 0), Vector2i(1, 0), Vector2i(1, 1), Vector2i(0, 1)]:
				var gx := (float(ix) + float(step.x)) / float(PLATE_CELLS)
				var gz := (float(iz) + float(step.y)) / float(PLATE_CELLS)
				var point := centre + Vector3((gx - 0.5) * width, 0.0, (gz - 0.5) * depth)
				corner.append(camera.unproject_position(_warp(point, mode)))
			var cell_area := _polygon_area(corner)
			total += _polygon_centroid(corner) * cell_area
			area += cell_area
	return total / area if area > 0.0 else centre


# The shoelace area of a projected quad, and its centroid: the two numbers a
# screen-space centroid has to be built out of.
func _polygon_area(points: Array) -> float:
	var total := 0.0
	for i in points.size():
		var a: Vector2 = points[i]
		var b: Vector2 = points[(i + 1) % points.size()]
		total += a.x * b.y - b.x * a.y
	return absf(total) * 0.5


func _polygon_centroid(points: Array) -> Vector2:
	var total := 0.0
	var sum := Vector2.ZERO
	for i in points.size():
		var a: Vector2 = points[i]
		var b: Vector2 = points[(i + 1) % points.size()]
		var cross := a.x * b.y - b.x * a.y
		total += cross
		sum += (a + b) * cross
	if absf(total) < 0.000001:
		var flat := Vector2.ZERO
		for point in points:
			flat += point
		return flat / float(points.size())
	return sum / (3.0 * total)


## Where a vertical marker's pixels should be, according to the include's own
## maths: the average of the camera's projection of a grid of points across its
## surface. `ground` is the foot of the marker, the same point `_place` stands it
## on - the grid spans the marker's own face, from the ground to its full height.
## A quad facing the camera is projected nearly affinely, so the average of the
## grid is the shape's own centroid - which is why the markers are used for the
## per-distance checks and the area-weighted `_predicted_plate` above for the flat
## one. Every mode is predicted this way, so the numbers being compared are the
## same kind of number.
func _predicted(ground: Vector3, size: float, mode: int) -> Vector2:
	var half := size * 0.5
	var total := Vector2.ZERO
	var count := 0
	for iy in SAMPLE:
		for ix in SAMPLE:
			# The middle of each cell of the grid, not its corner: the average is
			# then the marker's own centroid and not a corner of it.
			var fx := (float(ix) + 0.5) / float(SAMPLE) * 2.0 - 1.0
			var fy := (float(iy) + 0.5) / float(SAMPLE) * 2.0 - 1.0
			var point := ground + Vector3(fx * half, half + fy * half, 0.0)
			total += camera.unproject_position(_warp(point, mode))
			count += 1
	return total / float(count)


## The two includes, run here instead of on the GPU, in the order the world's
## materials apply them. The bend is the include's `world_bend_position` line for
## line and the curve is its `world_horizon_drop`, measured from where the point
## really is and not from where the bend put it.
func _warp(point: Vector3, mode: int) -> Vector3:
	var world := point
	if mode == BENT or mode == BOTH:
		world = _bend(world)
	if mode == CURVED or mode == BOTH:
		# The drop is measured at where the point *really* is and taken off
		# wherever the bend has put it - which is what both world shaders do, and
		# why `point` and not `world` is what the include is asked about. The bend's
		# own sideways pull is kept: it is `world` x and z that survive this.
		var drop: float = point.y - _curve(point).y
		world = Vector3(world.x, world.y - drop, world.z)
	return world


## The include's `world_horizon_drop`, run here instead of on the GPU: the same
## horizontal distance from the camera, the same ratio, the same sagitta.
func _curve(world: Vector3) -> Vector3:
	if radius <= 0.0:
		return world
	var origin := camera.global_position
	var horizontal := Vector2(world.x - origin.x, world.z - origin.z).length()
	var scaled: float = maxf(radius, 1.0)
	var ratio := horizontal / scaled
	var drop := scaled * (1.0 - 1.0 / sqrt(1.0 + ratio * ratio))
	return Vector3(world.x, world.y - drop, world.z)


## The bend's include, the same way .freebuff/probe_bend_geo.gd runs it: the same
## relative vector, the same arctan wrap, the same convex mix, the same
## levelled-off lift.
func _bend(world: Vector3) -> Vector3:
	if AMOUNT <= 0.0:
		return world
	var origin := camera.global_position
	var relative := world - origin
	var horizontal := Vector2(relative.x, relative.z).length()
	if horizontal < 0.0001:
		return world
	var scaled: float = maxf(BEND_RADIUS, 1.0)
	var wrapped := atan(horizontal / scaled) * scaled
	var closed := lerpf(horizontal, wrapped, AMOUNT)
	var ratio := horizontal / scaled
	var lift := AMOUNT * BEND_RISE * scaled * (1.0 - 1.0 / sqrt(1.0 + ratio * ratio))
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


## The grey the frame is mostly made of, read off a corner of it rather than taken
## on trust: the tonemapper and the swapchain both get a say in what a 0.25
## background actually comes out as.
func _background() -> float:
	await _settle()
	var img := await _capture()
	var data := img.get_data()
	return _patch(data, img.get_width(), 4, 4)


## The one marker in the frame, and a refusal to carry on without it: a blob that
## never drew, or one with a piece of itself outside the frame, is not something a
## centroid can be taken of.
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


## Where the marker's pixels are: the centroid of everything in the frame that is
## not the background grey, found in two passes so that a million pixels of grey
## are not walked one at a time. The `solid` centroid counts every covered pixel
## once whatever shade it is drawn in - so a brightness gradient, which the world
## shaders have plenty of and which has nothing to do with where the vertices
## went, cannot move it - and the `shaded` one weights each pixel by how far it is
## off the grey, which is a better read of a soft edge but which a gradient tilts.
## They agree on anything evenly lit, so the run takes the solid one and holds the
## two up against each other.
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
