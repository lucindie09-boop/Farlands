# Plan: Minecraft-Accurate C++ Player Controller

## Problem

The current `player.gd` is a placeholder that uses framerate-scaled movement (`velocity * delta`) with arbitrary constants (SPEED=100, JUMP=40, GRAVITY=20). Even with correct constants, this can never match vanilla Minecraft because Minecraft's physics are recurrence relations evaluated at a fixed 20 ticks/second, not a continuous ODE. The collision resolver also resolves axes in the wrong order (X→Y→Z instead of vanilla's Y→X→Z) and lacks step-up assistance.

## Architecture

Two-layer design matching the existing ChunkManager/VoxelEngineController pattern:

- **`VoxelEngine::PlayerController`** (`src/engine/player_controller.hpp/cpp`) — Pure simulation. Owns tick accumulator, velocity, position, state machine. The `tick()` method is deterministic: given input + collision resolver, produces one tick of motion. No Godot lifecycle dependencies. All positions and velocities are in **blocks per tick** (matching vanilla's convention) — never blocks per second. Conversion to blocks/s happens only in test assertion comments for human readability.
- **`PlayerController`** (`src/godot_bindings/player_controller.hpp/cpp`) — Godot **`Node3D`** node (matching ChunkManager's base class). Owns a `VoxelEngine::PlayerController` instance. Drives the tick accumulator from `_process(delta)`, polls input, handles mouse-look, camera interpolation, HUD, and block interaction. Registered via ClassDB, addable as a scene node. Does NOT use Godot's CharacterBody3D physics, move_and_slide, or CollisionShape3D — all collision goes through `CollisionResolver` → chunk map.

## Files to Create

### 1. `src/engine/player_controller.hpp`

```
VoxelEngine::PlayerController class:
  - Enum: MoveState { WALKING, SPRINTING, SNEAKING, AIRBORNE }
  - Struct: PlayerInput { wish_direction: Vector3 (camera-relative, XZ-normalized),
                           jump_pressed, sprint_held, sneak_held, yaw: float }
  - Constants (all blocks/tick unless noted):
      TICK_RATE = 20        (ticks/sec, for display/labeling only)
      TICK_DT = 1.0f/20.0f  (seconds per tick, for accumulator comparison only)
      GRAVITY = 0.08         (blocks/tick², subtracted from velocity.y per tick)
      VERTICAL_DRAG = 0.98   (multiplied onto velocity.y per tick)
      AIR_FRICTION = 0.91    (multiplied onto velocity.xz per tick when airborne)
      DEFAULT_SLIPPERINESS = 0.6
      GROUND_ACCEL = 0.1     (blocks/tick², applied to velocity.xz per tick)
      AIR_ACCEL = 0.02       (blocks/tick², applied to velocity.xz per tick)
      JUMP_VELOCITY = 0.42   (blocks/tick, set directly on velocity.y)
      SPRINT_JUMP_BOOST = 0.2 (blocks/tick, added to horizontal velocity on sprint-jump)
      STEP_HEIGHT = 0.6      (blocks, vertical retry distance for step-up)
      WALK_MULT = 1.0, SPRINT_MULT = 1.3, SNEAK_MULT = 0.3
      STANDING_SIZE = Vector3(0.6, 1.8, 0.6)
      SNEAKING_SIZE = Vector3(0.6, 1.5, 0.6)
      STANDING_EYE = 1.62, SNEAKING_EYE = 1.27
  - State: position_, prev_position_, velocity_, state_, on_floor_, accumulator_
  - Public API:
      void reset(Vector3 initial_pos)
          // Sets position_ = prev_position_ = initial_pos (teleport reset —
          // prevents one-frame glide from default-initialized Vector3(0,0,0))
      void accumulate_and_tick(double frame_delta, PlayerInput input,
                               CollisionResolver& cr, float step_height = 0.6f)
          // NOTE: edge-triggered input flags (jump_pressed) are consumed on first
          // use within the while loop and will NOT re-trigger on subsequent sub-ticks
          // in the same frame. If future edge-triggered inputs are added, they must
          // follow the same consume-on-first-use pattern to avoid multi-tick re-firing.
      float get_accumulator_fraction() const
          // Returns accumulator_ / TICK_DT — the fractional progress toward the next
          // tick, used as partial_tick for interpolation between prev_position_ and
          // position_.
      Vector3 get_render_position(float partial_tick) const
          // lerp(prev_position_, position_, partial_tick)
      Vector3 get_camera_position(float partial_tick) const
          // render_pos + Vector3(0, eye_height, 0)
      MoveState get_state() const
      bool is_on_floor() const
      float get_eye_height() const
      Vector3 get_velocity() const
      Vector3 get_position() const
```

### 2. `src/engine/player_controller.cpp`

Key logic in `tick()` — all values are blocks/tick, no dt multiplication anywhere:

```
1. Read input → compute move_multiplier (sprint=1.3, walk=1.0, sneak=0.3, no input=0.0)
   → compute wish_dir (camera-relative, XZ-normalized)

2. Determine ground slipperiness (0.6 default; extendable via block_definitions.json later)

3. If on_floor:
     accel = 0.1 * move_multiplier * (0.6 / slipperiness)^3
   Else:
     accel = 0.02 * move_multiplier

4. Apply horizontal accel to velocity.xz:
     velocity.xz += wish_dir * accel
     // NO speed clamp. 4.317/5.612/1.295 are emergent equilibrium from the
     // accel-vs-friction recurrence, not enforced ceilings. Clamping would break
     // legitimate transients (sprint-jump burst, future knockback/potions/pistons).

5. If jump_pressed && on_floor:
     velocity.y = 0.42
     if sprinting: add +0.2 in facing direction to velocity.xz

6. CollisionResolver::resolve(position, velocity, hitbox_size)
   // velocity IS the per-tick motion vector. No TICK_DT multiplication.
   // Vanilla does pos += motion with no dt — the constants already encode
   // the 20Hz rate. Multiplying by TICK_DT would apply the tick scaling twice
   // and make the player move at 1/20th intended speed.
   → position = result.position
   → on_floor = result.on_floor
   → if collided_x: velocity.x = 0
   → if collided_y: velocity.y = 0 (if positive, ceiling hit; if negative, floor land)
   → if collided_z: velocity.z = 0

7. Apply gravity: velocity.y -= 0.08
   // Gravity and drag are applied AFTER the move, using pre-move ground state
   // for friction. This ordering is why vanilla's jump timing near edges feels
   // different from continuous-ODE physics.

8. Apply drag: velocity.y *= 0.98; velocity.xz *= friction
   (friction = ground_slipperiness * 0.91 if on_floor, else 0.91)

accumulator_ pattern:
  prev_position_ = position_  // snapshot before this frame's ticks
  accumulator_ += frame_delta
  while (accumulator_ >= TICK_DT):
      tick(input, collision)
      accumulator_ -= TICK_DT
```

The `get_render_position(partial_tick)` interpolates between `prev_position_` and `position_` for smooth camera motion between ticks (vanilla's `partialTicks`).

### 3. `src/godot_bindings/player_controller.hpp`

```
PlayerController : Node3D
  GDCLASS(PlayerController, Node3D)
  - Member: VoxelEngine::PlayerController sim_
  - Member: Camera3D* camera_
  - Member: VoxelEngine::CollisionResolver* collision_resolver_  // raw pointer, owned by ChunkManager
  - Member: float pitch_, sensitivity_, fly_speed_
  - Member: bool fly_mode_
  - Member: int selected_block_type_
  - Overrides: _ready, _process, _input
  - Bound properties: sensitivity (FLOAT), fly_speed (FLOAT)
  - Bound methods: toggle_fly_mode, break_block, place_block, get_selected_block, set_selected_block
```

### 4. `src/godot_bindings/player_controller.cpp`

```
_ready():
  - Cache camera child ("Camera3D"), set camera.position.y = 1.62 (standing eye height)
  - Cast get_node("/root/Main/ChunkManager") to ChunkManager* → call get_collision_resolver()
    to get a VoxelEngine::CollisionResolver*. Store as collision_resolver_ member.
    (ChunkManager must expose this accessor — see §7 below.)
  - sim_.reset(global_position)  // both prev_position_ and position_ set to spawn
  - Create crosshair + HUD label (ported from player.gd)
  - Register "sprint" action (KEY_CTRL) and "sneak" action (KEY_SHIFT) if missing

_process(delta):
  - Poll input → PlayerInput struct
  - sim_.accumulate_and_tick(delta, input, *collision_resolver_)
  - Interpolate position for smooth rendering:
      partial = sim_.get_accumulator_fraction()
      global_position = sim_.get_render_position(partial)
  - Update camera position: camera.global_position = sim_.get_camera_position(partial)
  - Update HUD label

_input(event):
  - Mouse motion → rotate_y(-dx * sensitivity), pitch -= dy * sensitivity, clamp pitch
  - LMB → break_block() via chunk_manager raycast
  - RMB → place_block() via chunk_manager raycast
  - Number keys 1-9,0,- → select block type
  - F → toggle fly mode
  - T → toggle day/night cycle
  - G → advance time
  - Escape → release mouse

fly mode:
  - In fly mode, sim_.tick() bypasses gravity/friction, uses fly_speed for movement
  - Space = ascend, Shift = descend
```

## Files to Modify

### 5. `src/godot_bindings/chunk_manager.hpp`

Add one public accessor:
```cpp
VoxelEngine::CollisionResolver* get_collision_resolver() const;
```
This returns a pointer to the `collision_resolver` member owned by `VoxelEngineController`. No ownership transfer — the `PlayerController` node just holds a raw pointer for the lifetime of the scene. The `CollisionResolver` outlives any individual `_process` call because both nodes are freed in tree-exit order.

### 6. `src/godot_bindings/chunk_manager.cpp`

Implement the accessor:
```cpp
VoxelEngine::CollisionResolver* ChunkManager::get_collision_resolver() const {
    return &controller->get_collision_resolver();  // VoxelEngineController already exposes this
}
```
Note: `VoxelEngineController` already has `CollisionResolver collision_resolver` as a member with no public accessor. Add `CollisionResolver& get_collision_resolver()` to `VoxelEngineController` as well if not already present (it currently has `is_aabb_solid` and `resolve_voxel_collision` but no direct resolver reference getter).

### 7. `src/engine/collision_resolver.hpp`

- Change `resolve()` to accept an optional `step_height` parameter (default 0.0f).
- Add `bool stepped_up = false` to `CollisionResult`.

### 8. `src/engine/collision_resolver.cpp`

**Hoist the solid-check lambda** to a named local at the top of `resolve()`, reused by both the main loop and step-up:
```cpp
auto is_solid = [this](const AABB& aabb) { return is_aabb_solid_fast(aabb); };
```

**Axis reorder**: Change the loop from X→Y→Z to Y→X→Z:
```cpp
int axis_order[3] = {1, 0, 2};  // Y, X, Z
```

**Snapshot post-Y position** for step-up:
```cpp
Vector3 result_after_y;
// ... Y iteration runs ...
result_after_y = result;  // snapshot before X/Z
// ... X and Z iterations run ...
```

**Step-up assist** (after the Y→X→Z pass, only if `step_height > 0.0f && out.on_floor && (out.collided_x || out.collided_z)`):

1. **Sweep the vertical gap**: verify no solid block occupies the space from `result_after_y.y` to `result_after_y.y + step_height`. Build an AABB covering the player's horizontal footprint at the full step height and check for any solid blocks within that vertical range. If any exist, skip step-up entirely.

2. **Retry horizontal at raised position**:
```cpp
Vector3 stepped_pos = result_after_y;
stepped_pos.y += step_height;
Vector3 stepped_result = stepped_pos;
bool sx = false, sz = false;
resolve_axis(result_after_y, motion, size, 0, stepped_result, sx, is_solid);
resolve_axis(result_after_y, motion, size, 2, stepped_result, sz, is_solid);
```
Note: motion is the original input motion, but we re-resolve only X and Z components at the raised Y. The Y component was already resolved in the main pass; the step-up only adjusts Y post-hoc.

3. **Ceiling check**: Does the player fit at the final stepped position?
```cpp
AABB player_at_stepped(stepped_result, size);
if (!is_solid(player_at_stepped)) {
    out.position = stepped_result;
    out.collided_x = sx;
    out.collided_z = sz;
    out.stepped_up = true;
}
```
This checks the player's actual bounding box (including head) at the destination — NOT a box floating above.

### 9. `src/godot_bindings/register_types.cpp`

```cpp
#include "godot_bindings/player_controller.hpp"
// In initialize_chunk_module():
ClassDB::register_class<PlayerController>();
```

### 10. `Main.tscn`

Replace the Player node:
```
[node name="Player" type="PlayerController" parent="."]
transform = Transform3D(1,0,0, 0,1,0, 0,0,1, 0,280,0)
```
Remove the `script` property (was `player.gd`). Remove the `CollisionShape3D` child entirely (not needed — collision goes through `CollisionResolver`, not PhysicsServer3D).

### 11. `project.godot`

Add input actions:
```ini
[input]
sprint = Ctrl (keycode 4194341)
sneak = Shift (keycode 4194325)
```

### 12. `player.gd`

Delete entirely. All functionality moves to the C++ PlayerController node.

## Files to Create (Tests)

### 13. `tests/test_collision_resolver.cpp`

No collision tests exist today. Write baseline tests FIRST, before reordering axes, using a real minimal `ChunkMap` with synthetic blocks set directly (same pattern as `test_chunk_map.cpp`):

```
TEST_CASE("CollisionResolver flat floor stop"):
  - Create ChunkMap, fill y=0 layer with stone
  - Resolve downward motion into the floor → assert position stops at y=1.0 (top of block), on_floor=true

TEST_CASE("CollisionResolver wall stop"):
  - Create ChunkMap with a wall at x=5
  - Resolve horizontal motion toward the wall → assert position stops before the wall, collided_x=true

TEST_CASE("CollisionResolver step too tall"):
  - Create ChunkMap with a 1-block step (block at y=0, empty at y=1, motion into it from below)
  - Resolve with step_height=0 → assert player stops (step too tall to auto-climb)

TEST_CASE("CollisionResolver step within height"):
  - Synthetic fixture: no block in the current game produces a 0.6-tall obstruction
    (all blocks are full 1×1×1 cubes). Manually set a block at y=0, leave y=1 empty,
    and approach from a position where the horizontal AABB clips the block edge — the
    step-up should raise the player onto the block. Comment in test explains the fixture
    is synthetic so nobody searches for a "step block" in block_definitions.json later.
  - step_height=0.6 → assert player steps up onto the block
```

These tests establish a regression baseline. After they pass, THEN reorder axes and re-run to verify the Y→X→Z change doesn't break flat-floor/wall scenarios.

### 14. `tests/test_player_controller.cpp`

Uses a real `ChunkMap` with a flat stone floor (not a mock — `CollisionResolver` has no injectable interface). Set up a minimal world: fill y=0 layer with stone across a ~10×10 area. Construct a real `CollisionResolver` pointing at it.

```
TEST_CASE("Player walk steady state"):
  - Create PlayerController, place on flat floor
  - Simulate 100 ticks with forward input (walk, not sprint)
  - Assert speed ~4.317 blocks/tick (NOTE: this is blocks/TICK, not blocks/s.
    To express in blocks/s for human readability: 4.317 × 20 = 86.34 blocks/s.
    But the simulation value is 4.317 blocks/tick — that's what vanilla's
    velocity.xz actually holds.)

TEST_CASE("Player sprint steady state"):
  - Sprint + forward for 100 ticks
  - Assert speed ~5.612 blocks/tick

TEST_CASE("Player sneak steady state"):
  - Sneak + forward for 100 ticks
  - Assert speed ~1.295 blocks/tick

TEST_CASE("Player jump height"):
  - Apply jump impulse (velocity.y = 0.42), simulate tick-by-tick on flat floor
  - Count ticks until velocity.y transitions positive → negative (apex)
  - Compute apex height from position.y at that tick
  - Assert apex ~1.2522 blocks

TEST_CASE("Player sprint-jump horizontal boost"):
  - Sprint + jump, assert horizontal velocity has +0.2 boost in facing direction
    on the tick where jump is applied

TEST_CASE("Player gravity application order"):
  - Drop player from height with no input
  - After 1 tick from rest: velocity.y = -0.08 (gravity) × 0.98 (vertical drag)
    = -0.0784. Both steps 7 and 8 run in the same tick — gravity subtracts, then
    drag multiplies. The assertion must account for both.
  - Position.y should reflect the initial velocity (0.0) * 1 = 0 blocks moved
    (velocity was zero at tick start, move happens, THEN gravity+drag apply)

TEST_CASE("Player accumulator correctness"):
  - Feed 0.05s frame delta → 1 tick executed, residue < 1e-5f (tolerance, not exact —
    1.0f/20.0f and 0.05 likely fold to the same float32 but "likely" varies by compiler/OPT)
  - Feed 0.12s frame delta → 2 ticks executed, residue ~0.02
  - Assert accumulator residue is always < TICK_DT after the while loop

TEST_CASE("Player accumulator does not re-fire edge-triggered inputs"):
  - Set jump_pressed = true, feed 0.12s (2 ticks)
  - Assert jump only consumed on first tick (velocity.y set to 0.42 once),
    second tick proceeds with on_floor=false, no second jump
```

## Implementation Order

### Step 0: Baseline collision tests
Create `tests/test_collision_resolver.cpp` with the 4 tests above. Run, confirm they pass against the current X→Y→Z resolver. These are the regression baseline.

### Step 1: Collision resolver reorder (Y→X→Z)
Modify `collision_resolver.cpp` — hoist lambda, reorder axes, snapshot `result_after_y`. Re-run step-0 tests. Flat floor and wall stop should be axis-independent and pass unchanged.

### Step 2: Engine PlayerController (simulation only)
Create `src/engine/player_controller.hpp/cpp` with tick accumulator, velocity math, state machine. Test in `test_player_controller.cpp` using a real `ChunkMap` + `CollisionResolver` (not a mock). No Godot node involved.

### Step 3: Godot PlayerController node
Create `src/godot_bindings/player_controller.hpp/cpp` with ClassDB binding (`Node3D` base), `_process` driving accumulator, input polling, camera interpolation. Add `get_collision_resolver()` accessor to `ChunkManager` (§5-6). Register in `register_types.cpp`.

### Step 4: Scene + project wiring
Update `Main.tscn` to use `PlayerController` node type, remove `CollisionShape3D` child, add input actions to `project.godot`, delete `player.gd`.

### Step 5: Step-up assist
Add step-up retry logic to `CollisionResolver::resolve()` with the corrected vertical-gap sweep + ceiling check. Pass `STEP_HEIGHT=0.6` from the player controller's tick. The step-up path is currently unreachable in gameplay (all blocks are 1×1×1 cubes — no slab/stair produces a 0.6-tall obstruction), but the infrastructure is cheap, correct, and matches vanilla's algorithm. It pays off the moment sub-block geometry is added.

### Step 6: Sneak mechanics
Add sneak state: slower multiplier (0.3), reduced hitbox (1.5 tall), edge-safety AABB pre-check (project movement forward by 0.2, check if it would walk off an edge → block if sneak-held).

### Step 7: Sprint-jump boost, air control, polish
Add +0.2 horizontal boost on sprint-jump. Fine-tune air control. Add wall-stop (zero velocity when speed < epsilon after collision). Add diagonal input normalization quirks matching vanilla.

## Validation Targets (from spec §2)

| Metric | Expected (blocks/tick) | Expected (blocks/s) | Tolerance |
|--------|----------------------|-------------------|-----------|
| Walk steady-state | 4.317 | 86.34 | ±0.01 |
| Sprint steady-state | 5.612 | 112.24 | ±0.01 |
| Sneak steady-state | 1.295 | 25.90 | ±0.01 |
| Jump apex height | 1.252 blocks | — | ±0.01 |

Internal simulation uses blocks/tick. blocks/s figures are for display/test comments only.

## Design Decisions

1. **Tick accumulator over Godot physics FPS**: The simulation runs its own 20Hz fixed-step inside `_process(delta)`, completely decoupled from Godot's physics. This makes the simulation deterministic regardless of display framerate.

2. **Y→X→Z collision order**: Non-negotiable per spec — this is why jump timing near edges feels right in vanilla. The existing `resolve_axis` template is reused, just the call order changes.

3. **Step-up as retry, not special path**: After the normal Y→X→Z pass, if grounded and horizontally collided, we sweep the vertical gap (0→step_height), then retry X+Z at raised Y. If the player fits at the final position (ceiling check on actual player AABB), accept the raised position. Currently unreachable with 1×1×1 blocks, but cheap infrastructure for future sub-block geometry.

4. **Block-specific overrides deferred**: Ice friction, soul sand slowdown, etc. are not in Step 1-4. They'll use new optional fields in `block_definitions.json` (`friction`, `speed_multiplier`) when implemented.

5. **Fly mode preserved**: F toggles between walk and fly. Fly mode bypasses gravity/friction, uses a configurable `fly_speed`. Camera stays in first-person either way.

6. **Node3D base class, not CharacterBody3D**: Matching ChunkManager's pattern. No PhysicsServer3D registration, no move_and_slide, no CollisionShape3D. All collision goes through `CollisionResolver` → chunk map.

7. **No speed clamps**: 4.317/5.612/1.295 are emergent equilibrium values from the accel-vs-friction recurrence, used only as test assertions. Vanilla enforces no ceiling — and legitimate transients (sprint-jump, future knockback/potions) legitimately exceed these values briefly.

8. **CollisionResolver access via ChunkManager accessor**: The `PlayerController` godot node gets a `CollisionResolver*` through a `get_collision_resolver()` method on `ChunkManager`, which delegates to `VoxelEngineController`. This avoids Dictionary boxing overhead (20×/sec) and keeps the simulation layer free of Godot Variant types. Both nodes are children of the same Main scene and typically torn down together, but Godot doesn't strictly guarantee sibling destruction order — a null-check on `collision_resolver_` before use in `_process`/`_exit_tree` is a one-line insurance policy against teardown-order edge cases if scene reloading is ever added.
