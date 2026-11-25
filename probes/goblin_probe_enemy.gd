extends Node3D
## A body for the fight probe to chase: it stands where it is put and counts the blows that land
## on it. The goblin asks an enemy only three things -- where it is, whether it is still alive,
## and whether it can take a hit -- so this is all of an enemy there is.

var hits := 0
## Damage of each blow, in the half-hearts the goblin's own attack costs.
var damage: Array = []


func take_mob_hit(half_hearts: float, _from: Vector3) -> void:
	hits += 1
	damage.append(half_hearts)


func is_dead() -> bool:
	return false
