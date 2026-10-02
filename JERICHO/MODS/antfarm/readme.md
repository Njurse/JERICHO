# Ant Farm — screensaver / idle mode (JERICHO module)

> **Internals live in [`ARCHITECTURE.md`](ARCHITECTURE.md)** — every hook, the
> cut state machine, the archetype table and each camera model's maths, shot
> planning, region/area streaming and the world-recovery path, audio, config and
> the test harness. This file is the what-and-how-to-use.

A passive city observer for REDRIVER2. Turn it on and the game becomes a
screensaver: player input is cut off, the HUD hides, the player's own car
engine is silenced (the rest of the city stays audible; music stays), and cop
aggression is disabled while a cinematic camera tours
the whole map — every cut hops to a far area and picks a fresh shot:

Road and free angles:

- **Static track** — a fixed roadside camera a car drives past; the camera
  pans to follow, then relocates when the car leaves its reach.
- **Overhead** — scenic elevated 3/4 view down onto traffic (either
  high-following a car or watching a whole road).
- **Tripod** — parked, swaying (sometimes panning) roadside camera, aimed
  down the lane cars actually drive in.
- **Kerb pass** — the same placement at wheel height with a long lens, so
  traffic sweeps past with foreground occlusion.
- **Flyover** — a slow, eased dolly along a long straight, which carries on
  onto the connected road at a junction rather than stopping at the end of it.
- **Crane** — a slow vertical rise from road level, revealing the street ahead.
- **Orbit** — a slow circle around the subject (a car or a point on the road).
- **Ant level** — a ground-level camera beside the road, looking *across* it so
  traffic sweeps past the lens.
- **Waterfront** — a low dolly along a road that actually runs beside water
  (found by probing the surface for water / deep water / sand).
- **Junction** — a parked vantage on a junction mouth: it sits off the corner
  and slides its aim slowly *across* the crossroads, so cars entering and
  leaving the junction cross the frame. Junction shots are planned onto a
  road **end** (where the engine's straights meet), and the plan probes just
  past that end to confirm another surface is there and logs the junction it
  found.
- **Chase** — the **trail cam**: one continuous, damped behind-follow on a
  traffic car. It carries no camera modes (the old build cycled behind /
  overhead / front every few seconds and lerped 12% a frame, which read as a
  snappy camera that kept switching sides). The camera trails the car on its
  heading at a distance and height scaled to the car's own body, follows the
  car's yaw slowly and the short way round, never inherits roll or pitch, and
  aims a little ahead so the car rides low in frame. It opens up a little at
  speed. Off by default: it is the most agitated of the archetypes.

Angles locked onto a car (all of these focus on *one moving car*, and cut away
if it stops - a frozen frame is the one thing a screensaver must not show):

- **Fender** — a rig on the front bumper looking down the road.
- **Sill** — a rig low on the side, looking along the flank.
- **Nose 3/4** — front-quarter leading view.
- **Tail 3/4** — rear-quarter trailing view.
- **Tripod zoom** — a near static vantage beside the car that holds still while
  the car drives through, the camera panning to follow and the lens pushing in
  and easing back out across the shot.
- **Far pan** — the same idea from a distance, with a long lens: the car
  approaches and recedes, the classic long-lens observation.

The four rigs are **damped and yaw-only**: they follow the car's heading with a
slight lag and never inherit body roll or pitch, so the horizon stays level and
the shot stays watchable. Their offsets are scaled to the car's own body
(`ap.carCos->colBox`), so a bus and a sports car both get a camera that sits
just outside the panels, and they deliberately skip the scenery pull-back -
pulling a camera that is inches off the body back for clearance would tear it
off the car.

Every archetype is one row in a table (`antStyleDefs` in `antfarm.c`): how the
camera is driven (`model`), whether it takes a car or a road, its camera-height
/ FOV / orbit / offset ranges, whether it zooms, how fast the camera settles,
and how long it likes to dwell. A director picks weighted and heavily
de-weights any style seen in the last few cuts, so consecutive cuts never
repeat. Shot length is the configured interval scaled by scene interest and then
clamped to **15–45 s** and trimmed by a pace factor (shots run ~15% shorter
than the interest score alone would give, so the tour changes them a little
more often) - with the default 30 s interval a tour that cuts every few
seconds is a slideshow, and one that dwells for minutes stops being something
you can leave on in the background. The transition is a 0.7 s dissolve that
darkens through black rather than flashing through white, so it is easy on the
eye in a dark room — and the **very first shot of an activation fades in over
0.5 s the moment its assets have streamed**, instead of waiting out the usual
black beat, so turning it on never opens on a few seconds of black. (The log
says so: `[antfarm] first shot: fading in over 500ms`.)

Shot *areas* are picked by interest rather than at random: a sample of
candidate roads is scored on water beside the road, sheer length, and traffic
actually moving there, with a penalty for staying in the region it is already
in so the tour keeps moving. Car subjects are likewise biased away from
recently-framed cars, and the rig / long-lens angles demand a car that is
genuinely underway.

Each shot runs a scenery pass (line-of-sight pull-back + camera-collider
push-out) so buildings never obscure the view, and the lens *breathes* between
shots instead of snapping. There is a soft letterbox and an occasional
place-name caption (both optional, see Settings below).

A note on *named* landmarks: the engine has no landmark or POI table - overlays
are HUD data, and there is no junction surface id - so shots target features
(water, long straights, live traffic) rather than "the Loop". There is a
deliberately inert scaffold for the real thing in `antfarm.c`
(`antPois` / `AntFarmPoiNear`): fill in a city's landmark coordinates and set
`valid = 1` and the interest scorer starts biasing shots toward it.

**Rogue cars** (optional, off by default): a tiny chance per cut that the
car of interest turns rogue — it becomes LEAD AI and tears across the map,
cops give chase, and the camera follows it until it is totaled.

## Install

Drop this folder into `JERICHO/MODS/` (already done if you are reading this
from the repo), re-run premake from `src_rebuild/`, and rebuild:

```
premake5.exe vs2019
```

Then enable the module (Options -> JERICHO -> antfarm, or edit
`JERICHO/CONFIG/modlist.ini` — `antfarm = 1`). No game files are modified.
Ant Farm is compiled *into* the exe (it is not a runtime DLL), so code changes
need a rebuild of the game, not just a file copy.

With `enabled = 1` in its config the mode turns itself on as soon as a
playable single-player session is running — no keypress needed.

## Usage

- **F9** (keyboard, PC) — toggle the screensaver during gameplay.
- **Pause -> Modules -> Ant Farm** — toggle, plus a row per camera archetype
  and an adjuster for the cut interval.
- **While it is ON it is enforced**: a pause, a cutscene, a replay or a level
  restart only *suspends* it (the game is handed back for as long as the
  engine needs it) and it re-engages by itself. Nothing switches it off
  silently — only F9 / the menu row do.
- **START / opening the pause menu** — *suspends* the screensaver: the game
  (and the player's car) is handed back so the pause menu and its camera
  behave normally, and the mode re-engages on its own when the pause closes.
  It also survives a **level restart from the pause menu** — the mode hands
  back, drops the old level's references and starts fresh on the new level,
  rather than shooting at the previous level's roads. To turn it OFF for good,
  use **F9** or the **Ant Farm** row in the pause menu.
- Settings persist to `JERICHO/CONFIG/antfarm.ini`
  (`interval`, `style_<key>` for each archetype, `roll`, `letterbox`,
  `captions`, `debug_hud`, `lead_mode`, `enabled`). A missing `enabled` defaults to **on** — the mode turns itself
  on at the start of a game, and toggling it off (F9 / the pause menu) is what
  persists a `0`.

### Debugging a shot

Set `debug_hud = 1` in `antfarm.ini` for a small grey readout in the
**bottom-right** corner (the place the caption lives, opposite side) showing
what the camera is doing this frame:

```
antfarm: <style key> <model> <car|road> <show|fade-out|cut|fade-in>
```

`style` is the archetype key (`junction`, `flyover`, `tripzoom`…), `model` is
how the camera is driven (`roadside`, `dolly`, `attach`, `track`, `tripodz`,
`junction`, …). It takes the guessing out of "what am I looking at" when a
shot misbehaves. `debug_hud` is off by default and is not written to the config
by the menu, so it only appears if you put it there.

## How it works (module-only, no game edits)

| System | Mechanism |
|---|---|
| Input cutoff | `gStopPadReads = 1` (player car brakes and holds; on-foot pads zeroed via `JER_EVENT_PED_INPUT`) |
| HUD hidden | `gDoOverlays = 0` |
| Sound | keeps the master volume at the level the game itself treats as normal (music and CD audio pass through it, so music stays); the player's own car engine is silenced via `JER_EVENT_CAR_ENGINE_SOUND` (forces its idle + rev channel volumes to `-10000`). (Note: `SetMasterVolume(0)` is *unity*, not mute — silence is `-10000`, which is why the old master-volume-only attempt never silenced the idle engine.) |
| Camera | `JER_EVENT_CAMERA` — writes `camera_position`/`camera_angle`, sets `override = 1` so the engine rebuilds the view from our values |
| On-road framing | `GetSurfaceRoadInfo` + `GetNodePos` (lane + distance) + `ROAD_LANE_DIR` for the traffic heading — same helpers the civ AI drives on |
| Scenery clearance | `lineClear` LOS pull-back + `CheckScenaryCollisions` camera-collider push-out (world-space Y conversion, `camera.c:604` pattern) |
| Region streaming | the spool (`MainPlayer.spoolXZ`) is redirected to a module-owned `VECTOR`, re-asserted every frame in the camera hook (after `UpdatePlayers()` resets it), and pointed at the *camera* once a shot is live |
| Far hops | the engine only pre-loads *neighbouring* regions as you move, so a hop calls `UnpackRegion()` to force the destination region in, and the cut waits (black) for it before fading in |
| Fade | semi-transparent fullscreen wash drawn in `JER_EVENT_DRAW_OVERLAY` (same look as the stock `FadeGameScreen`), plus optional letterbox bars and the caption |
| FOV | per-shot `SetGeomScreen(scr_z = …)`, interpolated so the lens breathes |
| Timing / state machine | `JER_EVENT_FRAME` (wall-clock, SDL_GetTicks) |

## Caveats

- Aimed at **single-player free drive** (take-a-drive / survival). It refuses
  to engage during cutscenes, replays or multi-player, and auto-exits if a
  cutscene or replay starts while it is running.
- The player's car is **pinned to the camera focus** while the mode runs:
  teleported there each frame (so region streaming/streaming follows the
  action), hidden (`CONTROL_TYPE_NONE` + slot reserved so traffic can't reuse
  it), and its engine silenced. On exit it's restored to where you left it, along with the
  pad/overlay/cop/projection (`scr_z`) and `CameraCar` state.
- The engine keeps only **four** regions resident (a 2×2 barrel window,
  `regions_unpacked[4]`), and only ever pre-loads neighbours as you drive. A
  far hop therefore has to force its destination in and wait for it — that
  wait is the black between cuts. Without it the camera is shown over
  unloaded geometry (skybox/nodraw), which is what made earlier versions look
  broken.
- While a shot is live the spool follows the camera (the renderer culls from
  `camera_position`), so the rendered region is the resident one. A guard
  holds the previous camera if a shot would ever move into a region that is
  not resident yet.
- The fade uses the game's stock semi-transparent wash look rather than a
  true black — matching how the original game does its own screen fades.
- **Void / uncrolled scenery.** The camera is held whenever its region is not
  resident (the void guard), and while a shot is live the module also asks the
  engine for the *area* data of any new cell the camera enters (dolly/crane
  shots cross cells faster than the pager settles, which is the "scenery or
  palette did not load" look). `debug_hud` prints the current style if you need
  to pin which shot it was.
- Junction shots require a real crossroads: the plan probes past the road end
  and, if there is no neighbouring surface there (a boundary road end that
  opens onto nothing), it tries the other end and then other roads. A vantage
  on an open road end at the map edge is what looked off the edge of the world.
- Far areas are biased away from the world edge (`AntFarmNearWorldEdge`), so a
  shot - and the pinned player car - stay inside the map.
- Camera/pad compatibility: while the screensaver is on, Ant Farm's camera
  and pad hooks run at a higher JERICHO priority than d2pl's, so the
  screensaver takes precedence; it hands control back as soon as it is off.

## Source layout (next unit)

`antfarm.c` is still one file (~3.9k lines). It is deliberately organised in
sections and is safe to split along them **without behaviour change** (the
state layout and hook registration order must stay identical; only `static`
has to be dropped on the moved functions):

| New file | Moves out of `antfarm.c` |
|---|---|
| `antfarm_internal.h` | the `ANTFARM_STATE` struct, shared decls, constants |
| `antfarm_rng.c` | `AntRand*` / the run seed |
| `antfarm_roads.c` | the road cache, road/water picking, `AntFarmShotRoad*`, map-height/clamp helpers |
| `antfarm_director.c` | `antStyleDefs`, style/car picking + memory, interest/POI, shot vars, dwell, far-area pick |
| `antfarm_shotplan.c` | `AntFarmPlanShot` / `AntFarmSetupRoadShot` / `AntFarmPickStyleAndTarget` |
| `antfarm_camera.c` | `AntFarmComputeCamera`, car modes, `AntFarmStaticTrack`, `AntFarmFindClearCamera`, dolly/junction maths |
| `antfarm_lead.c` | the rogue-car (lead AI) event |

`antfarm.c` then keeps the module entry, boot/config, lifecycle, pause menu and
the hooks. New sources are picked up by re-running `premake5 vs2019` (the
module's `.c` files are globbed at generate time).

## Tools

`tools/antfarm_test.sh` runs one unattended, muted verification pass:

```
bash tools/antfarm_test.sh [frames] [city] [weather] [time]
STYLE=orbit bash tools/antfarm_test.sh 2400 chicago none day   # isolate one archetype
```

It boots straight into a take-a-drive with the screensaver enabled and its cut
interval shortened, runs with `ALSOFT_DRIVERS=null` (silent), watches by PID so
a genuine hang can be killed without touching any other session, snapshots
`JERICHO.log` (the engine's session log, `<appName>.log`), and prints a verdict
plus shots fired, distinct areas, black-cap hits, void-guard holds and the
**shot telemetry**. City, weather and time default to *random* on every run, so
repeated passes cover the maps instead of always testing one; pass them
explicitly (or `STYLE=<key>`) to pin a case.

**Strict test condition — antfarm is the only module.** The script rewrites the
runtime `JERICHO/CONFIG/modlist.ini` for the run (every module set to `0`,
`antfarm = 1`) and restores the user's own copy on exit, so a run can never be
contaminated by another module. It then asserts the condition straight out of
the engine's own `[jericho] --- module inventory ---`: exactly one module may be
`enabled=1`, and it has to be antfarm (the verdict line reads
`modules active: N (must be 1: antfarm)`).

The shot telemetry is what makes behaviour checkable without eyes: every shot
logs `model=… subject=… dist MIN..MAX fov MIN..MAX`, and the harness reads
invariants straight out of it - a rig must keep a bounded distance from its car
(`max subject distance` under 2500), and a zoom row must actually move its lens
(`largest lens span` at least 15). Both are reported and asserted per style.

House rules it follows: never kill by image name, never delete the shared log,
and remember that the ~40 s clean exit of a single-player `-level` run prints no
`JERICHO-RUN` line. Note it launches the game the ordinary way, so a game window
does appear while a run is in progress.

## Known issue (not this module)

The full-mods Debug build currently fails to **link** because the sandbox
module references `gUseRotatedMap`, which is `static` in `overmap.c` while
`overmap.h` still declares it `extern` — a pre-existing tree issue. Build
with `premake5.exe --with-mods="antfarm" vs2019` (or exclude sandbox) to
link cleanly.
