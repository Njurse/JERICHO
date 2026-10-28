# JERICHO mods

Most mods live in their own repositories; this folder is where they are checked
out. The superproject records a commit ("pin") for each one in the index, and
`.gitmodules` records where it comes from and which branch it tracks.

| mod | repository | tracks |
| --- | --- | --- |
| `mp` | `Njurse/JERICHO-Mod-MP` | `main` |
| `carhacks` | `Njurse/JERICHO-Mod-carhacks` | `main` |
| `crumple` | `Njurse/Jericho-Mod-CRUMPLE` | `main` |
| `bake` (Driver 1 tooling) | `Njurse/JERICHO-D1` | `betamiami` |
| `gaildrv2` | `Njurse/gailredriver2` | — (not published) |

The rest (`antfarm`, `cainescrossfire`, `d2pl`, `JerAI`, `sandbox`, …) are
ordinary folders tracked by this repository.

## Fetching them

```bat
git submodule update --init --recursive
```

A release does not depend on the pins being current: CI and
`tools/publish_release.ps1` both run `git submodule update --remote` before
building, so a release carries the latest commit of each tracked branch.
`tools/update_mod_submodules.sh` advances the pins in the working tree so they
match what a release actually built (it prints the diff and stages nothing).

## A mod that is not there must not build a smaller game

`mp`, `carhacks` and `crumple` are **required**. If one is missing the build
stops, twice over:

* `premake5.lua` errors at generation — `mp is a submodule and was not fetched;
  run: git submodule update --init JERICHO/MODS/mp`
* `src/jer_required_mods.h` raises the same `#error` at compile time, so an IDE
  or a plain `msbuild` run cannot skip it.

`gaildrv2` and `d1cars` are deliberately **not** on that list: they may be
absent and degrade the way they always have.

## Conventions

* **Never force-push a branch that a pin points at.** The superproject records a
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
