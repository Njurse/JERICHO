# External mods: core boundary and API gaps

This page is the working record of which modules can become `runtime = "dll"`
addons and what the host API still has to grow first. The source of truth for
the table is:

```sh
python tools/check_mod_api.py --gap
```

`tools/check_mod_api.py` also runs in CI. It refuses a runtime DLL that includes
an engine header, reaches a non-SDK include outside its own folder, or uses a
known engine function or data global. Compiled-in modules are audited instead:
`--gap` prints their direct-engine use as `module <tab> kind <tab> symbol`.

## Core boundary

These stay compiled in. They need engine data and game-flow internals that should
not become an ABI surface:

| Module | Why it stays deep |
|---|---|
| `mp` | Owns the network session, car state sync, host flow and engine input/timing. |
| `carhacks` | Owns vehicle availability, cross-city imports, texture/VRAM pages and the car query contract. |
| `d1cars` | Owns the Driver 1 bake unit and its car-data source. Kept with `carhacks`; the planned BAKE rename is a separate submodule-coordinated change. |

Everything else is a conversion candidate. Development harnesses may also stay
compiled in for convenience, but that is a tooling choice, not an API barrier.

## Candidate order

| Module | Status | Main work |
|---|---|---|
| `example`, `debugorbit` | already `runtime = "dll"` | none; they are the fixtures. |
| `fulltwist` | external (`runtime = "dll"`, pilot) | done: car matrix/transform, `hd.aacc` torque, wheel state, cosmetic/model metadata, main-player and camera state moved onto the host API. |
| `yarisbounce` | external (`runtime = "dll"`) | done: clean-model vertices plus the per-car draw buffer, frame counter and raw D-pad through the host API; body and wheel squash are render-only. |
| `collisiondevil` | next, physics-only | camera default and a physics/state setter view. |
| `antfarm` | later | car spawn/damage, scene streaming, camera and sound. |
| `d2pl` | later | render model, camera position/base angle, pedestrian state. |
| `cainescrossfire` | later, largest | render/FX/sound, car damage, model rebuild, player/camera state. |
| `JerAI` | later | road/cell/cop graph, model pointers, matrix table, car/player state. |
| `crumple` | later, contract-based | job/car damage path and body geometry; its `jer_crumple` contract already exists. |
| `sandbox`, `testmode`, `levelhacks`, `gaildrv2` | keep compiled-in for now | dev/private harnesses; no external need demonstrated. |

## Host API v1 coverage

Already in `jer_host_api.h`:

- **core**: log, fire event, override slot;
- **config**: get/set int, bool, string;
- **car**: count/at, slot usable, state get/set, wheel mask, model/cosmetic
  pointer, model index, init, `MapHeight`, road info, `CellEmpty`, active slots;
- **player**: count/at, state, felony, camera angle, horn, raw pad;
- **camera**: state, angle, set angle;
- **model**: clean/damaged/low model, cosmetics, vertices, vertex count;
- **world**: cells across, units across, road counts, region, region of, spool;
- **game**: level, game type, player count, cops allowed, max player damage,
  pedestrian felony;
- **UI**: HUD message/segments/clear, console, notify, pause menu, frontend menu,
  text draw/measure, sound lock/unlock;
- **debug**: world lines, depth lines.

## Direct-engine gaps by module

`F` is an engine function, `G` a data global. A symbol can be listed once per
module, not once per use.

| Module | Missing direct engine symbols |
|---|---|
| `JerAI` | F: `CellEmpty`, `CopsCanSeePlayer`, `GetFirstPackedCop`, `MapHeight`, `QuickUnpackCellObject`, `RCOS`, `RSIN`, `copsAreInPursuit`, `ratan2`; G: `NumDriver2Curves`, `NumDriver2Junctions`, `NumDriver2Straights`, `car_data`, `matrixtable`, `modelpointers`, `player` |
| `antfarm` | F: `GetPlayerFelony`, `GetSurfaceIndex`, `InitCar`, `MapHeight`, `RCOS`, `RSIN`; G: `MainPlayer`, `NumDriver2Straights`, `NumPlayers`, `camera_position`, `car_data` |
| `cainescrossfire` | F: `AddExplosion`, `ApplyDamage`, `DrawPlayerDot`, `DrawTargetBlip`, `GetFreeChannel`, `GetSurfaceIndex`, `InitCar`, `MapHeight`, `RCOS`, `RSIN`, `RebuildCarMatrix`, `RenderModel`, `Setup_Sparks`, `Start3DSoundVolPitch`, `ratan2`; G: `GameType`, `MainPlayer`, `MaxPlayerDamage`, `NumDriver2Curves`, `NumDriver2Junctions`, `NumDriver2Straights`, `NumPlayers`, `camera_position`, `car_data`, `gCameraDefaultScrZ`, `gCarCleanModelPtr`, `pPed`, `player`, `residentCarModels` |
| `carhacks` | F: `InitCar`; G: `GameType`, `NumPlayers`, `car_data`, `gCarCleanModelPtr`, `player`, `residentCarModels` |
| `collisiondevil` | G: `gCameraDefaultScrZ` |
| `crumple` | F: `RCOS`, `RSIN`; G: `MainPlayer`, `car_data`, `gCarCleanModelPtr` |
| `d2pl` | F: `MapHeight`, `RCOS`, `RSIN`, `RenderModel`, `ratan2`; G: `camera_position`, `car_data`, `gCameraAngle`, `gCameraDefaultScrZ`, `pPed`, `player` |
| `gaildrv2` | F: `GetPlayerFelony`, `GetSurfaceIndex`, `RCOS`, `RSIN`, `ratan2`; G: `GameType`, `MaxPlayerDamage`, `car_data`, `gCurrentMissionNumber`, `player` |
| `levelhacks` | G: `GameType`, `NumPlayers` |
| `mp` | F: `CellEmpty`, `DrawPlayerDot`, `EnablePause`, `GetSurfaceIndex`, `InitCar`, `MapHeight`, `Start3DSoundVolPitch`, `ratan2`; G: `GameType`, `MainPlayer`, `MaxPlayerDamage`, `NumPlayers`, `active_car_list`, `camera_position`, `car_data`, `num_active_cars`, `pPed`, `player`, `residentCarModels` |
| `sandbox` | F: `DrawCarWheels`, `InitCar`, `MapHeight`, `RCOS`, `RSIN`, `RenderModel`; G: `car_data`, `gCarCleanModelPtr`, `modelpointers`, `player`, `residentCarModels` |
| `testmode` | G: `NumPlayers`, `car_data`, `pPed`, `player` |

## API additions implied by the table

- **car2**: rebuild/get matrix, set orientation/position/velocity, `hd.aacc`
  torque, wheel compression/surface/suspension, damage (total, zone, wheel),
  thrust/steer/control, cosmetic fields (`cog`, `colBox`, `mass`, `twistRate`).
- **draw**: model/car/wheel draw, sprites, textured quads, `matrixtable`, OT and
  plot context for modules that draw into the world.
- **fx**: smoke, sparks, debris, leaves, explosions, target blips/player dots.
- **sound**: 3D voice start/stop, free channel, owner, volume/pitch.
- **world2**: surface index, road lookup/lanes, cell iteration, packed cops,
  active-car list, resident models/model data.
- **gameflow**: mission state, pause, next screen, fade, game type setter.
- **player2**: `MainPlayer`, `dying`, `upsideDown`, `onGrass`, `cameraView`,
  crash/skid state.

The rule is the same as the existing host API: opaque tokens and in/out state
structs, append-only, never raw engine globals or engine structs.
