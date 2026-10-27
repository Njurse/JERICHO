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

`bake` is still a mandatory core addon, but its runtime unit is now an external
DLL (`runtime = "dll"`); its BAKE tools and baked data stay outside the runtime.
It has no direct-engine symbols today.

Everything else is a conversion candidate. Development harnesses may also stay
compiled in for convenience, but that is a tooling choice, not an API barrier.

### Core addon gaps (exact, interface 3 / host API generation 1)

`python3 tools/check_mod_api.py --gap` reports these direct-engine uses for the
three modules that still stay compiled in:

| Module | Functions | Globals |
|---|---|---|
| `mp` | `CellEmpty`, `DrawPlayerDot`, `EnablePause`, `GetSurfaceIndex`, `InitCar`, `MapHeight`, `Start3DSoundVolPitch`, `ratan2` | `GameType`, `MainPlayer`, `MaxPlayerDamage`, `NumPlayers`, `active_car_list`, `camera_position`, `car_data`, `num_active_cars`, `player`, `residentCarModels` |
| `carhacks` | `InitCar` | `GameType`, `NumPlayers`, `car_data`, `gCarCleanModelPtr`, `player`, `residentCarModels` |
| `bake` | none | none |

The `bake` runtime unit is still a scaffold; its BAKE pipeline is offline
tooling/data and its direct-engine surface is empty today.

### Update semantics (core and optional addons)

- **Install is checksum-verified and gated before it touches `MODS/`.** The
  store checks the package id against `mod.toml`, the exact `interface` value,
  the SHA-256, and (when present) `host_api`. A package from a newer host API
  generation is refused with `needs host API vN; this build has vM`.
- **Versions are X.Y.Z and the repo only moves forward.** A newer repo version
  is an update, an equal version is "installed", and an older repo version is
  never offered as a downgrade. A compiled-in/replaced folder is reported as
  `replaced by the release` and can be reinstalled over.
- **Core addons are mandatory and shown as CORE** (light red) in the Mods table.
  They cannot be toggled off or reordered; `carhacks` is forced first, then
  `bake`, then `mp`. A missing or invalid core addon marks the core suite
  incomplete and refuses optional addons that assume it; it never silently
  runs a partial stack.
- **Rollback is staged, not destructive.** The package is unpacked and
  validated before an overwrite; the previous installed folder is retained for
  the next successful boot, and the install record keeps the last known-good
  version/hash. If the new core addon is refused, the previous folder is the
  recovery path. Automatic in-game rollback is a later loader feature, not
  something an addon update performs on its own.
- **Independent updates stop at the host API.** An addon that stays within
  interface 3 / host API generation 1 can be updated from the store without a
  game release. A change that needs a new host function or event still needs a
  game release and a new generation/interface.

### Runtime units vs data/tooling

- **Only runtime code becomes a DLL.** `bake.dll` is the runtime unit
  (currently a scaffold); `carhacks.dll` will be the runtime importer/selector.
  Their `tools/` directories and `D1CARS` baked data stay data/tooling: the
  build mirror strips tools from the runtime tree, and the package generator
  never ships `carhacks/D1CARS` (it is generated from the player's own Driver 1
  files). The offline BAKE pipeline remains a separate tool artifact.
- **The DLLs never own the baked/generated car data.** They call the host
  cardata surface to set/read model source, resident slots, availability and the
  frontend car list; the engine owns the loaded model, cosmetics, palette and
  VRAM pages.

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

## JMPPED ownership boundary (pedhacks extraction)

The JMPPED system is currently split across four places. The target boundary is
also the load boundary: the core order is `carhacks -> bake -> pedhacks -> mp`,
so the asset module is available before the multiplayer consumer.

| Layer | Owns | Must not own |
| --- | --- | --- |
| `pedhacks` (compiled-in) | Archive lifecycle and refusal state, v3/v5 format parsing, entry table/names, per-entry availability policy, character install/select orchestration | Engine model/page allocation, raw `pmJerichoModels`, `player[]` |
| Engine service | `CAR_IMPORT` model lookup/blob reads, model materialisation/rebuild, texture page pin/upload/pool lifetime, skeleton generation and resident-model binding, player-ped rebind/draw primitives | JMPPED archive format decisions, character choice |
| `mp` | Character-selection UI, wire IDs, remote stand-ins, ownership/sync, fallback choice | Archive parsing, page/model installation, direct `models.h`/`pedest.h` JMPPED access |

The audit points that force this split:

- `models.c`: `sJerPedModelNames`, `sJerPedModelIndex`, `sJerPedProbeCity`,
  `sJerPedProbeOk`, `sJerPedMem`, `sJerPedMemSize`, `sJerPedModels`,
  `sJerPedEntryCount`, `sJerPedEntryNames`, `sJerPedReady`, `sJerPedBuiltCity`,
  `sJerPedSet`, `sJerPedSetCount`; parser helpers `JerPedFindModelName`,
  `JerPedModelBlob`, `JerPedIsTexturedPoly`, `JerPedCollectModelSets`,
  `JerPedRemapModelSets`; lifecycle `JerPedImportProbe`, `JerPedImportBuild`,
  `JerPedImportBaked`, `JerPedImportBakedV5`, `JerPedImportReady`,
  `JerPedImportReset`, `JerPedBakedAvailable`; enumeration `JerPedEntryCount`,
  `JerPedEntryName`, `JerPedModelForEntry`.
- `texture.c`: `sJerPedPinsRecorded`, `sJerPedPinFailed`, `sJerPedAreaPool`,
  `sJerPedAreaClutY`, `sJerPedAreaClutRows`, `sJerPedAreaPoolCount`;
  `JerPedPinOffset`, `JerPedPinTpages`, `JerPedPinsReady`,
  `JerPedPageListsReport`, `JerPedUploadAreaPage`, `JerPedUploadBakedPage`,
  `JerPedReleasePins`.
- `pedest.c`: `pmJerichoModels`, `gJerPedSkeletonGeneration`,
  `JerPedSkeletonGeneration`, `JerPedSkeletonInvalidate`,
  `JerPedInstallModels`, and the `InitTanner` JERI_* rebind/reset.
- `players.c`: no JMPPED archive state; `player[].pPed` binding is engine
  service, not pedhacks.
- `mp_session.c`: `sMpJerPedInstalledLevel`, `sMpJerPedImportFailed`,
  `sMpJerPedImportLevel`, `sMpJerPedBakedFailed`,
  `MpEnsureJerichoInstalled`, `MpEnsureJerichoImported`,
  `MpPedArchiveReset`, `MpPedArchiveEnsure`; these lifecycle states move to
  pedhacks. `MpPedModelCount/Name/Selectable/Effective`,
  `MpApplyLocalPedModel`, `MpSetPedModel`, protocol IDs and sync stay in mp and
  consume the contract.

The contract must expose opaque archive/entry handles plus in/out state for
probe/load/reset/ready, baked import, entry enumeration, model/page
availability, palette selection, character install/select and player-ped
state. The engine service is the only layer allowed to touch page/model/VRAM
internals; `mp` must not include `models.h` or `pedest.h` for JMPPED.

### Parity target against carhacks

`carhacks` is the reference for a complete compiled-in asset module. It owns the
feature layer while the engine keeps the primitive services:

| Concern | carhacks owner | pedhacks target |
| --- | --- | --- |
| Feature toggles/config | `carhacks.c` | pedhacks: pedestrian options, archive enable/refusal policy |
| Asset/import policy | `carimport.c` | pedhacks: v3/v5 archive state, entry table, install/select policy |
| Selection UI | `carselect.c` | pedhacks: character selection/availability surface |
| Network advert/sync | `net.c`, `mplive.c` | stays in mp (protocol IDs, remote stand-ins, ownership/sync) |
| Spawn/placement | `spawn.c` | stays in mp for player peds; engine service for `player[].pPed` |
| Authoring tools | `carhacks/tools/` | BAKE `tools/ped/` (car tools remain `tools/car/`) |
| Engine primitives | `InitCarImport`, `JerHotLoadCarModel`, `CarImportApplyPaletteForCity`, `CarSlotResReport` | engine service: `JerPedImport*`, page pin/upload/pool, model/skeleton binding, `pmJerichoModels` |

Therefore pedhacks still needs to own: archive parser/state/entry table, the
lifecycle/refusal policy, character availability/install/select, and the
pedestrian selection surface. Engine service remains responsible for low-level
model allocation, page pin/upload/pool, skeleton generation and resident-model
binding. mp remains responsible for protocol IDs, remote stand-ins, ownership
and multiplayer sync. The JMPPED authoring toolkit is moving to BAKE under
`tools/ped/`, alongside the existing car tools under `tools/car/`; the
pedhacks runtime stays in the main repo.
