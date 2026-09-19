# How the cross-city hack works, end to end

Written to be followed, not admired: this is the train of logic, in the order the
pieces actually run, with the traps that cost real time recorded where they bite.

## The problem in one paragraph

A car model's polygons name *texture set numbers*. A set number is an index into the
engine's two global tables, `texture_pages[128]` and `texture_cluts[128][32]` — so **a
set number can only mean one thing at a time**, and the level you are playing already
means something by some of the numbers another city uses. Meanwhile VRAM is the entire
constraint: the 19 slot positions in `tpagepos[]` tile *all* of texture memory (with the
framebuffer, the sky and the CLUT column occupying the rest), so there is no free
region to put a foreign vehicle's pages in.

## The pipeline, in order

1. **The mod asks** — `carhacks` answers `JER_EVENT_CAR_DATA_SOURCE` (fired inside
   `SetupResidentModels`, mission.c) with `models[slot]` and `modelSource[slot]`: "put
   this city's model N in resident slot S". `InitCarImport()` then reads that city's
   `.LEV` and holds its car-models / palette / page-list blocks for the level.

2. **Geometry** — `ProcessCarModelLump` builds the slot from the source city's
   `LUMP_CAR_MODELS`, exactly as it does for the level's own cars. Nothing is
   interpreted; the format's own offset table is used.

3. **Colours** — `.LCF` gives the `CAR_COSMETICS` for that model, so the car wears its
   own shape *and* its own paint table. (An earlier attempt also merged the source
   city's palettes into `civ_clut`; that was removed, because `civ_clut` is
   `[8][32][6]` — eight rows the host level already uses — so merging retextured the
   *local* cars.)

4. **Which texture sets** — `carTpages[srcCity][0..5]` for a civilian body,
   `specTpages[srcCity]` for a special one. This is the engine's own answer; it never
   walks polygons, and neither do we. (A polygon walk was written and abandoned: car
   model polys are *compressed*, and a `PolySizes` walk stalls from polygon 5 on a
   zero entry, reporting nonsense forever.)

5. **Where the pages go** — draw time, not load time. `CarImportPin` (called from
   `draw.c` just before the car draw loop) places each wanted page: a genuinely free
   slot if there is one, otherwise it **evicts a world page** — safe because the world
   is demand-paged and re-streams what it needs. Evicting clears `tpageloaded[held]`,
   which is the engine's own "not loaded" marker and what makes it reload.
   Placement is *after* the world has drawn, so the world keeps its pages for its own
   frame and the car gets its own for the car pass.

6. **Numbers the host owns are re-indexed** — the page loads at a free index (110+;
   city car sets live at 10..68) and `CarSetRemap` translates the car's own polygons
   onto it as they are converted into engine form (`buildNewCarFromModel`). The remap is
   armed **per car**, because host cars whose set numbers collide must keep their own.

7. **Enforcement** — every page upload funnels through `LoadTPageAndCluts`, so an
   upload aimed at a rectangle an imported page owns is refused there
   (`sCarPageOwned`). Brutish on purpose; the cost is that a world stream aimed at that
   rectangle goes nowhere. Inert with no import: nothing is owned, the guard cannot
   fire, and a stock run's page state and `civ_clut` checksum are unchanged.

## The instruments, and how to use them

| what | how |
|---|---|
| drive a Rio special body in Havana | `tools/launch_havana_rio_police.bat 9\|10\|12 [test [frames]]` |
| any cross-city combination | `tools/devcheck.sh [frames]` — exits non-zero on failure |
| where the player's car came from | `JERICHO-RUN: level=… carslot=7 model=9 …` then the engine's `slot 7 geometry from RIO model 9` |
| where imported pages landed | `cross-city: pinned set 77 index 77: slot=14, rect=(512,0), page=0008 …` |
| what is actually in VRAM | `JERICHO_DUMPVRAM=1` then `tools/vramdump.py vram_dump.tga --png out.png` |
| replay a run exactly | `-seed N` (the seed picks module randomness) |

## Traps, all of which cost time

- **`ap.model` is a resident SLOT index, not a model number** (`gCarCleanModelPtr[8]`).
  Reporting it as "car" made a player in the imported special slot look domestic, and
  four test scenarios "failed" for a defect that did not exist. The summary now prints
  `carslot=` and `model=`.
- **The mission header assigns `PlayerStartInfo[0]->model` at `mission.c:573`**, before
  `SetupResidentModels` (called at `:806`) — so a module's `wantedCar` arrived too late
  and was silently replaced by the level's own car.
- **The resident search took the first slot holding a model.** Levels list models twice
  (Havana is `1 2 3 3 4`), so a civilian import lost to the host's copy; it now prefers
  a slot with a source city.
- **The 19-entry `tpagepos` list runs out before the slot indices do**, and
  `IncrementTPageNum` then leaves the position unchanged — so spot placement by slot
  index (`tpagepos[slot]`, as `spool.c:1722` does) rather than by walking `tpage`.
- **Never evict a page we imported.** Our own sets would fight over one rectangle; the
  guard checks host car sets, the host's special pages, *and* our own indices.
- **The module key is `cainescrossfire`, not `combatd2`** — the folder was renamed. A
  `combatd2 = 1` line in `modlist.ini` silently enables nothing.
- **Batch**: `> file echo text` may not write, and an `echo` containing parentheses
  inside a parenthesised `if` block breaks the parse of everything after it.
- **`REDRIVER2.log` is truncated at session start and flushed at close** — a kill
  throws the session away, so runs use `-frames` and exit by themselves.

## Still open

- **UV bleeding** on imported cars, reported by eye. The leading candidate was our own
  pages evicting each other (now fixed); the next check is the dump: decode the
  rectangle the `pinned set …` line names and see whether it still looks like a page.
- `civ_clut[carid][texture_id][0]` in the poly conversion still reads via the original
  set number, so one cache entry can hold the host's CLUT for a re-indexed set.
