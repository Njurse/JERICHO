# Ant Farm — screensaver / idle mode (JERICHO module)

A passive city observer for REDRIVER2. Turn it on and the game becomes a
screensaver: player input is cut off, the HUD hides, all SFX are muted
(music stays), and cop aggression is disabled while a cinematic camera tours
the whole map — every cut hops to a far area and picks a fresh shot:

- **Static track** — a fixed roadside camera a car drives past; the camera
  pans to follow, then relocates when the car leaves its reach.
- **Overhead** — scenic elevated 3/4 view down onto traffic (either
  high-following a car or watching a whole road).
- **Tripod** — parked, swaying (sometimes panning) roadside camera, aimed
  down the lane cars actually drive in.
- **Flyover** — a slow, eased dolly along a long straight.
- **Orbit** — a slow circle around the subject (a car or a point on the road).
- **Crane** — a slow vertical rise from road level, revealing the street ahead.
- **Ant level** — a ground-level camera beside the road, looking *across* it so
  traffic sweeps past the lens.
- **Chase** — behind-follow on a traffic car, framed to the vehicle's size.
  Off by default: it is the most agitated of the archetypes.

Every archetype is one row in a table (`antStyleDefs` in `antfarm.c`): weight,
whether it takes a car or a road, its camera-height / FOV / orbit ranges, how
fast the camera settles, and how long it likes to dwell. A director picks
weighted and heavily de-weights any style seen in the last few cuts, so
consecutive cuts never repeat. Shot length is the configured interval scaled
by scene interest, so long vistas hold longer and transient traffic does not
linger. Cuts last 10–300 s (default 45) and the transition is a 1.4 s wash.

Each shot runs a scenery pass (line-of-sight pull-back + camera-collider
push-out) so buildings never obscure the view, and the lens *breathes* between
shots instead of snapping. There is a soft letterbox and an occasional
place-name caption (both optional, see Settings below).

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
  and sliders for cut interval / car-mode interval / modes-per-cut.
- **START** during the screensaver — turns it off and opens the normal
  pause menu.
- Settings persist to `JERICHO/CONFIG/antfarm.ini`
  (`interval`, `mode_interval`, `modes_per_cut`, `style_<key>` for each
  archetype, `roll`, `letterbox`, `captions`, `lead_mode`, `enabled`).

## How it works (module-only, no game edits)

| System | Mechanism |
|---|---|
| Input cutoff | `gStopPadReads = 1` (player car brakes and holds; on-foot pads zeroed via `JER_EVENT_PED_INPUT`) |
| HUD hidden | `gDoOverlays = 0` |
| Sound | `SetMasterVolume(0)` on entry, restored on exit (music is a separate volume and stays) |
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
  it), and muted. On exit it's restored to where you left it, along with the
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
- Camera/pad compatibility: while the screensaver is on, Ant Farm's camera
  and pad hooks run at a higher JERICHO priority than d2pl's, so the
  screensaver takes precedence; it hands control back as soon as it is off.

## Tools

`tools/antfarm_test.sh` runs one unattended, muted verification pass:

```
bash tools/antfarm_test.sh [frames] [city] [weather] [time]
STYLE=orbit bash tools/antfarm_test.sh 2400 chicago none day   # isolate one archetype
```

It boots straight into a take-a-drive with the screensaver enabled and its cut
interval shortened, runs with `ALSOFT_DRIVERS=null` (silent), watches by PID so
a genuine hang can be killed without touching any other session, snapshots
`REDRIVER2.log`, and prints a verdict plus shots fired, distinct areas, black-cap
hits and void-guard holds. House rules it follows: never kill by image name,
never delete the shared log, and remember that the ~40 s clean exit of a
single-player `-level` run prints no `JERICHO-RUN` line.

## Known issue (not this module)

The full-mods Debug build currently fails to **link** because the sandbox
module references `gUseRotatedMap`, which is `static` in `overmap.c` while
`overmap.h` still declares it `extern` — a pre-existing tree issue. Build
with `premake5.exe --with-mods="antfarm" vs2019` (or exclude sandbox) to
link cleanly.
