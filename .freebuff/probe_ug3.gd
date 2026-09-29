extends Node
# Third probe: teleport the REAL player (fly mode, noclip) down a fresh
# column and screenshot through the player's own camera in the REAL
# environment (player light + fog + time of day) — exactly what the user sees.
# Camera yaw: set on the player node (read by the controller as look yaw).
# Camera pitch: a driver node processed AFTER the controller forces the
# camera's X rotation every frame (controller would otherwise reset it).

var OUT_DIR := ""
var main: Node
var player: Node3D
var cam: Camera3D
var driver: Node
var want_yaw := 0.0
var want_pitch := 0.0

func _ready() -> void:
    OUT_DIR = ProjectSettings.globalize_path("res://.freebuff/ug_probe3")
    DirAccess.make_dir_recursive_absolute(OUT_DIR)
    main = load("res://Main.tscn").instantiate()
    add_child(main)
    await get_tree().process_frame
    await get_tree().process_frame

    player = main.get_node("Player")
    cam = player.get_node("Camera3D")

    # Driver node: sibling added after Main, so its _process runs after the
    # player controller has applied its own camera transform each frame.
    driver = Node.new()
    driver.set_script(load("res://.freebuff/probe_ug3_driver.gd"))
    add_child(driver)
    driver.set("target", self)

    for i in range(1200):
        await get_tree().process_frame
        if i > 500:
            break
    var origin: Vector3 = player.global_position
    print("PROBE3 origin = ", origin, " ground=", origin.y)

    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)

    player.call("set_fly_mode", true)
    await wait(10)
    var ground_y: float = origin.y

    for dy in [5.0, 15.0, 25.0, 40.0, 60.0, 90.0, 130.0, 190.0, 260.0]:
        var y: float = ground_y - dy
        if y < 6.0:
            continue
        await to(Vector3(origin.x, y, origin.z))
        await wait(25)
        await shoot("o_%03d_n" % int(y), 0.0, 0.0)
        await shoot("o_%03d_up" % int(y), 0.0, 0.9)
        await shoot("o_%03d_down" % int(y), 0.0, -0.9)
        await wait(180)

    var fx: float = origin.x + 1500.0
    var fz: float = origin.z + 700.0
    await to(Vector3(fx, ground_y + 30.0, fz))
    await wait(400)
    for dy in [0.0, 15.0, 30.0, 55.0, 90.0, 140.0, 210.0, 300.0]:
        var y: float = ground_y + 30.0 - dy
        if y < 6.0:
            continue
        await to(Vector3(fx, y, fz))
        await wait(25)
        await shoot("f_%03d_n" % int(y), 0.0, 0.0)
        await shoot("f_%03d_up" % int(y), 0.0, 0.9)
        await shoot("f_%03d_down" % int(y), 0.0, -0.9)
        await wait(180)

    print("PROBE3 DONE")
    get_tree().quit()

func to(pos: Vector3) -> void:
    player.call("teleport_to", pos)
    await wait(3)

func shoot(name: String, yaw_deg: float, pitch: float) -> void:
    want_yaw = deg_to_rad(yaw_deg)
    want_pitch = pitch
    player.rotation.y = want_yaw
    await wait(8)
    var img: Image = get_viewport().get_texture().get_image()
    img.save_png(OUT_DIR + "/" + name + ".png")
    print("PROBE3 saved ", name)

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame
