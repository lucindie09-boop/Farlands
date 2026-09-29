extends Node
# Integrity-hunter probe: teleports the player through several terrain areas,
# settles for streaming at each, then calls ChunkManager.debug_scan_holes()
# and prints any diagnostic records that mention a suspected bug (BUG_...).
# A watchdog forces quit so the run never hangs.

var main: Node
var cm: Node
var player: Node3D

func _ready() -> void:
    main = load("res://Main.tscn").instantiate()
    add_child(main)
    await get_tree().process_frame
    await get_tree().process_frame
    cm = main.get_node_or_null("ChunkManager")
    player = main.get_node("Player")

    var hud := main.get_node_or_null("HUD")
    if hud: hud.set("visible", false)
    for n in ["Hotbar#HotbarTexture", "BlockOutline", "BlockBreakOverlay", "Crosshair"]:
        var node := main.get_node_or_null(n)
        if node: node.set("visible", false)
        if node and node is CanvasLayer:
            node.set("process_mode", Node.PROCESS_MODE_DISABLED)

    get_tree().create_timer(300.0).timeout.connect(func():
        print("PROBEH WATCHDOG QUIT")
        get_tree().quit())

    player.call("set_fly_mode", true)
    await wait(120)

    # Route: ring of area centers around origin + the old LOD probe area.
    var centers := [
        Vector3(0.0, 0.0, 0.0),
        Vector3(1200.0, 0.0, 400.0),
        Vector3(-800.0, 0.0, 1400.0),
        Vector3(600.0, 0.0, -1600.0),
        Vector3(-1200.0, 0.0, -600.0),
        Vector3(-2560.0, 0.0, -16896.0),
        Vector3(5000.0, 0.0, 3000.0),
    ]
    var seen := {}
    for c in centers:
        var surf := surface_at(c.x, c.z)
        if surf < 0.0:
            print("PROBEH no terrain at ", c.x, ",", c.z)
            continue
        # High settle first (mid/far LOD bands + streaming).
        player.call("teleport_to", Vector3(c.x, surf + 140.0, c.z))
        await wait(420)
        scan(seen, "high-" + str(int(c.x)) + "_" + str(int(c.z)))
        # Full detail: right at the surface.
        player.call("teleport_to", Vector3(c.x + 8.0, surf + 12.0, c.z + 8.0))
        await wait(300)
        scan(seen, "near-" + str(int(c.x)) + "_" + str(int(c.z)))

    print("PROBEH DONE found=", seen.size())
    get_tree().quit()

func scan(seen: Dictionary, tag: String) -> void:
    if cm == null:
        return
    var text: String = cm.debug_scan_holes()
    for line in text.split("\n"):
        if line.contains("BUG"):
            if not seen.has(line):
                seen[line] = true
                print("PROBEH HIT [", tag, "] ", line)

func surface_at(wx: float, wz: float) -> float:
    if cm == null:
        return -999.0
    var x := int(wx)
    var z := int(wz)
    var y := 1200
    while y > 0:
        if cm.get_block(x, y, z) != 0:
            return float(y)
        y -= 8
    return -999.0

func wait(frames: int) -> void:
    for i in range(frames):
        await get_tree().process_frame
