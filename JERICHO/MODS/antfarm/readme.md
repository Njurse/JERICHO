# Ant Farm — screensaver / idle mode (JERICHO module)

A passive city observer for REDRIVER2. Turn it on and the game becomes a
screensaver: player input is cut off, the HUD hides, all car/player SFX are
muted (music stays), and cop aggression is disabled while a cinematic camera
tours the whole map — every cut hops to a far area and picks a fresh shot:

- **Chase** — behind-follow on a traffic car, framed to the vehicle's size.
- **Static track** — a fixed roadside camera a car drives past; the camera
  pans to follow, then relocates when the car leaves its reach.
- **Overhead** — scenic elevated 3/4 view down onto traffic (either
  high-following a car or watching a whole road).
- **Tripod** — parked, swaying (sometimes orbiting) roadside camera at a
  junction, aimed down the lane cars actually drive in.
- **Flyover** — a slow, eased dolly along a long straight.

Static/overhead styles are weighted more heavily than the chase cam, and
cuts last 10–60 s (default 20). Each shot runs a scenery pass (line-of-sight
pull-back + camera-collider push-out) so buildings never obscure the view.

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
- The player's car is **pinned to the camera focus** while the mode runs:
  teleported there each frame (so region streaming follows the action),
  hidden (`CONTROL_TYPE_NONE` + slot reserved so traffic can't reuse it),
  and muted. On exit it's restored to where you left it.
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
