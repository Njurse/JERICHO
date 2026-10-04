# Car Hacks tools

Everything in this folder belongs to the **carhacks** module — the cross-city vehicle
import, the vehicle-availability hacks, and the VRAM work that grew out of them. It
used to live in `cainescrossfire/tools/`; it is here now because carhacks is its own
addon and Caine's Crossfire only *leverages* it (it declares carhacks as a
dependency — see `../mod.toml` and `cainescrossfire/mod.toml`).

All of it is source-of-truth here. The build's postbuild strips `MODS/<mod>/tools`
from the `bin/<config>/` mirror, so run these from the repo, not the bin copy.

Which run needs which module on: the VRAM/palette and cross-city tools need
`carhacks` (and, for the imports to be visible, the module that drives them);
the lower half pool/CC tools that stayed behind live in `../../cainescrossfire/tools/`.

## The VRAM / car-data tools

| tool | what it does |
|---|---|
| `vrammap.py <tga...> [--log JERICHO.log] [--lev L.LEV]` | the offline VRAM cell map from dumps: static / changing / never-written cells, the free rectangles, and every code claim next to what the dumps show |
| `vramdump.py <tga> [--png out.png] [--rect X Y W H label] [--log L --lev V] [--samples]` | decode one VRAM dump: per-slot map, page stats, a viewable PNG, and the palette check that proves an imported car's CLUTs are its own. Feed it `vram_live.tga`, which `-vramview` re-dumps |
| `cardump.py <tga> [--log JERICHO.log] [--lev SRC.LEV] [--out DIR] [--texnum N]` | the **last run's** imported car textures: renders each imported set's page under each of that car's palettes (plus the run's actual page CLUT as a control) to PNGs, to compare against `levpalette.py`'s defaults |
| `vram_baseline.txt` | the recorded *streamed* VRAM baseline a single dump cannot capture (see `../docs/VRAM.md`) |
| `hostdiff.py <stock.tga> <mashup.tga> [--log L]` | **did a mashup touch anything the HOST owns?** Dumps the same level twice (once with `cross_city_vehicles = 0`) and asserts the host's page slots and the level font are byte-identical. Exits 1 if not, and reports the two things allowed to differ (the display buffers, and the CLUT column where the import's palettes live). This is the check behind `docs/VRAM.md` §0's "vs a stock run" rows |
| `levmodels.py <L.LEV>` / `levmodels.py --has-model N <L.LEV>` | read a level's car-model table — and answer "does this city ship model N", exit 0/1/2, which is what the launchers gate on. **Models 5, 6 and 7 exist in NO city**; each ships civilians 0..4 and its own specials |
| `levgeom.py`, `levpages.py`, `levpalette.py` | the rest of a level read straight from its `.LEV`/`.LCF`: the geometry, the texture pages, and the palette lump |
| `make_vram_issues_png.py` | renders the annotated VRAM figure `../docs/vram-issues.png` from a dump + `vrammap.py`'s rectangles |
| `chk_vram_checkpoint.sh <level> <mix> <tag> [note]` | the dated VRAM checkpoint: runs a mashup with `JERICHO_DUMPVRAM=1`, reads the numbers from that run's own log, stamps them into the figure, and appends an index row |

## The cross-city import checks

| tool | what it does |
|---|---|
| `chk_suite.sh [frames]` | **the carhacks acceptance test**: build, then run every row - a stock control, the imported PLAYER once per host city, a 3-city `city mix`, a traffic import - and print one verdict. Exit 0 = all clean. Restores your `carhacks.ini` and the modlist afterwards. `SKIPBUILD=1` assumes the exe is current |
| `crosscheck.py <run text> [--tga vram_dump.tga] [--lev SRC.LEV]` | assert the cross-city invariants on ONE run: no imported page in the WORLD's slots, no world eviction, no imported set resolving to a HOST `civ_clut` row, and (with `--tga`) each pinned page still present with matching CLUTs. Exit 0 = held, 1 = violated, 2 = no import in this run |
| `measure_cities.sh` | the import's budget, one line per run, from the engine's own log lines |
| `chk_mashup.sh [host] [mix] [frames]` | a true 2–4-city mashup into several resident slots, and the log greps that say whether each import placed |
| `chk_all_cities.sh`, `chk_single_city_playtest.sh`, `chk_mp_foreign.sh` | the per-city, single-city playtest and mp-pair variants |

## Launchers

They all `cd` into `bin\Release_dev\` and `start` `JERICHO_dev.exe`. `test [frames]`
makes a run self-terminate and print a replayable seed; `dry` prints the plan without
writing config or launching.

| launcher | what it does |
|---|---|
| `launch_imported_player.bat [level] [srcCity] [model] [dry\|test [frames]]` | **any level, always an imported player car.** The source city defaults to a foreign one (the level's own + 1, mod 4). Writes the matching `import = <slot>:<city>:<model>` and passes `-car <model>` — a module cannot pick the player's car (`wantedCar` arrives after `mission.c:573`/`SetupResidentModels`); see `../docs/HACK.md`. **It refuses (exit 2) if the source city does not ship the model**, before the dry exit so `dry` validates too — a request for model 5/6/7 can never be satisfied and used to be a silent no-op ("the script doesn't replace the car") |
| `launch_havana_rio_police.bat [model] [dry\|test [frames]]` | drive RIO's police car (model 0) in HAVANA: import into resident slot 0, `-car 0`. `[model]` tries another Rio body |
| `launch_rio_havana_police.bat [model] [dry\|test [frames]]` | drive HAVANA's police car (model 0) in RIO: import into resident slot 3 (Rio's own model 0 lives there, so that is the slot to replace), `-car 0` |
| `launch_mp_foreign_car.bat` | the multiplayer pool with a **guaranteed** foreign player car, drawn from the source city's whole usable roster (civilian bodies 0..4 as well as the specials 8, 9, 10, 12, plus 11 where the city has it). Civilian bodies are imported slot-for-slot over slots 0..4, specials into spare slot 5. Supports `dry` |
| `launch_mp_random_mix.bat` | the same pool mix with the roll left in: a random pool city, a **different** city to import from, and a random roster. `dry` prints the roll |
| `cycle_vehicles.bat <city> [frames]` | walk a city's model numbers to find what each one is (this is how `../docs/VEHICLES.md` was measured) |

Both `launch_mp_*` launchers write `JERICHO/CONFIG/carhacks.ini`, so two runs at once
clobber each other's roll. `_enable_module.bat <id>` turns a module ON in the **bin**
copy of `JERICHO/CONFIG/modlist.ini` — the copy the game reads — which is why the
launchers call it: the repo's `modlist.ini` pins gameplay modules OFF.

## House rules these follow, learned the hard way

- **Never kill by image name.** `taskkill /IM JERICHO_dev.exe` also kills the user's
  own session. Harnesses here let the game exit itself (`-frames`).
- **Never delete the session log** — the user's sessions write it too. Snapshot it.
- **The session log is `<appName>.log`, and this build's app name is JERICHO** — so the
  live file is `JERICHO.log`. A `JERICHO.log` may sit beside it from an older build
  name and look authoritative; prefer capturing the run's **stdout**.
- **A launcher's `start` detaches**, so its PID is unknowable afterwards: never kill
  after a launcher run — use `dry`, or a direct launch with a captured PID.
- **Never use `-car slot9`** (car model 11, the reserved truck slot): no city ships data
  for it in Chicago, and forcing it kills the game during load with no dump.
