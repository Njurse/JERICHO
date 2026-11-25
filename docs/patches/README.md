# Patches for the mod repositories

These changes belong in the mod repositories, not in this tree, and this tree's
contributors cannot push there, so each is a `git format-patch` series to apply
with `git am`. Every series was applied to the commit `0.14` pins **and** to the
tip of the branch that pin lives on, then tested there.

| repo (submodule) | `0.14` pins | the pin's branch | apply on | series | checked |
|---|---|---|---|---|---|
| `JERICHO-D1` (`bake`) | `a68bc81` | `cop-lightbar` (6 commits past the pin) | `cop-lightbar` | `bake/0001`, `bake/0002` | `tools/car`: 234 tests pass (18 skipped, as before) on the pin and on the tip; `tools/common`: 31 pass |
| `JERICHO-Mod-MP` (`mp`) | `7f8e0ef` | `0.24.0` (the pin is its tip) | `0.24.0` | `mp/0001`, `mp/0002` | both game builds; every CI "mp servers" test (proto_check, dedi test_plugins, master+dedi e2e, ...) |
| `JERICHO-Mod-carhacks` | `3f4e60b` | `jer/role-car-select` (the pin is its tip) | `jer/role-car-select` | `carhacks/0001` | `bigrig_extract.py` and `slotmap.py` start; `pages1.decomp_asm` imports from `bake/tools/car` |

## What each patch does

- **`bake/0001`**: `tools/car/README.md` and comments name `bake/tools/car` (the
  old `MODS/d1cars/tools` paths pointed at a folder that no longer exists).
- **`bake/0002`**: split pages no longer draw "confetti". A page whose colours do
  not fit one 16-entry CLUT row is split across rows, and a texel's 4-bit index
  only means something against the row it is drawn with. Three things broke
  that: a group past texture id 31 overwrote its material's row; a poly moved to
  a second row was drawn with the first; and a later group's bbox re-indexed
  texels an earlier group draws. Now every drawn texel is checked against the
  row its poly really uses and moved to its exact colour, a free slot, or the
  nearest colour that row holds (perceptual distance, never onto a slot cleared
  for transparency). On the new test scenes the old planner drew 80, 171 and
  400 texels with the wrong palette; the new one draws 0, 0 and 0. Also: the D1
  bake no longer stops at 33 flat colours (the rest use the nearest kept one),
  and `carbank_bake.py --check` no longer calls every textured page STALE.
  Details: `docs/CAR_BAKE.md` in that repo.
- **`mp/0001`**: the `pack_lan` packagers name `bake` (one stopped with "no
  d1cars module in the repo").
- **`mp/0002`**: a mods refusal says what the server runs (an optional `JPMR`
  list after `JPRJ`, no protocol bump: older clients ignore it), so a player is
  offered the server's addons from their own mod repos instead of a dead end;
  JERICHO asks before downloading anything (core: `jer_joinmods.c`). mp's
  manifest and beacon hash also describe what **runs** this boot
  (`jer_module_running`) instead of what `modlist.ini` says, which a relaunch
  with `-mod`/`-nomod` (or `-testmode`) used to misreport. The dedicated server
  sends the same list.
- **`carhacks/0001`**: `bigrig_extract.py`, `slotmap.py` and the docs point at
  `bake` (they looked in `MODS/d1cars/tools`).

## How to land them

1. **Core first.** `mp/0002` calls `jer_joinmods_offer` and
   `jer_module_running`, which this tree's branch adds (`core: fetch the addons a
   server needs...`, `vanilla: ...`, `joinmods: ...`). bake and carhacks have no
   core dependency.
2. **Apply each series on its branch and push**, from that repo's own clone:

   ```sh
   cd JERICHO-D1 && git checkout cop-lightbar && git am /path/to/JERICHO/docs/patches/bake/*.patch && git push origin cop-lightbar
   ```
   ```sh
   cd JERICHO-Mod-MP && git checkout 0.24.0 && git am /path/to/JERICHO/docs/patches/mp/*.patch && git push origin 0.24.0
   ```
   ```sh
   cd JERICHO-Mod-carhacks && git checkout jer/role-car-select && git am /path/to/JERICHO/docs/patches/carhacks/*.patch && git push origin jer/role-car-select
   ```

3. **Pin the exact new commits here**, by branch name:

   ```sh
   for m in "bake cop-lightbar" "mp 0.24.0" "carhacks jer/role-car-select"; do set -- $m; git -C JERICHO/MODS/$1 fetch origin "$2" && git -C JERICHO/MODS/$1 checkout --detach FETCH_HEAD; done
   git add JERICHO/MODS/bake JERICHO/MODS/mp JERICHO/MODS/carhacks
   ```

   The bake pin then also takes the 6 commits already on `cop-lightbar` past
   `a68bc81` (OBJ winding, police voices bank, wheel island sharing, ...). To
   take the patches alone, apply them on `a68bc81` on a new branch instead.

4. **Build it**: Actions → Build → *Run workflow* on the branch, `release_tag`
   blank (artifacts only: no tag, no release).

## Do not use `tools/update_mod_submodules.sh` (or `publish_release.ps1`'s step 0) on `0.14` yet

Both move every core mod to the tip of the branch `.gitmodules` names, and on
`0.14` none of those is the line the pins come from:

| submodule | `.gitmodules` branch | its tip | the pin's branch | what the script would build |
|---|---|---|---|---|
| `bake` | `betamiami` | `dc5aaf8` | `cop-lightbar` | an OLDER tree: 121 files differ, about 29,500 lines gone (the d1cars era) |
| `mp` | `main` | `435992c` | `0.24.0` | `main`, not the 0.24.0 line 0.14 was tested with |
| `carhacks` | `main` | `474bb29` | `jer/role-car-select` | a diverged history (the pin is not an ancestor) |
| `crumple` | `main` | `75d4c10` | `cursor/mp-invuln-immunity-528a` | a diverged history (the pin is not an ancestor) |

`publish_release.ps1` runs that advance before it builds, so cutting `0.14` with
it today would ship mod code nobody tested. Either point those `branch =` entries
at the pins' branches, or merge those lines into the tracked branches first.
