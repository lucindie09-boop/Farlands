extends SceneTree
## Does the far mode's material WRITE DEPTH?
##
## The far field is one merged mesh per spacing level, which is what took its cost
## from ~130 draw calls to a handful (docs/lod-modes.md). Merging gives up the one
## thing that had been ordering its triangles for free: a mesh per tile is sorted
## back to front by the transparent pass, and a mesh that contains everything is
## sorted against nothing. So a material that does not write depth now paints its
## triangles in index order -- and a hill drawn after the hill in front of it
## appears THROUGH it, which is what the player sees as "terrain behind terrain".
##
## Which is a property of the shader, not of the tiles: a spatial shader that
## writes ALPHA goes down the transparent pipeline, whose whole contract is no
## depth write, and `depth_draw_opaque` (the "write depth for opaque materials"
## mode) has nothing to write for it. The world's terrain writes ALPHA too, and gets
## away with it only because each chunk is its own instance and gets sorted.
##
## So this probe asks the question where the answer is unambiguous: two quads, one
## behind the other, BOTH IN ONE MESH, drawn with a material built from the far
## mode's own shader. Near quad first in the index order, far quad second.
##
##   * depth written  -> the near quad wins the overlapping pixels.
##   * depth NOT written -> the last triangle drawn wins, so the far quad does.
##
## The colours come from a texture array the probe builds itself (layer 0 pure red,
## layer 1 pure blue), so "who won" is a comparison of the red and blue channels and
## has no threshold to tune. Three measurements, because one of them is the control:
##
##   1. near only        -- the instrument can see red at all (texture array, UV,
##                          UV2 layer, the light uniforms all plumbed correctly).
##   2. both, far FIRST  -- index order already agrees with depth, so this passes
##                          whether or not depth is written: it proves the test can
##                          pass and is not just structurally failing.
##   3. both, near FIRST -- the subject. Red means the material writes depth; blue
##                          means it does not, and the far quad painted over the
##                          near one, which is the bug.
##
## It needs no world, no chunks and no seed: it is the shader's pipeline state, and
## putting it in an empty scene is what keeps it a measurement rather than a
## coincidence. That also means it runs in about a second, unlike a grid probe.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_lod_depth.gd 60
##
## Shots go to user://lod_depth_shots/ for the eye.

const SIZE := Vector2i(640, 360)
const NEAR_Z := -0.5      # camera-local distance of the quad in front
const NEAR_HALF := 0.30   # ...and its half-width: 86% of the frame's height there
const FAR_Z := -50.0      # the quad behind it
const FAR_HALF := 26.0    # ...sized to cover the same screen area (74% of the height)
const LAYER_NEAR := 0.0
const LAYER_FAR := 1.0
## How much red must lead blue for "the near quad won" to hold. The two colours are
## a pure red and a pure blue, so the gap is the whole channel; this is just far
## enough above the frame's own noise.
const WIN_MARGIN := 60

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _frames(count: int) -> void:
	for _i in range(count):
		await process_frame


func _frame() -> Image:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return null
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


## The centre pixel's red and blue, as bytes. Both quads cover the frame's centre.
func _centre(shot: Image) -> Array:
	if shot == null:
		return [0, 0]
	var o := ((shot.get_height() / 2) * shot.get_width() + shot.get_width() / 2) * 4
	var data := shot.get_data()
	return [data[o], data[o + 2]]


func _shot(img: Image, name: String) -> void:
	if img == null:
		return
	DirAccess.make_dir_recursive_absolute("user://lod_depth_shots")
	img.save_png("user://lod_depth_shots/%s.png" % name)


## A 1x1 texture per layer: layer 0 red, layer 1 blue. `source_color` decodes sRGB,
## and a pure 0 or 255 survives that untouched.
func _layer_array() -> Texture2DArray:
	var red := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	red.fill(Color(1.0, 0.0, 0.0, 1.0))
	var blue := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	blue.fill(Color(0.0, 0.0, 1.0, 1.0))
	var arr := Texture2DArray.new()
	var err := arr.create_from_images([red, blue])
	if err != OK:
		print("probe: create_from_images returned %d" % err)
	return arr


## One quad's six vertices (two triangles, deindexed), in camera-local space.
func _push_quad(pts: PackedVector3Array, cols: PackedColorArray, uvs: PackedVector2Array,
		uvs2: PackedVector2Array, z: float, half: float, layer: float) -> void:
	var h := half
	var corners := [Vector3(-h, -h, z), Vector3(h, -h, z), Vector3(h, h, z), Vector3(-h, -h, z),
		Vector3(h, h, z), Vector3(-h, h, z)]
	var uv := [Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 0), Vector2(1, 1), Vector2(0, 1)]
	for i in range(6):
		pts.append(corners[i])
		# COLOR.rgb is the baked face constant; a quad facing the camera takes the
		# top one (1.0), so the shader's `lit` is the albedo and nothing else.
		cols.append(Color(1.0, 1.0, 1.0, 0.0))
		uvs.append(uv[i])
		# UV2.x is the texture layer, UV2.y the liquid flag (0: terrain).
		uvs2.append(Vector2(layer, 0.0))


## One mesh containing the quads in the order given, which IS the draw order within
## a single surface.
func _mesh(quads: Array) -> ArrayMesh:
	var pts := PackedVector3Array()
	var cols := PackedColorArray()
	var uvs := PackedVector2Array()
	var uvs2 := PackedVector2Array()
	for q in quads:
		_push_quad(pts, cols, uvs, uvs2, q["z"], q["half"], q["layer"])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = pts
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_TEX_UV2] = uvs2
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


## The far mode's own shader, with every uniform that could colour the answer pinned:
## a full white sky, no fog, no clip, no water mix, no grading. The layers came from
## the probe, so a red pixel is the near quad and a blue one the far quad.
func _material(arr: Texture2DArray) -> ShaderMaterial:
	var shader: Shader = load("res://shaders/lod_grid.gdshader")
	if shader == null:
		print("PROBE FAIL: shaders/lod_grid.gdshader did not load")
		return null
	var mat := ShaderMaterial.new()
	mat.shader = shader
	mat.set_shader_parameter("texture_array", arr)
	mat.set_shader_parameter("mipmap_bias", 0.0)
	mat.set_shader_parameter("sky_light_intensity", 1.0)
	mat.set_shader_parameter("sky_light_color", Vector3(1.0, 1.0, 1.0))
	mat.set_shader_parameter("sky_light_warmth", Vector3(1.0, 1.0, 1.0))
	mat.set_shader_parameter("saturation", 1.0)
	mat.set_shader_parameter("contrast", 1.0)
	mat.set_shader_parameter("darkness_color", Vector3(0.0, 0.0, 0.0))
	mat.set_shader_parameter("water_mix", 0.0)
	mat.set_shader_parameter("fog_begin", 0.0)
	mat.set_shader_parameter("fog_end", 1.0e9)
	mat.set_shader_parameter("clip_center", Vector2(0.0, 0.0))
	mat.set_shader_parameter("clip_radius", 0.0)
	return mat


func _run() -> void:
	DisplayServer.window_set_size(SIZE)
	var scene := Node3D.new()
	root.add_child(scene)

	var cam := Camera3D.new()
	cam.fov = 70.0
	cam.near = 0.05
	cam.far = 4000.0
	scene.add_child(cam)
	cam.make_current()

	var arr := _layer_array()
	_ok("the probe's own two-layer array exists", arr != null and arr.get_layers() == 2,
		"layers=%d" % (arr.get_layers() if arr != null else -1))
	var mat := _material(arr)
	if mat == null:
		quit(1)
		return

	# Three meshes: the subject, its control, and one quad alone to prove the
	# colours arrive. Each is one mesh with one surface, which is the shape the
	# merged far field has.
	var near_quad := {"z": NEAR_Z, "half": NEAR_HALF, "layer": LAYER_NEAR}
	var far_quad := {"z": FAR_Z, "half": FAR_HALF, "layer": LAYER_FAR}
	var subjects := {
		"near only": [near_quad],
		"far first": [far_quad, near_quad],
		"near first": [near_quad, far_quad],
	}
	var holders := {}
	for name in subjects:
		var mi := MeshInstance3D.new()
		mi.mesh = _mesh(subjects[name])
		mi.material_override = mat
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		cam.add_child(mi)
		holders[name] = mi

	await _frames(30)   # let the renderer settle before the first read

	var results := {}
	for name in subjects:
		for other in holders:
			holders[other].visible = (other == name)
		cam.make_current()
		await _frames(8)
		var shot := await _frame()
		results[name] = _centre(shot)
		_shot(shot, String(name).replace(" ", "_"))
		print("probe: %-11s centre rgb red=%3d blue=%3d" % [name, results[name][0], results[name][1]])

	# 1. The instrument sees the colours it built.
	_ok("a lone near quad renders its own red",
		results["near only"][0] > results["near only"][1],
		"red=%d blue=%d" % results["near only"])
	# 2. With the far quad first, index order already agrees with depth: a pass here
	#    is what makes the third measurement mean something.
	_ok("control: with the far quad first the near quad still wins",
		results["far first"][0] > results["far first"][1] + WIN_MARGIN,
		"red=%d blue=%d" % results["far first"])
	# 3. The subject: near quad into the mesh first, far quad after it.
	var won: int = results["near first"][0] - results["near first"][1]
	_ok("the material writes depth, so the near quad wins over the far one",
		won > WIN_MARGIN,
		"red=%d blue=%d -- %s" % [results["near first"][0], results["near first"][1],
			"near won" if won > 0 else "THE FAR QUAD PAINTED OVER IT: this material never writes depth"])

	for name in holders:
		holders[name].visible = false
	await _frames(2)

	print("PROBE lod depth: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
