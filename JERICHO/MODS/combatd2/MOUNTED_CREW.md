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
   the car's door (`padId = -1`, so the player-only pose/shadow paths never
   fire) and play `PED_ACTION_GETOUTCAR` 0→14, holding at 14 (the engine's own
   transition would fire at 15).
2. **held** — while the hold is refreshed the ped keeps the get-out pose and is
   **re-placed from the car's transform every frame** so it rides the car.
3. **in** — the hold lapses (or the weapon is deselected): play
   `PED_ACTION_GETINCAR` 0→15 and `DestroyPedestrian` the ped as it disappears.
   This is done by the module, **never** through the engine's `PedGetInCar`
   (that calls `ChangePedPlayerToCar` and would hijack the player's car).

If fire resumes mid-retract the side flips straight back to *out*.

### Placement (the two things that were wrong first time)

`cd2CrewPlace` hangs the ped on the door: lateral = `colBox.vx * 1.08` out to
the side, forward = `colBox.vz / 4` (a touch *ahead* of centre, not the rear),
grounded with `MapHeight`, facing outward (`hd.direction -/+ 90°`).

The placement runs on **`JER_EVENT_CAMERA`** (fired from `InitCamera`, just
before `DrawAllPedestrians`), *not* only on `FRAME`. `FRAME` fires before
`StepCars()`, so a ped placed there is a whole step behind the car at speed —
plainly visible as lag. The `CAMERA` pass uses the cars' final transforms for
the frame.

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

## 7. Player death: hold the camera

While the local player's car is a wreck the crew module holds the camera at its
last live frame (`JER_EVENT_CAMERA`, `override = 1`), so the death plays without
the view sliding; it releases when the car respawns.

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
