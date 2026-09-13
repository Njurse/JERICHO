# Cross-city vehicle imports

> **Format reference:** `FORMATS.md` in this folder documents the file layouts
> themselves — the `.LEV` container and its citylumps table, the 4-byte aligned
> segment walk, `LUMP_CAR_MODELS`, `LUMP_PALLET`, the `.LCF`, texture sets vs
> `texture_pages`/`texture_cluts`, the car draw path, and Python recipes to
> re-measure any of it. Read that first for how the bytes are laid out; this file
> is only about the import feature built on top of them.

Design notes for importing another city's vehicles into a level: what the feature
does, how it is wired, what it costs, and where it is honest about its limits.
Line references are into `src_rebuild/Game/`.

## What it does

A resident car slot can take its **geometry** and its **car colours** from a
*different* city's level file instead of the level's own — e.g. Chicago's school
bus driving in a Havana level. No external tool or data export is involved: the
other city's data is already in its `.LEV`.

It is **per slot**: the module names, for each resident slot, the city that slot's
model should come from, so one slot can be foreign while the rest of the level is
untouched. The per-slot city lives in `JER_ARGS_CAR_DATA_SOURCE.modelSource[]`
(`jer_events.h:521`), read back through `GetCarModelSourceCity(slot)`
(`mission.c:334`). `-1` means "the level's own city".

There is a second, level-wide lever: `sourceLevel` (`jer_events.h:518`) names one
city whose `LEVELS\<city>` folder the **loose-file** loaders (`.MDL`/`.COS`/`.DEN`)
read — that is the separate hand-made-car path, kept for a whole-city override
(`GetCarDataFolder`, `cars.c:1066`). It is not what puts a foreign vehicle in the
level; `modelSource[]` is.

## Where a level's car models actually live

Each city's car geometry is in **that city's own level file** — `LEVELS\<CITY>.LEV`
(single-player) or `MLEVELS\<CITY>.LEV` (multiplayer arenas). It is *not* in the
loose `LEVELS\<city>\CARMODEL_*` files: those are only an override the engine
consults when present (`gContentOverride`), and nothing in the tree currently
ships in the name the engine asks for.

So "a car from another city" means reading that city's level file.

## Reading a foreign level file

`InitCarImport` (`models.c:419`) calls `LoadCarImport` (`models.c:336`), which
uses plain stdio — no CD layer is involved on PC. The sequence:

1. `fopen(gDataFolder + LevelFiles[city], "rb")`; if that fails, retry with an
   `M` prefix (`gDataFolder + "M" + LevelFiles[city]`) for the multiplayer arena
   file (`models.c:349-356`). City order is CHICAGO, HAVANA, VEGAS, RIO
   (`system.c:130`, `:137`).
2. `fseek(fp, 8, SEEK_SET)`, then read the 8-int (`4 × XYPAIR`) citylumps table
   (`models.c:362`). See `FORMATS.md` §1 for the table layout and the container
   fact that follows.
3. Take DATA1 from the table (`table[0]` = byte offset, `table[1]` = byte size);
   reject it if the offset is ≤ 0 or the size is ≤ 8 / > 4 MB (`models.c:368-371`).
4. `malloc` the DATA1 size and read it whole with one `fseek`+`fread`
   (`models.c:377-390`).
5. DATA1's body is itself a container, so its segment list starts 8 bytes in
   (`models.c:395`); `FindLumpSegment` (`models.c:253`) walks it using the
   4-byte-alignment rule of `FORMATS.md` §1 ("Walking segments") to find type 28
   (`LUMP_CAR_MODELS`) and type 25 (`LUMP_PALLET`).
6. The car colours come from a **separate** file, `gDataFolder +
   CosmeticFiles[city]` (`models.c:409`), read whole.

The file handle is closed before the walk; only the mallocs survive.

## What the import carries

Three pieces come from the foreign city, each behind a `GetCarImport*` getter:

| Piece | Where | Getter | Consumer |
|---|---|---|---|
| Car geometry (`LUMP_CAR_MODELS`, id 28) | foreign `.LEV` DATA1 | `GetCarImportModels(slot)` (`models.c:475`) | `ProcessCarModelLump` (`models.c:633`) |
| Car colours (`CAR_COSMETICS`) | foreign `<CITY>.LCF` | `GetCarImportCosmetics(slot)` (`models.c:529`) | `ProcessCosmeticsLump` (`cosmetic.c:71`) |
| Car palettes (`LUMP_PALLET`, id 25) | foreign `.LEV` DATA1 | `GetCarImportPallet(&size)` (`models.c:462`) | `ProcessImportedPalette` (`cars.c:1496`) |

For geometry, `ProcessCarModelLump` swaps only its **base pointer** (`models.c:642-651`)
— the foreign block has the same layout as the level's own, so the offset-table
walk is unchanged. The foreign bytes are only read during the build:
`GetCarModel`/`buildNewCarFromModel` **copy into the level heap** (`mallocptr`).
The source buffer itself is not freed after the build — see "Cost and lifetime".

## Palette mapping

The imported city's `LUMP_PALLET` is merged with **that city's** texture-set
mapping (`ProcessPalletLumpForCity(lump, size, city)`, `cars.c:1431`), and
`GetCarPalIndex` (`cars.c:1933`) falls back to the imported city's table for a
page the host level does not know — otherwise a foreign vehicle's pages would all
collapse to slot 0 and it would be painted with the **host's** palette.
`ProcessImportedPalette` (`cars.c:1496`) does this right after the level's own
`ProcessPalletLump` (`texture.c:517-518`). The mechanism (`carTpages`, `civ_clut`,
the per-city set numbers) is `FORMATS.md` §2 and §4.

## Cost and lifetime

The import mallocs and holds the **whole foreign DATA1 region**
(`imp->region`, `models.c:377`) — not just the car-models block it points into
(the block is a sub-range; `FORMATS.md` §1 sizes DATA1, §3 sizes the block) —
plus the foreign `.LCF` (4096 bytes on disk). All of it is held for the **whole
level**: it is freed only when the next level initialises the import again
(`FreeCarImport` at `models.c:424`), or on a failed load. So one foreign city's
data is resident at a time.

A stock level pays nothing: `gCarImportCity` stays `-1` and every `GetCarImport*`
getter answers NULL (`models.c:455-537`).

## Failure behaviour

Everything fails soft. If the file, the DATA1 region or the `LUMP_CAR_MODELS`
segment is missing, `LoadCarImport` returns 0 and the level keeps its own
vehicles (`models.c:441-445`, logged as `cross-city: no usable car data in …`).

A slot that asks for a **model the foreign city lacks** keeps the level's own car
too: `GetCarImportModels` returns NULL when the foreign offset table says `-1`
(no such model) or when the damaged/low variants are missing
(`models.c:508-522`), and `ProcessCarModelLump` falls back to its own `lump_ptr`.
It logs why (`models.c:638-640`), because otherwise it just looks like "the model
did not load".

## Traps

- **`-car slot9` = model 11 on a Chicago level crashes during load.** Chicago has
  no model 11 (the model-completeness table is `FORMATS.md` §3). A slot forced to
  a model the city lacks, with no fallback, is left with NULL model pointers —
  the state `CreateDentableCar`'s guard flags (`denting.c:224`, `:237`). The
  carhacks module lists this explicitly (`carhacks.c:27-28`).
- **The special slot ignores the per-city colours.** `car_cosmetics[SPECIAL_CAR_SLOT]`
  is not taken per model — it uses the cache `levelSpecCosmetics[model - 8]`,
  filled once for models 8..12 (`SetupSpecCosmetics`, `cosmetic.c:150-168`).
  `GetCarImportCosmetics()` is therefore consulted *before* either path, so an
  imported slot wears its own city's colours whichever slot it lands in
  (`cosmetic.c:71-78`).
- **Denting stays local.** There is no denting entry anywhere in the lump enum
  (`main.c:82-120`); `LoadCustomCarDentingFromFile` reads loose `.DEN` files
  (`denting.c:25-30`, called at `:454`, `:487`). An imported model dents as its
  host slot would.

## Turning it on

In `JERICHO/CONFIG/carhacks.ini` (off by default — `carhacks.c:47`):

```
cross_city_vehicles = 1
import = 2:0:10
```

`cross_city_vehicles` gates the whole hack. `import` takes comma-separated
`slot:city:model` entries (`carhacks.c:143-206`) — here, Chicago's school bus
(model 10) into resident slot 2. City numbers are `0` CHICAGO, `1` HAVANA,
`2` VEGAS, `3` RIO (`system.c:130`). Slots 0..4 feed ambient traffic (the model
list is `modelRandomList`, `civ_ai.c:47`), 5.. up to count-2 are spare capacity
that stock levels leave empty, and the last slot (`SPECIAL_CAR_SLOT =
MAX_CAR_RESIDENT_MODELS - 1` = 7, `dr2limits.h:29`, `:34`) is the special slot
(`jer_events.h:512`); several entries may name different cities.

Verified in the log by the size of the block that was actually read — with
`import = 2:0:10` on a **Havana** level:

```
[carhacks] import: slot 2 <- model 10 from CHICAGO
cross-city: car data from CHICAGO (123324 bytes of models, 15576 of car palettes, 4096 of cosmetics)
```

The 123324 is Chicago's car-models block, not Havana's own (which is larger), so
the foreign file really is what got loaded — `FORMATS.md` §3 owns those figures.

## Where this is implemented

- **`models.c`** owns the import: `InitCarImport()` (`:419`), the reader
  `LoadCarImport()` (`:336`), the aligned segment walker `FindLumpSegment()`
  (`:253`), and the getters `GetCarImportCity` / `GetCarImportPallet` /
  `GetCarImportModels` / `GetCarImportCosmetics` (`:455` / `:462` / `:475` / `:529`).
- **`mission.c`** fires `JER_EVENT_CAR_DATA_SOURCE` and calls `InitCarImport` right
  after (`:433-451`), and holds the per-slot city (`GetCarModelSourceCity`, `:334`).
- **`carhacks.c`** is the module: `ChkOnCarDataSource` (`:213`) reads
  `source_city` / `player_model` / `traffic_model` / `traffic_slot` and calls
  `ChkApplyImports` (`:143`) for the `import` list. It does not compute anything —
  it just writes model numbers and source cities and lets the engine read them.
- **Consumers:** `ProcessCarModelLump` (`models.c:633`), `ProcessCosmeticsLump`
  (`cosmetic.c:71`), `ProcessImportedPalette` (`cars.c:1496`) + `GetCarPalIndex`
  (`cars.c:1933`).

Both `GetCarImportModels` and `GetCarImportCosmetics` answer NULL for every slot
the module did not import from, so a stock level takes exactly its old path.

## Status: geometry and palettes, not yet correct pixels

The import brings a foreign vehicle's **geometry** and its **car
colours/palettes**, but a foreign vehicle is **not yet visually correct**. Its
polygons carry `texture_set` numbers that name *its own* city's texture pages, and
those pages are not in this level's `texture_pages` / `texture_cluts` tables
(`FORMATS.md` §2 and §6) — so its polys reference a page the host level never
loaded and it draws wrong (or not at all). Importing the foreign `TPAGE` region
(and its spool page list) so the host level's tables carry the pages the geometry
points at is **planned, not done**.

## Related

The folder override (`GetCarDataFolder()`, driven by
`JER_EVENT_CAR_DATA_SOURCE.sourceLevel`) redirects the **loose-file** loaders only
(`.MDL` / `.COS` / `.DEN`). On its own it cannot produce foreign vehicles, because
there is no loose vehicle data to find. It stays as the hand-made-car path.
