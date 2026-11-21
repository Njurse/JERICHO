# JERICHO mods

Mods live in their own repositories; this folder is where they are checked
out. Core uses **no submodules** for them: [`mods.json`](mods.json) records each
mod's repository, the commit core was last built against (`ref`) and the branch
that commit is followed on. Every listed folder is ignored by core's
`.gitignore`, so each one is simply that mod's own clone.

| mod | repository | branch |
| --- | --- | --- |
| `mp` | `Njurse/JERICHO-Mod-MP` | `main` (required, release) |
| `carhacks` | `Njurse/JERICHO-Mod-carhacks` | `main` (required, release) |
| `crumple` | `Njurse/Jericho-Mod-CRUMPLE` | `main` (required, release) |
| `bake` (Driver 1 tooling) | `Njurse/JERICHO-D1` | `betamiami` (release) |
| `antfarm` | `Njurse/JERICHO-Mod-Antfarm` | `0.12` (screensaver; store-only) |
| `cainescrossfire` | `Njurse/JERICHO-Mod-CainesCrossfire` | `jer/standalone` |
| `civviecops` | `Njurse/JERICHO-Mod-CivvieCops` | `main` |
| `democars` | `Njurse/JERICHO-Mod-DemoCars` | `main` |

`gaildrv2` is hibernated at https://github.com/Njurse/JERICHO-Mod-GAIL-DRV2 and is not listed.

The rest (`Alleycat`, `sandbox`, `pedhacks`, `fulltwist`, …) are still ordinary
folders tracked by this repository.

## Fetching them

```bat
python tools/fetch_mods.py              :: clone every missing mod at its pin
python tools/fetch_mods.py --release    :: only mp, carhacks, crumple, bake
python tools/fetch_mods.py --status     :: what is on disk vs mods.json
```

A folder that already exists is never re-cloned, reset or deleted. `--update`
(move to `ref`) and `--advance` (move to the branch tip) only touch checkouts with
no modified tracked files, and only with `checkout --detach`, so untracked work
survives. `--pin` writes each checkout's HEAD into `mods.json`.

CI runs `fetch_mods.py --release --depth 1`, so a build carries the commits
`mods.json` pins. `tools/publish_release.ps1` runs `--advance` first, so a local
release carries each branch tip. To change what CI ships, run `--pin` and commit
`mods.json`.

## A mod that is not there must not build a smaller game

`mp`, `carhacks` and `crumple` are **required**. If one is missing the build
stops, twice over:

* `premake5.lua` errors at generation — `mp is its own repository and was not
  fetched; run: python tools/fetch_mods.py mp`
* `src/jer_required_mods.h` raises the same `#error` at compile time, so an IDE
  or a plain `msbuild` run cannot skip it.

`gaildrv2` is not on that list: it is hibernated and is not part of this tree.

## Conventions

* **Never force-push a branch that a pin points at.** `mods.json` records a
  commit; rewriting it leaves every clone unable to check the mod out. Land work
  as new commits on the tracked branch (`main`, or `betamiami` for `bake`).
* **Never commit build output inside a mod repo.** `lib/`, `obj/`, `*.exp`,
  `*.lib`, `*.pdb`, `tools/car/out/` and `python/checkpoints/` belong in that
  repo's `.gitignore` — they are what makes `git status` look dirty here while
  the gitlinks are in fact unchanged.
* Each mod declares `interface = N` in its `mod.toml`, matching
  `JERICHO_SDK_VERSION` in `src_rebuild/Game/C/JERICHO/include/jericho.h`. A
  mismatch is refused at activation, and the store refuses such an entry before
  downloading it. `python tools/check_mod_interface.py` checks every manifest
  against the engine, plus duplicate custom event ids and cross-mod includes.
* Mods talk to each other through the core, never by including another mod's
  path: a contract is a core header of custom events (see
  `src_rebuild/Game/C/JERICHO/docs/interfaces.md`).
