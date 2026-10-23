# FULL TWIST

Driv3r-style tumbles from severe off-centre collisions, built on the collision
response the engine already computes.

## What it does

- Adds one roll/pitch angular impulse from the **mesh hull** of the car, not a
  box heuristic: the contact point, lever and impulse angle come from the clean
  body mesh.
- Splits the impulse between roll and pitch by the hull's axis inertias
  (`hy^2 + hz^2` for pitch, `hx^2 + hz^2` for roll), then lets the stock
  quaternion integrator, suspension and ground contacts carry the tumble.
- Adds a **rollover catch**: once the tumble window closes, a car on its side or
  roof gets extra damping and a bounded uprighting torque while it is still in
  ground contact, instead of spinning on the stock damping alone.
- Offers a **3D scenery override** of the engine's 2D `CarBuildingCollision`
  check. A car whose hull does not vertically overlap a `BUILDING_BOX` is not
  treated as a wall hit; a real hit gets its contact point moved to the hull
  support. This is the ceiling/underside false-positive fix.

## Where it hooks

| Hook | Engine site | What FULL TWIST uses it for |
|---|---|---|
| `JER_EVENT_COLLISION` | `GlobalTimeStep` (`handling.c`) for car-vs-car; `CarBuildingCollision` (`bcollide.c`) for car-vs-world | impact point, surface normal, `howHard`, both cars |
| `JER_EVENT_CAR_STEP` | top of `StepOneCar` (`wheelforces.c`) | wheel mask, uprightness, airborne/landing detection, test levers |
| `JER_EVENT_CAR_TORQUE` | after `ConvertTorqueToAngularAcceleration` (`wheelforces.c`) | tumble damping and the rollover catch |
| `JER_EVENT_FRAME` | `GlobalTimeStep` | frame counter, per-frame latch reset |
| `JER_EVENT_RESET_CAR` | damage/reset path | clear per-car tumble state |
| `JER_OVERRIDE_SLOT_SCENERY_COLLISION` | `CarBuildingCollision` (`bcollide.c`) | reject a false 2D scenery hit or replace its contact point |

The module is compiled in (no `runtime = "dll"`). It needs the game's config
store and pause-menu API, which are linked into the exe but not the runtime DLL
surface.

## Files

| File | Owns |
|---|---|
| `fulltwist.c` | entry point and the event handlers |
| `fulltwist_config.c` | defaults, clamps, presets, ini load/save |
| `fulltwist_menu.c` | the FULL TWIST pause page |
| `fulltwist_hull.c` | clean-mesh hull, XZ convex footprint, 3D scenery test |
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
| `window` | 120 | frames the tumble response stays active |
| `trip` | 20 | share redirected into roll/pitch when the physical torque is mostly yaw |
| `launch` | 80 | contact impulse along the collision normal, 0–200 % |
| `scenery3d` | 1 | enable the 3D hull/scenery override |
| `player_only` | 0 | 1 = only the player car / cars hit by the player |
| `debug_log` | 0 | recorder lines in the JERICHO log |
| `test_kick` | 0 | deterministic settle kick at level start |
| `test_crash` | 0 | spawn an oncoming car and drive both together |
| `test_rollover` | 0 | rotate the player 90° about its long axis and shove it |

Presets: **Cinematic**, **Arcade**, **Off**. Any hand adjustment switches the
preset to **Custom**.

## Test levers

`FULL TWIST -> Debug -> Test Kick / Test Crash / Test Rollover`, or the matching
ini keys for a `-frames` run:

- **Test Kick** applies a known roll/pitch rate and upward launch. Use it to
  compare settling on flat ground, after a jump, or after roof/side contact.
- **Test Crash** spawns a car about 420 units ahead of the player, facing back,
  and gives both a closing velocity. It exercises the car-car hull path.
- **Test Rollover** turns the car's heading 90° (yaw, not roll), holds that
  pose for one second (30 frames at the 30 Hz sim step), then applies a lateral
  impulse perpendicular to the car's direction — 8× the old test shove
  (1,200,000) plus a 7,200,000 roll rate. The log reports `rollover count` each
  time the up vector crosses the horizon, so multiple rollovers are visible
  directly.

## Measuring

Turn `debug_log` on and the log carries:

```
[fulltwist] hull slot=0 ... size=(...) cog=(...) colBox=(...) footprint=15
[fulltwist] hit frame=N car=... mode=... torque=(pitch,yaw,roll) rollMix=...
            pitchMix=... rate=... hull=1 inertia=(...) contact=(...)
[fulltwist] scenery hull car=... hullY=(...) buildingY=(...) point=(...)
[fulltwist] scenery skip car=... hullY=(...) buildingY=(...)
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
  death (`mission.c`). The catch is what keeps a tumble from ending there; tune
  `rollover_damping` / `rollover_catch` rather than editing `mission.c`.
- The hull is built from `gCarCleanModelPtr[slot]` and cached per resident slot
  by model pointer + vertex count. A level change or a hot import that swaps the
  model rebuilds it on the next query.
- The scenery override is conservative: it only rejects a hit when the hull and
  the building box do not overlap vertically. A real hit keeps the stock wall
  normal and only moves the contact point.
- The vehicle deformation/import features still own their own collision logic;
  FULL TWIST only writes `st.n.angularVelocity`, `st.n.linearVelocity` and
  `hd.aacc`.
