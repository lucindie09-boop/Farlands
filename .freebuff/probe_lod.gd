extends Node
# LOD hole probe: instantiate the real game, find a steep peak/ridge, then
# fly a camera into the far-LOD band (96-256 blocks) and screenshot the peak
# from several angles. Holes (chunks that failed to render at LOD) show as
# magenta gaps against the magenta background.

var OUT_DIR := ""
var main: Node
var cam: Camera3D
var cm: Node

func _ready() -> void:
    OUT_DIR = ProjectSettings.globalize_path("res://.freebuff/lod_probe_shots")
    DirAccess.make_dir_recursive_absolute(OUT_DIR)
    main = load("res://Main.tscn").instantiate()
    add_child(main)
    await get_tree().process_frame
    await get_tree().process_frame
    cm = main.get_node_or_null("ChunkManager")

    var env := Environment.new()
    env.background_mode = Environment.BG_COLOR
    env.background_color = Color(1.0, 0.0, 1.0)
    env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    env.ambient_light_color = Color(1, 1, 1)
    env.ambient_light_energy = 1.0
    env.fog_enabled = false
    var we := main.get_node_or_null("WorldEnvironment")
    if we != null:
        (we as WorldEnvironment).environment = env

    var player: Node3D = main.get_node("Player")
    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay", "Crosshair", "GodRaysOverlay"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)

    # Wait for spawn + initial streaming.
    var origin: Vector3 = player.global_position
    print("PROBE spawn at ", origin)
    await wait(1200)
    print("PROBE settled")

    cam = Camera3D.new()
    cam.fov = 81.7
    main.add_child(cam)
    cam.make_current()
    await wait(5)

    get_tree().create_timer(240.0).timeout.connect(func():
        print("PROBE WATCHDOG QUIT")
        get_tree().quit())

    # Scan a set of rings around the origin for surface heights, then find a
    # steep drop (cliff / peak side) to look at from the LOD band.
    var cols: Array = []
    var rings := [[64.0, 20], [96.0, 24], [128.0, 28], [160.0, 32]]
    for ring in rings:
        var r: float = ring[0]
        var n: int = ring[1]
        for i in n:
            var a: float = TAU * float(i) / float(n)
            cols.append(Vector2(origin.x + cos(a) * r, origin.z + sin(a) * r))
    var best_pair: Array = []
    var best_drop := 0.0
    var heights: Array = []
    for c in cols:
        var h := surface_at(c.x, c.y)
        heights.append(h)
    for i in cols.size():
        for j in cols.size():
            if i == j: continue
            var d: float = cols[i].distance_to(cols[j])
            if d < 4.0 or d > 90.0: continue
            var drop: float = heights[i] - heights[j]
            if drop > best_drop:
                best_drop = drop
                best_pair = [cols[i], cols[j]]
    print("PROBE best drop ", best_drop, " pair ", best_pair)
    if best_pair.is_empty():
        print("PROBE NO STEEP TERRAIN FOUND")
        get_tree().quit()
        return

    var peak := best_pair[0] as Vector2
    var low := best_pair[1] as Vector2
    var peak_h: float = surface_at(peak.x, peak.y)
    var low_h: float = surface_at(low.x, low.y)
    print("PROBE peak (", peak, ") h=", peak_h, " low h=", low_h)

    # Camera in the far-LOD band (~180 blocks out), at the peak's height,
    # looking back at the peak.
    var dir_to_peak := (peak - low).normalized()
    var eye := Vector3(peak.x - dir_to_peak.x * 180.0, peak_h + 20.0, peak.y - dir_to_peak.y * 180.0)
    var yaw_look := atan2(peak.x - eye.x, peak.y - eye.z)
    print("PROBE eye ", eye, " yaw ", yaw_look)
    await move_to(eye)
    await wait(400)  # let the far region settle
    await shoot("far_peak_front", eye, yaw_look, 0.0)
    await shoot("far_peak_front_up", eye, yaw_look, deg_to_rad(-20))
    await shoot("far_peak_front_down", eye, yaw_look, deg_to_rad(20))

    # A second vantage: perpendicular to the drop.
    var eye2 := Vector3(peak.x - dir_to_peak.y * 170.0, peak_h + 30.0, peak.y + dir_to_peak.x * 170.0)
    var yaw2 := atan2(peak.x - eye2.x, peak.y - eye2.z)
    await move_to(eye2)
    await wait(300)
    await shoot("far_peak_side", eye2, yaw2, 0.0)

    # Straight down from high above the peak (peak in the LOD band below).
    var eye3 := Vector3(peak.x, peak_h + 260.0, peak.y)
    await move_to(eye3)
    await wait(300)
    await shoot("above_peak_down", eye3, 0.0, deg_to_rad(-88))
    await shoot("above_peak_side", eye3, 0.0, 0.0)

    print("PROBE DONE")
    get_tree().quit()

func surface_at(wx: float, wz: float) -> float:
    var x := int(wx)
    var z := int(wz)
    if cm == null:
        return -999.0
    var found := -999.0
    var y := 1000
    while y > 0:
        if cm.get_block(x, y, z) != 0:
            found = float(y)
            break
        y -= 8
    return found

func move_to(pos: Vector3) -> void:
    cam.global_position = pos
    await wait(3)

func shoot(name: String, eye: Vector3, yaw: float, pitch: float) -> void:
    cam.global_position = eye
    cam.rotation = Vector3(pitch, yaw, 0.0)
    await wait(8)
    var img: Image = get_viewport().get_texture().get_image()
    img.save_png(OUT_DIR + "/" + name + ".png")
    print("PROBE saved ", name)

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame