# Caine's Crossfire tools

Everything in this folder is for driving and inspecting the game from a terminal. All
of it is source-of-truth here; copies for convenience also sit in
`src_rebuild/bin/Release_dev/`.

## Debug flags the engine understands

These only exist in `Release_dev` (they are inside the engine's `DEBUG_OPTIONS`
block). All default to off, so shipping behaviour is untouched.

| flag | what it does |
|---|---|
| `-frames N` | run N gameplay frames, then exit **cleanly** (that is the point: `exit(0)` runs `atexit(PsyX_Shutdown)`, which finalises the log — a kill throws the log away) |
| `-seed N` | pin every module's run randomness, so two runs are comparable and an A/B diff means something |
| `-level <city>` `-car slotN` `-mp 0\|1` `-weather <w>` `-time <t>` `-gamemode <g>` | the pre-existing boot options |
| `-vramview [frames]` | open a second window showing the live VRAM, refreshed every frame, and re-dump `vram_live.tga` every N frames (default 15) for `vramdump.py`. Independent of `-level` |
| `-console` | attach a Win32 console (sent to the bottom of the Z-order) showing the engine log live, not just in the session log file |

Note `-car <model>` is the **model number**; `-car slotN` is a **frontend** slot.
`carNumLookup` makes `slot5` = model 0 and `slot7` = model 9 — see
`carhacks/VEHICLES.md` for the full vehicle map.

Every `-frames` run ends with one parseable line, which is what harnesses should read
instead of guessing at prose:

```
JERICHO-RUN: level=HAVANA car=1 frames=300 seed=7 status=ok
```

A run that crashes or hangs never prints it, so **its absence plus a timeout is the
failure signal**. Note `-frames` counts gameplay frames; the same tick is called from
the frontend state as well, but a frontend run does not load in every environment.

## Scripts

| script | what it does |
|---|---|
| `devcheck.sh [frames]` | build, run the cross-city scenario matrix, print one verdict. Exit 0 = all clean. Restores your `carhacks.ini` afterwards |
| `arena_test.sh [frames]` | one random city/car/weather/time arena run. `SEED=N` replays an exact scenario, because the seed picks the scenario too, not just module randomness |
| `levpages.py <city.LEV>` | read a level file's citylumps and segment sizes without launching the game |
| `levmodels.py <city.LEV> ...` | which car models each city ships (from `LUMP_CAR_MODELS`), plus its `carTpages`/`specTpages`. The data behind `carhacks/VEHICLES.md` |
| `vramdump.py <tga> [--png out.png] [--rect X Y W H label] [--log L --lev V]` | decode a VRAM dump: per-rectangle stats, a viewable PNG, and a palette check that proves an imported car's CLUTs are its own. Feed it `vram_live.tga` re-dumped by `-vramview` |
| `vrammap.py <tga> [<tga> ...] [--log L]` | **where the 1 MiB of VRAM goes**: classifies every 64x64 cell of one or more dumps as never-written / static / in-use, prints the map and the largest free rectangles, and puts each rectangle the code claims (with `file:line`) next to what the dumps show - so a region reserved but never written shows up as `RESERVED BUT UNUSED`. Read it before changing any VRAM layout; `vrammap.py -h` prints the method |
| `levpalette.py <city.LEV> [--out DIR]` | a city's **default car palettes** from `LUMP_PALLET`: a swatch PNG + a text table, so "what the car should look like" is a diffable file |
| `cardump.py <tga> [--log JERICHO.log] [--lev SRC.LEV] [--out DIR] [--texnum N]` | the **last run's** imported car textures: renders each imported set's page under each of that car's palettes (plus the run's actual page CLUT as a control) to PNGs, to compare against `levpalette.py`'s defaults |
| `crosscheck.py <run text> [--tga vram_dump.tga] [--lev SRC.LEV]` | assert the three cross-city invariants on ONE run: no imported page in the WORLD's slots, no world eviction, no imported set resolving to a HOST `civ_clut` row, and (with `--tga`) each pinned page still present with matching CLUTs. Exit 0 = held, 1 = violated, 2 = no import in this run |
| `launch_*.bat` | boot a specific scenario for playing. `test [frames]` makes it self-terminate and print a replayable seed; `dry` prints the roll without launching or writing config |
| `_enable_module.bat <id>` | turn a module ON in the **bin** copy of `JERICHO/CONFIG/modlist.ini` — the copy the game reads. Called by the cross-city launchers, which are useless without `cainescrossfire` and used to rely on the bin mirror happening to agree with the repo |
| `launch_havana_rio_police.bat` | drive RIO's police car (model 0) in HAVANA: import into resident slot 0, `-car 0`. `[model]` tries another Rio body |
| `launch_rio_havana_police.bat` | drive HAVANA's police car (model 0) in RIO: import into resident slot 3 (Rio's own model 0 lives there, so that is the slot to replace), `-car 0` |

## House rules these follow, learned the hard way

- **Never kill by image name.** `taskkill /IM REDRIVER2_dev.exe` also kills the user's
  own session. Harnesses here let the game exit itself; the only kill is PID-scoped
  and only fires on a genuine hang.
- **Never delete the session log** — the user's sessions write it too. Snapshot it.
- **The session log is `<appName>.log`, and this build's app name is JERICHO**
  (`PsyX_Initialise("JERICHO", …)`, `PsyX_main.cpp:380`) — so the live file is
  `JERICHO.log`. A `REDRIVER2.log` may still sit in the folder from an older build
  name and look authoritative; grepping it silently reads a stale file. Prefer
  capturing the run's **stdout** (which `printInfo` also writes), which is correct
  and per-scenario.
- **A launcher's `start` detaches**, so its PID is unknowable afterwards: never kill
  after a launcher run. That is why the test modes self-terminate instead.
- **Confirm the build before trusting behaviour.** A failed build leaves the previous
  exe on disk, which silently ignores any flag it predates — this has produced
  "the flag did nothing" more than once. Check the link line or the exe timestamp.
- **The session log is flushed at close; a kill discards it.** A kill throws the whole
  session away, and a stale line-count boundary reads nothing — so runs use `-frames`
  and exit by themselves (see the name trap above).
- The engine's own `printInfo` lines are the evidence: `JERICHO-RUN:` for a verdict,
  `cross-city:` for imports, and `JERICHO-CHECK`-style invariants where they exist.
