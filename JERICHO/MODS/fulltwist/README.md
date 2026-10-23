# FULL TWIST

Driv3r-style tumbles from severe off-centre collisions, built on the collision
response the engine already computes.

## What it does

- Adds a **free-spin** state for cars that have been hit, are airborne, or are
  resting on their body: the stock angular damping is lowered, the engine's
  lift-triggered `-avel/8` is cancelled, and the stopped-car spin kill is
  bypassed while the window lasts. Untouched traffic keeps the stock damping.
- Adds one roll/pitch angular impulse from the **mesh hull** of the car, not a
  box heuristic: the contact point, lever and impulse angle come from the clean
  body mesh.
- Splits the impulse between roll and pitch by the hull's axis inertias
  (`hy^2 + hz^2` for pitch, `hx^2 + hz^2` for roll), then lets the stock
  quaternion integrator, suspension and ground contacts carry the tumble.
- Offers a **hull scenery override** of the engine's 2D `CarBuildingCollision`
  check. The hull's world-projected XZ footprint gates false 2D hits. The
  engine's contact point is never rewritten; the hull contact is logged and
  used only for FULL TWIST's own torque assist. This is also the
  ceiling/underside false-positive fix.
- Prebuilds every resident convex hull at the top of the frame after a level
  start or a model swap. Collision callbacks never build a hull; if one is not
  ready they fall back to the colBox.
- Restores a **wall bounce** slider (`wall_bounce`, 100 = stock restitution) in
  place of the old absorb-on-snag path, and scales the stock car-car recoil
  (`car_car_impulse`) so a shunt does not launch traffic.
- Keeps the **rollover catch** as a tunable for the arcade presets; Free Physics
  turns it off and lets the car settle where the physics puts it.

## Where it hooks

| Hook | Engine site | What FULL TWIST uses it for |
|---|---|---|
| `JER_EVENT_COLLISION` | `GlobalTimeStep` (`handling.c`) for car-vs-car; `CarBuildingCollision` (`bcollide.c`) for car-vs-world | impact point, surface normal, `howHard`, both cars |
| `JER_EVENT_CAR_STEP` | top of `StepOneCar` (`wheelforces.c`) | wheel mask, uprightness, airborne/landing detection, free-spin window, test levers |
| `JER_EVENT_CAR_TORQUE` | after `ConvertTorqueToAngularAcceleration` (`wheelforces.c`) | tumble damping, the rollover catch, and the lift `-avel/8` cancellation |
| `JER_EVENT_GET_PHYSICS_PARAMS` | top of `StepOneCar` (`wheelforces.c`) | lower the base `angularDamping` for free-spin cars only |
| `JER_EVENT_GET_WALL_RESTITUTION` | `CarBuildingCollision` (`bcollide.c`) | `wall_bounce` (100 % = stock restitution) |
| `JER_EVENT_FRAME` | `GlobalTimeStep` | frame counter, per-frame latch reset, hull prebuild |
| `JER_EVENT_RESET_CAR` | damage/reset path | clear per-car tumble state |
| `JER_OVERRIDE_SLOT_SCENERY_COLLISION` | `CarBuildingCollision` (`bcollide.c`) | reject a false 2D scenery hit; never rewrites the point |

The module is compiled in (no `runtime = "dll"`). It needs the game's config
store and pause-menu API, which are linked into the exe but not the runtime DLL
surface.

## Files

| File | Owns |
|---|---|
| `fulltwist.c` | entry point and the event handlers |
| `fulltwist_config.c` | defaults, clamps, presets, ini load/save |
| `fulltwist_menu.c` | the FULL TWIST pause page |
| `fulltwist_hull.c` | prebuilt clean-mesh hull, projected XZ footprint, read-only scenery gate |
| `fulltwist_impulse.c` | contact/lever/torque/impulse math |
| `fulltwist_test.c` | deterministic crash and rollover levers |

## Why the latch is mandatory

The collision event is fired per collision pair per RK/subframe step:

- car-vs-car: up to **8** reports per frame for one pair;
- car-vs-world: up to **3** reports per frame for one contact.

A per-car, per-frame latch applies the first severe impact and lets a stronger
impact the same frame top up only the difference. Weaker repeats are ignored.

## Sensor and tunables

`Options -> JERICHO -> FULL TWIST` (or `JERICHO/CONFIG/fulltwist.ini`):

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 1 | master switch |
| `intensity` | 100 | 0–200 % of the computed impulse |
| `min_impact` | 72000 | ignore contacts below this raw `howHard`/`strikeVel` |
| `off_center` | 30 | minimum lever offset, 0–200 % |
| `max_rate` | 4000000 | angular impulse at full severity |
| `ground_kick` | 50 | share of the tumble re-applied once on the first wheel contact |
| `damping` | 25 | extra roll/pitch damping while tumbling, 0–100 % |
| `rollover_damping` | 45 | extra damping after the window while on the side/roof |
| `rollover_catch` | 50 | uprighting torque while on the side/roof and grounded |
| `spin_retention` | 50 | legacy offset of the stock 128 damping while tumbling |
| `window` | 120 | frames the tumble response stays active |
| `trip` | 20 | share redirected into roll/pitch when the physical torque is mostly yaw |
| `launch` | 80 | **scenery** contact impulse along the normal, 0–200 % |
| `wall_snag` | 45 | tangential velocity removed on a wall hit, 0–100 % |
| `wall_bounce` | 0 | wall restitution, 0–100 % (100 = stock bounce) |
| `free_spin` | 0 | 1 = lower the stock damping for cars in the free-spin state |
| `angular_damping` | 128 | 0–128, damping used for free-spin cars (stock 128) |
| `cancel_extra_damping` | 0 | 0–100 %, cancel the engine's lift `-avel/8` |
| `keep_spin_at_rest` | 0 | 1 = keep `hd.speed` off zero while free-spinning |
| `free_spin_window` | 90 | frames the free-spin state stays active |
| `hull_contact` | 0 | 1 = log/use the hull lever for FULL TWIST's assist (never rewrites the engine point) |
| `hull_footprint` | 0 | 1 = use the projected hull footprint as a planar gate |
| `scenery_mode` | 0 | 0 = severity assist, 1 = hull-led (launch `r x J` only) |
| `car_car_impulse` | 100 | 0–100 % of the stock car-car impulse and torque |
| `car_car_launch` | 80 | car-car contact impulse, 0–200 % |
| `car_car_tumble` | 100 | car-car roll/pitch assist, 0–200 % |
| `body_kick` | 50 | body-landing assist, 0–200, clamped to the angular clamp |
| `scenery3d` | 1 | enable the hull/scenery override |
| `player_only` | 0 | 1 = only the player car / cars hit by the player |
| `debug_log` | 0 | recorder lines in the JERICHO log |
| `test_kick` | 0 | deterministic settle kick at level start |
| `test_crash` | 0 | spawn an oncoming car and drive both together |
| `test_smash` | 0 | spawn a car beside the player and shove them together |
| `test_rollover` | 0 | rotate the player 90° about its long axis and shove it |

The defaults above are the legacy/Arcade values. **Free Physics** sets
`free_spin=1`, `angular_damping=16`, `cancel_extra_damping=100`,
`keep_spin_at_rest=1`, `free_spin_window=120`, `hull_contact=0`,
`hull_footprint=1`, `scenery_mode=1`, `wall_snag=0`, `wall_bounce=100`,
`car_car_impulse=50`, `car_car_launch=0`, `car_car_tumble=50`, `body_kick=0`,
`damping=0`, `rollover_damping=0`, `rollover_catch=0`, `spin_retention=0`,
`ground_kick=25`, `window=180`, `trip=20`, `launch=40`.

Presets: **Cinematic**, **Arcade**, **Free Physics**, **Off**. Any hand
adjustment switches the preset to **Custom**. Free Physics is used when the ini
has no `preset` key and by **Reset to Defaults**.

## Test levers

`FULL TWIST -> Debug -> Test Kick / Test Crash / Test Smash / Test Rollover`, or
the matching ini keys for a `-frames` run:

- **Test Kick** applies a known roll/pitch rate and upward launch. Use it to
  compare settling on flat ground, after a jump, or after roof/side contact.
- **Test Crash** spawns a car about 420 units ahead of the player, facing back,
  and gives both a closing velocity. It exercises the car-car hull path.
- **Test Smash** spawns a car beside the player, parks it, and applies the
  roll-test shove toward it. It exercises the car-car contact path.
- **Test Rollover** turns the car's heading 90° (yaw, not roll), holds that
  pose for one second (30 frames at the 30 Hz sim step), then applies a lateral
  impulse perpendicular to the car's direction — 8× the old test shove
  (1,200,000) plus a 7,200,000 roll rate. The log reports `rollover count` each
  time the up vector crosses the horizon, so multiple rollovers are visible
  directly.

## Measuring

Turn `debug_log` on and the log carries:

```
[fulltwist] hull ready slot=... verts=... size=(...) footprint=...
[fulltwist] hull skipped slot=... (reason)
[fulltwist] hull pending slot=... (model changed; colBox fallback)
[fulltwist] hit frame=N car=... mode=... torque=(pitch,yaw,roll) rollMix=...
            pitchMix=... rate=... launch=... hull=1 inertia=(...) contact=(...)
[fulltwist] scenery lever car=... stock=(...) hull=(...) hullY=(...) buildingY=(...)
[fulltwist] scenery response car=... point=(...) hullContact=... footprint=...
[fulltwist] scenery reject car=... hullY=(...) buildingY=(...) point=(...)
[fulltwist] scenery skip car=... hullY=(...) buildingY=(...)
[fulltwist] car-car recoil car0=... car1=... scale=...% normal=(...)
[fulltwist] car-car scale skip car0=... car1=... pair=... usable=... scale=...
[fulltwist] free spin car=... damp=... cancel=... av=(...)
[fulltwist] body contact car=... point=(...) groundY=... vy=... rate=... free=...
[fulltwist] rollover catch car=... up=... ux=... uz=... catchK=...
[fulltwist] test rollover rotate car=... frame=... up=... impulse_at=...
[fulltwist] test rollover impulse car=... frame=... lateral=... rate=...
[fulltwist] rollover count car=... count=... up=...
[fulltwist] settled car=... rolls=N
```

Headless recipe:

```
JERICHO_dev.exe -level chicago -nointro -frames 400
grep '\[fulltwist\]' <log>
```

with the relevant `test_* = 1` in the ini.

## Notes / risks

- The engine treats `hd.where.m[1][1] < 100` for ~180 frames as an upside-down
  death (`mission.c`). Free Physics sets `rollover_damping`/`rollover_catch` to
  0, so a car that comes to rest on its roof can end there; the arcade presets
  keep the uprighting assist.
- The scenery collision event is fired **after** `bcollide.c` negates
  `collisionResult.hit.vy` (`bcollide.c:942`). The override runs before that
  negation and is read-only; `ftHandleCar` un-negates the event point for
  scenery hits. Do not mix the two conventions.
- The car-car recoil scale writes the engine's shared collision normal in place
  at the end of `ftOnCollision`. The engine uses that normal for the stock
  impulse and torque immediately afterwards. FULL TWIST is listed after crumple
  in `JERICHO/CONFIG/modlist.ini`, so crumple's dent snapshot sees the original;
  a collision handler registered after FULL TWIST would see the scaled normal.
- `angular_damping` is applied only to cars in the free-spin state, so another
  physics module's value (cainescrossfire uses 512) survives on every other car.
- Every resident hull is built from `gCarCleanModelPtr[slot]` at the frame top
  after a level start or a model swap. A swap marks the slot dirty and rebuilds
  at the next frame; the contact that saw the swap falls back to the colBox.
- The scenery gate is conservative: it rejects only when the projected hull and
  the building box do not overlap in XZ (with the engine's +15 car margin) or
  vertically. A real hit keeps the engine's stock point and normal; the hull
  contact is only logged and used for FULL TWIST's own torque.
- Rewriting `args->point` was removed in `438fe59a` because it crashed after
  repeated wall hits; FULL TWIST must never do it again. `hull_contact` is an
  assist-only log/lever switch and defaults to 0.
- A live traffic car can have no cosmetics (a model that is not built or was
  released mid-level). The engine now skips it in the car-car box build, step,
  revs, scenery test and damage paths; the skip is logged once. FULL TWIST also
  requires `JerCarSlotUsable` before touching a car.
- The car-car normal scale is skipped unless the other car is a real, resident
  model (`JerCarSlotUsable`); the skip is logged as `car-car scale skip`.
- The vehicle deformation/import features still own their own collision logic;
  FULL TWIST only writes `st.n.angularVelocity`, `st.n.linearVelocity` and
  `hd.aacc`.
