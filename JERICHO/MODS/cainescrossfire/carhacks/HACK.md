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

   **The guard had to reach the spool too**, and that was the last real bug. The
   streamer does NOT go through `LoadTPageAndCluts`: `spool.c` writes `texture_pages[]`
   and calls `LoadImage`/`LoadImage2` directly, at two sites — the "Send palettes"
   block (`spool.c:478`) and the special-slot spool (`spool.c:1712`). The second is
   the one that mattered: it re-uploads the HOST's special-car pages into
   `tpagepos[specialSlot + i]`, and the imported player car IS the special slot, so
   every spool pass repainted the host's pages over ours — and rewrote
   `texture_pages[]`, so the car's polys resolved to a different VRAM rectangle
   mid-frame. That is what "the UVs are bleeding" was. Both sites now check
   `CarPageRectOwned`.

   **The thrash meter proves it.** `CarImportPin` counts re-uploads of pages it has
   already placed, and the dump reports it:

     150 frames: 2 pinned, 2 world pages evicted, 2 page re-uploads
     600 frames: 2 pinned, 2 world pages evicted, 2 page re-uploads

   Two re-uploads is the initial placement of two pages; four times the frames adds
   none. Before the spool guard, this number grew with runtime — the engine kept
   taking the pages back.

## The instruments, and how to use them

| what | how |
|---|---|
| drive a Rio police car in Havana | `tools/launch_havana_rio_police.bat [model] [test [frames]]` (model 0 = the Rio police car) |
| drive a Havana police car in Rio | `tools/launch_rio_havana_police.bat [model] [test [frames]]` (model 0 = the Havana police car) |
| any cross-city combination | `tools/devcheck.sh [frames]` — exits non-zero on failure |
| where the player's car came from | `JERICHO-RUN: level=… carslot=5 model=9 …` then the engine's `slot 5 geometry from RIO model 9` |
| where imported pages landed | `cross-city: pinned set 77 index 77: slot=14, rect=(512,0), page=0008 …` |
| what is actually in VRAM | `JERICHO_DUMPVRAM=1` then `tools/vramdump.py vram_dump.tga --png out.png` |
| what a city's car palettes *should* be | `tools/levpalette.py LEVELS/<CITY>.LEV --out out/` — a swatch PNG + a text table per city, from `LUMP_PALLET` (see `PALETTES.md` §7) |
| the last run's car textures under each palette | `tools/cardump.py vram_dump.tga --log JERICHO.log --out out/` — one PNG per imported set, a row per palette, plus the run's actual page CLUT as a control (`PALETTES.md` §7) |
| whether a run's palettes/pages behaved | `tools/crosscheck.py <run text> [--tga vram_dump.tga] [--lev SRC.LEV]` — the three invariants the engine's own summary misses (exit 0 held / 1 violated / 2 no import) |
| VRAM live, while you play | `-vramview [frames]` opens a second window showing the live VRAM every frame (and re-dumps `vram_live.tga` every N frames, default 15, for `vramdump.py`) |
| replay a run exactly | `-seed N` (the seed picks module randomness) |

## Choosing the player's car

The player's car is **not** chosen by the module. It comes from the command
line: `-car <model>` (or `-car slotN`, the frontend slot). The engine then spawns
the player in the first resident slot holding that model (`InitPlayer`), so an
import into a spare slot plus `-car <model>` makes that car the player's:

```
import = 5:3:9      # RIO model 9 into spare resident slot 5
-car 9              # player spawns in slot 5, the imported car
```

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
- **The session log is `JERICHO.log` in this build, not `REDRIVER2.log`** (`<appName>.log`;
  the app name is JERICHO). A stale `REDRIVER2.log` can sit beside it. The log flushes at
  close, so a kill on a late-flushed run throws the session away — runs use `-frames` and
  exit by themselves, and the text to read is the run's captured stdout.

## The two texture defects, fixed

- **GT4 never had its texture set remapped.** `texture_pages[pgt4->texture_set]` was
  read raw in `buildNewCarFromModel`, while FT3/FT4/GT3 all wrapped theirs in
  `CarSetRemap`. For a re-indexed set the quad sampled the host's page (or the unloaded
  dummy), which is the *"textures are shifted"* artefact. Now wrapped, like its siblings.
- **GT3/GT4 routed their CLUT through the HOST's `civ_clut` rows**, keyed by
  `GetCarPalIndex(raw set)`. The host already owns those rows, so a re-indexed set
  painted the imported car with the host's palette *and* overwrote the host's cache
  entry with the imported CLUT. An imported car now carries a **direct CLUT id** on its
  GT polys, taken from its own page's CLUTs (like the FT path): `CAR_MODEL.imported`
  → `plotCarGlobals.directClut`, and `plotCarPolyGT3*` emits the value verbatim. No
  host `civ_clut` row is read or written for an imported car. Stock cars are untouched
  (`imported` = 0, `directClut` stays off).

## A civilian import pulls only what the car names

A civic body used to request the whole `carTpages` list — **six** sets. The level
leaves five usable slots, so the sixth page never placed, and an unplaced page leaves
`texture_pages`/`texture_cluts` at the **dummy** `(960,0)`/`(960,16)`, which sits inside
a live page — so the car sampled another page's pixels. That was the "contamination".

Now `buildNewCarFromModel` records the `texture_set` of every FT3/FT4/GT3/GT4 poly it
walks (the engine's own `PolySizes` walk, not the abandoned raw-lump scan) into a
per-slot list, and `LoadImportedTPages` imports only those. Havana model 0 in Rio asks
for **2** sets (36 and 21), both place, and the palette check reports MATCH. Special
bodies are unchanged (two `specTpages`, already fits). The list is cleared per level by
`CarImportResetState`, which now runs from `InitCarImport` — before the models are built.

## The page pool: spend what nothing uses (#3, first cut)

An imported page used to pay for its rectangle by **evicting a streamed world page**
(world sets 30/31 from slots 14/15) — and the ownership claim kept it, so the world's
distant textures went stale. It did that because `CarPageFindSlot` only ever looked at
slots `>= nperms`, and by draw time the free spares are already streaming.

But the level loads its **whole** `carTpages`/`specTpages` list up front, and a model
only draws the sets it names. Measured: Rio loads 8 host car pages and **2 of them
(55, 59) are named by no built model** — pure waste, and they sit inside the level's own
range, exactly where the old pass refused to look.

So `CarModelSetsAdd` now records every model's sets (host or imported),
`CarModelSetUsed(set)` answers "does any model name this?", and `CarPageFindSlot` has a
first pass that takes a wasted car page — ignoring `nperms` — before it will evict a
live world page. Result: `2 wasted car pages taken, 0 world pages evicted`.

## Where an imported page may live now (and the guard table)

Placement is now strictly "something nobody is drawing", in this order (`CarPageFindSlot`):

1. a free slot inside the level's own range — `nperms <= idx < slotsused`, never the
   world's pool;
2. a host car page that **no built model names** (the `UNUSED` map, any idx >= nperms);
3. as a last resort, **evict a streamed world page** — logged, and counted in the final
   page state. That is the same thing the engine already does when its own pool is full,
   and it is now safe: `LoadInAreaTSets` no longer offers the car's rectangle, so the
   world re-streams into another slot instead of overwriting the car. The count is the
   cost to watch — not damage.

A page REPLACES a car, so the rectangle that car already used is preferred over all of
the above: `pref[]` (special bodies only) and `CarPinPreferredAllowed` accept a
preferred rectangle **only if it currently holds one of the host special car's own two
pages**. That check is the fix for the worst of the leaks — see below.

**Every upload path asks before it writes.** The old guard was applied to two of four
paths, which is worse than none (the world's page pixels landed on the car's page while
its CLUTs were refused, leaving the world drawing a page pointer that no longer matched
what was at that rectangle):

| path | file | guarded |
|---|---|---|
| `LoadTPageAndCluts` (level load, .TIM override) | `texture.c:553` | always was |
| `SendTPage` CLUT/palette pass | `spool.c:~479` | always was |
| `SendTPage` **page rows + slot table** | `spool.c:~518` | **now** — both halves refused together |
| `SpecClutsSpooled` | `spool.c:~1729` | always was |
| `Tada` **special page rows** | `spool.c:~2023` | **now** |

And `LoadInAreaTSets` (`spool.c:~593`) no longer offers the streamer a slot an imported
page owns (`CarPageSlotOwned`, by slot — the pin's claims are slot-indexed, and
`slot_tpagepos[]` need not equal `tpagepos[]`). Without that the world simply picked the
car's rectangle again the next time that area was streamed.

### The trap that put a car's page on a building

`pref[nsets] = SPECIAL_CAR_SLOT + k` used the **resident-model** constant (7) where the
runtime **texture slot** was meant (`specialSlot`, 11-13 depending on the level). Measured
on Havana with `nperms=12`: an imported special body pinned its two pages onto slots **7
and 8** — two of the level's *permanent* page rectangles — while the map showed slot 8
held a live host car page. `0 wasted car pages taken, 0 world pages evicted` hid it,
because that path never consults the allocator at all. Now it is `specialSlot + k`, and
`CarPinPreferredAllowed` refuses anything that is not the replaced car's own page.

### The trap that collapsed a car's two pages onto one index

`FindFreeSetIndex` answered "first index >= 110 with `tpageloaded[] == 0`", and
`tpageloaded[]` only becomes non-zero when a page is **placed** — which is draw-time. So
two sets imported in one load both saw 110 free and BOTH took it: the second's pixels and
CLUTs replaced the first's, and the car sampled one page for both parts (`CHICAGO set 54
-> index 110` and `set 55 -> index 110` in the log, 16 + 5 CLUT rows fighting for the
same rows in VRAM — the `vramdump.py` MISMATCH). `FindFreeSetIndex` now reserves what it
hands out (`sReservedSet`, cleared by `CarImportResetState`).

## Still open

- **The import's CLUT rows are still carved from the level's own strip.** The pin band
  starts at `max(clutpos.y + 4, 480)`; measured on a full level (`clutpos.y = 482`,
  "imported CLUT rows start at y=486, 5 slots spare") two sets (16 + 5 rows) just fit in
  `486..507`. A bigger import would run off the column, and the runtime team-palette
  allocation shares the same column (it is now capped at `CAR_CLUT_IMPORT_LIMIT` so it
  cannot walk into the import's rows — `texture.c`, `JerichoMakeClutRow`). Reserving the
  import a fixed region, and bounding `clutpos.y += 8` in the slot-band loop, is the
  remaining work here.
- The victim is still chosen greedily (first wasted car page, round-robin). Weighing "is
  the car that uses this page on screen" is the real pool over the host's car pages;
  `sCarPageClaimFrame` / `CAR_PAGE_CLAIM_FRAMES` and the `UNUSED` slot map are in place
  for it.
- The thrash meter is the thing to watch. If `page re-uploads` in the final page state
  grows with the frame count, something is still taking pages back — check the two
  `spool.c` sites first, since they bypass `LoadTPageAndCluts` by design.
- A set the imported model names that is a car page in *neither* city still answers
  `civ_clut` row 0, so those polys keep the host row-0 palette (as a host car's would).
  Nothing is corrupted — the pin now refuses that row and logs `pin - set N resolves to
  civ_clut row M (a HOST row)` — but the colour is the host's. Fixing it needs a bank row
  for a page the engine never treats as a car page.

## The palette half: what cost the time, and where the detail now lives

`GetCarImportPallet()` holds the imported city's `LUMP_PALLET` — the car palettes —
and for a while **nothing read it** (14792 bytes for Havana, buffered and dropped).
That was the "textures load but the palette is wrong" bug, and chasing it produced
the two-bank `civ_clut` design, the `directClut` shortcut that was tried and removed,
and the draw-time pin.

All of that is now documented once, in **`PALETTES.md`**: the `clut_uv0` high word
being a `civ_clut` *index* (and why an FT poly's is a CLUT id), the `(carid-1)*192`
formula, the two banks, `carTpages`/`specTpages`, the shared `clutpos` strip, and the
per-frame re-point. Read it before touching anything palette-shaped.

The shortcuts that cost the time, kept because they will tempt you again:

- **The pin is draw-time; `buildNewCarFromModel` is load-time.** So the cached
  `civ_clut[carid][tex][0]` for an imported set holds the `(960,16)` dummy and has to be
  re-pointed after the page actually uploads (`PALETTES.md` §5).
- **The imported palettes share the level's CLUT strip**, so the pin's own band has to
  start below whatever the level left (`PALETTES.md` §4) — that starvation is what left
  the second imported set unplaced entirely.
- **A `specTpages` page is in neither `carTpages` table**, so `GetCarPalIndex` answers 0
  for it — the host's row (`PALETTES.md` §3).





