# Ant Farm — how it works (internals)

This is the deep companion to `readme.md`. The readme says *what* Ant Farm is
and how to use it; this says **how every part of it works, why it is built that
way, and what will bite you if you change it.** Written against the current
`antfarm.c` — symbol names are the anchors.

Ant Farm is a JERICHO module compiled *into* the exe (not a runtime DLL), so
every code change needs a rebuild: `src_rebuild/build_dev.bat`, i.e.

```
premake5 vs2019          # only needed when files are ADDED
msbuild build/REDRIVER2.vcxproj /p:Configuration=Release_dev /p:Platform=x64
```

A stale exe silently ignores code changes, so check the exe timestamp.

---

## 1. What it takes over

The module is a **camera and game-state takeover**, not a new game mode. It

| Global / subsystem | How it is taken | Restored |
|---|---|---|
| Player input | `gStopPadReads = 1`; the on-foot pad is zeroed in `JER_EVENT_PED_INPUT` (`AntFarmOnPedInput`) | saved/restored in `AntFarmSetActive` |
| HUD / overlays | `gDoOverlays = 0` | ditto |
| Cop aggression | `CopsAllowed = 0` | ditto |
| The player's car | teleported to the shot focus every frame, hidden (`CONTROL_TYPE_NONE` + `reservedSlots[]`) so the draw loop (`draw.c`) skips it, and its engine silenced | `AntFarmPinPlayerCar` + the restore block in `AntFarmSetActive(0)` |
| Camera | `JER_EVENT_CAMERA` at priority **100** → writes `camera_position` / `camera_angle` and sets `a->override = 1` | `scr_z` and `CameraCar` are restored on exit |
| Region streaming | `MainPlayer.spoolXZ` is redirected at a module-owned `VECTOR` (`s.spool`) | `savedSpoolXZ` |
| Projection distance | `SetGeomScreen(scr_z = …)` every frame (the lens breathes) | `savedScrZ` |
| Audio | the player's own car engine is forced silent via `JER_EVENT_CAR_ENGINE_SOUND`; the master volume is kept at the game's normal level while active | `savedMasterVolume` |

Priority 100 for the camera is deliberate: any other module's camera hook must
run *below* it, so the screensaver's camera wins while it is on.

### Hooks registered

| Event | Handler | Priority |
|---|---|---|
| `JER_EVENT_BOOT` | `AntFarmOnBoot` — reads config, builds the road cache, builds the pause menu | 0 |
| `JER_EVENT_GAME_START` | `AntFarmOnGameStart` — adopts `-seed`, and re-engages on a level (re)start | 0 |
| `JER_EVENT_FRAME` | `AntFarmOnFrame` — F9, auto-enable, the whole state machine, the texture-area guard | 0 |
| `JER_EVENT_CAMERA` | `AntFarmOnCamera` — the render-time camera takeover, void guard, FOV | 100 |
| `JER_EVENT_CAR_ENGINE_SOUND` | `AntFarmOnCarEngineSound` — silences the player's car engine | 0 |
| `JER_EVENT_PED_INPUT` | `AntFarmOnPedInput` — zeroes the on-foot pad | 10 |
| `JER_EVENT_PAUSE_MENU` | `AntFarmOnPauseMenu` — suspends on pause-open | 0 |
| `JER_EVENT_DRAW_OVERLAY` | `AntFarmOnDrawOverlay` — fade wash, letterbox, caption, debug HUD | 0 |

---

## 2. Lifecycle, and the meaning of "on"

`AntFarmSetActive(on)` is the only place engine state is taken or handed back.
**`on` is enforced**: while the mode is on, nothing else may quietly switch it
off. The three ways it can be interrupted all *suspend* it:

```c
static void AntFarmSuspend(void)      /* hand back, but stay WANTED */
{
    AntFarmSetActive(0);
    s.pendingEnable = 1;
}
```

| Trigger | Call site | Effect |
|---|---|---|
| The pause menu opens | `AntFarmOnPauseMenu` (`JER_PAUSE_OPEN`) | suspend — the pause menu needs the camera and the player's car back |
| A cutscene, a replay, or the game-over screen | `AntFarmOnFrame`'s active guard | suspend — the engine drives the screen |
| A level (re)start | `AntFarmOnGameStart` | `savedPlayerCarControlType = 0` first (so the hand-back cannot write the old level's car state into the new one), then suspend |

`pendingEnable` is the **want**, and it is what the auto-enable path re-reads:

```c
if (s.pendingEnable && !s.active && pauseflag == 0 && AntFarmMapReady() &&
    !game_over && !gInGameCutsceneActive && !quick_replay &&
    NumPlayers == 1 && !NoPlayerControl)
{
    AntFarmSetActive(1);      /* clears pendingEnable on success */
}
```

`pauseflag == 0` is what stops a suspended mode from re-arming *while the pause
menu is still up* — without it the module would re-take the camera behind the
pause screen. `AntFarmSetActive(0)` clears `pendingEnable`, so the only ways to
turn the mode off for good are `AntFarmToggle` (F9 / the menu row) and a
refused activation.

`AntFarmToggle` is **intent-based**, not "flip the running flag":

```c
if (s.active || s.pendingEnable) { AntFarmSetActive(0); config enabled = 0; }
else                             { pendingEnable = 1; AntFarmSetActive(1); config enabled = 1; }
```

That is why the pause-menu row reads `(s.active || s.pendingEnable) ? ON : OFF`
— a suspended mode still reads ON.

### Activation sequence

1. refuse unless the game is in a playable single-player state (`game_over`,
   `gInGameCutsceneActive`, `quick_replay`, `NumPlayers != 1`,
   `NoPlayerControl`, `!AntFarmMapReady()` are all rejected);
2. save every global listed in §1;
3. take them (§1);
4. seed the camera from `AntFarmPickNearArea()` (the nearest usable straight to
   the camera) — **only** an initial position for `camPos` and `spool`, so the
   takeover frame sits on loaded ground;
5. enter `ANTFARM_STATE_CUT` with `fade = 255`, i.e. the first frame is black
   and the normal cut machinery plans and streams the first shot. (Planning and
   revealing a shot straight from activation is what produced the old
   grey/skybox first frame.)

**The activation shot is the one exception to the CUT hold.** The normal cut
waits `ANTFARM_CUT_HOLD_MS + ANTFARM_TEX_SETTLE_MS +` a jump-scaled extra before
revealing, which on the first cut is a couple of seconds of black. `s.firstShot`
(set here, cleared when the first `FADE_IN` starts) makes that first cut reveal
the moment its region is resident and the shot is planned — nothing else — and
fades in over `ANTFARM_FIRST_FADE_MS` (500 ms) instead of `ANTFARM_FADE_MS`
(700 ms). Every later cut keeps the full hold and the normal transition, and the
transition logs `[antfarm] first shot: fading in over 500ms` so the behaviour is
checkable from the log alone.

### Deactivation sequence

`AntFarmSetActive(0)` restores pads, overlays, cop flag, master volume,
`SetGeomScreen(scr_z = savedScrZ)`, `CameraCar`, `MainPlayer.spoolXZ`, and (when
`savedPlayerCarControlType != 0`) the player's car — `controlType`,
`reservedSlots`, the world position, and zeroed linear/angular velocity and
`fposition`. It also ends a rogue-car event first (`AntFarmEndLead`).

---

## 3. The cut state machine

Driven from `AntFarmOnFrame` on wall-clock time (`AntTick` = `SDL_GetTicks`).
Four states, `s.state`:

```
SHOW ──(dwell, or subject gone/parked)──▶ FADE_OUT ──(fade 255)──▶ CUT ──▶ FADE_IN ──▶ SHOW
```

| State | What happens |
|---|---|
| `SHOW` | the shot is on screen. Ends on `s.dwellMs` (interest-scaled), or early if the subject despawned and no replacement car exists, or if a rig/long-lens subject parks (`ANTFARM_STILL_MS`, `s.stillSince`) or recedes out of range |
| `FADE_OUT` | `s.fade` ramps 0→255 over `ANTFARM_FADE_MS` (700 ms) |
| `CUT` | the black beat. Plans the shot, forces the destination region in, waits for it, then reveals. On the **first** cut (`s.firstShot`) the wait is skipped and the shot is revealed as soon as it is resident + planned, so activation does not open on black |
| `FADE_IN` | `s.fade` ramps 255→0 over `s.fadeInMs` (`ANTFARM_FIRST_FADE_MS` 500 on the activation shot, else `ANTFARM_FADE_MS` 700); on reaching 0 → `SHOW`, `shotStart = now` |

Timings live in `antfarm.h`: `ANTFARM_FADE_MS 700`,
`ANTFARM_FIRST_FADE_MS 500` (the activation shot only), `ANTFARM_CUT_HOLD_MS 250`,
`ANTFARM_CAR_WAIT_MS 5000`, `ANTFARM_STREAM_TIMEOUT_MS 400`,
`ANTFARM_BLACK_CAP_MS 700`, `ANTFARM_TEX_SETTLE_MS 250`,
`ANTFARM_STILL_MS 2500`. Shot length is clamped to `ANTFARM_SHOT_MIN_MS
15000 … ANTFARM_SHOT_MAX_MS 45000`.

A **watchdog** in `AntFarmOnFrame` recovers from any non-`SHOW` state older
than 30 s (`state <n> stuck — recovering` → `SHOW`). It exists because a stuck
CUT used to mean a permanently black screen.

### The CUT: shot planning + streaming

`if (!s.cutInit)` runs once per cut. **Every cut — the first included — picks a
fresh far area and a style by interest** (`AntFarmPickFarArea`,
`AntFarmPickStyleAndTarget`). The first cut used to keep the player's own
neighbourhood and force a road-only scenery angle, which is what made a
mid-game toggle open on a parked camera pointed at the road you were just on.

Then, per frame while black:

1. **Force the destination region in.** The engine only pre-loads *neighbouring*
   regions as you drive (`CheckUnpackNewRegions` / `force_load_boundary`), so a
   far hop's region is never resident. The module computes the region, its
   **barrel slot** (`(region_x & 1) + (region_z & 1) * 2`), and calls
   `UnpackRegion(region, barrel)` + `StartSpooling()` + `UpdateSpool()` so it
   lands *in the same frame* (on PC the spool copies synchronously).
2. **Force the destination's texture AREA in.** Texture pages stream per *area*
   keyed off `camera_position`, so the camera is also parked at the destination
   during the black, and `CheckLoadAreaData(cellX, cellZ)` + a second
   `StartSpooling`/`UpdateSpool` flushes them.
3. Wait until the region is resident and the level has road nodes
   (`nodesReady = NumDriver2Straights > 0`). On timeout
   (`ANTFARM_STREAM_TIMEOUT_MS`) re-pick a far area, up to 3 retries; the
   `ANTFARM_BLACK_CAP_MS` cap then forces progression with a fallback anchored
   on the camera position (which is definitely loaded) and says so in the log.
4. Plan the shot (`AntFarmPlanShot`), pick a car subject if the style wants one
   (`AntFarmPickCarNear`; if none appears within `ANTFARM_CAR_WAIT_MS`, fall
   back to a road-only style), then hold the black for
   `ANTFARM_CUT_HOLD_MS + ANTFARM_TEX_SETTLE_MS + jump-scaled extra` and reveal.

### Region residency — the two engine facts that shaped this

* `regions_unpacked[4]` / `loading_region[4]` are indexed by **barrel slot**,
  not by region number. Treating them as a per-region map reads out of bounds
  and never fails. → use `jer_map_region_resident()` (`JER_EVENT_*`-shared, see
  `JERICHO/docs/map-streaming.md`).
* The engine keeps only **four** regions resident (a 2×2 window), so a
  "centre + 4 neighbours" residency test can never pass. The achievable test is
  "this region has data (`spoolinfo_offsets[r] != 0xffff`) and it is the region
  currently unpacked into its barrel slot".

### The void guard (render time, `AntFarmOnCamera`)

The renderer culls from `camera_position`, so the **camera's own region** is
what must be resident. If it is not, the module holds the previous camera
(`s.camPos`) instead of drawing an unloaded one, and logs
`void guard: region not resident, holding camera (#n)`. A 4 s cap means a
data-less region can never freeze the view forever. The spool is pointed at the
camera *before* this guard, or the hold would freeze the spool and the camera
could never become resident (a deadlock).

### The texture-area guard (frame time)

A travelling shot (dolly, flyover, crane) crosses map cells faster than the
pager settles — the "scenery / palette did not load" look. When the camera
enters a new cell during a live shot, `AntFarmOnFrame` calls
`CheckLoadAreaData(cellX, cellZ)` + `StartSpooling`/`UpdateSpool`, exactly as the
CUT does for a destination. Tracked by `s.lastCellX/lastCellZ`.

### Traffic pre-seed (`AntFarmPreseedTraffic`)

The engine only dribbles civilians in a few per frame, around
`MainPlayer.spoolXZ`, so a cut that hops to a fresh area opens on an empty road
and a car-subject style has nothing to frame. `AntFarmPreseedTraffic(nMax)`
fills the area at once with the engine's own spawner, `PingInCivCar` — the same
call the game uses, so the cars are ordinary traffic.

It is called from three places: **level start** (`AntFarmOnGameStart`, only when
the mode is active or wanted, so plain play is untouched), **activation /
re-engage** (`AntFarmSetActive(1)`), and **once per cut** as soon as that cut's
destination region is resident (`s.seededThisCut`, reset in the CUT's `cutInit`,
right before the shot is planned — so a car-subject cut has a subject waiting).

It hands `MainPlayer.spoolXZ` the focus for the duration (the module's shot area
while active, otherwise the player's own spool) and restores it after. The
player's car is never a candidate: it is pinned with `reservedSlots[] = 1` and
`CONTROL_TYPE_NONE`, which the spawner's free-slot search skips.

**No invisible cars.** `InitCar` places every pinger with `MapHeight()`, which
answers 0 for a cell that is not resident — the car lands in the void and is
never drawn. So the focus region is streamed in first (`jer_map_spool_to` when
it is not resident) and the seed only pings when the region has data *and* is
resident; otherwise it logs
`[antfarm] pre-seed skipped: region N not resident (cars would spawn invisible)`
and spawns nothing. Each seed logs its count:
`[antfarm] pre-seed: N civ car(s) around (x,z) region R`.

---

## 4. The director

### `antStyleDefs[]` — one row per archetype

Both selection and framing read this table, so a new angle is one row plus one
camera case. Columns (`ANT_STYLE_DEF`):

| Column | Meaning |
|---|---|
| `label`, `key` | menu label, and the config suffix `style_<key>` |
| `model` | `ANT_MODEL_*` — **how the camera is driven** |
| `weight` | selection weight |
| `roadOnly` | never takes a car subject |
| `carFirst` | takes a car subject (and prefers one) |
| `behind` | attached rigs: sit behind the car |
| `zoom` | push the lens in and back out across the shot |
| `heightLo/Hi` | camera elevation above ground/road |
| `scrZLo/Hi` | FOV (projection distance; smaller = wider). For a zoom row these are the ramp's two ends |
| `sideLo/Hi` | lateral offset (rig offset, or the roadside margin) |
| `fwdLo/Hi` | longitudinal offset (attached rigs) |
| `aimLo/Hi` | how far ahead of the subject the aim sits |
| `orbitLo/Hi` | pan/sweep amplitude (0 = hold still) |
| `across` | aim across the road rather than along it |
| `settle` | settle divisor (bigger = calmer) |
| `dwell` | scene-interest dwell multiplier, ×100 |
| `nearWater` | prefer a road that runs beside water |

### Models (`ANT_MODEL_*`) and their maths

| Model | Drives | Used by |
|---|---|---|
| `ROADSIDE` | camera parked `side` off the lane, `height` above the road, aiming `shotLookAhead` along (or across, if `across`) the lane; optional slow pan (`shotOrbitAmp`), tiny sway/bob | Overhead, Tripod, Ant level, Kerb |
| `DOLLY` | travels along the road, carrying on onto the connected road at a junction (`AntFarmAdvanceDolly` + `AntFarmNextRoad`) | Flyover, Waterfront |
| `CRANE` | a dolly that also rises from `ANTFARM_CRANE_LOW` to `shotHeight` across the shot | Crane |
| `ORBIT` | slow circle around the subject, one revolution per `ANTFARM_ORBIT_PERIOD_MS` | Orbit |
| `FOLLOW` | the **trail cam** (see below) | Chase |
| `ATTACH` | a damped, yaw-only rig on the car's own body (scaled by `colBox`), offsets in the car's frame | Fender, Sill, Nose 3/4, Tail 3/4 |
| `TRIPODZ` | a fixed vantage resolved from the car once per shot; the camera yaw-tracks while the lens pushes | Tripod zoom, Far pan |
| `JUNCTION` | parked off a crossroads corner; the **aim** slides slowly across the mouth | Junction |
| `TRACK` | **retired** — see §6 | Static track |

`AntFarmStyleImplemented(model)` is the gate: a model that is not implemented is
never offered by the picker and never defaults on. That is how a row is retired
without renumbering the enum (indices must stay aligned with
`ANTFARM_STYLE_*` in `antfarm.h` and with `antStyleDefs[]`).

### Picking a style — the weighted director

`AntFarmPickStyle(carOnly)` sums the weights of styles that are enabled, whose
model is implemented, and which match the request (`carOnly` 1 = car-capable
only, 0 = road-capable only, −1 = any), then rolls. A style seen in the last
`ANTFARM_STYLE_MEMORY` (3) cuts is **divided by 8** (not forbidden — a
two-style config must not stall), and a zero weight is floored to 1 so it can
still appear. `AntFarmPickStyleAndTarget` then decides the subject from
`carFirst` / `roadOnly`.

Car subjects are likewise de-weighted if recently framed
(`AntFarmCarRecent`/`AntFarmRememberCar`), and a style's minimum accepted car
speed is `AntFarmModelMinSpeed` (40 for rigs and long-lens rows, 8 otherwise).

### Picking an area — `AntFarmPickFarArea`

Refreshes the road cache first (`AntFarmEnsureRoadCache`) — at the first
activation it is still the empty boot one, and without this the first cut falls
back to the camera position (which, on an auto-enable at level load, is still
`(0,0)`).

Then it scores ~14 random usable roads with `AntFarmRoadInterest`:

* `+26` if longer than 2600, `+14` if longer than 1500;
* `+45` if a road runs beside water (`AntFarmRoadNearWater` probes 4 points along
  it with `GetSurfaceIndex` for `SURF_WATER` / `SURF_DEEPWATER` / `SURF_SAND`);
* `+6` per moving civilian within 6000 (capped at 6);
* `+40` if a POI is near — today always inert, see below;
* `−20` for staying in the region we are already in (the tour must move).

Candidates **near the world edge** are skipped: a shot (and the pinned car) on a
boundary road sits against the nodraw skybox, and the pinned car out there can
trip the game's own end-of-world handling. `AntFarmNearWorldEdge` is
`units_*_halved − 9000`.

### The POI scaffold (deliberately inert)

`antPois[]` / `AntFarmPoiNear` are a hook for named landmarks, and every row has
`valid = 0`, so the function always returns −1 and the scorer is unaffected.
Filling a row's `.x/.z` and setting `.valid = 1` is the only change needed to
start biasing shots toward it. The engine has **no** landmark or POI table (and
no junction surface id), which is why shots are picked by *feature*.

### Dwell

`AntFarmComputeDwell` = `interval × dwell/100`, ×1.3 for a road longer than
3000, ×0.8 for one shorter than 900, ×0.7 for a car subject (transient — do not
linger on an empty frame), then ×`ANTFARM_PACE_PCT`/100 (**85 — the tour changes
shots ~15 % more often than the interest score alone would suggest**, applied
before the clamps so the 15 s floor still protects against a short interval),
then clamped to 15–45 s. This is what makes the pace breathe instead of ticking
metronome-steady.

---

## 5. The follow cam (rewritten)

The follow shot used to cycle a chase-behind, an overhead and a chase-front
framing every few seconds and lerp 12 % of the way to the new framing per
frame. That is a camera that visibly **jumps and switches sides** mid-shot — it
was scrap. `AntFarmFollowCam` replaces it with **one continuous perspective**:

* the camera trails the car on its heading, at `back = vz*2 + vy + 420`
  (clamped 700…1400, plus `speed*3` up to 1900 so the shot opens up at speed)
  and `high = 190 + vy/2` (clamped 150…420), so a bus and a sports car both get
  a camera outside the panels;
* **yaw** is a damped slew toward the car's heading, capped at `d/12` per frame
  and always the short way round — a turn draws the camera around the corner
  instead of whipping it;
* **position** eases toward the ideal trail point at 6 %/frame horizontally and
  8 % vertically (*damped, not snapped*);
* roll and pitch are never inherited: the horizon stays level;
* the aim sits `600 + speed*4` ahead of the car, so the car rides low in frame
  and the road reads beyond it;
* `s.trailSet` (reset in `AntFarmInitShotVars`) re-seeds the trail on the car's
  heading at the start of every shot, so the first frame is already a
  behind-shot rather than a swing in from the gameplay camera;
* it returns `outLerp = 100`, i.e. it disables the generic per-frame position
  lerp — **no double smoothing**.

---

## 6. Retired: Static track

`ANT_MODEL_TRACK` ("Static track", key `static`) is **not implemented**. It
relocated its fixed roadside spot every time the subject passed it or drifted
away (`!trackPlaced || far`), so the view jumped/interpolated between new
vantage points every few seconds — hysterical. Removing `case ANT_MODEL_TRACK`
from `AntFarmStyleImplemented` retires it from the rotation (and from the boot
default) while keeping the enum/table indices stable. Its row and
`AntFarmModelName("track(retired)")` remain so the table stays aligned; the
camera case and `AntFarmStaticTrack` are gone.

---

## 7. Shot planning

`AntFarmPlanShot` → `AntFarmSetupRoadShot` → `AntFarmInitShotVars`; for a car
subject the car is attached *after* planning (`AntFarmSetCarSubject`), because
`PlanShot` resets `targetCarId`.

* **Road sampling.** `AntFarmShotRoadPoint` resolves a straight/curve + lane +
  distance into a world point and a traffic heading with `GetNodePos` and
  `ROAD_LANE_DIR` (the same helpers the civ AI drives on). It seeds a sentinel
  and treats "untouched" as a failed sample, because `GetNodePos` declines to
  write its outputs for some segments (which used to leave the shot anchored at
  the world origin).
* **Roadside shots** sometimes frame a road **end** (`AntRandChance(40)`), where
  junctions cluster; 45 % of those sit just past the end looking back.
* **Travelling shots** start at distance 0 and advance every frame.
* **Junction shots** always sit on a road **end**, but only one that is a *real*
  crossroads: `AntFarmJunctionEnd` steps 320 units past the end along the lane
  heading and asks `GetSurfaceIndex` which surface is there. A different usable
  straight means the roads meet; no road (a boundary end) means the vantage
  would look off the edge of the world, so the plan tries the other end and then
  other roads (10 tries), and says so if it finds nothing.
* **Dolly handoff.** `AntFarmAdvanceDolly` advances by the shot's share of the
  road; when it runs out, `AntFarmNextRoad` continues onto the connected road —
  `ConnectIdx` first, and since these level files leave it unpopulated, by
  stepping just past the end and asking `GetSurfaceIndex` (exactly how the civ
  AI tracks its current road). Up to 8 handoffs.

`AntFarmInitShotVars` randomises the framing from the row's ranges, sets the FOV
(one end of the ramp for a zoom row), decides whether the row pans
(`AntRandChance(72)`), and clamps the lens (200…360). It also resets
`trailSet`.

**Flank variety.** The same function picks `shotSideSign` (±1), which every model
uses symmetrically — the attached rigs, the tripod vantages, the roadside and the
junction placements. It deliberately **alternates** rather than coin-flipping:
a plain coin flip streaks, and several shots in a row on one flank is exactly
what reads as "the camera is always on the right". So ~75 % of shots take the
opposite side to the previous one and ~25 % repeat it (`s.lastSideSign`).

---

## 8. Camera rendering

`AntFarmOnCamera` (priority 100):

* in `CUT`, hold everything and park `camera_position` (x/z) at the destination
  so the engine streams the right texture areas;
* otherwise compute the desired position/aim (`AntFarmComputeCamera`), advance
  the dolly if the style travels, then:
  * scenery clearance — `AntFarmFindClearCamera` tries up to 32 candidate
    positions (the desired one, pull-backs, rotated angles, height offsets) and
    accepts the first with `lineClear` + a `CheckScenaryCollisions` camera-collider
    push-out. An `ATTACH` rig skips this on purpose: it is inches off the body,
    and pulling it back would tear it off the car;
  * the void guard (§3);
  * clamp above ground (`−90` below minimum);
  * smooth `s.camPos` toward the desired by `lerp/100`;
  * `PointAtTarget` for the angle, with the angle then slewed 18 %/frame (short
    way round), plus an optional barely-there horizon roll;
  * breathe the lens: `s.fovCurrent += (AntFarmFovTarget() − s.fovCurrent)/18`
    then `SetGeomScreen(scr_z = s.fovCurrent)`. `AntFarmFovTarget` returns the
    shot's lens, or, for a zoom row, a half-sine push to the other end and back;
  * `CameraCar = s.targetCarId` for a FOLLOW shot on a car.

A degenerate aim == camera pair is pushed 500 units along the current view
direction, or `PointAtTarget` has no direction and the view whips.

Per-shot telemetry is accumulated (`shotDistMin/Max`, `fovMin/Max`) and logged
at `FADE_OUT`:

```
[antfarm] shot #3 model=junction subject=road dist 808..808 fov 253..321
```

That line is what makes shots checkable without eyes.

---

## 9. Presentation

`AntFarmOnDrawOverlay` draws, in order: the transition wash (up to 3 stacked
50 % black `POLY_F4` passes, plus a solid black at ≥250 — it darkens through
black rather than flashing through white), the optional soft letterbox
(`ANTFARM_LETTERBOX_H`), the occasional place-name caption
(`ANTFARM_CAPTION_MS`, fading in and out, positioned bottom-left), and the
optional `debug_hud` readout bottom-right:

```
antfarm: <style key> <model> <car|road> <show|fade-out|cut|fade-in>
```

Place names come from `AntFarmPlaceName` (`GameLevel` → Chicago / Havana / Las
Vegas / Rio).

---

## 10. Audio

* The **player's own car engine** is silenced in `AntFarmOnCarEngineSound` by
  forcing `idleVolume` and `revVolume` to `-10000` (the PSX "silent" volume).
  The car is pinned and hidden, but its idle loop keeps sounding — the one noise
  a screensaver must not have.
* The **master volume** is kept at the game's normal level (`SetMasterVolume(0)`)
  while active and the player's own value is restored on exit. Note the scale:
  **0 is UNITY gain and `-10000` is silence**, so the old
  `SetMasterVolume(0)`-as-a-mute was a no-op that actually raised the game to
  full volume — it never silenced the idle engine, and it is why removing it
  alone loses the music (music and CD audio pass through the master volume).
* Music is otherwise the engine's business (`gMusicVolume` / XM).

---

## 11. RNG

`Random2()` is a pure function of the frame counter, so every call in one frame
returns the SAME value — 24 "random" picks in a frame were 24 identical picks.
The module carries its own LCG (`AntRand`/`AntRandRange`/`AntRandChance`),
seeded once per run from ASLR + `rdtsc` + `Random2(0)`, or pinned by the
engine's debug `-seed` (`AntFarmSetRunSeed` via `AntFarmOnGameStart`) so two
runs are comparable.

---

## 12. Config and pause menu

`JERICHO/CONFIG/antfarm.ini`, read in `AntFarmOnBoot`, written by the menu:

| Key | Meaning | Default |
|---|---|---|
| `enabled` | start the screensaver automatically | **1 (on)** |
| `interval` | seconds per shot (10…300) | 30 |
| `style_<key>` | one per archetype; a row whose model is unimplemented is off | chase off, rest on |
| `roll` | subtle horizon roll | 1 |
| `letterbox` | cinematic bars | 1 |
| `captions` | occasional place-name caption | 1 |
| `debug_hud` | bottom-right shot readout | 0 |
| `lead_mode` | rogue-car events | 0 |

The menu (`AntFarmBuildMenu`) is built at boot from the archetype table, so a
new style appears automatically. **`item_count` must equal the number of filled
rows** — the engine builds submenus eagerly, and a mismatch is an access
violation. `antMenuItems[ANTFARM_STYLE_COUNT + 5]` is sized to fit the fixed
rows plus one per style.

---

## 13. Testing

`tools/antfarm_test.sh [frames] [city] [weather] [time]`:

* boots straight into a take-a-drive with the screensaver force-enabled and a
  shortened interval, muted (`ALSOFT_DRIVERS=null`);
* `STYLE=<key>` isolates one archetype (all others off), which proves that
  archetype's camera code ran;
* watches by **PID** and only kills that PID, and only on a genuine hang; it
  never deletes the session log (it snapshots it);
* the session log is **`JERICHO.log`** (`<appName>.log`), *not* `REDRIVER2.log`
  — a stale `REDRIVER2.log` made an earlier version of this script read the
  wrong file and fail a passing run;
* verdict: PASS needs `[antfarm] ready`, `[antfarm] enabled`, no new `*.dmp`, no
  engine error marker, and either `JERICHO-RUN … status=ok` or `LOG CLOSED`;
* it prints cuts, distinct areas, `black-cap hits` (each used to be ~6 s of
  grey), `void guards`, the shot telemetry, and checks invariants straight out
  of that telemetry (a rig stays inside its envelope; a zoom row must actually
  move its lens; a long-lens row must get its subject into range).

Good runs read: several cuts, several **distinct** areas, `black-cap hits 0`,
`void guards 0`.

---

## 14. Source layout, and the planned split

`antfarm.c` is one file (~3.9 k lines), organised in clearly marked sections. It
can be split along them **without behaviour change** — the state layout and the
hook registration order must stay identical, and `static` must be dropped on the
moved functions:

| New file | Moves out |
|---|---|
| `antfarm_internal.h` | the `ANTFARM_STATE` struct, shared decls, constants |
| `antfarm_rng.c` | `AntRand*` and the run seed |
| `antfarm_roads.c` | the road cache, road/water picking, `AntFarmShotRoad*`, map-height/clamp helpers |
| `antfarm_director.c` | `antStyleDefs`, style/car picking + memory, interest/POI, shot vars, dwell, far-area pick |
| `antfarm_shotplan.c` | `AntFarmPlanShot` / `AntFarmSetupRoadShot` / `AntFarmPickStyleAndTarget` |
| `antfarm_camera.c` | `AntFarmComputeCamera`, `AntFarmFollowCam`, `AntFarmStaticTrack`-style helpers, `AntFarmFindClearCamera`, dolly/junction maths |
| `antfarm_lead.c` | the rogue-car (lead AI) event |

`antfarm.c` then keeps the entry, boot/config, lifecycle, pause menu and hooks.
New sources need a `premake5 vs2019` re-run (module `.c` files are globbed at
generate time).

---

## 15. Traps to remember

* **`regions_unpacked[]` is a barrel table, not a region map** — and only four
  regions are ever resident.
* **The renderer culls from `camera_position`; the spool drives the regions.**
  Point the spool at the camera *before* the void guard, or the hold freezes the
  spool and the camera can never become resident.
* **Texture pages stream per AREA off `camera_position`** — park the camera at
  the destination during a cut, and re-request the area when a travelling shot
  crosses a cell.
* **`SetMasterVolume(0)` is unity, `-10000` is silence.**
* **`MAP_CELL_SIZE` / `MAP_REGION_SIZE` are level-header fields and are zero in
  the frontend** — every region/cell divide must go through the
  `jer_map_ready()` guard.
* **A style's index in `antStyleDefs[]` must match `ANTFARM_STYLE_*`** in
  `antfarm.h`.
* **`item_count` must match the pause-menu rows.**
* **The player's car is hidden by `CONTROL_TYPE_NONE`** (the draw loop skips
  those) and pinned to the shot focus every frame; hand it back before any
  engine-driven camera (pause, cutscene, replay, restart) uses it.
* **Module code is compiled into the exe** — a stale exe ignores your changes.
