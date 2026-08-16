# Ant Farm — screensaver / idle mode (JERICHO module)

A passive city observer for REDRIVER2. Turn it on and the game becomes a
screensaver: player input is cut off, the HUD hides, and a cinematic camera
cycles between three shot types while the city keeps living around it.

- **Junction / Tripod** — a parked, gently swaying (and sometimes slowly
  orbiting) elevated camera watches traffic flow. Shots anchor to the road
  network's actual **lanes** (`GetNodePos` + `ROAD_LANE_DIR`), often beside
  an intersection, and aim down the lane cars actually drive in.
- **Car Follow** — the camera chases a random moving civilian car
  (chase-cam), and audio follows it too. If the car despawns or is wrecked
  mid-shot, the shot cuts early.
- **Boulevard Flyover** — a slow, eased, high dolly along a long straight,
  glued to a lane and looking ahead along the road.

Every cut fades out, switches target **during the black**, and fades back
in — the region spool is redirected to the new focus inside the black frame,
so map geometry and traffic stream in without pop-in.

**Clear shots:** before rendering, the camera runs a scenery pass — a
`lineClear` line-of-sight check pulls the camera in when a building blocks
the view, and the engine's camera collider (`CheckScenaryCollisions`) pushes
it out of anything it would sit inside. Each shot also gets its own framing
(side, height, distance, look-ahead) and a subtle per-shot FOV, so no two
cuts look alike.

## Install

Drop this folder into `JERICHO/MODS/` (already done if you are reading this
from the repo), re-run premake from `src_rebuild/`, and rebuild:

```
premake5.exe vs2019
```

Then enable the module (Options -> JERICHO -> antfarm, or edit
`JERICHO/CONFIG/modlist.ini` — `antfarm = 1`). No game files are modified.

## Usage

- **F9** (keyboard, PC) — toggle the screensaver during gameplay.
- **Pause -> Modules -> Ant Farm** — toggle, and a Settings submenu for:
  - **Cut interval** (8–20 s, default 12) — how long each shot is held.
  - **Junction / Car follow / Flyover** — enable/disable each shot type.
- **START** during the screensaver — turns it off and opens the normal
  pause menu.
- Settings persist to `JERICHO/CONFIG/antfarm.ini`.

## How it works (module-only, no game edits)

| System | Mechanism |
|---|---|
| Input cutoff | `gStopPadReads = 1` (player car brakes and holds; on-foot pads zeroed via `JER_EVENT_PED_INPUT`) |
| HUD hidden | `gDoOverlays = 0` |
| Camera | `JER_EVENT_CAMERA` — writes `camera_position`/`camera_angle`, sets `override = 1` so the engine rebuilds the view from our values |
| On-road framing | `GetSurfaceRoadInfo` + `GetNodePos` (lane + distance) + `ROAD_LANE_DIR` for the traffic heading — same helpers the civ AI drives on |
| Scenery clearance | `lineClear` LOS pull-back + `CheckScenaryCollisions` camera-collider push-out (world-space Y conversion, `camera.c:604` pattern) |
| Region streaming | `MainPlayer.spoolXZ` redirected to a module-owned `VECTOR`, re-asserted every frame in the camera hook (after `UpdatePlayers()` resets it) |
| Fade | semi-transparent fullscreen wash drawn in `JER_EVENT_DRAW_OVERLAY` (same look as the stock `FadeGameScreen`) |
| FOV | per-shot `SetGeomScreen(scr_z = …)` — wider lens on flyovers, subtle variety elsewhere |
| Timing / state machine | `JER_EVENT_FRAME` (wall-clock, SDL_GetTicks) |

## Caveats

- Aimed at **single-player free drive** (take-a-drive / survival). It refuses
  to engage during cutscenes, replays or multi-player, and auto-exits if a
  cutscene or replay starts while it is running.
- The player's car stays where you left it: it brakes (handbrake) when the
  mode engages and is not moved. Traffic may bump into it.
- The spool follows the camera, so the player car can end up far from the
  action; when the mode ends you are back at your car wherever it is.
- The fade uses the game's stock semi-transparent wash look (brightens to a
  white-ish wash) rather than a true black — matching how the original game
  does its own screen fades.
- Camera/pad compatibility: while the screensaver is on, Ant Farm's camera
  and pad hooks run at a higher JERICHO priority than d2pl's, so the
  screensaver takes precedence; it hands control back as soon as it is off.

## Known issue (not this module)

The full-mods Debug build currently fails to **link** because the sandbox
module references `gUseRotatedMap`, which is `static` in `overmap.c` while
`overmap.h` still declares it `extern` — a pre-existing tree issue. Build
with `premake5.exe --with-mods="antfarm" vs2019` (or exclude sandbox) to
link cleanly.
