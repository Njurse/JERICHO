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
   * **driver** — holds the mid-climb lean (frame 9) with his right arm forced
     into a weapon-holding reach (see *The poses* below);
   * **gunner** — the **same** motion at the **same** frame, raised
     `CD2_CREW_SILL_RAISE` so he perches *on* the windowsill. Both sides use one
     animation and one frame on purpose: a later frame stands the ped up, and
     raising *that* just floats him.
3. **in** — the hold lapses (or the weapon is deselected): play
   `PED_ACTION_GETINCAR` 0→15 (same step rate) and `DestroyPedestrian` the ped
   as it disappears. This is done by the module, **never** through the engine's
   `PedGetInCar` (that calls `ChangePedPlayerToCar` and would hijack the
   player's car).

If fire resumes mid-retract the side flips straight back to *out*.

### Placement

`cd2CrewPlace` hangs the ped on the door: lateral = `colBox.vx * 1.08` out to
the side (a *per-side* sign), forward = `colBox.vz / 4` (a touch *ahead* of
centre, not the rear), **riding the car** in all three axes (see *3D placement*
below), and rotated to `hd.direction + CD2_CREW_YAW_OFFSET`.

**Facing forward, mirrored per side.** `CD2_CREW_YAW_OFFSET` is 0, so both crew
face *forward along the car* — and they are a horizontal mirror pair: the driver
leans out of the car's left door, the gunner out of the right. (The knob reads
`0` = forward, `1024` = turned a quarter to face out of the door — the earlier
look — `2048` = backwards.)

That the yaw is the car's own heading is exact rather than fitted. Both angles
come off the car's matrix: the car's local `+Z` (its nose) sits at world angle
`1024 - hd.direction`, and a ped's forward `(RSIN yaw, RCOS yaw)` sits at
`1024 - yaw`, so `yaw = hd.direction` puts the two on top of each other —
verified in game as `pedFwd == carFwd` on both sides.

The *mirroring* is **not** done by flipping the yaw (the ped model's front is
not aligned with the yaw vector, so a per-side `hd.direction -/+ 90°` — which is
how the engine orients a ped climbing out in `SetupGetOutCar` — leaves one door
correct and the other 180° out). It goes through the engine's own get-out mirror
flag instead; see *The get-out mirror* below. Only the lateral offset and that
flag are per-side.

The placement runs on **`JER_EVENT_CAMERA`** (fired from `InitCamera`, just
before `DrawAllPedestrians`), *not* only on `FRAME`. `FRAME` fires before
`StepCars()`, so a ped placed there is a whole step behind the car at speed —
plainly visible as lag. The `CAMERA` pass uses the cars' final transforms for
the frame.

### The poses, the mirror flag, and who may be posed

Both sides reuse the engine's own `GETOUTCAR` motion — no new art, no invented
pose (`PED_ACTION_SIT` was tried and reverted: its legs dangle ~95 below the hip
and land ~13 units *inboard* of it, so they hung through the door panel).

* **weapon arm** — both crew get it, each on his own side, forced through
  **`JER_EVENT_PED_SKELETON` phase 0** (the POSITION channel, `vCurrPos`),
  mirroring d2pl's `poseArmPose` shape: shoulder at its rest offset, forearm
  raised and pushed quarter-turned out of the ped's own door
  (`CD2_CREW_ARM_DOOR_TURN`, sign flipped for the gunner), hand extended past
  it. Re-applied every draw (the skeleton resets `vCurrPos` each frame) — and it
  goes through `jer_anim_rotate_offset`, because by phase 0 the chain has
  already been rotated and the position channel is *world-oriented*; a raw
  body-relative offset lands in the wrong place entirely.
  Note the pose constants are single-digit ped-local units and `>> 12` in the
  rotate helper truncates them to ±1 — effectively invisible. The crew in fact
  read correctly on the plain motion; treat this as a hook, not the look.
* **the mirror flag** — the one that actually mattered. For a get-out the engine
  sets the shared `bReverseYRotation` (`SetupGetOutCar`) and
  `newRotateBones` **mirror-flips the ped's root rotation** when it is set. Both
  crew play that same motion at the same frame with the same forward-facing
  orientation, so the second door needs the flip to read as a mirror image:
  `cd2CrewOnPedPose` sets it per side (driver = unmirrored, gunner = mirrored)
  and the skeleton phase-1 pass hands the game's value straight back. That is the
  only effective *rotation* channel.

Reaching a module's own ped is what the **ownership gate** is for: the ped-pose
hooks (`PED_POSE`, `PED_SKELETON`) fire for the player ped **or any ped a module
owns**. Spawning through `jer_npc_spawn*` marks the ped (`jer_npc_owned`),
despawning unmarks it, and the query also checks `pUsedPeds` — so a ped the
engine destroyed behind our back never matches a recycled slot. Ambient
pedestrians are never posed, and the engine pays nothing for them.

> Two channels, and they are not interchangeable (full map:
> `JERICHO/docs/ped-animation.md`): **positions** take from `PED_SKELETON`
> phase 0, **rotations** only from `JER_EVENT_PED_POSE` (before
> `newRotateBones`).

### 3D placement — the crew rides the car, not the map

Two bugs lived here, both from treating a mounted ped as a grounded one:

* **Y** came from `-MapHeight(x,z)` — the map under the ped, which is *not*
  attached to the car. A car in the air (or tossed by a wreck) left its crew
  standing on the road below. Now `y = -cp->hd.where.t[1] - CD2_CREW_BODY_LIFT`,
  riding the car. Note the conventions: the car matrix is **Y-UP** while a ped's
  `position` is **Y-DOWN** (`camera.c` proves it: `where.t[1] == -cameraPos.vy`),
  hence the negation. `CD2_CREW_BODY_LIFT` (99) reproduces the old on-ground
  offset, measured with the car level.
* **body rotation** was the 2D `hd.direction` with the body always upright.
  `newRotateBones` builds a ped's root with `RotMatrixYXZ(dir)` over **all
  three** of `dir.vx/vy/vz`, so pitch and roll are settable.
  `cd2CrewMatrixEuler` inverts `RotMatrixYXZ` (PsyCross `LIBGTE.C`) into the
  car's YXZ triple and `jer_npc_set_orient()` applies it, so the crew banks and
  pitches with the car — and the arm, posed in the ped's own frame, follows.
  The extracted yaw is **identical to `hd.direction`** (verified), so it feeds
  the facing directly (`CD2_CREW_YAW_OFFSET` = 0 = face forward); the pitch/roll
  sign is the one `CD2_CREW_TILT_SIGN` knob.

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
