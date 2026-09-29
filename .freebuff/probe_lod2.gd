extends Node
# LOD-hole probe v4 (based on the v2 that completed): teleport player near a
# signature chunk, measure surface, then shoot from the mid/far LOD band with
# look_at() orientation (fixes the v2 yaw bug), then full detail for comparison.
# A watchdog forces quit so the run never hangs the CI step.

var OUT_DIR := ""
var main: Node
var cam: Camera3D
var cm: Node
var player: Node3D

func _ready() -> void:
    OUT_DIR = ProjectSettings.globalize_path("res://.freebuff/lod_probe2_shots")
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

    player = main.get_node("Player")
    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay", "Crosshair", "GodRaysOverlay"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)
        if node and node is CanvasLayer:
            node.set("process_mode", Node.PROCESS_MODE_DISABLED)

    cam = Camera3D.new()
    cam.fov = 81.7
    main.add_child(cam)
    cam.make_current()

    get_tree().create_timer(240.0).timeout.connect(func():
        print("PROBE WATCHDOG QUIT")
        get_tree().quit())

    var cx := -80.0
    var cz := -528.0

    player.call("teleport_to", Vector3(cx, 460.0, cz))
    await wait(900)
    var surf := surface_at(cx, cz)
    print("PROBE surface at (", cx, ",", cz, ") = ", surf)
    if surf < 0.0:
        surf = 312.0

    # --- mid-LOD band ---
    player.call("teleport_to", Vector3(cx, surf + 60.0, cz + 320.0))
    await wait(1500)
    print("PROBE settled mid")
    await shoot_at("mid_look", Vector3(cx, surf + 120.0, cz + 290.0), Vector3(cx, surf - 5.0, cz))
    await shoot_at("mid_look_hi", Vector3(cx, surf + 220.0, cz + 290.0), Vector3(cx, surf + 20.0, cz))
    await shoot_at("mid_look_lo", Vector3(cx, surf + 60.0, cz + 290.0), Vector3(cx, surf - 40.0, cz))

    # --- far-LOD band ---
    player.call("teleport_to", Vector3(cx, surf + 60.0, cz + 620.0))
    await wait(1800)
    print("PROBE settled far")
    await shoot_at("far_look", Vector3(cx, surf + 160.0, cz + 560.0), Vector3(cx, surf - 5.0, cz))
    await shoot_at("far_look_hi", Vector3(cx, surf + 260.0, cz + 560.0), Vector3(cx, surf + 20.0, cz))
    await shoot_at("far_above", Vector3(cx, surf + 380.0, cz + 20.0), Vector3(cx, surf - 200.0, cz))

    # --- full detail ---
    player.call("teleport_to", Vector3(cx, surf + 40.0, cz + 24.0))
    await wait(1500)
    print("PROBE settled full")
    await shoot_at("full_look", Vector3(cx, surf + 120.0, cz + 290.0), Vector3(cx, surf - 5.0, cz))
    await shoot_at("full_look_hi", Vector3(cx, surf + 220.0, cz + 290.0), Vector3(cx, surf + 20.0, cz))
    await shoot_at("full_above", Vector3(cx, surf + 380.0, cz + 20.0), Vector3(cx, surf - 200.0, cz))

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

func shoot_at(name: String, eye: Vector3, target: Vector3) -> void:
    cam.global_position = eye
    cam.look_at(target, Vector3.UP)
    await wait(8)
    var img: Image = get_viewport().get_texture().get_image()
    img.save_png(OUT_DIR + "/" + name + ".png")
    print("PROBE saved ", name)

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame