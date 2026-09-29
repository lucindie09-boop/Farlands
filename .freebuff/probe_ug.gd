extends Node
# Underground-artifact probe: instantiate the real game, then fly a probe
# camera below the surface and screenshot whatever geometry renders there.
# Environment is overridden (magenta sky, flat white ambient, no fog) and the
# HUD is hidden so stray chunk geometry POPS clearly.

const SENS := 0.003
var OUT_DIR := ""
var main: Node
var player: Node3D
var cam: Camera3D

func _ready() -> void:
    OUT_DIR = ProjectSettings.globalize_path("res://.freebuff/underground_probe")
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

    # Wait for spawn + initial terrain streaming (player falls to the ground).
    for i in range(1500):
        await get_tree().process_frame
        var y: float = player.global_position.y
        if i > 250 and y < 500.0 and y > 1.0 and i > 500:
            break
    await wait(150)
    var ground_y: float = player.global_position.y
    var origin: Vector3 = player.global_position
    print("PROBE ground_y = ", ground_y, " at ", origin)

    # Hide UI so frames are pure geometry (HUD is a CanvasLayer; the others
    # are CanvasItems/Node3D — duck-typed .set avoids cast traps).
    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay", "Crosshair"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)

    # Probe camera: replaces the player camera as the active view, so the
    # chunk manager streams around IT and we control yaw/pitch fully.
    cam = Camera3D.new()
    cam.fov = 81.7
    cam.position = Vector3(origin.x, ground_y + 1.62, origin.z)
    cam.rotation = Vector3(0, 0, 0)
    main.add_child(cam)
    cam.make_current()
    await wait(5)

    # Watchdog.
    get_tree().create_timer(170.0).timeout.connect(func():
        print("PROBE WATCHDOG QUIT")
        get_tree().quit())

    # Reference: on the surface looking around.
    await shoot("surf_n", origin + Vector3(0, 1.62, 0), 0.0, 0.0)
    await shoot("surf_e", origin + Vector3(0, 1.62, 0), deg_to_rad(90), 0.0)
    await shoot("surf_down", origin + Vector3(0, 1.62, 0), 0.0, deg_to_rad(-88))
    await shoot("surf_up", origin + Vector3(0, 1.62, 0), 0.0, deg_to_rad(88))

    var depths := [8.0, 20.0, 40.0, 70.0, 110.0, 170.0, 250.0, 330.0]
    for i in depths.size():
        var y: float = ground_y - depths[i]
        if y < 8.0:
            break
        var tag := "d%d_%03d" % [i, int(y)]
        print("PROBE descending to y=", int(y))
        var eye := Vector3(origin.x, y + 1.62, origin.z)
        await move_to(eye)
        await wait(15)
        await shoot(tag + "_early", eye, 0.0, 0.0)
        await wait(200)
        await shoot(tag + "_n", eye, 0.0, 0.0)
        await shoot(tag + "_e", eye, deg_to_rad(90), 0.0)
        await shoot(tag + "_s", eye, deg_to_rad(180), 0.0)
        await shoot(tag + "_w", eye, deg_to_rad(270), 0.0)
        await shoot(tag + "_down", eye, 0.0, deg_to_rad(-88))
        await shoot(tag + "_up", eye, 0.0, deg_to_rad(88))

    print("PROBE DONE")
    get_tree().quit()

func move_to(pos: Vector3) -> void:
    cam.global_position = pos
    await wait(3)

func shoot(name: String, eye: Vector3, yaw: float, pitch: float) -> void:
    cam.global_position = eye
    cam.rotation = Vector3(pitch, yaw, 0.0)
    await wait(6)
    var img: Image = get_viewport().get_texture().get_image()
    img.save_png(OUT_DIR + "/" + name + ".png")
    print("PROBE saved ", name)

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame
