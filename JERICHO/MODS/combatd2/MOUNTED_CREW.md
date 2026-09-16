# Mounted crew — the driver and gunner leaning out to fire

Some weapons are fired by a **crew member leaning out of the car**, not by the
vehicle itself: the driver hangs out of their window to fire one, the gunner
(the passenger) hangs out of the other. Others — the machine gun, the missile,
the seeker, the mine, the freeze — fire **from the vehicle directly**. This
document is the map of that split and of the ped lifecycle that draws it.

Source: `weapons/core/crew.c` / `crew.h`, plus the muzzle helpers in
`weapons/core/weapons.c`.

## 1. Which weapon leans whom (`CD2_WEAPON_DEF.leanOut`)

`leanOut` is a **bitmask** — which crew member leans out *while this weapon is
firing*:

| bit | value | crew |
|---|---|---|
| 0 | `1` (`CD2_CREW_DRIVER`) | the driver — the **left** window |
| 1 | `2` (`CD2_CREW_GUNNER`) | the gunner — the **right** window |
|   | `3` | both (a single muzzle uses the driver's window) |
|   | `0` | nobody (the vehicle fires it) |

Assignments today:

| weapon | leanOut | who leans |
|---|---|---|
| `SHOTGUN` | 1 | driver |
| `CLUSTER` | 2 | gunner |
| `ZOOMY`   | 2 | gunner |
| `MG`, `MISSILE`, `HOMING`, `MINE`, `FREEZE` | 0 | the vehicle |

Both crew members use **Tanner's model** (ped model 0). That is not a
preference: it is the only model that is always loaded and renders through the
engine's *skeleton* path. Civilians render through the sprite path (`DrawCiv`),
which reads a different motion format, cannot play the get-out pose, and gets
recycled by the ambient-pedestrian system.

## 2. The request: firing **or** selected

Every weapon fire — the player's trigger and the AI's — goes through
`cd2WpnTryFire`, which calls `cd2CrewNotifyFire(car, leanOut)`. Two things feed
the same request:

- **Selected** — `cd2WpnOnFrame` calls `cd2CrewArmed(car, selectedDef->leanOut)`
  every frame, so **picking** a leaning weapon brings its ped out and *holds it
  out* while that weapon stays selected. Switching to a non-leaning weapon stops
  refreshing the hold and the ped gets back in.
- **Fired** — `cd2CrewNotifyFire` refreshes the hold at the moment of a shot.
  This is what drives the **AI**, which has no "selection": it leans a ped out
  when it fires a leaning weapon.

The flags **OR** together and each set side gets its hold (24 frames) re-armed.
So a side that any firing/selected leaning weapon wants out **stays out**, even
while the base machine gun (leanOut 0) fires: the MG contributes nothing and can
never clear another weapon's side.

## 3. The ped lifecycle

Driven off the request, one ped per side (`weapons/core/crew.c`):

1. **out** — a side's hold goes live: spawn a parked `TANNER_MODEL` ped beside
   the car's door (`padId = -1`) and climb it out with `PED_ACTION_GETOUTCAR`.
   The animation advances `CD2_CREW_ANIM_STEP` (3) frames per tick (the
   engine's own get-out steps one per tick, which reads sluggish) and stops at
   the side's end frame: the **driver at 9**, still leaning out of the window,
   the **gunner at 14**, fully out.
2. **held** — the ped settles into its resting pose and is **re-placed from the
   car's transform every frame** so it rides the car:
   * **driver** — holds the mid-climb lean, with his right arm forced into a
     weapon-holding reach (see *The poses* below);
   * **gunner** — switches to `PED_ACTION_SIT` raised `CD2_CREW_SILL_RAISE`
     above normal height, so he perches *on* the windowsill.
3. **in** — the hold lapses (or the weapon is deselected): play
   `PED_ACTION_GETINCAR` 0→15 (same step rate) and `DestroyPedestrian` the ped
   as it disappears. This is done by the module, **never** through the engine's
   `PedGetInCar` (that calls `ChangePedPlayerToCar` and would hijack the
   player's car).

If fire resumes mid-retract the side flips straight back to *out*.

### Placement (the two things that were wrong first time)

`cd2CrewPlace` hangs the ped on the door: lateral = `colBox.vx * 1.08` out to
the side (a *per-side* sign), forward = `colBox.vz / 4` (a touch *ahead* of
centre, not the rear), grounded with `MapHeight` plus the pose's raise, and
rotated by **one** yaw for both doors — `hd.direction - 90°`.

The facing is deliberately *not* mirrored per side. The ped model's front is
not aligned with the yaw vector, so `hd.direction -/+ 90°` (which is how the
engine orients a ped climbing out in `SetupGetOutCar`) leaves one door correct
and the other 180° out — verified both ways round in game. Only the lateral
offset is per-side.

The placement runs on **`JER_EVENT_CAMERA`** (fired from `InitCamera`, just
before `DrawAllPedestrians`), *not* only on `FRAME`. `FRAME` fires before
`StepCars()`, so a ped placed there is a whole step behind the car at speed —
plainly visible as lag. The `CAMERA` pass uses the cars' final transforms for
the frame.

### The poses, and who is allowed to be posed

Both poses come from the engine's own motion data (`PED_ACTION_*`) — no new art:

* **driver, weapon arm** — forced through **`JER_EVENT_PED_SKELETON` phase 0**
  (the POSITION channel, `vCurrPos`), mirroring d2pl's `poseArmPose` shape: the
  shoulder stays at its rest offset, the forearm is raised and pushed forward,
  and the hand extends past it (`CD2_CREW_ARM_*`; `handZ` must exceed `elbowZ`
  or the arm folds back on itself). Both crew peds share one body yaw, so the
  driver's reach is rotated half a turn (`CD2_CREW_ARM_FLIP`) to point out of
  *his* window. The pose is re-applied on every draw, because the skeleton
  resets `vCurrPos` each frame.
* **gunner, sill perch** — `PED_ACTION_SIT` plus the `raiseY` above.

Reaching a module's own ped from a pose hook is what the **ownership gate** is
for: the ped-pose hooks (`PED_POSE`, `PED_SKELETON`) fire for the player ped
**or any ped a module owns**. Spawning through `jer_npc_spawn*` marks the ped
(`jer_npc_owned`), despawning unmarks it, and the query also checks `pUsedPeds`
— so a ped the engine destroyed behind our back never matches a recycled slot.
Ambient pedestrians are never posed, and the engine pays nothing for them.

> Two channels, and they are not interchangeable (full map:
> `JERICHO/docs/ped-animation.md`): **positions** take from `PED_SKELETON`
> phase 0, **rotations** only from `JER_EVENT_PED_POSE` (before
> `newRotateBones`). The arm reach uses positions; a lean built from a bone
> *rotation* would have to go through `PED_POSE`.

## 4. Firing from the window

`cd2WpnShotMuzzle(def, car, side, out)` sources a shot: when the weapon leans
(`cd2WpnMuzzleSide` maps `leanOut` → −1 driver / +1 gunner) it returns
`cd2WpnWindowMuzzle` — the door plane (`colBox.vx`) and up toward the roof
(`65% colBox.vy`) — so the pellets/missile visibly leave from the window the ped
hangs out of. Non-leaning weapons keep the centred body muzzle. Used by the
shotgun scatter, the cluster and the zoomy burst (and the shotgun's sound).

## 5. AI contestants

So "all contestant cars" lean, `ai/opponent.c` fires the leaning weapons too:
SHOTGUN (driver) up close, CLUSTER/ZOOMY (gunner) at range. Each is its own
`if (!launched …)` so a cooling-down weapon doesn't block the next, and the
missile/seeker primaries still fire in between. The AI goes through the same
`cd2WpnTryFire → cd2CrewNotifyFire` path, so its cars get crew for free.

## 6. Wreck bail-out (run away on fire)

When a car the module drives is destroyed (`cd2CarTotaled`), both crew sides
**bail out**:

- a ped already leaning converts to fleeing; a side that wasn't out is spawned
  at the door first;
- it runs **away from the wreck**: base heading = `ratan2` of the ped's offset
  from the wreck point, swung by a sine each step (a wavy path), stepping with
  the engine's own `AnimatePed` forward math and a 16-frame `PED_ACTION_RUN`
  cycle, for ~3s before it is cleaned up;
- it is painted **flat black** through the engine's `JER_EVENT_PED_DRAW` hook
  (matched back to the crew by ped pointer) and `SMOKE_FIRE` is set up at its
  origin every few frames — reading as running from the wreckage in flames.

## 7. Player death: hold + drift the camera

While the local player's car is a wreck the crew module takes over the camera
(`JER_EVENT_CAMERA`, `override = 1`), so the death plays without the stock view
sliding around. Rather than freeze it dead, the held view **drifts** over the
respawn window (`gCd2Cfg.respawnDelay`, ~150 frames / 5s):

- it **pulls back** along (camera − wreck) up to `CD2_DEATHCAM_ZOOM` (700
  world units) — the wreck shrinks, i.e. a zoom out at the engine's fixed FOV;
- it **rises** up to `CD2_DEATHCAM_RISE` (220) — engine camera Y is *down*, so
  "up" is a smaller `vy`;
- both are interpolated from the pose captured on the last live frame, and the
  focus is the car's position at the moment of death (so a tumbling wreck does
  not drag the view).

It is released (stock camera resumes) when the car respawns. Constants live in
`weapons/core/crew.c`; tune them there.

## 8. The engine hook

`JER_EVENT_PED_DRAW` (`JER_ARGS_PED_DRAW { ped, flatBlack, tintR/G/B }`) is the
ped analogue of `JER_EVENT_CAR_DRAW_COLOR`. It is fired in `newShowTanner`
before a ped's bones are drawn; a module may set `flatBlack` (or a tint) and the
engine holds `plotContext.planeColours` at that colour for the ped's
`RenderModel` calls, restoring afterwards. With no handler the ped renders
stock. It is appended at the end of the event enum so existing ids are
unchanged, and mirrored into `JERICHO/sdk/include/`.

## 9. Guards

- **Distance** — no crew is spawned beyond ~2000 units from the camera.
- **Ped pool** — `MAX_PEDESTRIANS` (28) is shared with ambient civs; a full pool
  is a silent skip.
- **Stale pointers** — every stored ped is re-validated against `pUsedPeds`
  each frame (a cutscene/level reset can destroy it).
- **Level reset** — `JER_EVENT_GAME_START` clears all crew state *without*
  touching the pool, which `InitPedestrians` has already rebuilt by then.

## 10. Verifying headlessly

`cd2debug.c` drives the whole thing from `JERICHO/CONFIG/cd2_debug.txt`
(frames from GAME_START, 30 fps):

```
60:grant                 # every weapon to max (the cycle skips unowned)
70:select:shotgun        # driver leans out - no firing needed
90:crew                  # dump: 'crew frame=.. sel=SHOTGUN player car=0 driver=1 ..'
130:select:missile       # non-leaning: the driver gets back in
160:crew
170:muzzle               # centred vs left/right window muzzles
180:fire:cluster         # gunner leans + fires
190:killplayer:self      # wreck -> both bail out, camera held
```

Poses, the flat-black tint, the sine path and the fire are **not** verifiable
headlessly — they need a play-test. What a run *does* prove: the request/mask
bits, the spawn/hold/retract/despawn lifecycle (ped count returns to 0), the
window muzzles, that AI contestants fire leaning weapons, the wreck bail-out and
the camera hold/release, with no crash and no ped leak.
