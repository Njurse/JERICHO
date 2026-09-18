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
| [`HOOKS.md`](../src_rebuild/Game/C/JERICHO/docs/HOOKS.md) | writing a module: anatomy, pause menus, logging, boot arguments |
| [`ped-animation.md`](../src_rebuild/Game/C/JERICHO/docs/ped-animation.md) | the pedestrian animation and skeleton pipeline |
| [`module-activation.md`](../src_rebuild/Game/C/JERICHO/docs/module-activation.md) | how a module gets enabled (`modlist.ini` → `mod.toml` → fail-closed), the `src=` boot log (`modlist`/`default`/`forced`/`nomods`), `-nomods`, forcing one module on for a test (`jer_force_module` / `-testmode`), which modules override car handling, and why no handling module ⇒ vanilla handling |
| [`screens.md`](../src_rebuild/Game/C/JERICHO/docs/screens.md) | presentation screens: `jer_screen.h` (register/show/tick, the boot loop) and the host-owned `jer_prompt.h` yes/no prompt |
| [`ped-palette.md`](../src_rebuild/Game/C/JERICHO/docs/ped-palette.md) | giving one Tanner instance its own colours: the measured palette footprint (1 page, 2 CLUT entries) and the `texture_cluts` bracket that makes it per-instance |
| [`sdk/README.md`](../JERICHO/sdk/README.md) | the addon SDK: headers, building, installing, platform notes |

## Mods

Each mod documents itself in its own folder.

| Mod | Docs |
|---|---|
| combatd2 | [`README.md`](../JERICHO/MODS/combatd2/README.md) (index + test tooling), [`FX.md`](../JERICHO/MODS/combatd2/FX.md), [`HANDLING.md`](../JERICHO/MODS/combatd2/HANDLING.md), [`SOUNDS.md`](../JERICHO/MODS/combatd2/SOUNDS.md), [`AI.md`](../JERICHO/MODS/combatd2/AI.md), [`MOUNTED_CREW.md`](../JERICHO/MODS/combatd2/MOUNTED_CREW.md), [`FACTIONS.md`](../JERICHO/MODS/combatd2/FACTIONS.md), [`carhacks/CROSS_CITY.md`](../JERICHO/MODS/combatd2/carhacks/CROSS_CITY.md), [`carhacks/FORMATS.md`](../JERICHO/MODS/combatd2/carhacks/FORMATS.md) (**the proprietary file formats**) |
| crumple | [`README.md`](../JERICHO/MODS/crumple/README.md), [`crumple.md`](../JERICHO/MODS/crumple/crumple.md) (car deformation model) |
| d2pl | [`readme.md`](../JERICHO/MODS/d2pl/readme.md) |
| sandbox | [`README.md`](../JERICHO/MODS/sandbox/README.md) |
| collisiondevil | [`README.md`](../JERICHO/MODS/collisiondevil/README.md) |
| antfarm | [`readme.md`](../JERICHO/MODS/antfarm/readme.md) |
| levelhacks | [`README.md`](../JERICHO/MODS/levelhacks/README.md) |
| example | [`README.md`](../JERICHO/MODS/example/README.md) |
| aidriver | [`README.md`](../JERICHO/MODS/aidriver/README.md) |
| gaildrv2 | [`README.md`](../JERICHO/MODS/gaildrv2/README.md) |
| mp | [`README.md`](../JERICHO/MODS/mp/README.md) (LAN multiplayer) |
| testmode | [`README.md`](../JERICHO/MODS/testmode/README.md) (asset-test mode: quiet world, census) |

`docs/crumple.md` is a pointer to the canonical copy in the mod folder.

## Engine, ports and tooling

- [`CI.md`](CI.md) — GitHub Actions builds: what is produced, the `alpha` pre-release,
  and how to cut a tagged release.
- [`src_rebuild/PsyCross/README.md`](../src_rebuild/PsyCross/README.md) — Psy-X /
  Psy-Cross, the PlayStation-to-host layer the engine is ported onto.
- [`PSXToolchain/README.md`](../PSXToolchain/README.md) — the PSX build toolchain.
- `JERICHO/MODS/combatd2/tools/` — unattended test tooling (`arena_test.sh` and the
  `launch_*.bat` launchers); described in combatd2's README under "Test tooling".
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
  part of the change.
