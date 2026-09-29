extends SceneTree

# What the engine actually expects in MultiMesh.buffer, learned from the engine.
#
# Must run WINDOWED: under --headless the dummy renderer keeps no multimesh data, so
# `buffer` reads back empty and every assignment is a no-op. That is why the first
# version of this probe "proved" the wrong layout.

func _line(buf: PackedFloat32Array) -> String:
	var s := ""
	for i in buf.size():
		s += "%d=%s " % [i, str(snappedf(buf[i], 0.001))]
	return s


func _initialize() -> void:
	# 1. What does the engine itself pack for a known transform?
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.instance_count = 1
	mm.set_instance_transform(0, Transform3D(Basis(), Vector3(11, 22, 33)))
	var packed: PackedFloat32Array = mm.buffer
	print("MM_PROBE set_identity_origin(11,22,33) -> buffer size=", packed.size())
	print("MM_PROBE   ", _line(packed))

	# 2. A probe layout: basis columns first, origin last.
	var columns := PackedFloat32Array()
	columns.resize(12)
	columns[0] = 1.0
	columns[4] = 1.0
	columns[8] = 1.0
	columns[9] = 11.0
	columns[10] = 22.0
	columns[11] = 33.0
	var mm2 := MultiMesh.new()
	mm2.transform_format = MultiMesh.TRANSFORM_3D
	mm2.instance_count = 1
	mm2.buffer = columns
	print("MM_PROBE columns_first reads back as origin=", mm2.get_instance_transform(0).origin,
		" basis=", mm2.get_instance_transform(0).basis)

	# 3. The layout the ported code wrote: origin first, identity after.
	var origin_first := PackedFloat32Array()
	origin_first.resize(12)
	origin_first[0] = 11.0
	origin_first[1] = 22.0
	origin_first[2] = 33.0
	origin_first[3] = 1.0
	origin_first[7] = 1.0
	origin_first[11] = 1.0
	var mm3 := MultiMesh.new()
	mm3.transform_format = MultiMesh.TRANSFORM_3D
	mm3.instance_count = 1
	mm3.buffer = origin_first
	print("MM_PROBE origin_first reads back as origin=", mm3.get_instance_transform(0).origin,
		" basis=", mm3.get_instance_transform(0).basis)

	# 4. And the real question for the ghost: pack a cell the way the C++ now has to,
	#    through the buffer, and read the transform back.
	var one := PackedFloat32Array()
	one.resize(12)
	one[0] = 1.0
	one[4] = 1.0
	one[8] = 1.0
	one[9] = 100.5
	one[10] = 64.5
	one[11] = 100.5
	var mm4 := MultiMesh.new()
	mm4.transform_format = MultiMesh.TRANSFORM_3D
	mm4.instance_count = 1
	mm4.buffer = one
	print("MM_PROBE cell_centre_via_buffer origin=", mm4.get_instance_transform(0).origin,
		" basis=", mm4.get_instance_transform(0).basis)
	quit(0)
