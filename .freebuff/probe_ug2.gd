extends Node
# Second underground probe: teleport the camera to a FRESH area (never
# generated), then descend from high above the terrain to deep underground,
# screenshotting the whole vertical profile. Robust to unknown terrain height.

var OUT_DIR := ""
var main: Node
var player: Node3D
var cam: Camera3D

func _ready() -> void:
    OUT_DIR = ProjectSettings.globalize_path("res://.freebuff/ug_probe2")
    DirAccess.make_dir_recursive_absolute(OUT_DIR)
    main = load("res://Main.tscn").instantiate()
    add_child(main)
    await get_tree().process_frame
    await get_tree().process_frame

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
    for i in range(1200):
        await get_tree().process_frame
        if i > 500:
            break
    var origin: Vector3 = player.global_position
    print("PROBE2 origin = ", origin)

    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay", "Crosshair"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)

    cam = Camera3D.new()
    cam.fov = 81.7
    main.add_child(cam)
    cam.make_current()
    cam.global_position = origin + Vector3(0, 1.62, 0)
    cam.rotation = Vector3(0, 0, 0)
    await wait(5)

    get_tree().create_timer(175.0).timeout.connect(func():
        print("PROBE2 WATCHDOG QUIT")
        get_tree().quit())

    # Fresh location ~1.5km away, likely a different terrain column.
    var fx: float = origin.x + 1500.0
    var fz: float = origin.z + 700.0

    # Phase 1: descend from high sky to deep underground at the fresh spot.
    var ys := [850.0, 700.0, 580.0, 470.0, 380.0, 310.0, 260.0, 220.0, 180.0,
               150.0, 120.0, 95.0, 70.0, 50.0, 30.0]
    for i in ys.size():
        var y: float = ys[i]
        var eye := Vector3(fx, y, fz)
        print("PROBE2 stop y=", int(y))
        cam.global_position = eye
        cam.rotation = Vector3(0, 0, 0)
        await wait(4)
        # catch streaming: shoot immediately
        await shoot("p%02d_y%d_stream" % [i, int(y)], eye, 0.0, 0.0)
        await wait(50)   # ~0.85s of streaming
        await shoot("p%02d_y%d_n" % [i, int(y)], eye, 0.0, 0.0)
        await shoot("p%02d_y%d_e" % [i, int(y)], eye, deg_to_rad(90), 0.0)
        await shoot("p%02d_y%d_up" % [i, int(y)], eye, 0.0, deg_to_rad(85))
        await shoot("p%02d_y%d_down" % [i, int(y)], eye, 0.0, deg_to_rad(-85))
        await wait(140)  # settle a bit before the next stop

    # Phase 2: horizontal cruise at y=95 through more fresh territory.
    for step in range(5):
        var cx: float = fx + step * 180.0
        var eye := Vector3(cx, 95.0, fz)
        cam.global_position = eye
        await wait(10)
        await shoot("cruise%d_stream" % step, eye, 0.0, 0.0)
        await shoot("cruise%d_up" % step, eye, 0.0, deg_to_rad(80))
    await wait(200)
    await shoot("cruise_settle", Vector3(fx + 4 * 180.0, 95.0, fz), 0.0, 0.0)

    print("PROBE2 DONE")
    get_tree().quit()

func shoot(name: String, eye: Vector3, yaw: float, pitch: float) -> void:
    cam.global_position = eye
    cam.rotation = Vector3(pitch, yaw, 0.0)
    await wait(6)
    var img: Image = get_viewport().get_texture().get_image()
    img.save_png(OUT_DIR + "/" + name + ".png")
    print("PROBE2 saved ", name)

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame
