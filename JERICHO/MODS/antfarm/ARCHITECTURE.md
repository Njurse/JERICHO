# Ant Farm — how it works (internals)

This is the deep companion to `readme.md`. The readme says *what* Ant Farm is
and how to use it; this says **how every part of it works, why it is built that
way, and what will bite you if you change it.** Written against the current
`antfarm.c` — symbol names are the anchors.

Ant Farm is a JERICHO module compiled *into* the exe (not a runtime DLL), so
every code change needs a rebuild: `src_rebuild/build_dev.bat`, i.e.

```
premake5 vs2019          # only needed when files are ADDED
msbuild build/JERICHO.vcxproj /p:Configuration=Release_dev /p:Platform=x64
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
the moment its region *block* is resident and the shot is planned — nothing
else — and
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

### Region residency — the engine facts that shaped this

* `regions_unpacked[4]` / `loading_region[4]` are indexed by **barrel slot**,
  not by region number. Treating them as a per-region map reads out of bounds
  and never fails. → use `jer_map_region_resident()` (`JER_EVENT_*`-shared, see
  `JERICHO/docs/map-streaming.md`).
* The engine keeps only **four** regions resident (a 2×2 window), so a
  "centre + 4 neighbours" residency test can never pass. The achievable test is
  "this region has data (`spoolinfo_offsets[r] != 0xffff`) and it is the region
  currently unpacked into its barrel slot".

### The region block — pre-seeding the world around a hop

The four barrels are the *whole* window: the engine fills the three neighbour
slots itself as you drive (only when you come within `force_load_boundary` of a
region edge, `map.c CheckUnpackNewRegions`). A far hop forces in **only** the
region under the destination (`jer_map_spool_to`), so the other three barrels
still hold the *previous* shot's regions — the new area opens as one island of
loaded geometry, `block N/M` short of what it should be. That is the “empty
world when it loads into a new region” look.

`AntFarmBlockAround(pos, out[4])` computes the four regions a camera at `pos`
needs: the region under it, its x-neighbour, its z-neighbour and the matching
diagonal, each on the side the point sits nearest (the boundary rule mirrors the
engine’s, 18 cells / 13 with a lead car, so antfarm and the engine never pick
opposite neighbours and thrash a region load every frame). Every one of the four
lands in its own parity barrel — `(rx & 1) + (rz & 1) * 2` — so forcing one
never evicts another the block needs.

`AntFarmStreamBlock(block, n)` forces each region in by the engine’s own method
(`ClearRegion` + `UnpackRegion` + a synchronous spool flush), and the CUT now
waits for `blockReady >= blockUsable` — all of the block’s regions that *can*
load (an off-map or data-less neighbour is excluded from the count) — before it
reveals. The cut log carries it: `cut #N -> area x,z region R block 4/4 style S
… (frame F)`. `AntFarmRecoverToRoad` streams the block around its new anchor
too, and the CUT re-streams it around the **final** anchor after a car subject
(`AntFarmSetCarSubject` moves `s.spool` onto the car, whose anywhere-fallback
pick is not bounded to the area).

**`doSpooling == 0` levels (an mp arena) never stream on their own.**
`ControlMap` is only called when `doSpooling` is set, and `mission.c` sets
`doSpooling = (MissionHeader->region == 0)` — false for an mp level
(`-mp`), where only the four `multiplayerregions` are loaded at level start and
nothing else ever is. On such a level `current_region` also stays `-1`, so
`CheckLoadAreaData` (the engine’s area/model loader) indexes
`spoolinfo_offsets[current_region]` out of bounds and the shot’s texture area and
scenery models never load. `AntFarmSeedCurrentRegion()` therefore points
`current_region` at the region the shot is about to use (a no-op on a normal
level, where ControlMap sets it every frame), and the region block is what
actually makes the world stream there. (This is why the *first* mp shot used to sit in a void:
it is the one shot that skips the long black hold.)

### The void guard (render time, `AntFarmOnCamera`)

The renderer culls from `camera_position`, so the **camera's own region** is
what must be resident. If it is not, the module holds the last camera that
*was* over a resident region — `s.holdPos`/`s.holdAngle`, not `s.camPos`, which
is snapped to the new shot on its first frame — and logs
`void guard: region not resident, holding camera (#n)`. (Holding `s.camPos` held
the very void the guard exists to hide: the first frame of a shot after
unpausing, or in an mp level, was a blank skybox.) A 4 s cap means a data-less
region can never freeze the view forever; `s.holdPos` is seeded from the
pre-takeover camera at activation and from the CUT's parked camera while black,
so it is always a loaded spot. The spool is pointed at the camera *before* this
guard, or the hold would freeze the spool and the camera could never become
resident (a deadlock).

### The texture-area guard (frame time)

A travelling shot (dolly, flyover, crane) crosses map cells faster than the
pager settles — the "scenery / palette did not load" look. When the camera
enters a new cell during a live shot, `AntFarmOnFrame` calls
`CheckLoadAreaData(cellX, cellZ)` + `StartSpooling`/`UpdateSpool`, exactly as the
CUT does for a destination. Tracked by `s.lastCellX/lastCellZ`. It only runs
while `current_region` is valid — on a `doSpooling == 0` level that loader
indexes `spoolinfo_offsets[current_region]`, and `-1` would read out of bounds.

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

It fills the whole **region block**, not one point: the budget is split across
the block's usable regions (`ceil(nMax / usable)`) and `MainPlayer.spoolXZ` is
pointed at each region's centre in turn, so the traffic the camera can actually
see is spread across the area rather than piled into one. It restores the spool
after. The player's car is never a candidate: it is pinned with
`reservedSlots[] = 1` and `CONTROL_TYPE_NONE`, which the spawner's free-slot
search skips.

**Stranded slots.** The engine evicts a civilian once it is more than 24000
units from the spool, but only a few per frame — so right after a hop the slots
are still full of the *previous* area's cars and `PingInCivCar` refuses every
ping (the old “0 civ car(s)” seed). The pre-seed does that eviction at once,
measured against the new focus, before it pings.

**No invisible cars.** `InitCar` places every pinger with `MapHeight()`, which
answers 0 for a cell that is not resident — the car lands in the void and is
never drawn. So the whole block is streamed in first, and a region that is still
not resident is skipped (loudly: `pre-seed: region N not resident (cars would
spawn invisible)`) rather than dropping cars into nothing. Each seed logs its
count and coverage:
`[antfarm] pre-seed: N civ car(s) across S/U region(s) around (x,z)`.

### Known limits / further work

The block is streamed **at the cut** — once, under the black. Two cases are
therefore still only as good as the engine's own streaming, and on a
`doSpooling == 0` (mp) level that is nothing:

1. **A travelling shot crossing a region boundary** (dolly / flyover / crane,
   or a long chase). The camera moves into a region that was not in the cut's
   block. On a normal level `ControlMap` streams it within a frame; on an mp
   level it does not, so the far half of the travel can show unloaded geometry
   until the next cut. The texture-area guard covers the *textures* of a cell
   change, not the *region*. A fix would re-run `AntFarmStreamBlock` for the
   camera's region when the camera crosses into a new one mid-shot (the same
   `s.lastCellX/lastCellZ` hook the area guard already uses).
2. **A subject that drives out of the block.** The car-subject block is forced
   at the final anchor, but a chase then follows the car across a boundary; the
   void guard only checks the *camera's* region, so an unloaded subject region
   silently stops drawing the car rather than showing a void. A matching
   subject-region check (or a re-block when the subject's region changes) would
   close it.
3. **`block N/M` below `4/4` is not always a bug.** On an mp arena only some of
   the block's regions have spool data (measured 2/2 at rio `-mp 0`), and the
   cut ignores the unusable ones rather than waiting for a region that can never
   load. A `M < 4` is the map, not a failure.

Also worth knowing when reading logs: the cut line's `region R` is the region
of the shot's *area* (computed from `s.areaPos`); it used to be
`current_region`, which is `-1` on an mp level.

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
| `scrZLo/Hi` | FOV (projection distance; smaller = wider). For a zoom row these are the ramp's two ends. Both clamp to `ANTFARM_FOV_MIN` 256 … `ANTFARM_FOV_MAX` 360 |
| `sideLo/Hi` | lateral offset (rig offset, the roadside margin, or a follow row's rear-3/4 offset) |
| `fwdLo/Hi` | longitudinal offset: attached rigs' along-the-body position; a **follow** row's trail distance (and the switch that makes the row table-driven); a dolly/crane road row's extra lead |
| `aimLo/Hi` | how far ahead of the subject the aim sits |
| `orbitLo/Hi` | pan/sweep amplitude (0 = hold still) |
| `across` | aim across the road rather than along it |
| `settle` | settle divisor (bigger = calmer) |
| `dwell` | scene-interest dwell multiplier, ×100 |
| `nearWater` | prefer a road that runs beside water |

### Models (`ANT_MODEL_*`) and their maths

| Model | Drives | Used by |
|---|---|---|
| `ROADSIDE` | camera parked `side` off the lane, `height` above the road, aiming `shotLookAhead` along (or across, if `across`) the lane; optional slow pan (`shotOrbitAmp`), tiny sway/bob. On a CAR subject it holds `side + margin` off the car's flank and travels with it (`AntFarmTrackCarCam`) | Overhead, Tripod, Ant level, Kerb |
| `DOLLY` | travels along the road, carrying on onto the connected road at a junction (`AntFarmAdvanceDolly` + `AntFarmNextRoad`) — and that continuation may be a **curve**. On a CAR subject it runs `ANTFARM_TRACK_LEAD + fwd` AHEAD of the car and aims back at it (a leading shot) | Flyover, Waterfront |
| `CRANE` | a dolly that also rises from `ANTFARM_CRANE_LOW` to `shotHeight` across the shot; on a CAR subject it leads like the dolly | Crane |
| `ORBIT` | slow circle around the subject, one revolution per `ANTFARM_ORBIT_PERIOD_MS` | Orbit |
| `FOLLOW` | the **trail cam** (see below); four rows, the extra three table-driven | Chase, High chase, Low chase, Side track |
| `ATTACH` | a damped, yaw-only rig on the car's own body (scaled by `colBox`), offsets in the car's frame | Fender, Sill, Nose 3/4, Tail 3/4 |
| `TRIPODZ` | a fixed vantage resolved from the car once per shot; the camera yaw-tracks while the lens pushes | Tripod zoom, Far pan |
| `JUNCTION` | parked off a crossroads corner; the **aim** slides slowly across the mouth | Junction |
| `TRACK` | **retired** — see §6 | Static track |

`AntFarmStyleImplemented(model)` is the gate: a model that is not implemented is
never offered by the picker and never defaults on. That is how a row is retired
without renumbering the enum (indices must stay aligned with
`ANTFARM_STYLE_*` in `antfarm.h` and with `antStyleDefs[]`).

A `ROADSIDE`/`JUNCTION`/`DOLLY`/`CRANE` row that takes a **car** subject does not
fall through to the trail cam any more: `AntFarmTrackCarCam` holds a fixed offset
in the car's own frame — `side + margin` off its flank for ROADSIDE/JUNCTION, so
the camera travels alongside it; `ANTFARM_TRACK_LEAD + fwd` ahead of it for
DOLLY/CRANE, so the car drives toward the lens — and aims at the car. That is
what makes Flyover/Crane travelling car shots and Kerb/Tripod/Ant level/Overhead
side-tracking car shots.

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

### Table-driven follow rows (High chase, Low chase, Side track)

The legacy numbers above are the default when a row leaves its forward range at
`0` (Chase cam does, so its shot is unchanged). A follow row that **sets**
`fwdLo/Hi` drives the whole framing itself: `heightLo/Hi` → camera height,
`fwdLo/Hi` → trail distance, `sideLo/Hi` (× `shotSideSign`) → a lateral rear-3/4
offset along the car's right, `aimLo/Hi` → the look-ahead. That is how the three
extra chase angles exist with no new model and no new struct field — only the
legacy path is kept so the original chase framing is bit-for-bit the same.

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
* **Curves.** A driveable surface is a STRAIGHT or a CURVE, and a curve's
  surface id carries the `0x4000` bit (`IS_CURVED_SURFACE`/`GET_CURVE`) and has
  `start`/`end` angles and an `inside` radius but NO `length` or `angle` field.
  `AntFarmRoadLen`/`AntFarmRoadHeading` give either one, a curve's length being
  the engine's own arc-length form `((end-start) & 0xfff) * inside * 11 / 7`
  (civ_ai.c). The probes and the dolly use them, so a straight meeting or
  continuing onto a curve is found (before the fix both neighbour probes tested
  `surf < NumDriver2Straights`, which threw every curved meeting away) and the
  dolly does not index `Driver2StraightsPtr` with a curve id.
* **Roadside shots** sometimes frame a road **end** (`AntRandChance(40)`), where
  junctions cluster; 45 % of those sit just past the end looking back.
* **Travelling shots** start at distance 0 and advance every frame.
* **Junction shots** always sit on a road **end**, but only one that is a *real*
  crossroads: `AntFarmJunctionEnd` steps 320 units past the end along the lane
  heading and asks `GetSurfaceIndex` which surface is there. A different usable
  surface (straight **or curve**) means the roads meet; no road (a boundary end)
  means the vantage would look off the edge of the world, so the plan tries the
  other end and then other roads (10 tries), and says so (and which kind it
  found) if it finds nothing.
* **Dolly handoff.** `AntFarmAdvanceDolly` advances by the shot's share of the
  road; when it runs out, `AntFarmNextRoad` continues onto the connected road —
  `ConnectIdx` first, and since these level files leave it unpopulated, by
  stepping just past the end and asking `GetSurfaceIndex` (exactly how the civ
  AI tracks its current road); the continuation may be a **curve** (the log says
  which). Up to 8 handoffs.

`AntFarmInitShotVars` randomises the framing from the row's ranges, sets the FOV
(one end of the ramp for a zoom row), decides whether the row pans
(`AntRandChance(72)`), and clamps the lens to `ANTFARM_FOV_MIN` 256 …
`ANTFARM_FOV_MAX` 360. The floor is the engine's own widest lens
(`gCameraDefaultScrZ`), so no shot is ever wider than ordinary gameplay - a
deeper floor (200) read as a fisheye when a shot's lens expanded. It also resets
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
  * **road-following tilt, height-scaled.** The four road models (roadside,
    dolly, crane, junction) aim `AntFarmRoadAimLift()` units above the road —
    `max(ANTFARM_ROAD_AIM_LIFT, shotHeight × ANTFARM_ROAD_AIM_PCT/100)`, i.e. 60
    for a low camera and half the eye height for an elevated one (the crane
    passes its *current* height, since it rises). A flat 60 was invisible at
    height 400+ (≈3° over a 1000-unit aim), which is why those vantages aimed
    at tarmac. Their final pitch also gets a small upward bias
    (`ANTFARM_ROAD_PITCH_UP`, 20 ≈ 1.8°) — but that bias lands in
    `AntFarmOnCamera`, on the angle the slew actually reaches: the same
    correction in `AntFarmComputeCamera` writes a local the slew overwrites, so
    it never reached the screen. Attached rigs, the trail cam, orbit and
    tripod-zoom keep their own framing;
  * **Never stare at the ground — or the sky.** Whatever the model, the rendered
  pitch is clamped 
    * down to `ANTFARM_PITCH_DOWN_MAX` (220 ≈ 19°), and
    * up to `-ANTFARM_PITCH_UP_MAX` (340 ≈ 30°).
  `camera_angle.vx` is 0 at level, grows downward and wraps at 4096, so the
  clamp reads the signed value and rewrites `tgt.vx`. The down cap stops an
  elevated vantage aiming at tarmac; the up cap catches a shot whose aim sits on
  ground far above the camera (a junction on a steep grade was measured at
  −1039, ≈ 91° up). An archetype or row value can no longer defeat either.
    archetype or row value can no longer leave a shot pointing at the road
    surface — the shot looks across the scene instead;
  * breathe the lens: `s.fovCurrent += (AntFarmFovTarget() − s.fovCurrent)/18`,
    then clamped to `ANTFARM_FOV_MIN`…`ANTFARM_FOV_MAX` and passed to
    `SetGeomScreen(scr_z = s.fovCurrent)`. The clamp matters: `fovCurrent` is
    seeded from the *gameplay* lens (`scr_z`) at activation and
    `AntFarmFovTarget` returns 0 until the first shot is planned, so without it
    the first frames swept the lens far outside the range (measured down to
    **17** — a fisheye). `AntFarmFovTarget` returns the shot's lens, or, for a
    zoom row, a half-sine push to the other end and back — now run over the
    **first third** of the shot (`ANTFARM_ZOOM_MIN_MS` floor), because spreading
    it over the whole dwell meant a shot cut early never moved its lens at all;
  * `CameraCar = s.targetCarId` for a FOLLOW shot on a car.

A degenerate aim == camera pair is pushed 500 units along the current view
direction, or `PointAtTarget` has no direction and the view whips.

Per-shot telemetry is accumulated (`shotDistMin/Max`, `fovMin/Max`,
`pitchMin/Max`) and logged at `FADE_OUT`:

```
[antfarm] shot #3 model=junction subject=road dist 808..808 fov 253..321 pitch 120..260
```

`pitch` is the rendered camera tilt (signed: negative = up, 0 = level, positive
= down), which is what makes the road-camera upward tweak checkable from the
log rather than by eye. The line as a whole is what makes shots checkable
without eyes.

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

There are **20** style rows (17 original + High chase, Low chase, Side track);
`ANTFARM_STYLE_COUNT` in `antfarm.h`, the enum, the table, the config keys and
the harness's style-key list must all stay in step. Chase is the one archetype
that defaults **off** (`def = implemented && i != ANTFARM_STYLE_CHASE`), because
it is the most agitated; the three extra chase rows default on.

---

## 13. Testing

`tools/antfarm_test.sh [frames] [city] [weather] [time]`:

* boots straight into a take-a-drive with the screensaver force-enabled and a
  shortened interval, muted (`ALSOFT_DRIVERS=null`);
* `STYLE=<key>` isolates one archetype (all others off), which proves that
  archetype's camera code ran;
* watches by **PID** and only kills that PID, and only on a genuine hang; it
  never deletes the session log (it snapshots it);
* the session log is **`JERICHO.log`** (`<appName>.log`), *not* `JERICHO.log`
  — a stale `JERICHO.log` made an earlier version of this script read the
  wrong file and fail a passing run;
* verdict: PASS needs `[antfarm] ready`, `[antfarm] enabled`, no new `*.dmp`, no
  engine error marker, and either `JERICHO-RUN … status=ok` or `LOG CLOSED`;
* it prints cuts, distinct areas, `black-cap hits` (each used to be ~6 s of
  grey), `void guards`, the shot telemetry, the vehicle-vs-road subject mix
  (`subjects: car=N road=M`), and checks invariants straight out of that
  telemetry (a rig stays inside its envelope; a zoom row must actually move its
  lens; a long-lens row must get its subject into range; the rendered pitch must
  stay inside `ANTFARM_PITCH_DOWN_MAX`/`ANTFARM_PITCH_UP_MAX`).

Good runs read: several cuts, several **distinct** areas, `black-cap hits 0`,
`void guards 0`. (A single void guard at cut #1 turns up in a minority of runs
and has done since long before the vehicle/curve work — it is the first shot's
region still streaming, and the guard recovers immediately.)

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
  regions are ever resident. A far hop must force the whole **block** around the
  destination (`AntFarmBlockAround` / `AntFarmStreamBlock`), not just its own
  region, or the shot opens as one island of geometry.
* **On a `doSpooling == 0` level (an mp arena) the engine never streams at all**
  — only the four `multiplayerregions` load at level start, and `current_region`
  stays `-1` so `CheckLoadAreaData` indexes `spoolinfo_offsets[-1]`. antfarm has
  to force the regions itself and seed `current_region`.
* **The renderer culls from `camera_position`; the spool drives the regions.**
  Point the spool at the camera *before* the void guard, or the hold freezes the
  spool and the camera can never become resident.
* **Hold `s.holdPos`, not `s.camPos`, in the void guard** — `camPos` is snapped
  to the new shot on its first frame, so holding it holds the void.
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
* **`AntFarmComputeCamera`'s `*outAngle` is discarded.** `AntFarmOnCamera`
  recomputes `PointAtTarget` from the clamped camera and the aim, then slews
  `s.camAngle` toward *that*. Any pitch tweak written into the local `ang` in
  `AntFarmComputeCamera` is silently overwritten — that is why the old
  `ANTFARM_ROAD_PITCH_UP` "tilt up" fix never showed. Put framing corrections
  on `tgt` in `AntFarmOnCamera`.
* **A curve's surface id is not a straight index.** A curved surface carries the
  `0x4000` bit (`IS_CURVED_SURFACE`/`GET_CURVE`) and has no `length`/`angle`
  field. Indexing `Driver2StraightsPtr` with one reads the wrong road (or out of
  bounds). When a surfId could be either, go through `AntFarmRoadLen` /
  `AntFarmRoadHeading`.
* **Module code is compiled into the exe** — a stale exe ignores your changes.
* **`PingInCivCar` no longer spams the log.** The `JERICHO-DIAG PINGIN:` census
  lines fire on every ping, so they are gated behind `JERICHO_DIAG_PINGIN=1`
  (same opt-in shape as `JERICHO_DUMPVRAM`). Pre-seeding makes many pings, so a
  normal run has none of them.
