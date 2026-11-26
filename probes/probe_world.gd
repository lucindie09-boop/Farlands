extends RefCounted
## Shared world plumbing for the probes that spawn mob bodies, so each one stops inventing its
## own and getting the same two things wrong.
##
## Two things went wrong often enough to be written down. **The world is not ready when the
## player is on the floor**: chunks install for a couple of hundred frames after that (`0.0s
## player on the floor, settled=false, 5,260 chunks loaded, 367 generating` in
## probe_world_ready.gd), and a write or a spawn placed in that window can be overwritten or land
## in terrain that is still arriving. `is_fill_settled()` is the updater's own "generation has
## gone quiet and the walk has finished" predicate, so that is what to wait for. **A column's
## surface is not found by walking down from the feet**: this world's spawn is under a roof, so a
## downward scan from `player.y + 3` returns a CEILING, and a body placed on it is inside the
## hill -- it suffocates and cannot move, which reads as "mobs are broken in probes". Scan down
## from HIGH above instead, and ask the body's own resolver (`MobBody.is_solid_at`), because that
## is the view that decides whether it stands or suffocates.
##
##   const W := preload("res://probes/probe_world.gd")
##   await W.wait_settled(self, cm)

## A whole loading disc installs in a few hundred frames on this machine; the cap is so a probe
## fails with a number rather than hanging.
const SETTLE_FRAMES := 6000


## The world installed: generation quiet and every chunk the walk asked for in place.
static func wait_settled(tree: SceneTree, cm: Node, frames: int = SETTLE_FRAMES) -> bool:
	var spent := 0
	while spent < frames:
		if bool(cm.call("is_fill_settled")):
			return true
		await tree.process_frame
		spent += 1
	return bool(cm.call("is_fill_settled"))


## The feet cell of the highest place a body can stand in one column, or -1.
##
## Solid under it, air in its own cell and in the one over it (a 1.8-block body), and neither of
## those two cells liquid: a probe that wants a DRY stand (the pond's rim, a spawn point) gets
## one, and `ceiling` is where the scan starts -- pass something well above the area of interest,
## because the first solid cell under a roof is the roof.
static func stand_in_column(sim: Object, x: int, z: int, ceiling: int, depth: int = 96) -> int:
	for y in range(ceiling, ceiling - depth, -1):
		if not bool(sim.call("is_solid_at", x, y - 1, z)):
			continue
		if bool(sim.call("is_solid_at", x, y, z)) or bool(sim.call("is_solid_at", x, y + 1, z)):
			continue
		if bool(sim.call("is_liquid_at", x, y, z)) or bool(sim.call("is_liquid_at", x, y + 1, z)):
			continue
		return y
	return -1


## A WET stand in one column: liquid in the feet cell and in the one over it, a body's worth of
## room, and the liquid's own top is what a swimmer rides at.
static func water_in_column(sim: Object, x: int, z: int, ceiling: int, depth: int = 96) -> int:
	for y in range(ceiling, ceiling - depth, -1):
		if bool(sim.call("is_liquid_at", x, y, z)) and bool(sim.call("is_liquid_at", x, y + 1, z)) \
				and not bool(sim.call("is_liquid_at", x, y + 2, z)):
			return y
	return -1


## A level, open place to build a fixture on: the top of a column near (x, z) whose neighbours
## across a `footprint` square are all within a block of its own height, scanning outward from the
## given column. This world is streamed, so the ground under the player's feet is not a safe
## assumption for a fixture seven blocks away -- a pond cut into a hillside hangs in the air, and
## one cut under a roof is built in rock. Answers Vector3.INF when no such place is found.
static func flat_spot(sim: Object, x: int, z: int, ceiling: int, footprint: int,
		reach: int = 12) -> Vector3:
	for r in range(0, reach + 1):
		for dx in range(-r, r + 1):
			for dz in range(-r, r + 1):
				if maxi(absi(dx), absi(dz)) != r:
					continue
				var top := stand_in_column(sim, x + dx, z + dz, ceiling)
				if top < 0 or not _level(sim, x + dx, z + dz, ceiling, footprint, top):
					continue
				return Vector3(float(x + dx) + 0.5, float(top), float(z + dz) + 0.5)
	return Vector3.INF


static func _level(sim: Object, x: int, z: int, ceiling: int, footprint: int, top: int) -> bool:
	for dx in [-footprint, 0, footprint]:
		for dz in [-footprint, 0, footprint]:
			var other := stand_in_column(sim, x + dx, z + dz, ceiling)
			if other < 0 or absi(other - top) > 1:
				return false
	return true


## A level stone pad, carved out of whatever is there: a `w` x `d` square of stone at (x, y, z) with
## `height` cells of air over it. A probe that builds its own floor works on a hillside, in a cave
## and on a seabed alike, which is what a probe needs in a world whose surface is not the same from
## one run to the next. Answers false when the writes did not take, because a fixture that is only
## assumed is the kind of failure that reads as "mobs are broken".
static func pad(tree: SceneTree, cm: Node, sim: Object, x: int, y: int, z: int, w: int = 3,
		d: int = 3, height: int = 5) -> bool:
	var stone := BlockTextures.get_block_id_by_name("stone")
	if stone <= 0:
		return false
	for attempt in 12:
		for dx in range(-w, w + 1):
			for dz in range(-d, d + 1):
				for dy in range(1, height + 1):
					cm.call("set_block", x + dx, y + dy, z + dz, 0)
				cm.call("set_block", x + dx, y, z + dz, stone)
		await tree.process_frame
		if cell_text(sim, x, y + 1, z) == "air" and cell_text(sim, x, y, z) == "solid":
			return true
	return false


## The body's own view of a cell, as a word, for a probe's print or its own check.
static func cell_text(sim: Object, x: int, y: int, z: int) -> String:
	if bool(sim.call("is_solid_at", x, y, z)):
		return "solid"
	if bool(sim.call("is_liquid_at", x, y, z)):
		return "water"
	return "air"
