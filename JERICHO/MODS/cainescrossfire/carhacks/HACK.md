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
- **`REDRIVER2.log` is truncated at session start and flushed at close** — a kill
  throws the session away, so runs use `-frames` and exit by themselves.

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

## Still open

- The victim is still chosen greedily (first wasted car page, round-robin). A car page
  that a *live* model names can still only be had by evicting a world page — the next
  step is to weigh "is the car that uses this page on screen" the same way, i.e. a real
  pool over the host's car pages. `sCarPageClaimFrame` / `CAR_PAGE_CLAIM_FRAMES` and the
  `UNUSED` slot map are the pieces already in place for it.
- The thrash meter is the thing to watch. If `page re-uploads` in the final page state
  grows with the frame count, something is still taking pages back — check the two
  `spool.c` sites first, since they bypass `LoadTPageAndCluts` by design.

## Discovery: an imported car's GT CLUT word is a CLUT *id*, not a palette row

`GetCarImportPallet()` holds the imported city's `LUMP_PALLET` — the car palettes —
and **nothing read it**: 14792 bytes for Havana, buffered and dropped. That is the
imported car's only source of body colour, so this is the "textures load but the
palette is wrong" bug.

The second half of the story is the *encoding*, and it is why my `directClut` shortcut
turned out to be the right shape:

    cross-city: GT3 imported: clut_uv0=043cff34 -> page CLUT row 1084, emitted 043cff34 (verbatim).
                The host civ_clut path would use row 1084 -> 0000

For a **host** car, `clut_uv0 >> 16` is a small palette *row index* (0..30) that indexes
`civ_clut[carpal][tex][palette]`, with the CLUT id substituted in the high word of the
emitted prim. For the **imported** model it is `0x043c` = 1084 — a whole `GetClut(x,y)`
id: `y = 1084 >> 6 = 16`, `x = (1084 & 63) << 4 = 448`, i.e. VRAM (448,16). Feeding that
to the host path indexes `pciv_clut[1084]` — far out of range — which is why the host
path emits `0x0000`.

So the imported model carries its CLUT *location* in the poly, exactly like `FT3` does,
and every body poly of the car points at one id. The remaining question is therefore
not "how do we map it" but "what is loaded at (448,16)": if the level's own CLUT area
already holds the imported palette's colours there, the fix is mapping-only; if not,
the imported `LUMP_PALLET` has to be uploaded to the positions the imported models name.

### Settled: `clut_uv0 >> 16` is a civ_clut *index*, and the palette is not in the page

Measuring the same field on a **host** car settles it. Stock run, Rio, the player on
Rio model 0 (so the host path runs):

    cross-city: GT3 HOST     clut_uv0=007e9163 hi=126  -> pciv_clut[126]  = 5abf
    cross-city: GT3 IMPORTED clut_uv0=043cff34 hi=1084 -> pciv_clut[1084] = 0000

`civ_clut` is `u_short[8][32][6]` = 1536 entries and `pciv_clut = &civ_clut[1]`, so both
indices are *in range* — they are not `GetClut()` ids at all. A host car uses a small
index (126) that resolves to a real CLUT id (`0x5abf` = a CLUT at (1008,362), i.e. the
level's **palette area**); the imported model's index (1084) lands in the same table
where nothing is loaded, and reads 0.

Two things follow, and they close off the cheaper fix:

1. The car's body colour comes from `civ_clut`, which the level fills from its
   `LUMP_PALLET` (`ProcessPalletLump` → `LoadImage` into the palette area). It does
   **not** come from the texture page's own CLUTs — those live at (960,471)/(992,478)
   and the model never names them. So the imported `LUMP_PALLET` really does have to be
   uploaded; the fix is not mapping-only.
2. The index is baked into the model's geometry. `ProcessImportedPalette` cannot simply
   pour the imported palettes into `civ_clut`'s existing eight rows — that is exactly
   the collision that repainted the host's own cars. The index has to be moved into
   rows the host does not use, which means either offsetting the imported model's index
   at build time or resolving it against the imported pallet there.


