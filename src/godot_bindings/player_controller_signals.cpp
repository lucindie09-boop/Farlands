// The node's signals: the one list GDScript connects to — the place/punch
// strikes, a held item's swap, breaks, damage, death and respawn, the wand's
// clicks and the spawn egg. Kept in its own translation unit so the node's own
// file stays under the tree's size cap; the declaration is in
// player_controller.hpp beside the other registration hooks (bindings, then
// signals, then properties, in that order, as they have to be).

#include "godot_bindings/player_controller.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

void PlayerController::add_signals() {
    ADD_SIGNAL(MethodInfo("crafting_table_used"));
    ADD_SIGNAL(MethodInfo("block_placed"));
    // The held item replaced itself in its slot and nothing else happened: a
    // rod's cast/retract (items.json "use" kind "swap"). Separate from
    // `block_placed` because a throw plays the full punch, not the place stroke.
    // `slot` is the slot it changed, so a listener can pin its answer to that
    // slot instead of taking whatever id change it sees first.
    ADD_SIGNAL(MethodInfo("item_swapped", PropertyInfo(Variant::INT, "slot")));
    // A hold-to-break break: the id before the drop rule (so debris wears the
    // block's own texture) and the cell it left, after the edit.
    ADD_SIGNAL(MethodInfo("block_broken", PropertyInfo(Variant::INT, "block_id"),
                          PropertyInfo(Variant::VECTOR3I, "position")));
    ADD_SIGNAL(MethodInfo("died"));
    ADD_SIGNAL(MethodInfo("respawned"));
    // A registered hit landed. `amount` is the size of the hit in half-hearts
    // after the reduction chain and before absorption, so it reports a hit the
    // yellow buffer fully soaked too. The visual tilt is applied in-engine; a
    // listener uses this for anything else (a hurt sound, a red flash, a damage
    // number). Emitted on the same frame the health is applied.
    ADD_SIGNAL(MethodInfo("damaged", PropertyInfo(Variant::INT, "amount")));
    // A held wand's clicks: middle opens its menu, right works the tool, left
    // commits what it shows -- no placing or breaking, so one thing answers a
    // click while it is held. A held dummy egg's right click is the fourth signal.
    ADD_SIGNAL(MethodInfo("wand_menu"));
    ADD_SIGNAL(MethodInfo("wand_use"));
    ADD_SIGNAL(MethodInfo("wand_confirm"));
    ADD_SIGNAL(MethodInfo("dummy_spawn"));
    // The goblin egg's half of the same story: the entity is built in GDScript (the goblin spawn
    // tool), so the item only has to say that one was asked for.
    ADD_SIGNAL(MethodInfo("goblin_spawn"));
}
