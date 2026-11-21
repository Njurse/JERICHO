# Fix-everything patch: the plan this branch carried out

This is the plan approved before the work began. Live status, and corrections to the
audit, are in `docs/FIX_PLAN.md`. The work was first done on v0.13.4 and then ported
onto `0.14`; the differences from that port are noted in `FIX_PLAN.md`.

## Context
The audit in `docs/FIX_PLAN.md` found bugs, doc drift and CI problems. The estimate
was about 2–3 hours for everything that can be fixed and checked in a Linux container
with no Driver 2 game data. The bigger roadmap items take days to weeks and are not
part of this patch. Fixes that belong in the mod repositories (mp, carhacks, bake)
are delivered as patch files, not pushed there.

## Tier 1: fixes in this repo, tested here
- **Silent hook drop:** `jerCtxRegisterHook()` in `src_rebuild/Game/C/JERICHO/src/jer_system.c` dropped hooks past 256 without a word. It now logs once with the module id and event; the README limits table says so.
- **Hooks added during dispatch:** a hook registered while `jerCtxFire()` runs is queued and added once dispatch ends.
- **Required-mods guard:** `bake` is now in `JERICHO_REQUIRED_MODS` (`src_rebuild/premake5.lua`) and in `src_rebuild/Game/C/JERICHO/src/jer_required_mods.h`.
- **Generated registry:** `src_rebuild/Game/C/JERICHO/gen/jer_registry.c` is untracked and gitignored. CI generates it before `tools/check_release_mods.py` runs.
- **Doc drift:** `tools/README.md` and `JERICHO/MODS/README.md` match the code (release set, the `bake` name, the version scheme, artifact names).
- **Checkers:** `tools/doccheck.py` fails on retired module ids (`d1cars` → `bake`), and CI plants a fault to prove the check works. The custom-id collision check turned out to exist already.
- **CI:** a serial build retry that succeeds no longer posts error annotations.

## Tier 2: repo cleanup (needs the owner's OK first)
- Remove the committed dependency archives (`OPENAL.zip`, `SDL2.zip`, `JPEG.zip`, `PREMAKE.zip`) and the duplicate `libs/` copies, once it's confirmed that `windows_dev_prepare.ps1` and CI download all of them.
- Remove the stray one-off scripts, and the committed `obj/` build files under `libs/`.
- Remove or fix the old REDRIVER2 build configs (appveyor, flatpak, Docker).

## Tier 3: fixes in the mod repos, as patch files
- `docs/patches/{mp,carhacks,bake}`: `git format-patch` exports made against the pinned commits. `docs/patches/README.md` says how to apply them.

## Tier 4: roadmap (not part of this patch)
| Item | Rough size |
|---|---|
| Unit tests for the mod runtime, plus warnings as errors in JERICHO code | 2–4 h |
| Safe mode: remember which mod crashed, offer to disable it on next launch | 3–5 h, plus Windows testing |
| Protocol versions and feature flags in mp | 1–2 days, needs multiplayer testing |
| Give separately loaded mods access to engine data, so crumple and carhacks can become libraries | days, plus runtime testing with game data |
| Move the core mods into this repo | a repo-structure decision, about half a day of mechanics |

## Verification
- `tools/doccheck.py`, `check_release_mods.py`, `check_mod_api.py`, `check_mod_interface.py` and `check_host_api_abi.py` all pass.
- Planted faults fail where they should.
- A clean Linux build of `make config=release_x64` and `release_dev_x64` in `src_rebuild/build` succeeds.
- `src_rebuild/Game/C/JERICHO/test/run_tests.sh` passes for both configurations.
- The Windows build and runtime behaviour are covered by this branch's CI and by people with the game data.
