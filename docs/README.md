# Documentation index

Driver 2's data formats have no public spec, so what we have reverse-engineered is
written down next to the code it describes. Start here.

## JERICHO — the engine-side mod framework

Canonical docs live in **`src_rebuild/Game/C/JERICHO/docs/`** (beside the code and
its SDK mirrors); `docs/JERICHO/` holds pointers only.

| Doc | Covers |
|---|---|
| [`README.md`](../src_rebuild/Game/C/JERICHO/docs/README.md) | what JERICHO is, API addons vs deep mods, layout, building, troubleshooting |
| [`events.md`](../src_rebuild/Game/C/JERICHO/docs/events.md) | **the event reference** — every event id, its args struct, where it fires, query vs notification, stock behaviour |
| [`HOOKS.md`](../src_rebuild/Game/C/JERICHO/docs/HOOKS.md) | writing a module: anatomy, pause menus, **saying things on screen** (HUD, `jer_notify`, `jer_error`), what a module can do, logging, boot arguments |
| [`textures.md`](../src_rebuild/Game/C/JERICHO/docs/textures.md) | **custom textures** — the `jer_texture.h` API: the `MODS/<mod>/textures/<name>.tga` convention, the two targets (hires GPU texture vs PSX VRAM page), and the draw recipe's traps |
| [`ped-animation.md`](../src_rebuild/Game/C/JERICHO/docs/ped-animation.md) | the pedestrian animation and skeleton pipeline |
| [`map-streaming.md`](../src_rebuild/Game/C/JERICHO/docs/map-streaming.md) | **world region streaming** — the 2×2 barrel window, why the engine only pre-loads neighbours, and the `jer_map` helper to stream somewhere it has not been (arena spawns, teleports) |
| [`module-activation.md`](../src_rebuild/Game/C/JERICHO/docs/module-activation.md) | how a module gets enabled (`modlist.ini` → `mod.toml` → fail-closed), the `src=` boot log (`modlist`/`default`/`forced`/`nomods`), `-nomods`, forcing one module on for a test (`jer_force_module` / `-testmode`), which modules override car handling, and why no handling module ⇒ vanilla handling |
| [`screens.md`](../src_rebuild/Game/C/JERICHO/docs/screens.md) | presentation screens: `jer_screen.h` (register/show/tick, the boot loop) and the host-owned `jer_prompt.h` yes/no prompt |
| [`ped-palette.md`](../src_rebuild/Game/C/JERICHO/docs/ped-palette.md) | giving one Tanner instance its own colours: the measured palette footprint (1 page, 2 CLUT entries) and the `texture_cluts` bracket that makes it per-instance |
| [`sdk/README.md`](../JERICHO/sdk/README.md) | the addon SDK: headers, building, installing, platform notes |

## Mods

Each mod documents itself in its own folder.

| Mod | Docs |
|---|---|
| Caine's Crossfire | [`README.md`](../JERICHO/MODS/cainescrossfire/README.md) (index + test tooling), [`PROFILES.md`](../JERICHO/MODS/cainescrossfire/PROFILES.md) (**the vehicle roster — how a profile maps to a car, its stats and its physics**), [`SPECIALS.md`](../JERICHO/MODS/cainescrossfire/SPECIALS.md) (**the eleven vehicle specials**), [`ARENAS.md`](../JERICHO/MODS/cainescrossfire/ARENAS.md) (**the arena registry: bounded play areas, their region barriers and spawn points, and the `.cca` file format**), [`FX.md`](../JERICHO/MODS/cainescrossfire/FX.md), [`HANDLING.md`](../JERICHO/MODS/cainescrossfire/HANDLING.md), [`SOUNDS.md`](../JERICHO/MODS/cainescrossfire/SOUNDS.md), [`AI.md`](../JERICHO/MODS/cainescrossfire/AI.md), [`MOUNTED_CREW.md`](../JERICHO/MODS/cainescrossfire/MOUNTED_CREW.md), [`FACTIONS.md`](../JERICHO/MODS/cainescrossfire/FACTIONS.md), [`TURBO.md`](../JERICHO/MODS/cainescrossfire/TURBO.md) (**the turbo meter, the trigger, and where the numbers came from**), [`MOTION.md`](../JERICHO/MODS/cainescrossfire/MOTION.md) (**the procedural motion layers: idle fidget, pitch-back, and why they are not the knock**), [`KNOCK.md`](../JERICHO/MODS/cainescrossfire/KNOCK.md) (**the vehicle knock - bucking and rocking for effect**), [`CREW_POSES.md`](../JERICHO/MODS/cainescrossfire/CREW_POSES.md) (**the crew's aim poses per weapon class**) |
| carhacks | [`README.md`](../JERICHO/MODS/carhacks/README.md) (index + the hack/config table), [`HANDOFF.md`](../JERICHO/MODS/carhacks/HANDOFF.md) (**where the cross-city/mp workstream stands — the state, the open items, and the traps — read this first when picking the work up**), [`CARSELECT.md`](../JERICHO/MODS/carhacks/CARSELECT.md) (**the car-select menu: a city-roster row, replacing the stock Take-a-Ride car screen**), [`MP_ADAPTER.md`](../JERICHO/MODS/carhacks/MP_ADAPTER.md) (**the car identity schema `carid.h` + the connection channel over the JERICHO net bridge, and the mp deltas it proposes**), [`tools/chk_mp_foreign.sh`](../JERICHO/MODS/carhacks/tools/chk_mp_foreign.sh) (**three-city mp stress test**). Plus its own docs — [`CROSS_CITY.md`](../JERICHO/MODS/carhacks/docs/CROSS_CITY.md), [`HACK.md`](../JERICHO/MODS/carhacks/docs/HACK.md), [`VEHICLES.md`](../JERICHO/MODS/carhacks/docs/VEHICLES.md), [`PALETTES.md`](../JERICHO/MODS/carhacks/docs/PALETTES.md), [`VRAM.md`](../JERICHO/MODS/carhacks/docs/VRAM.md), [`FORMATS.md`](../JERICHO/MODS/carhacks/docs/FORMATS.md) (**the proprietary file formats**) (the car-data tools moved with them), with
[`docs/vram/`](../JERICHO/MODS/carhacks/docs/vram/README.md) keeping the CLUT-column checkpoint log |
| crumple | [`README.md`](../JERICHO/MODS/crumple/README.md), [`crumple.md`](../JERICHO/MODS/crumple/crumple.md) (car deformation model) |
| d2pl | [`readme.md`](../JERICHO/MODS/d2pl/readme.md) |
| sandbox | [`README.md`](../JERICHO/MODS/sandbox/README.md) |
| collisiondevil | [`README.md`](../JERICHO/MODS/collisiondevil/README.md) |
| antfarm | [`readme.md`](../JERICHO/MODS/antfarm/readme.md) (what it is + how to use it), [`ARCHITECTURE.md`](../JERICHO/MODS/antfarm/ARCHITECTURE.md) (**internals: every hook, the cut state machine, the archetype table and each camera model's maths, the trail cam, shot planning, region/area streaming and the recovery, audio, config, testing**) |
| levelhacks | [`README.md`](../JERICHO/MODS/levelhacks/README.md) |
| example | [`README.md`](../JERICHO/MODS/example/README.md) |
| aidriver | [`README.md`](../JERICHO/MODS/aidriver/README.md) |
| mp | [`README.md`](../JERICHO/MODS/mp/README.md) (LAN multiplayer) — start with [`JERICHO-MP.md`](../JERICHO/MODS/mp/docs/JERICHO-MP.md); test tools in [`tools/`](../JERICHO/MODS/mp/tools/README.md) |
| testmode | [`README.md`](../JERICHO/MODS/testmode/README.md) (asset-test mode: quiet world, census) |
| debugorbit | [`README.md`](../JERICHO/MODS/debugorbit/README.md) (**Debug Orbit Camera** — camera-only: takes the camera over at level start and orbits the player at a fixed radius/elevation, for inspecting a car or Tanner from every side; also the worked example of the camera y inversion) |

`docs/crumple.md` is a pointer to the canonical copy in the mod folder, as are
`docs/JERICHO/ped-animation.md` and `docs/JERICHO/map-streaming.md`.

## Engine, ports and tooling

- [`CI.md`](CI.md) — GitHub Actions builds: what is produced, the `alpha` pre-release,
  and how to cut a tagged release.
- [`src_rebuild/PsyCross/README.md`](../src_rebuild/PsyCross/README.md) — Psy-X /
  Psy-Cross, the PlayStation-to-host layer the engine is ported onto.
- [`PSXToolchain/README.md`](../PSXToolchain/README.md) — the PSX build toolchain.
- [`tools/`](../JERICHO/MODS/cainescrossfire/tools/README.md) — Caine's Crossfire's
  tooling: the arena editor, the live palette editor and the arena smoke test
  (`arenaedit.py`, `view3d.py`, `paletteedit.py`, `arena_test.sh`); described in Caine's
  Crossfire's README under "Test tooling". The mod's own `arenas/`
  folder holds the `.cca` arena files ([`arenas/README.md`](../JERICHO/MODS/cainescrossfire/arenas/README.md)).
- `JERICHO/MODS/carhacks/tools/` — the cross-city import checks and the VRAM/car-data
  tools; see [`README.md`](../JERICHO/MODS/carhacks/tools/README.md).
- [`changelog.txt`](../changelog.txt) — upstream changelog.

## Conventions

- **Engine/format facts are written where the code is**, and cited as `file:line` —
  a claim without a citation should be treated as unverified.
- **One canonical copy per document.** Duplicates are replaced by short pointers;
  if you find a body duplicated in two places, that is a bug.
- **Measured numbers beat remembered ones.** Anything quoted (block sizes, page
  sets, frame rates) was read out of this install's data or logs — the recipes to
  re-check them live at the end of `carhacks/FORMATS.md`.
- **Documentation drifts.** When behaviour changes, the doc that describes it is
  part of the change. **`python tools/doccheck.py`** is the check for that, and it is
  meant to be run whenever docs are touched: it verifies every relative link resolves,
  that each engine event is in `events.md`, that every doc is reachable from this index,
  and that the counted claims (the resident slot pool, the guest-city ceiling, the roster
  size) still match the code they describe. It reads only tracked files, because `bin/`
  holds untracked mirror copies of these docs from the last build. See `tools/README.md`.
