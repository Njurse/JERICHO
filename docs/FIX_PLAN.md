# JERICHO fix plan (audit of 2026-11-21, paused after the first pass)

Scope: the superproject at `7c9f4c1` (v0.13.4) plus the pinned submodules
(`PsyCross`, `mp`, `carhacks`, `crumple`, `bake`). Commit messages were treated as
leads, not as truth; each item below says what was actually checked.

## Ported onto `0.14` (branch `c-vibes/fix-everything-0.14`)

The fixes were first made on v0.13.4, then re-checked and ported onto `0.14`
(`2f01241`), which shares that base. Differences on `0.14`:

- Still present on `0.14` and fixed here: the hook dispatch bugs, `bake` missing from
  the required mods, the tracked generated registry, the CI retry annotations, and
  the stale `tools/README.md` release count.
- Already fixed upstream: the dedicated server's `core_mods` (mp now lists
  `mp, carhacks, pedhacks` and treats `d1cars` as `bake`'s old alias), the CI pin
  comments, and most of the `tools/README.md` version and pin text.
- **New on `0.14`, fixed here:** `cainescrossfire/profiles/rows/*.c` were orphaned
  when profiles moved to JSON (`aca6b643`). They still use the removed per-car enum,
  so **both Linux builds failed** (`'CD2_VEH_AVALANCHE' was not declared`). They are
  deleted; `generated_profiles.c` defines all 11 cars, and nothing referenced the old
  rows.
- **New on `0.14`, not fixed (needs the author):** `tools/test_vehicle_profiles.py`,
  which the Checks job runs, needs `JERICHO/MODS/cainescrossfire/vehicles/bank.json`.
  That folder was never committed, so the check fails on a clean checkout.
- Tests: `JerAI` is now `Alleycat`, so the runner uses `test_alleycat_path`. It also
  runs `0.14`'s new `test_world_reload` and `test_d1cars`, which no CI job ran either.

## Status (first pass, on v0.13.4)

| Item | State |
|---|---|
| P0.1 dedi example config refuses every client | **patch ready**: `docs/patches/mp/` (mod repo); verified the old config refused a core-suite client and the new one admits it |
| P0.2 silent hook drop | **fixed**: logged once per pass with module and event; covered by `test_dispatch.c` |
| P0.3 required-mods guard missed `bake` | **fixed**: `premake5.lua` and `jer_required_mods.h` |
| P0.4 hooks registered mid-dispatch | **fixed**: queued until the outermost fire returns. The bug was real: on the old code the registrar ran twice |
| P1 `d1cars` rename | **fixed** in this tree; `doccheck.py` now fails on retired ids. **patches ready** for mp, carhacks and bake |
| P1 `tools/README.md` drift | **fixed** |
| P1 release reproducibility | **corrected**: CI already builds the pinned commits. Only the comments and `tools/README.md` said otherwise, and both are fixed. A typo'd pin now fails at `git fetch origin <sha>` |
| P1 custom-id collisions | **corrected**: `check_mod_interface.py` already scans the engine headers and catches the +45 clash (re-tested). No change needed |
| P2 CI retry annotations | **fixed** |
| P2 flaky parallel link | open: still retried, not fixed |
| P2 tracked generated registry | **fixed**: untracked; CI generates it. A stale copy broke the `Release_dev` link here (`antfarm`/`gaildrv2` entries), so this was more than cosmetic |
| P2 repo hygiene | open: needs the owner's OK (deletions) |
| P3 warnings, `jer_modstore.c` split, generated SDK headers | open |
| New: host-side tests never ran in CI | **fixed**: `test/run_tests.sh` runs the dispatcher test plus `test_car_release`, `test_ai_path` and `test_jerai_path` (C and C++) on the Linux job |

## What was verified

| Check | Result |
|---|---|
| Linux `release_x64` build (premake 5.0.0-beta1 gmake2, same as CI) | **builds**; `bin/Release/jericho` produced, **114 compiler warnings** |
| `tools/doccheck.py`, `check_mod_interface.py`, `check_host_api_abi.py` | pass |
| `tools/check_release_mods.py`, `check_mod_api.py` | pass with submodules fetched; fail (expected, clearly) without |
| `bake/tools/ped/check_debug_independence.py` | pass |
| SDK headers vs engine headers | identical; `jer_events.h` exists only in the SDK |

**Not verified yet:** Windows build, `release_dev_x64`, the runtime addon `.so`
build (`JERICHO/build.py mods`), and anything at runtime (no Driver 2 data here).

## P0 - real bugs

1. **The dedicated server's example config rejects every client.**
   `mp: tools/servers/dediserver/dediserver.example.json` sets
   `"core_mods": ["mp", "carhacks", "d1cars"]`. The `d1cars` module was renamed
   `bake`, so no client ever reports `d1cars`, and `mods_match()` in
   `dedi/core.py` refuses everyone once `mod_check >= 1`. Fix in the
   JERICHO-Mod-MP repo (also the docstring in `dedi/core.py` and
   `test_console.py`), then bump the pin. Before choosing `bake` as the
   replacement, confirm the client's HELLO mod rows include runtime-DLL
   addons; if they don't, `bake` can't be a core mod either and the list
   should be `["mp", "carhacks"]`.
2. **Hook registration fails silently past 256 handlers.**
   `src_rebuild/Game/C/JERICHO/src/jer_system.c` `jerCtxRegisterHook()` returns
   with no log when `gHandlerCount >= JER_MAX_HANDLERS`. The README's Limits
   table admits it. A module loses events with no trace. Fix: log the module
   id and event once; consider growing the table or returning a status. Then
   update the README row.
3. **The "a missing mod must not build a smaller game" guard is incomplete.**
   `src_rebuild/premake5.lua` `JERICHO_REQUIRED_MODS = { "mp", "carhacks", "crumple" }`,
   but `JERICHO_RELEASE_MODS` also ships `bake` (a submodule) and `pedhacks`.
   A local checkout without `bake` builds a release without it, which is the
   exact failure that comment says must be impossible. Only CI's own
   `mod.toml` check catches it. Add `bake` to the list and to
   `jer_required_mods.h`.
4. **Hooks registered during dispatch can run twice or be skipped.**
   `jerCtxFire()` walks `gHandlers[]` by index, and `jerCtxRegisterHook()`
   insertion-sorts into the same array. A handler that registers a hook
   mid-fire shifts entries under the loop. Fix: defer registrations made
   during a fire, or document and assert that it never happens.

## P1 - the `d1cars` -> `bake` rename was never finished

Stale references break tools, not just docs:

- `mp: tools/pack_lan/make_lan_package_micheal.sh` exits with
  `no d1cars module in the repo`; `make_lan_package.bat`, `sync_lan.bat` and
  the pack_lan README describe a d1cars module that no longer exists.
- `carhacks: tools/bigrig_extract.py` and `tools/slotmap.py` build paths to
  `MODS/d1cars/tools`, which no longer exists.
- `bake: tools/car/README.md` (`T=JERICHO/MODS/d1cars/tools`),
  `add_car_sounds.py`, `bake.py`, `obj2geom.py` comments.
- Superproject: `JERICHO/MODS/README.md:40`, the `premake5.lua` comment above
  `JERICHO_REQUIRED_MODS`, and `tools/README.md` (below).

Prevention: `doccheck.py` passed even though all of these exist. Add a
renamed-ids check (a small table such as `d1cars -> bake`) that greps tracked
files *and* the pinned submodules.

## P1 - `tools/README.md` contradicts the code

- It says a release is "exactly three modules: carhacks, crumple and mp". The
  code ships five: carhacks, crumple, mp, bake and pedhacks.
- "The four public mod source repos (mp, carhacks, crumple, d1cars)" should
  be mp, carhacks, crumple and bake. The CI loop it quotes lists 4 paths;
  CI actually fetches 5 (adds bake and antfarm).
- "`JERICHO_BUILD_VERSION` comes from `git describe --tags --match 'v*' --always --dirty`"
  is no longer true. Since `7710ea8` it is an exact-match `v*` tag, otherwise
  `JERICHO_BASE_VERSION` ("0.13").
- The Linux artifact name `REDRIVER2_*_linux-x64.tar.gz` should be
  `JERICHO_Release_linux-x64.tar.gz`.

## P1 - releases are not reproducible from a JERICHO commit

> **Correction (second pass):** wrong as written. The fetch steps already check out
> `HEAD:<path>` (the pin); the comments above them, and `tools/README.md`, described
> the old branch-tip behaviour. Both now match the code.

CI clones each mod at its pin, then **advances it to the tracking branch's
HEAD** (`tools/README.md`: "a mod fix pushed after the last gitlink bump still
reaches the next download without a JERICHO commit"). So a `v*` tag does not
pin what ships. The history shows the cost:

- `d2145c0`: a stale `bake@betamiami` got compiled in and caused `LNK2001`.
- `de81284`: a typo'd submodule SHA.
- 0.13.4 needed three follow-up "build fix" commits (`f7bfca0`, `b8f8d7e`,
  and mp `d4e824d`).

Plan: build exactly the pinned SHAs. Advance pins only through a reviewed
commit (`tools/update_mod_submodules.sh` already exists for this). Add a CI
step that checks every gitlink exists on its remote, and run the full build on
pull requests before a tag.

## P1 - custom-id collisions still slip past the checker

> **Correction (second pass):** wrong as written. The checker already reads
> `src_rebuild/Game/C/JERICHO/include/`, and planting the old +45 value makes it fail
> by name. The clash in `b8f8d7e` was caught and fixed.

`b8f8d7e` moved `JER_CARQ_CITYWIRE` from custom +45 to +47 because +45 was
already `JER_CRUMPLE_GET_SCALE`. `check_mod_interface.py` reports "10 unique
id(s)", so it apparently doesn't count ids declared in engine/SDK headers
(`jer_carquery.h`, `jer_crumple.h`). Widen it to cover every
`JER_EVENT_CUSTOM + n` in the engine, the SDK and the mods. Then plant a
collision in the CI fault test.

## P2 - CI correctness

- **The serial-retry step paints a green build red.** In `build.yml` (Linux
  build), the `-j1` retry pipes its last 10 lines out as `::error`
  annotations even when the retry succeeds. Capture the output to a file and
  annotate only when `rc != 0`.
- **The flaky parallel link is retried, not fixed.** The post-build mirror
  lives in the link rule and races with the exe's output path. Move the
  mirror into its own premake post-build step or project, then drop the
  retry.

## P2 - a committed generated file dirties every build

`src_rebuild/Game/C/JERICHO/gen/jer_registry.c` is generated by premake but
tracked. Its dev table lists whatever optional submodules the last committer
happened to have checked out (`antfarm`, `gaildrv2`). Running `premake5 gmake2`
on a plain CI-style checkout rewrites it (12 to 10 modules), so the tree goes
`-dirty` on every build. That is why CI needs a "settle the version" step. The
release half (`#ifdef JERICHO_RELEASE_MODS`) is stable. Fix: gitignore it and
always generate it, and have `check_release_mods.py` generate its own copy
instead of reading the committed one.

## P2 - repo hygiene (about 17 MB of vendored binaries plus one-off files)

- At the root: committed `OPENAL.zip` (12.7 MB), `SDL2.zip` (2.7 MB),
  `JPEG.zip` (1.1 MB) and `PREMAKE.zip`. Under `libs/`: a full extracted
  libjpeg plus its `jpegsrc.v9f.tar.gz`, SDL2 DLLs, and a committed build
  artifact (`libs/jpeg/obj/x86/Debug/*.FileListAbsolute.txt`). The README
  says deps are fetched and SHA-verified by `windows_dev_prepare.ps1`, and
  says libjpeg is **jpeg-9d**, but the tree carries **v9f**. Pick one source
  of truth (the verified fetch) and remove the rest.
- One-off scripts at the root: `.cd2_patch16.py`, `.cd2_patch23.py`, and the
  `gaildrv2_*.ps1/.bat` files for a submodule whose repo is not published.
  `reasonix.toml` (agent-harness config) is also there; decide whether it
  belongs in the repo.
- Upstream REDRIVER2 leftovers that probably build the wrong app:
  `appveyor.yml`/`.appveyor`, `io.github.opendriver.redriver2.yaml`,
  `.flatpak/`, `Dockerfile`/`dockerbuild.sh`. Verify each, then fix or
  remove it.
- `.gitmodules`: `gaildrv2` points at an unpublished repo, and `antfarm`
  tracks a branch named `0.12` even though it is now store-only. Decide
  whether either belongs.

## P3 - code quality

- Triage the 114 warnings from the Linux Release build. Log:
  `make config=release_x64` in `src_rebuild/build`.
- `jer_modstore.c` is 3,659 lines. Split it into fetch/index, install and UI.
- SDK headers are hand-copied from `src_rebuild/Game/C/JERICHO/include/`
  (`f89ff5f` forgot `jer_carquery.h`; `f7bfca0` fixed it). `doccheck.py` now
  catches drift, but generating `JERICHO/sdk/include` from the engine headers
  would remove the class of bug.

## Suggested order

1. P0 items 2-4 (small, local, testable here), plus the `tools/README.md`
   and superproject `d1cars` doc fixes.
2. The renamed-id and custom-id checker upgrades, so regressions fail CI.
3. P0 item 1 and the submodule-side `d1cars` fixes (upstream mod repos), then
   one reviewed pin bump.
4. CI reproducibility (build pins, verify gitlinks, PR builds) and the retry
   annotation fix.
5. Hygiene and warnings.
