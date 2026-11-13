extends SceneTree
## Where the cloud layer's boot time goes: one call per stage of `_ready`, timed.
## The mask is 512x512 per layer, and `_cloud_values` only ever samples a 256x256
## tile (the 2x2 quadrants are identical), so the values/count/sort stages are
## also reported for the 256x256 tile they duplicate.
##
##   probes/probe_cloud_boot.gd

const SEED := 20261103
const THRESHOLD := 0.58

func _initialize() -> void:
	_run()

func _ms(since: int) -> float:
	return float(Time.get_ticks_usec() - since) / 1000.0

func _run() -> void:
	var script := load("res://scripts/cloud_layer.gd") as GDScript
	var layer: Node = script.new()
	var total := Time.get_ticks_usec()

	var t := Time.get_ticks_usec()
	var values: PackedFloat32Array = layer.call("_cloud_values", SEED)
	print("PROBE cloud values ms=%.1f n=%d" % [_ms(t), values.size()])

	t = Time.get_ticks_usec()
	var count: int = layer.call("_count_above", values, THRESHOLD)
	print("PROBE cloud count ms=%.1f n=%d" % [_ms(t), count])

	t = Time.get_ticks_usec()
	var threshold: float = layer.call("_threshold_for_count", values, int(round(float(count) * 0.5)))
	print("PROBE cloud threshold ms=%.1f value=%.4f" % [_ms(t), threshold])

	t = Time.get_ticks_usec()
	var mask: PackedByteArray = layer.call("_generate_cloud_mask", values, THRESHOLD)
	print("PROBE cloud mask ms=%.1f n=%d" % [_ms(t), mask.size()])

	t = Time.get_ticks_usec()
	var texture: ImageTexture = layer.call("_mask_to_texture", mask)
	print("PROBE cloud texture ms=%.1f size=%s" % [_ms(t), str(texture.get_size())])

	t = Time.get_ticks_usec()
	var arrays: Array = layer.call("_build_cloud_arrays", mask)
	var mesh: ArrayMesh = layer.call("_arrays_to_mesh", arrays)
	print("PROBE cloud mesh ms=%.1f surfaces=%d" % [_ms(t), mesh.get_surface_count()])
	if mesh.get_surface_count() > 0:
		var surface := mesh.surface_get_arrays(0)
		print("PROBE cloud mesh verts=%d indices=%d" % [
			(surface[Mesh.ARRAY_VERTEX] as PackedVector3Array).size(),
			(surface[Mesh.ARRAY_INDEX] as PackedInt32Array).size()])

	t = Time.get_ticks_usec()
	var tile := _tile_of(values)
	print("PROBE cloud tile_extract ms=%.1f n=%d" % [_ms(t), tile.size()])
	t = Time.get_ticks_usec()
	var tile_count: int = layer.call("_count_above", tile, THRESHOLD)
	var tile_threshold: float = layer.call("_threshold_for_count", tile, int(round(float(tile_count) * 0.5)))
	print("PROBE cloud tile_count_sort ms=%.1f count=%d threshold=%.4f" % [
		_ms(t), tile_count, tile_threshold])

	print("PROBE cloud summary total_ms=%.1f" % _ms(total))
	# Identity of the layer: the same hashes after a rewrite mean the same clouds.
	var surface := mesh.surface_get_arrays(0)
	print("PROBE cloud hash values=%d mask=%d verts=%d colors=%d normals=%d indices=%d" % [
		hash(values), hash(mask),
		hash(surface[Mesh.ARRAY_VERTEX]), hash(surface[Mesh.ARRAY_COLOR]),
		hash(surface[Mesh.ARRAY_NORMAL]), hash(surface[Mesh.ARRAY_INDEX])])
	layer.free()
	quit(0)

# The unique quadrant of the 512x512 mask, read out of the already-built values.
func _tile_of(values: PackedFloat32Array) -> PackedFloat32Array:
	var tile := PackedFloat32Array()
	tile.resize(256 * 256)
	for y in 256:
		for x in 256:
			tile[y * 256 + x] = values[y * 512 + x]
	return tile
