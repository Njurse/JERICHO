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
| `antfarm` | `Njurse/JERICHO-Mod-Antfarm` | `0.12` (screensaver; not a core release mod) |

`gaildrv2` is hibernated at https://github.com/Njurse/JERICHO-Mod-GAIL-DRV2 and is not a submodule of this tree.

The rest (`cainescrossfire`, `d2pl`, `Alleycat`, `sandbox`, …) are
ordinary folders tracked by this repository.

## Fetching them

```bat
git submodule update --init --recursive
```

CI builds the commit the superproject pins, so a release carries the gitlinks in
the tagged tree. `tools/publish_release.ps1` advances each submodule to the latest
commit of its tracking branch before it builds; `tools/update_mod_submodules.sh`
makes the same advance in the working tree and prints the pins to commit (it
stages nothing). Commit the gitlinks to change what CI ships.

## A mod that is not there must not build a smaller game

`mp`, `carhacks`, `crumple` and `bake` are **required**. If one is missing the build
stops, twice over:

* `premake5.lua` errors at generation — `mp is a submodule and was not fetched;
  run: git submodule update --init JERICHO/MODS/mp`
* `src/jer_required_mods.h` raises the same `#error` at compile time, so an IDE
  or a plain `msbuild` run cannot skip it.

`gaildrv2` is not on that list: it is hibernated and is not part of this tree.
(`bake`, formerly `d1cars`, used to be exempt too; it is a release mod now, so it
is required.)

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
