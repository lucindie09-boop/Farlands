extends SceneTree

# Checks the preview path the wand now goes through: the decoded build is cached
# between re-aims, so the risk is not "is it faster" but "does it ever hand back the
# WRONG file". So this aims at a big file, then a different one, then the first one
# again, and checks each answer against that file's own dimensions.
#
# It also checks the new instance buffer, since the ghost is drawn from it now: one
# unit transform per cell, twelve floats each, translation at the cell's centre. The
# buffer goes through a real MultiMesh for that check, because comparing the floats to
# this file's own idea of their order is how a wrong order passes.
#
# WINDOWED (no --headless): the dummy renderer keeps no multimesh data, so an
# assignment is a no-op there and every layout looks broken.

const A := "res://schematics/10179.schematic"
const B := "res://schematics/6670.schematic"

func _dims(cm, bytes: PackedByteArray) -> Array:
	var info: Dictionary = cm.inspect_schematic(bytes, {})
	return [info.get("file_width", -1), info.get("file_height", -1), info.get("file_length", -1)]


func _aim(cm, bytes: PackedByteArray, origin: Vector3i) -> Dictionary:
	return cm.preview_schematic(bytes, origin.x, origin.y, origin.z, {})


func _initialize() -> void:
	var cm = ClassDB.instantiate("ChunkManager")
	if cm == null:
		print("PREVIEW_PROBE could_not_instantiate_ChunkManager")
		quit(1)
		return

	var a := FileAccess.get_file_as_bytes(A)
	var b := FileAccess.get_file_as_bytes(B)
	print("PREVIEW_PROBE bytes_a=", a.size(), " bytes_b=", b.size())
	if a.is_empty() or b.is_empty():
		print("PREVIEW_PROBE missing_file")
		quit(1)
		return

	var dims_a := _dims(cm, a)
	var dims_b := _dims(cm, b)
	print("PREVIEW_PROBE dims_a=", dims_a, " dims_b=", dims_b)

	# Aim at A, then B, then A again. The reported dimensions must follow the bytes,
	# not the cache.
	var first := _aim(cm, a, Vector3i(100, 64, 100))
	var after_b := _aim(cm, b, Vector3i(0, 64, 0))
	var second := _aim(cm, a, Vector3i(100, 64, 100))
	var a1 := [first.get("file_width", -1), first.get("file_height", -1), first.get("file_length", -1)]
	var ab := [after_b.get("file_width", -1), after_b.get("file_height", -1), after_b.get("file_length", -1)]
	var a2 := [second.get("file_width", -1), second.get("file_height", -1), second.get("file_length", -1)]
	print("PREVIEW_PROBE aim_a=", a1, " aim_b=", ab, " aim_a_again=", a2)
	print("PREVIEW_PROBE cache_serves_right_file=", a1 == dims_a and ab == dims_b and a2 == dims_a)

	# The instance buffer the ghost uploads: 12 floats per returned cell, translation
	# at the cell's centre, and the same cell order as the legacy byte array.
	var cells: PackedByteArray = first.get("cells", PackedByteArray())
	var xform: PackedFloat32Array = first.get("transforms", PackedFloat32Array())
	var returned := int(first.get("cells_returned", -1))
	var values := cells.to_int32_array()
	print("PREVIEW_PROBE returned=", returned, " cells_bytes=", cells.size(),
		" transforms=", xform.size())
	print("PREVIEW_PROBE sizes_match=",
		returned > 0 and cells.size() == returned * 16 and xform.size() == returned * 12)

	# The layout check has to go THROUGH the engine. Comparing the floats to my own idea
	# of the order is exactly how a wrong order looks correct, so this hands the buffer
	# to a MultiMesh and asks it where the instances landed.
	# Runs windowed: the dummy renderer keeps no multimesh data, so every assignment
	# under --headless is a silent no-op that reads back as identity.
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	# A mesh is required for the AABB check below: with none, the renderer has no box
	# to compute and get_aabb() is the empty one.
	mm.mesh = BoxMesh.new()
	mm.instance_count = max(returned, 0)
	mm.buffer = xform
	var slot_first := mm.get_instance_transform(0)
	var slot_last := mm.get_instance_transform(max(returned - 1, 0))
	var last_i: int = max(returned - 1, 0) * 4
	var first_ok := slot_first.origin.is_equal_approx(
		Vector3(values[0] + 0.5, values[1] + 0.5, values[2] + 0.5))
	var last_ok := slot_last.origin.is_equal_approx(
		Vector3(values[last_i] + 0.5, values[last_i + 1] + 0.5, values[last_i + 2] + 0.5))
	var basis_ok := slot_first.basis.is_equal_approx(Basis())
	print("PREVIEW_PROBE engine_reads first_origin=", slot_first.origin,
		" expected=", Vector3(values[0] + 0.5, values[1] + 0.5, values[2] + 0.5))
	print("PREVIEW_PROBE engine_reads last_origin=", slot_last.origin,
		" expected=", Vector3(values[last_i] + 0.5, values[last_i + 1] + 0.5, values[last_i + 2] + 0.5))
	print("PREVIEW_PROBE first_transform_ok=", first_ok, " unit_basis_ok=", basis_ok,
		" last_transform_ok=", last_ok)

	# The frustum test is the other way instances go unseen: a MultiMesh is culled as
	# ONE object by its AABB, so a box that does not cover the cells would hide the
	# ghost at some camera angles. The renderer computes that box from the instance
	# data, so it should follow the buffer.
	var box: AABB = mm.get_aabb()
	var covers_first := box.has_point(slot_first.origin)
	var covers_last := box.has_point(slot_last.origin)
	print("PREVIEW_PROBE aabb=", box, " covers_first=", covers_first,
		" covers_last=", covers_last,
		" covers_expected_bounds=",
		box.has_point(Vector3(values[0] + 0.5, values[1] + 0.5, values[2] + 0.5))
		and box.has_point(Vector3(values[last_i] + 0.5, values[last_i + 1] + 0.5, values[last_i + 2] + 0.5)))

	# A moved aim must move the plan: same file, origin shifted, first cell shifted too.
	var shifted := _aim(cm, a, Vector3i(140, 64, 100))
	var shifted_values: PackedInt32Array = shifted.get("cells", PackedByteArray()).to_int32_array()
	print("PREVIEW_PROBE origin_moves_plan=",
		shifted_values.size() >= 4 and shifted_values[0] - values[0] == 40)

	quit(0)
